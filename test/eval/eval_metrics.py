#!/usr/bin/env python3
"""eval_metrics.py - aggregate eval_btt results into per-track and summary metrics.

Reads a results dir produced by run_eval.sh (shard-*/states.csv + shard-*/beats.csv)
plus the manifest, and writes per_track.csv + summary.csv into the results dir and
prints the summary.

Per-track metrics:
  ref_bpm, est_bpm (median readout while locked, after t_lock+2 s; falls back to median
  of positive readouts in the last 30 s), t_lock_s, locked_frac, n_lock_losses,
  octave_jumps (locked readout jumps with |log2| > 0.9), raw_ratio = est/ref,
  err_pct = (est/ref-1)*100, acc2/acc4 (raw, no octave folding - a MIDI clock at 2x is
  wrong), class ok/double/half/other, beat_rate_ratio (beats/s while locked vs ref/60),
  ibi_cv (CV of inter-beat intervals while locked), phase_R + beat_cov (only when beat
  annotations exist: circular phase consistency of emitted beats vs annotated beats and
  fraction of annotated beats matched within 70 ms).

Summary: overall and per BPM band (<=120, 120-130, 130-150, >150) and per dataset.
"""
import argparse
import csv
import math
import os
import statistics
import sys
from collections import defaultdict

LOG2_2 = math.log2(2.0)


def fold_octave(r):
    while r >= 2 ** 0.5:
        r /= 2.0
    while r < 2 ** -0.5:
        r *= 2.0
    return r


def _csv_paths(results, name):
    """states.csv/beats.csv either directly in results/ (single run) or in shard-*/ subdirs."""
    direct = os.path.join(results, name)
    if os.path.exists(direct):
        return [direct]
    paths = []
    for d in sorted(os.listdir(results)):
        p = os.path.join(results, d, name)
        if os.path.isdir(os.path.join(results, d)) and os.path.exists(p):
            paths.append(p)
    return paths


def load_states(results):
    states = defaultdict(list)   # id -> list of dict
    for p in _csv_paths(results, "states.csv"):
        with open(p) as f:
            for row in csv.DictReader(f):
                states[row["id"]].append({
                    "t": float(row["t_s"]), "bpm": float(row["bpm"]),
                    "period": int(row["period_samples"]), "cert": float(row["certainty"]),
                    "locked": int(row["locked"]), "silent": int(row["silent"])})
    return states


def load_beats(results):
    beats = defaultdict(list)    # id -> list of (sample, bpm)
    for p in _csv_paths(results, "beats.csv"):
        with open(p) as f:
            for row in csv.DictReader(f):
                beats[row["id"]].append((int(row["beat_sample"]), float(row["bpm"])))
    return beats


def ref_beats(path):
    ts = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if parts:
                ts.append(float(parts[0]))
    return ts


def band(bpm):
    if bpm <= 120:
        return "<=120"
    if bpm <= 130:
        return "120-130"
    if bpm <= 150:
        return "130-150"
    return ">150"


def track_metrics(row, sts, bts, ds_dir):
    rid = row["id"]
    ref = float(row["ref_bpm"])
    st = sorted(sts.get(rid, []), key=lambda s: s["t"])
    out = {"id": rid, "dataset": row["dataset"], "ref_bpm": ref}
    if not st:
        out.update(est_bpm=float("nan"), t_lock_s=float("nan"), locked_frac=0.0,
                   n_lock_losses=0, octave_jumps=0, raw_ratio=float("nan"),
                   err_pct=float("nan"), cls="nostates", beat_rate_ratio=float("nan"),
                   ibi_cv=float("nan"), phase_R=float("nan"), beat_cov=float("nan"))
        return out
    dur = st[-1]["t"]
    locked = [s for s in st if s["locked"]]
    t_lock = locked[0]["t"] if locked else float("nan")
    # engine-style estimate: median readout while locked after lock+2 s
    tail = [s["bpm"] for s in locked if s["bpm"] > 0 and s["t"] >= t_lock + 2.0] if locked else []
    if not tail:
        tail = [s["bpm"] for s in st if s["bpm"] > 0 and s["t"] >= max(0.0, dur - 30.0)]
    est = statistics.median(tail) if tail else 0.0
    # lock losses after first lock
    losses = 0
    seen_lock = False
    prev = 0
    for s in st:
        if s["locked"]:
            seen_lock = True
        elif seen_lock and prev:
            losses += 1
        prev = s["locked"]
    # octave jumps between consecutive locked readouts
    lj = [s for s in locked if s["bpm"] > 0]
    jumps = sum(1 for a, b in zip(lj, lj[1:]) if abs(math.log2(b["bpm"] / a["bpm"])) > 0.9)
    raw = est / ref if ref > 0 and est > 0 else float("nan")
    if math.isnan(raw):
        cls = "noest"
    elif abs(raw - 1.0) <= 0.04:
        cls = "ok"
    elif raw > 1.5:
        cls = "double"
    elif raw < 0.67:
        cls = "half"
    else:
        cls = "other"
    # beats while locked
    bs = sorted(bts.get(rid, []))
    if locked:
        lo = int(t_lock * 44100)
        lb = [b for b in bs if b[0] >= lo]
        if len(lb) >= 2 and dur > t_lock:
            rate = len(lb) / (dur - t_lock)
            beat_rate_ratio = rate / (ref / 60.0)
            ibis = [b - a for (a, _), (b, _) in zip(lb, lb[1:])]
            ibi_cv = (statistics.pstdev(ibis) / statistics.mean(ibis)) if ibis and statistics.mean(ibis) > 0 else float("nan")
        else:
            beat_rate_ratio = float("nan")
            ibi_cv = float("nan")
    else:
        beat_rate_ratio = float("nan")
        ibi_cv = float("nan")
    # phase vs annotated beats (gtzan)
    phase_R = beat_cov = float("nan")
    if row.get("beats"):
        bp = os.path.join(ds_dir, row["beats"])
        if os.path.exists(bp) and est > 0:
            refb = ref_beats(bp)
            period = 60.0 / est
            angles = []
            matched = 0
            est_times = [b[0] / 44100.0 for b in bs]
            for rb in refb:
                if not est_times:
                    break
                # nearest est beat
                import bisect
                i = bisect.bisect_left(est_times, rb)
                cands = [est_times[j] for j in (i - 1, i) if 0 <= j < len(est_times)]
                if not cands:
                    continue
                te = min(cands, key=lambda x: abs(x - rb))
                if abs(te - rb) <= 0.07:
                    matched += 1
                delta = (rb - te) % period
                angles.append(2 * math.pi * delta / period)
            if angles:
                R = abs(sum(complex(math.cos(a), math.sin(a)) for a in angles)) / len(angles)
                phase_R = R
                beat_cov = matched / len(refb) if refb else float("nan")
    out.update(
        est_bpm=est, t_lock_s=t_lock, locked_frac=len(locked) / len(st),
        n_lock_losses=losses, octave_jumps=jumps, raw_ratio=raw,
        err_pct=(raw - 1) * 100 if raw == raw else float("nan"), cls=cls,
        beat_rate_ratio=beat_rate_ratio, ibi_cv=ibi_cv, phase_R=phase_R, beat_cov=beat_cov)
    return out


def summarize(rows, keyfn, title, w):
    groups = defaultdict(list)
    for r in rows:
        groups[keyfn(r)].append(r)
    w.writerow([f"--- {title} ---"])
    w.writerow(["group", "n", "acc4", "acc2", "med_abs_err_pct", "double", "half", "other",
                "noest", "med_t_lock_s", "med_locked_frac", "oct_jumps_per_track",
                "med_beat_rate_ratio", "med_ibi_cv", "med_phase_R", "med_beat_cov"])
    for g in sorted(groups):
        rs = groups[g]
        n = len(rs)
        acc4 = sum(1 for r in rs if r["cls"] == "ok") / n
        acc2 = sum(1 for r in rs if r["raw_ratio"] == r["raw_ratio"] and abs(r["raw_ratio"] - 1) <= 0.02) / n
        errs = [abs(r["err_pct"]) for r in rs if r["err_pct"] == r["err_pct"]]
        tl = [r["t_lock_s"] for r in rs if r["t_lock_s"] == r["t_lock_s"]]
        lf = [r["locked_frac"] for r in rs]
        oj = sum(r["octave_jumps"] for r in rs) / n
        brr = [r["beat_rate_ratio"] for r in rs if r["beat_rate_ratio"] == r["beat_rate_ratio"]]
        cv = [r["ibi_cv"] for r in rs if r["ibi_cv"] == r["ibi_cv"]]
        pr = [r["phase_R"] for r in rs if r["phase_R"] == r["phase_R"]]
        bc = [r["beat_cov"] for r in rs if r["beat_cov"] == r["beat_cov"]]
        w.writerow([g, n, f"{acc4:.3f}", f"{acc2:.3f}",
                    f"{statistics.median(errs):.2f}" if errs else "",
                    sum(1 for r in rs if r["cls"] == "double"),
                    sum(1 for r in rs if r["cls"] == "half"),
                    sum(1 for r in rs if r["cls"] == "other"),
                    sum(1 for r in rs if r["cls"] == "noest"),
                    f"{statistics.median(tl):.1f}" if tl else "",
                    f"{statistics.median(lf):.3f}",
                    f"{oj:.2f}",
                    f"{statistics.median(brr):.3f}" if brr else "",
                    f"{statistics.median(cv):.3f}" if cv else "",
                    f"{statistics.median(pr):.3f}" if pr else "",
                    f"{statistics.median(bc):.3f}" if bc else ""])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--ds", required=True)
    args = ap.parse_args()
    with open(args.manifest) as f:
        manifest = list(csv.DictReader(f))
    states = load_states(args.results)
    beats = load_beats(args.results)
    rows = [track_metrics(r, states, beats, args.ds) for r in manifest]

    out_csv = os.path.join(args.results, "per_track.csv")
    fields = ["id", "dataset", "ref_bpm", "est_bpm", "t_lock_s", "locked_frac", "n_lock_losses",
              "octave_jumps", "raw_ratio", "err_pct", "cls", "beat_rate_ratio", "ibi_cv",
              "phase_R", "beat_cov"]
    with open(out_csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow(r)

    sum_csv = os.path.join(args.results, "summary.csv")
    with open(sum_csv, "w", newline="") as f:
        w = csv.writer(f)
        summarize(rows, lambda r: "ALL", "all tracks", w)
        summarize(rows, lambda r: r["dataset"], "by dataset", w)
        summarize(rows, lambda r: band(r["ref_bpm"]), "by ref BPM band", w)
        summarize(rows, lambda r: f'{r["dataset"]}|{band(r["ref_bpm"])}', "dataset x band", w)
    with open(sum_csv) as f:
        print(f.read())
    print(f"wrote {out_csv} and {sum_csv}")


if __name__ == "__main__":
    main()
