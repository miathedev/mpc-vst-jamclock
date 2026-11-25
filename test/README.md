# test/ - offline real-time harness for the JamClock tracker + clock

`harness.c` feeds synthetic signals in **real time** (128-frame blocks, paced on
CLOCK_MONOTONIC) through the engine's `process()` and logs every emitted MIDI event
(START/CONTINUE/STOP/CLOCK/SPP) with a timestamp. The build flag `-DJAMCLOCK_HARNESS`
compiles a log hook (`jclk_log_event`) into the engine instead of the ALSA output. Each
run prints one CSV row:

```
engine,scenario,dur_s,t_lock_s,bpm_readout,bpm_ticks,bpm_err,octave_ratio,rate_ppm,drift_ppm,
drift_ppm_stallfree,jitter_pct,jitter_p95_pct,jitter_rms_pct,sweep_max_err_pct,octave_jumps,
n_start,n_cont,n_stop,n_spp,pass_bad,final_status,max_process_us,ring_full_calls,ring_full_frames
```

Column groups: lock (`t_lock_s`, `bpm_ticks` = BPM from the median tick period after
lock+2 s, `bpm_err`/`rate_ppm` vs ground truth), drift (`drift_ppm`, and `drift_ppm_stallfree`
without host stalls, both empty for `sweep`/`volllast`), jitter (`jitter_pct` = max incl.
host stalls, `jitter_p95_pct`/`jitter_rms_pct` = the engine's own share), sweep
(`sweep_max_err_pct` vs the instantaneous tempo), event counts, passthrough errors
(`pass_bad`), and for `volllast` only: `max_process_us,ring_full_calls,ring_full_frames`.

## Scenarios

| Scenario | Signal | Expectation |
|---|---|---|
| click60/90/120/174 | synthetic click | LOCKED < 10 s, err < +-0.5 BPM (click60 needs `bias=Half`, see below) |
| drums, drums_lp120, drums_lp80 | kick 55 Hz + hat 8 kHz (lp = biquad lowpass, only kick+bass remain), 120/80 BPM | LOCKED < 10 s, ~120 BPM, no octave jump |
| sweep | drums 90->130 BPM over 18 s | `octave_jumps=0`, follows (`sweep_max_err_pct` vs instantaneous tempo) |
| steady120 / steady120_5min | 45 s / 300 s drums at 120 BPM | small drift, jitter < 5 % (5-min run is not in the default set) |
| silence | 10 s drums, 6 s silence, 10 s drums | STOP in the silence, then CONTINUE (no 2nd START), SPP at both |
| volllast | 8 s flat-out burst (ring fills), then 12 s paced | no crash, bounded `process()`, normal lock after |

Note on click60: BTT's CBSS normalization is skewed at the lower edge of the search range,
so `minbpm` must sit *below* the true tempo (minbpm=60 flips a 60 BPM click to 120, the
default 40 does not). A pure click is octave-ambiguous, hence `bias=Half` for click60.

## Running

```sh
./test/run_ab.sh                 # all scenarios (~6 min real time)
SCEN="click120 drums_lp120" ./test/run_ab.sh
./test/run_ab.sh steady120_5min  # long drift run (~5 min)
```

- Runs in real time (engine clocks on CLOCK_MONOTONIC), `HARNESS_DEBUG=1` dumps events,
  `HARNESS_PRIOR=`/`HARNESS_BIAS=` override chunk-only params.
- Worker-scheduling is verified on stderr every run (`policy=0(SCHED_OTHER)`, audio core
  excluded).
- Jitter on a shared host includes host scheduling noise (`jitter_pct` max varies),
  `jitter_p95_pct`/`jitter_rms_pct` isolate the engine (steady120: p95 0.53 %). The
  authoritative < 5 % check is on-device - see `test/on_device_wp2.md`.
- `btt_solo.c` feeds a signal straight into the vendored BTT (no engine):
  `build/btt_solo click 120 0 10 120 0.625 40 240`. Harness compiles warning-free under
  `-Wall -Wextra`:
  `gcc -O2 -Wall -Wextra -DJAMCLOCK_HARNESS -I src -I "$FW/wrapper" -c test/harness.c -o /dev/null`
- ASan/UBSan coverage is in `./test.sh` (framework host test), the harness deliberately
  runs without sanitizers to keep timing clean.

## Dataset evaluation (real audio)

`test/eval/` runs the same tracker stack offline over real music (GiantSteps + GTZAN with
Beat-This! annotations) - see `test/eval/README.md`. Headline (1660 tracks): 130-150 BPM
raw 4% accuracy 0.69 -> 0.96, >150 BPM 0.16 -> 0.60 after the CBSS mode-2 + Auto-anchor-120
change. Reproduce with `./test/eval/run_eval.sh <name>`.
