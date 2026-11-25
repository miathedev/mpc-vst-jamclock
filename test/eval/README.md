# test/eval/ - offline dataset evaluation of the JamClock tracker

Real-time synthetic tests live in `test/` (`run_ab.sh`). This directory validates the
tracker against **real music datasets**, offline (~20-30x realtime per core), on the exact
same code path as the engine (`btt_wrap` + vendored BTT, 128-sample hops, engine lock
rule and parameter mapping).

## Datasets (test/datasets/, gitignored - `prep_datasets.py` fetches/decodes)

| dataset | source | content | ref tempo |
|---|---|---|---|
| `giantsteps` | [GiantSteps tempo dataset](https://github.com/GiantSteps/giantsteps-tempo-dataset) (lofi mp3 previews from `cp.jku.at`, md5-verified, 120 s) | 661 EDM tracks, `annotations_v2` tempo (565 >120 BPM) | `.bpm` per track |
| `gtzan` | GTZAN audio (marsyas HF mirror, 22050 Hz wav -> 44.1 kHz) + beat annotations from "Beat This!" ([Zenodo 13922116](https://zenodo.org/records/13922116)) | 999 tracks, 30 s, per-beat annotations | 60 / median IBI of the beat annotations |

`prep_datasets.py` writes `test/datasets/manifest.csv` (`id,dataset,raw,ref_bpm,beats`) and
decodes all audio to mono 44.1 kHz f32le in `test/datasets/raw/` (ffmpeg, cached).

## Running

```sh
python3 test/eval/prep_datasets.py     # once: annotations + audio + decode (~1 GB, ~18 GB raw)
./test/eval/run_eval.sh baseline       # full manifest, engine defaults
EVAL_OPTS="--anchor 100 --cbss-mode 0" ./test/eval/run_eval.sh old_behavior
MANIFEST=test/datasets/manifest_sweep.csv JOBS=6 ./test/eval/run_eval.sh sweep_subset
```

`run_eval.sh` shards the manifest into resumable chunks (`results/<name>/chunk-*/` with
`.done` markers - safe to re-run after an interruption), then `eval_metrics.py` writes
`per_track.csv` + `summary.csv`. `eval_btt` options mirror the engine parameters
(`--min/--max` = minbpm/maxbpm, `--anchor`, `--bias`, `--ratio` = prior, `--decay`,
`--sil`, `--sil-rms`, `--cbss-mode`, onset/OSS knobs, `--max-seconds`, `--id`/`--limit`).
With no options it runs the engine's current defaults.

## Metrics

Per track: `est_bpm` (median readout while locked after lock+2 s), `t_lock_s`,
`locked_frac`, `n_lock_losses`, `octave_jumps` (|log2| > 0.9), raw `err_pct` vs the
annotated BPM and class `ok`/`double`/`half`/`other` (**raw** - a clock at 2x is wrong),
`beat_rate_ratio`/`ibi_cv` (regularity while locked), and for GTZAN `phase_R` (circular
phase consistency vs annotated beats) + `beat_cov` (annotated beats matched within 70 ms).
Summaries: overall, per dataset, per ref-BPM band (<=120 / 120-130 / 130-150 / >150).

## Results (2026-11-24, full manifest, 1660 tracks)

Raw 4% tempo accuracy (`acc4`), old engine defaults vs new (CBSS mode 2 + Auto anchor 120 +
maxbpm 200). Raw results in `test/eval/results/{baseline,full_new}`:

| group | n | old acc4 | new acc4 | old 130-150 / >150 | new 130-150 / >150 |
|---|---|---|---|---|---|
| ALL | 1660 | 0.651 | **0.717** | 0.687 / 0.156 | **0.958 / 0.602** |
| giantsteps | 661 | 0.610 | **0.791** | 0.726 / 0.243 | **0.949 / 0.711** |
| giantsteps >120 | 565 | 0.641 | **0.905** | - | - |
| gtzan | 999 | 0.679 | 0.669 | 0.613 / 0.069 | **0.976 / 0.494** |
| <=120 (all) | 670 | 0.791 | 0.558 | - | - |

Locking quality (ALL): octave jumps/track 0.73 -> 0.50, GTZAN phase_R 0.61 -> 0.67, beat
coverage 0.66 -> 0.75, median time-to-lock unchanged (4.2 s), locked fraction 0.86.

Known trade-off: with the Auto anchor at 120, slow material (< ~100 BPM) double-locks more
(<=120 acc4 0.79 -> 0.56). Measured remedy on the slow subset (`manifest_slow.csv`, 670
tracks, results `sl_*`) - which is why `maxbpm` is settable down to 60 in the UI:

| slow-subset config | acc4 | doubles | halves |
|---|---|---|---|
| old default (max 180, mode 0, anchor 84.9) | 0.791 | 86 | 2 |
| new default (max 200, mode 2, anchor 120) | 0.558 | 245 | 0 |
| **maxbpm=115 (mode 2, anchor = geo-mid 67.8)** | **0.812** | 34 | 24 |
| maxbpm=110 (mode 2, anchor 66.4) | 0.712 | 32 | 83 |
| maxbpm=180, mode 2, anchor 85 | 0.745 | 107 | 2 |

So for music up to ~115 BPM: set `maxbpm=115` (blocks the double octave and re-anchors the
prior low). For 60 BPM material additionally `bias=Half`.

## What was tried

Sweeps over anchor (84-160), range max (110-240), prior ratio, histogram decay, candidate
count and CBSS eta on a 206-track subset, plus BTT patch candidates - only one kept:
per-candidate prior weighting, disabling the 2i/4i autocorrelation boost, and a
z-score-with-slow-tie-break mode 4 (measured worse: 0.50 acc4, 127 halves on the slow
subset) were reverted. Kept: CBSS mode 2 = raw `score_max` argmax
(`btt_set_cbss_score_mode(2)`, `JAMCLOCK PATCH` in `src/btt/`). See `src/btt/VENDORED.md`.
