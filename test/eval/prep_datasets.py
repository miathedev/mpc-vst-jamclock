#!/usr/bin/env python3
"""prep_datasets.py - build test/datasets/manifest.csv and decode all audio to raw
mono 44.1 kHz f32le cache (test/datasets/raw/*.f32).

Datasets (see test/eval/README.md for provenance/licensing):
  giantsteps : GiantSteps Tempo Dataset (annotations_v2), audio = lofi mp3 previews
               downloaded from cp.jku.at (md5-verified), one BPM per track.
  gtzan      : GTZAN audio (marsyas mirror of the original genres.tar.gz) + beat
               annotations from "Beat This!" (Zenodo 13922116). Reference BPM is
               derived from the beat annotations (60 / median inter-beat interval).

Manifest (CSV, paths relative to test/datasets/):
  id,dataset,raw,ref_bpm,beats

Usage: python3 test/eval/prep_datasets.py [--jobs 12] [--only giantsteps|gtzan]
Idempotent: existing raw files are skipped.
"""
import argparse
import csv
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
DS = os.path.normpath(os.path.join(HERE, "..", "datasets"))
RAW = os.path.join(DS, "raw")
GS = os.path.join(DS, "giantsteps")
GTZAN_AUDIO = os.path.join(DS, "gtzan_audio", "genres")
GTZAN_BEATS = os.path.join(DS, "beat_this_annotations", "gtzan", "annotations", "beats")


def decode(src, dst):
    """mono 44.1 kHz f32le via ffmpeg. LD_LIBRARY_PATH is stripped: the ambient one
    (editor extension dir) breaks the system ffmpeg's shared libs."""
    env = dict(os.environ)
    env.pop("LD_LIBRARY_PATH", None)
    env.pop("LD_PRELOAD", None)
    tmp = f"{dst}.tmp{os.getpid()}"
    try:
        r = subprocess.run(
            ["ffmpeg", "-v", "error", "-y", "-i", src, "-vn", "-ac", "1", "-ar", "44100",
             "-f", "f32le", "-acodec", "pcm_f32le", tmp],
            env=env, capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(tmp) or os.path.getsize(tmp) == 0:
            sys.stderr.write(f"ffmpeg failed for {src}: {r.stderr[:300]}\n")
            return False
        os.replace(tmp, dst)
        return True
    except OSError as e:
        sys.stderr.write(f"decode error for {src}: {e}\n")
        return False
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def gs_manifest_rows():
    """One row per giantsteps track with a valid BPM annotation."""
    rows = []
    tempo_dir = os.path.join(GS, "annotations_v2", "tempo")
    audio_dir = os.path.join(GS, "audio")
    for fn in sorted(os.listdir(tempo_dir)):
        if not fn.endswith(".bpm"):
            continue
        base = fn[:-4]                      # e.g. 1030011.LOFI
        bpm = 0.0
        with open(os.path.join(tempo_dir, fn)) as f:
            tok = f.read().split()
            if tok:
                try:
                    bpm = float(tok[0])
                except ValueError:
                    bpm = 0.0
        if bpm <= 1.0:
            continue                        # un-annotated track
        src = os.path.join(audio_dir, base + ".mp3")
        if not os.path.exists(src):
            continue
        rows.append((f"gs_{base}", "giantsteps", f"raw/gs_{base}.f32", f"{bpm:.4f}", "", src))
    return rows


def gtzan_manifest_rows():
    """GTZAN: ref BPM from the Beat This! beat annotations (median IBI)."""
    rows = []
    for fn in sorted(os.listdir(GTZAN_BEATS)):
        if not fn.endswith(".beats"):
            continue
        stem = fn[:-6]                      # gtzan_blues_00000
        genre, num = stem.split("_")[1], stem.split("_")[2]
        src = os.path.join(GTZAN_AUDIO, genre, f"{genre}.{num}.wav")
        beats = os.path.join(GTZAN_BEATS, fn)
        if not os.path.exists(src) or not os.path.exists(beats):
            continue
        times = []
        with open(beats) as f:
            for line in f:
                parts = line.split()
                if parts:
                    times.append(float(parts[0]))
        ibis = [b - a for a, b in zip(times, times[1:]) if 0.2 < b - a < 3.0]
        if len(ibis) < 8:
            continue
        ibis.sort()
        ref = 60.0 / ibis[len(ibis) // 2]
        rel_beats = os.path.relpath(beats, DS)
        rows.append((stem, "gtzan", f"raw/{stem}.f32", f"{ref:.4f}", rel_beats, src))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=min(12, os.cpu_count() or 4))
    ap.add_argument("--only", choices=["giantsteps", "gtzan"], default=None)
    args = ap.parse_args()
    os.makedirs(RAW, exist_ok=True)

    rows = []
    if args.only in (None, "giantsteps"):
        rows += gs_manifest_rows()
    if args.only in (None, "gtzan"):
        rows += gtzan_manifest_rows()
    print(f"{len(rows)} tracks in manifest")

    # decode everything that is not cached yet (row = id,dataset,raw,ref_bpm,beats,src)
    todo = [(os.path.join(DS, r[2]), r[5]) for r in rows if not os.path.exists(os.path.join(DS, r[2]))]
    print(f"{len(todo)} files to decode")
    ok = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for res in ex.map(lambda t: decode(t[1], t[0]), todo):
            ok += res
    print(f"decoded {ok}/{len(todo)}")

    manifest = os.path.join(DS, "manifest.csv")
    with open(manifest, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["id", "dataset", "raw", "ref_bpm", "beats"])
        for r in rows:
            w.writerow(r[:5])
    print(f"wrote {manifest}")


if __name__ == "__main__":
    main()
