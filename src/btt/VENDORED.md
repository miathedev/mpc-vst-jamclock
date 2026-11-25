# Vendored: Beat-and-Tempo-Tracking (BTT)

Source: <https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking>, upstream commit
`c039090` (MIT License - see `BTT_LICENSE`).

Vendored from <https://github.com/DVA-4/AudioClock>, `firmware/audioclock_5/` (BTT.h + src/),
which is exactly upstream `c039090` **plus the four AudioClock patches** (all marked with
`TEENSY PORT:` comments; full diffs in the AudioClock repo `patches/`, documented in its
`docs/btt-patches.md`):

1. `src/DFT.c` - rect-to-polar computes magnitude only (drops the unused `atan2`).
2. `src/DFT.c` - `sqrtf` fast path in the generalized autocorrelation (exponent 0.5).
3. `src/BTT.c` - tempo tracking decimated by `BTT_TEMPO_DECIMATION` (4); the histogram decay
   coefficient must then be the 4th power of the per-frame value (we set
   `btt_set_gaussian_tempo_histogram_decay()` accordingly in `btt_wrap.c`).
4. `src/BTT.c` - sub-bin tempo interpolation (parabola on the autocorrelation peak, exposed via
   the getters only; internal indexing stays on the integer lag).

Local JamClock patches on top (marked `JAMCLOCK PATCH` in `src/BTT.c` / `BTT.h`):

5. `src/BTT.c` - CBSS candidate score mode (`btt_set_cbss_score_mode()`, default 0 = upstream).
   Mode 2 picks the candidate with the raw maximum pulse-pattern cross-correlation peak
   (`score_max`), instead of upstream's `score_max/sum_max + variance/sum_variance`.
   Upstream compares maxima taken over *different* numbers of phases (one per unit of
   lag), which biases the winner toward large lags and made material above ~130 BPM lock
   at half tempo; the engine sets mode 2 in `bttw_init()`. Measured on GiantSteps + GTZAN
   (see `test/eval/`): raw 4% tempo accuracy 130-150 BPM 0.69 -> 0.96, >150 BPM 0.16 -> 0.60
   (1660 tracks, `test/eval/results/`). A z-score tie-break-to-slower variant (against
   double-tempo locks on slow material) was measured and rejected - see test/eval/.

Upstream's `.old` / `.old.txt` files are left out (duplicate symbols when compiling `src/`).

License note: BTT is MIT (permissive, GPL-3.0-compatible), so vendoring it into this
GPL-3.0-or-later plugin is fine; the notice is kept in `BTT_LICENSE`.

RT-safety: all allocation happens in `btt_new_default()` / `btt_destroy()` (calloc/free);
`btt_process()` and the callbacks do **no** malloc/free - safe to call from the worker thread
(checked against the vendored sources; the plugin only ever calls BTT from the worker).

## Known upstream quirks (dead code paths)

Two latent defects exist in the vendored upstream code; neither is reachable from the
plugin (the engine only ever uses `btt_set_beat_tracking_callback`):

- `btt_get_onset_tracking_callback` (BTT.c, around line 1177) has an unreachable
  statement after its `return`, and dereferences `self` without a NULL check.
- Metronome mode (BTT.c, around line 757) invokes `beat_callback` without a NULL check.
