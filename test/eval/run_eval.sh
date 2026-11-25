#!/usr/bin/env bash
# run_eval.sh - offline dataset evaluation of the JamClock tracker (eval_btt) over
# test/datasets/manifest.csv, in parallel resumable chunks, then aggregate metrics.
#
#   ./test/eval/run_eval.sh <run-name>          # engine defaults (40..180, geo-mid anchor)
#   EVAL_OPTS="--anchor 140 --ratio 0.8" ./test/eval/run_eval.sh a140
#   EVAL_OPTS="--id gs_" ./test/eval/run_eval.sh gs_only
#   JOBS=12 CHUNK=24 ./test/eval/run_eval.sh baseline
#
# The manifest is split into chunks of CHUNK tracks; each chunk runs as one eval_btt
# process writing results/<name>/chunk-XX/ and touches chunk-XX/.done on success.
# Already-done chunks are skipped, so re-invoking resumes an interrupted run (e.g. under
# a shell time cap) instead of restarting it. eval_btt runs ~20x realtime per core, so a
# full pass over all ~1660 tracks takes a few minutes on a 12-core host.
#
# Results land in test/eval/results/<run-name>/: chunk-*/states.csv + beats.csv plus
# per_track.csv and summary.csv from eval_metrics.py.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
NAME=${1:?usage: run_eval.sh <run-name>}
JOBS=${JOBS:-12}
CHUNK=${CHUNK:-24}
EVAL_OPTS=${EVAL_OPTS:-}
OUT="$HERE/results/$NAME"
DS="$REPO/test/datasets"
MANIFEST=${MANIFEST:-$DS/manifest.csv}

CC=${CC:-gcc}
mkdir -p "$HERE/build" "$OUT"
# shellcheck disable=SC2086
$CC -O2 -g -I"$REPO/src" "$HERE/eval_btt.c" "$REPO/src/btt_wrap.c" "$REPO"/src/btt/src/*.c \
    -lm -lpthread -o "$HERE/build/eval_btt"

mkdir -p "$OUT/chunks"
tail -n +2 "$MANIFEST" | split -l "$CHUNK" -d -a 3 - "$OUT/chunks/m-"
head -1 "$MANIFEST" > "$OUT/chunks/header"
for f in "$OUT/chunks"/m-*; do
  cat "$OUT/chunks/header" "$f" > "$f.h" && mv "$f.h" "$f"
done

pids=()
for f in "$OUT/chunks"/m-*; do
  n=$(basename "$f" | sed 's/^m-//')
  cdir="$OUT/chunk-$n"
  if [ -f "$cdir/.done" ]; then continue; fi
  mkdir -p "$cdir"
  # the chunk wrapper touches .done itself, so a killed parent still leaves finished
  # chunks marked and the next invocation resumes only the rest
  ( JAMCLOCK_DS="$DS" "$HERE/build/eval_btt" "$f" "$cdir" $EVAL_OPTS \
      > "$cdir/stdout.log" 2> "$cdir/stderr.log" && touch "$cdir/.done" ) &
  pids+=($!)
  # bound the number of concurrent chunk processes
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
done
fail=0
for p in "${pids[@]}"; do wait "$p" || fail=1; done
[ "$fail" = 0 ] || { echo "some eval_btt chunks failed - see $OUT/chunk-*/stderr.log" >&2; exit 1; }

python3 "$HERE/eval_metrics.py" "$OUT" --manifest "$MANIFEST" --ds "$DS"
