#!/usr/bin/env bash
# A/B harness: build test/harness against v0.2 (src/jamclock_engine.c + vendored BTT) and
# against the v0.1 baseline (git HEAD copy with JAMCLOCK_HARNESS hooks, via make_v01.py),
# run all scenarios for both engines in real time, print one CSV line per run.
#
#   ./test/run_ab.sh            # full A/B (about 6 min real time)
#   ./test/run_ab.sh click120   # single scenario, both engines
#   SCEN="click60" ./test/run_ab.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# framework location: probe candidate paths in order; set FW=/path/to/framework to override
# (FW= if the framework is not a sibling checkout)
FW="${FW:-}"
for c in "$REPO/framework" "$REPO/../../mpc-vst-jamclock/framework" "$REPO/../mpc-vst-plugins"; do
  if [ -z "$FW" ] && [ -d "$c/wrapper" ]; then FW="$c"; fi
done
[ -n "$FW" ] || { echo "no framework/wrapper found - set FW=/path/to/framework" >&2; exit 1; }
CC="${CC:-gcc}"
CFLAGS="-O2 -g -DJAMCLOCK_HARNESS -I$REPO/src -I$FW/wrapper"
mkdir -p "$HERE/build"

# v0.1 baseline: make_v01.py extracts src/jamclock_engine.c from git HEAD, applies the
# harness hooks and verifies the result really is the v0.1 engine; on any mismatch it
# fails loudly (exit 1). Without a verified v0.1 baseline there is no A/B comparison:
# any stale prebuilt build/engine_v01.c is removed and only the v0.2 scenarios run,
# with a loud warning - never a silent wrong A/B.
if ! python3 "$HERE/make_v01.py"; then
  rm -f "$HERE/build/engine_v01.c"
  echo "WARNING: no verified v0.1 baseline (make_v01.py failed) - running v0.2 scenarios only, A/B comparison skipped" >&2
fi
# shellcheck disable=SC2086
$CC $CFLAGS "$HERE/harness.c" "$REPO/src/jamclock_engine.c" "$REPO/src/btt_wrap.c" \
    "$REPO"/src/btt/src/*.c -lm -ldl -lpthread -o "$HERE/build/harness_v02"
if [ -f "$HERE/build/engine_v01.c" ]; then
  # shellcheck disable=SC2086
  $CC $CFLAGS "$HERE/harness.c" "$HERE/build/engine_v01.c" -lm -ldl -lpthread -o "$HERE/build/harness_v01"
fi

# Default A/B stays short (steady120 = 45 s). Long/extra scenarios on demand, e.g.:
#   ./test/run_ab.sh steady120_5min volllast     # 5-min drift run + volllast stress scenario
SCEN="${SCEN:-click60 click90 click120 click174 drums drums_lp120 drums_lp80 sweep steady120 silence}"
[ $# -gt 0 ] && SCEN="$*"

# CSV columns per harness.c: rate_ppm = bpm_err/truth*1e6, jitter_p95_pct/jitter_rms_pct
# next to jitter_pct (max); drift_ppm/drift_ppm_stallfree stay empty for sweep/volllast
# (meaningless there). drift_ppm_stallfree accumulates only deltas <= 1.1x median of the
# steady segment (drift without host stalls). jitter_p95_pct is symmetric over |d-med| like
# max/rms. The last 3 columns (max_process_us,ring_full_calls,ring_full_frames) appear only
# in volllast lines.
echo "engine,scenario,dur_s,t_lock_s,bpm_readout,bpm_ticks,bpm_err,octave_ratio,rate_ppm,drift_ppm,drift_ppm_stallfree,jitter_pct,jitter_p95_pct,jitter_rms_pct,sweep_max_err_pct,octave_jumps,n_start,n_cont,n_stop,n_spp,pass_bad,final_status,max_process_us,ring_full_calls,ring_full_frames"
ENGS="v02"
[ -x "$HERE/build/harness_v01" ] && ENGS="v01 v02"
for eng in $ENGS; do
  for sc in $SCEN; do
    "$HERE/build/harness_$eng" "$sc" | sed "s/^/$eng,/"
  done
done
