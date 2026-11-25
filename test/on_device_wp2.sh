#!/bin/sh
# on_device_wp2.sh - runs ON THE MPC X SE (as root), only on your own device and only on
# explicit request - never from a PC agent.
# Usage on device:
#   sh on_device_wp2.sh <ref_bpm> <duration_s> "<app seq port, e.g. 128:0>"
# Example:
#   sh on_device_wp2.sh 120 300 "128:0"
#
# What it does (see test/on_device_wp2.md):
#   1. root check + JamClock ALSA seq port present (client "JamClock", port "MIDI Out")
#   2. aconnect "JamClock:MIDI Out" -> app port; drop any old subscription first,
#      verify the connection afterwards (aconnect -l, else exit 1)
#   3. record <duration_s> s:
#      - aseqdump --help is probed for -t. alsa-utils 1.2.x (MPC OS 3.10) has NO -t
#        -> fallback: prefix every aseqdump line with epoch-ms (date +%s%3N).
#        Preflight guard BEFORE recording: date must support %3N (exactly 13 digits,
#        else abort exit 1 - busybox date without %3N).
#      - clean recording end via `timeout -s INT` (SIGINT) + stdbuf -oL - only if stdbuf
#        exists (coreutils-only; busybox without stdbuf: start without it, a log tail
#        <= 4 KB may be missing)
#      - plausibility guard: 'invalid option' in the log OR 0 events -> exit 1 (no silent pass)
#   4. metrics via awk - understands BOTH timestamp formats ($1 epoch-ms vs. aseqdump -t
#      relative in s -> *1000): START/CONTINUE/STOP/SPP counts, tick-to-tick jitter % =
#      max|d-mean|/mean*100 (symmetric, early AND late ticks), drift ppm vs. the ideal
#      24-ppq tempo at <ref_bpm>. Transport gaps (STOP/CONTINUE pause mid-run,
#      delta > 3x mean) are segmented - metrics per segment, worst case decides.
# Exit 0 = PASS, exit 1 = FAIL/invalid measurement.
# Acceptance: drift < 50 ppm/5min, jitter < 5 %, STOP on silence, CONTINUE on resume.
set -u

# HARD RULE: this script runs on the MPC as root - only on your own device, only on
# explicit request, never from a PC agent (no SSH/deploy/aconnect/aseqdump from an agent).
# Root check: pluginList/acvs need root.
if [ "$(id -u)" != "0" ]; then
    echo "FAIL: run as root on the MPC X SE (id -u=$(id -u))"
    exit 1
fi

BPM="${1:?usage: on_device_wp2.sh <ref_bpm> <duration_s> \"<app port>\"}"
DUR="${2:?usage: on_device_wp2.sh <ref_bpm> <duration_s> \"<app port>\"}"
APP="${3:?usage: on_device_wp2.sh <ref_bpm> <duration_s> \"<app port>\"}"
# Env overrides for synthetic PC tests only (never needed on the device):
LOG="${JAMCLOCK_WP2_LOG:-/tmp/jamclock_wp2.txt}"
SEQ_CLIENTS="${SEQ_CLIENTS:-/proc/asound/seq/clients}"

echo "== 1/4 JamClock port present?"
# Why plain 'JamClock' instead of '"JamClock"': old kernels quote in seq/clients with '...',
# newer ones with "..." - match both.
if ! grep -q 'JamClock' "$SEQ_CLIENTS"; then
    echo "FAIL: no ALSA seq client 'JamClock' in $SEQ_CLIENTS - is the plugin loaded and the app running?"
    exit 1
fi
grep -A3 'JamClock' "$SEQ_CLIENTS" | head -8

echo "== 2/4 aconnect JamClock:MIDI Out -> $APP"
# drop any old subscription first (idempotent, errors ignored - e.g. re-run after abort)
aconnect -d "JamClock:MIDI Out" "$APP" 2>/dev/null || true
aconnect "JamClock:MIDI Out" "$APP" || { echo "FAIL: aconnect failed"; exit 1; }
# verify hard: the connection must be visible in aconnect -l, else no silent pass
CONN="$(aconnect -l | grep -i -A3 jamclock)"
echo "$CONN"
if ! echo "$CONN" | grep -qi 'connecting to'; then
    echo "FAIL: connection JamClock:MIDI Out -> $APP not visible in 'aconnect -l'"
    exit 1
fi

echo "== 3/4 recording $DUR s of clock -> $LOG"
command -v timeout >/dev/null 2>&1 || { echo "FAIL: 'timeout' missing (busybox/coreutils)"; exit 1; }
# detect timestamp mode: -t does NOT exist in alsa-utils 1.2.x (MPC 3.10)
if aseqdump --help 2>&1 | grep -q -- '-t'; then
    TS_MODE=t          # aseqdump -t: $1 = relative timestamp (s)
else
    TS_MODE=epoch-ms   # fallback: prefix every aseqdump line with epoch-ms
fi
echo "ts mode: $TS_MODE"
# Preflight guard BEFORE recording - busybox date may not know %3N (width specifier);
# without milliseconds all tick deltas would be 0 (second resolution) and the
# measurement invalid. Check: exactly 13 digits (epoch-ms), else clear abort.
# On-device recommendation: date +%s%3N | wc -c == 14 (13 digits + newline).
if [ "$TS_MODE" = "epoch-ms" ]; then
    D13="$(date +%s%3N)"
    case "$D13" in
        [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]) : ;;
        *)
            echo "FAIL: busybox date without %3N - 'date +%s%3N' returned '$D13' (not 13 digits)."
            echo "      Without epoch-ms timestamps the measurement is invalid (second resolution),"
            echo "      and aseqdump -t is missing too (alsa-utils 1.2.x). Aborting."
            exit 1
            ;;
    esac
fi
rm -f "$LOG"
# clean recording end: timeout sends SIGINT to aseqdump after $DUR s (stdio flush in the
# SIGINT handler); stdbuf -oL (if present) writes every line to the log immediately.
# stdbuf is coreutils-only - busybox without stdbuf block-buffers stdio into the pipe
# (up to 4 KB); a SIGINT kill without flush then loses the log tail. Guard:
# start without stdbuf (works; the n>2 event guard does not misfire, only the tail may
# be missing - see test/on_device_wp2.md).
STDBUF=""
if command -v stdbuf >/dev/null 2>&1; then
    STDBUF="stdbuf -oL"
else
    echo "note: stdbuf not available (busybox?) - recording runs without stdbuf -oL; a short log tail (<= 4 KB) may be missing at the SIGINT end (see test/on_device_wp2.md)"
fi
if [ "$TS_MODE" = "t" ]; then
    timeout -s INT "$DUR" $STDBUF aseqdump -t -p "JamClock:MIDI Out" > "$LOG" 2>&1
else
    # busybox-compatible: read loop prefixes epoch-ms as the first field of every aseqdump
    # line (write per line via printf -> no stdio loss in the shell loop)
    timeout -s INT "$DUR" $STDBUF aseqdump -p "JamClock:MIDI Out" 2>&1 \
    | while IFS= read -r l; do printf '%s %s\n' "$(date +%s%3N)" "$l"; done > "$LOG"
fi
echo "recorded $(wc -l < "$LOG") lines"
# Plausibility guard: aseqdump did not understand an option -> measurement invalid
if grep -qi 'invalid option' "$LOG"; then
    echo "FAIL: 'invalid option' in the recording log - aseqdump call invalid, see $LOG"
    exit 1
fi

echo "== 4/4 metrics (ref ${BPM} BPM, 24 ppq)"
# AWK metrics block start - the synthetic PC test extracts this block 1:1 (sed) and feeds
# it sample logs; see the verification section in test/on_device_wp2.md.
AWK_PROG=$(cat <<'AWK_EOF'
function ts_ms(s,  t) {
    t = s + 0
    # $1 epoch-ms (13 digits) taken directly; aseqdump -t relative in s -> *1000.
    # threshold 1e11: epoch-s (~1.8e9) and relative s (<1e4) are below, epoch-ms (~1.8e12) above.
    return (t > 1e11) ? t : t * 1000
}
/0xfa|0xf8|0xfb|0xfc|0xf2/ || /Clock|Start|Continue|Stop|Song/ {
    t = ts_ms($1)
    if ($0 ~ /0xf8|Clock/) {
        n++
        if (n > 1) { d = t - pt; deltas[n - 1] = d; sum += d }
        pt = t
    }
    if ($0 ~ /0xfa|Start/)    ns++
    if ($0 ~ /0xfb|Continue/) nc++
    if ($0 ~ /0xfc|Stop/)     nx++
    if ($0 ~ /0xf2|Song/)     nsp++
}
END {
    ideal = 60.0 / (bpm * 24) * 1000.0     # ms pro Clock-Tick (24 ppq)
    printf "clock_ticks=%d start=%d continue=%d stop=%d spp=%d\n", n, ns, nc, nx, nsp
    fail = 0
    if (n <= 2) {
        # no silent pass: without events the measurement is invalid
        printf "FAIL: only %d clock events in the log - measurement invalid, see log\n", n
        fail = 1
    } else {
        nd = n - 1
        mean = sum / nd
        if (mean <= 0) {
            print "FAIL: tick intervals <= 0 - timestamps in the log invalid (date %3N supported? check log format)"
            fail = 1
        } else {
            # Segment transport gaps. A STOP/CONTINUE pause mid-run produces a huge delta
            # (gap log: jitter 1668 %, drift 1.4e10 ppm -> wrong FAIL). Delta > 3x mean =
            # transport gap (pause in which no clock runs) -> segment boundary; analogous to
            # harness.c, which segments runs within [0.5,2]x median. Metrics per segment,
            # worst case decides (1 segment == old metric).
            gap_thr = 3.0 * mean
            nseg = 0; lo = 1
            for (i = 1; i <= nd; i++) {
                if (deltas[i] > gap_thr) {
                    if (i > lo) { nseg++; seg_lo[nseg] = lo; seg_hi[nseg] = i - 1 }
                    lo = i + 1
                }
            }
            if (nd >= lo) { nseg++; seg_lo[nseg] = lo; seg_hi[nseg] = nd }
            used = 0
            for (s = 1; s <= nseg; s++) used += seg_hi[s] - seg_lo[s] + 1
            ngaps = nd - used        # discarded transport gaps
            wjit = -1; wmean = 0; wmaxdev = 0; wadrift = -1; wdrift_worst = 0
            for (s = 1; s <= nseg; s++) {
                len = seg_hi[s] - seg_lo[s] + 1
                ssum = 0
                for (i = seg_lo[s]; i <= seg_hi[s]; i++) ssum += deltas[i]
                smean = ssum / len
                # Jitter symmetric: max |d-smean| / smean - early AND late ticks count.
                smaxdev = 0
                for (i = seg_lo[s]; i <= seg_hi[s]; i++) { a = deltas[i] - smean; if (a < 0) a = -a; if (a > smaxdev) smaxdev = a }
                sjit = (smean > 0) ? smaxdev / smean * 100 : 0
                # drift ppm per segment: mean tick period vs. ideal (cumulative = mean deviation)
                sdrift = (smean - ideal) / ideal * 1e6
                if (nseg > 1)
                    printf "  segment %d/%d: ticks=%d mean=%.3fms drift=%.1fppm jitter=%.2f%% (max|dev|=%.1fms)\n", s, nseg, len + 1, smean, sdrift, sjit, smaxdev
                if (sjit > wjit) { wjit = sjit; wmean = smean; wmaxdev = smaxdev }
                adrift = sdrift < 0 ? -sdrift : sdrift
                if (adrift > wadrift) { wadrift = adrift; wdrift_worst = sdrift }
            }
            printf "tick_mean=%.3fms ideal=%.3fms drift=%.1fppm jitter=%.2f%% (max|dev|=%.1fms) segments=%d transport_gaps=%d\n", wmean, ideal, wdrift_worst, wjit, wmaxdev, nseg, ngaps
            if (wdrift_worst < 50 && wdrift_worst > -50) print "DRIFT < 50 ppm: PASS (worst-case segment)"; else { print "DRIFT >= 50 ppm: FAIL (worst-case segment)"; fail = 1 }
            if (wjit < 5) print "JITTER < 5%: PASS (worst-case segment)"; else { print "JITTER >= 5%: FAIL (ms stalls of the SCHED_OTHER worker visible - re-measure on device)"; fail = 1 }
        }
    }
    if (fail) { print "RESULT: FAIL"; exit 1 }
    print "RESULT: PASS"
}
AWK_EOF
)
# AWK metrics block end
awk -v bpm="$BPM" "$AWK_PROG" "$LOG"
RC=$?
echo "done. raw log: $LOG  (copy off-device for the report: scp root@<mpc>:$LOG .)"
exit $RC
