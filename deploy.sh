#!/usr/bin/env bash
# deploy.sh - deploy a JamClock release zip
#
#   ./deploy.sh <mpc-ip> [zip]          deploy (zip default: newest dist/JamClock-*-mpc-*.zip)
#   ./deploy.sh build <mpc-ip> [ver]    first build+pack fresh from the CURRENT sources
#                                       (plugins/build-gen1.sh jamclock <ver>; ver default from
#                                       vst.json "version"), then deploy
#   ./deploy.sh rollback <mpc-ip> [zip] rollback via uninstall.sh from the zip
#   MPC_IP=<ip> ./deploy.sh [zip]       IP also via env
#
# Stale protection: if sources (everything under vst/ except vst/build + vst/dbg, plus src/)
# are newer than the chosen zip, an auto-selected zip triggers an AUTOMATIC fresh build
# (build step), so the skin on the MPC never silently becomes stale. To deliberately deploy
# an old zip:
#   ALLOW_STALE=1 ./deploy.sh <mpc-ip> [zip]     or pass a zip explicitly as an argument.
#
#   JAMCLOCK_EXPECTED_SHA256=<sha> ./deploy.sh <ip> [zip]
#                                       if set: sha256 of the zip MUST match
#                                       (verified values are recorded in the deploy log)
#   SKIP_TESTS=1 ./deploy.sh <ip> [zip]  do not upload test/on_device_wp2.sh
#
# Allowed on the device: ONLY the verified release zip (sha256 recorded in the deploy log;
# on the device checked against the internal SHA256SUMS), the test script
# (test/on_device_wp2.sh) and pulling back result logs. Never any other destination,
# never internet/GitHub.
# The IP is NEVER written to the repo - it is passed per session as argument/env only.
# Logs (which may contain the IP) live OUTSIDE the repo, under
# ${XDG_STATE_HOME:-$HOME/.local/state}/jamclock/.
#
# Flow: sha256 locally -> check internal SHA256SUMS locally -> scp root@<ip>:/tmp ->
# on device: fresh temp dir, zip-slip guard, unzip + sha256sum -c SHA256SUMS +
# sh install.sh -y (the installer stops/starts the MPC app acvs itself and creates the
# MPC.settings backup; back up your project first!)
# -> stage test/on_device_wp2.sh to /tmp/on_device_wp2.sh.
# The log lands in ${XDG_STATE_HOME:-$HOME/.local/state}/jamclock/deploy-<timestamp>.log
# (outside the repo). Afterwards add results to the deploy log by hand (set the date).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DIST="$HERE/dist"
SSH_OPTS="${SSH_OPTS:--o BatchMode=yes -o ConnectTimeout=10}"
mkdir -p "$DIST"
LOGDIR="${XDG_STATE_HOME:-$HOME/.local/state}/jamclock"
mkdir -p "$LOGDIR"
LOG="$LOGDIR/deploy-$(date +%Y%m%d-%H%M%S).log"

MODE=deploy
BUILD=0
if [ "${1:-}" = "build" ]; then BUILD=1; shift; fi
if [ "${1:-}" = "rollback" ]; then MODE=rollback; shift; fi
if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then sed -n '2,37p' "$0"; exit 0; fi

IP="${1:-${MPC_IP:-}}"
[ -n "$IP" ] || { echo "usage: $0 [build [ver]] [rollback] <mpc-ip> [zip]   (or MPC_IP=<ip> $0 ...)" >&2; exit 1; }
# Whitelist the IP before ANY use: it is embedded into the ssh/scp command strings below.
case "$IP" in
  ""|*[!0-9A-Za-z.:-]*) echo "deploy.sh: refusing unsafe IP/host '$IP' (allowed chars: 0-9 A-Z a-z . : -)" >&2; exit 1 ;;
esac

ZIP=""
VER=""
EXPLICIT_ZIP=0
if [ $# -ge 2 ]; then
  if [ "$BUILD" = 1 ]; then VER="$2"; else ZIP="$2"; EXPLICIT_ZIP=1; fi
fi

VER_DEFAULT=$(python3 -c "import json; v=json.load(open('$HERE/vst/vst.json'))['version']; print('0.%d.%d' % (v//100, v%100))")

# build step: build the release zip fresh from the CURRENT sources (skin render via html_art,
# armv7 .so cross-compile, zip via release.py - overwrites a same-named zip) and set $ZIP.
do_build() {
  local ver="$1" start
  local GEN1="$HERE/../../build-gen1.sh"
  [ -x "$GEN1" ] || { echo "deploy.sh: $GEN1 not found or not executable" >&2; exit 1; }
  start=$(date +%s)
  echo "== build: plugins/build-gen1.sh jamclock $ver (current vst/ + src/, Docker) ==" | tee -a "$LOG"
  ( cd "$HERE/../.." && ./build-gen1.sh jamclock "$ver" ) 2>&1 | tee -a "$LOG"
  ZIP="$DIST/JamClock-$ver-mpc-armv7.zip"
  [ -f "$ZIP" ] || { echo "deploy.sh: $ZIP missing after build" >&2; exit 1; }
  # release.py writes the zip anew (mode "w") - make sure it is really fresh
  [ -n "$(find "$ZIP" -newermt "@$start" -print 2>/dev/null)" ] || {
    echo "deploy.sh: $ZIP was not rewritten (old mtime) - aborting" >&2; exit 1; }
  echo "fresh zip: $ZIP" | tee -a "$LOG"
}

if [ "$BUILD" = 1 ]; then
  [ "$MODE" = deploy ] || { echo "deploy.sh: build + rollback combined is not possible" >&2; exit 1; }
  do_build "${VER:-$VER_DEFAULT}"
elif [ "$MODE" = deploy ]; then
  if [ -z "$ZIP" ]; then
    ZIP=$(ls -t "$DIST"/JamClock-*-mpc-*.zip 2>/dev/null | head -1 || true)
  fi
  [ -n "$ZIP" ] && [ -f "$ZIP" ] || { echo "deploy.sh: no release zip found (give dist/ or pass it as argument)" >&2; exit 1; }
  ZIP=$(cd "$(dirname "$ZIP")" && pwd)/$(basename "$ZIP")
  # Stale check only for the auto-selected zip (explicit zip argument = deliberate choice)
  if [ "$EXPLICIT_ZIP" = 0 ]; then
    STALE=$( { find "$HERE/vst" \( -path "$HERE/vst/build" -o -path "$HERE/vst/dbg" \) -prune -o -type f -newer "$ZIP" -print
               find "$HERE/src" -type f -newer "$ZIP" -print; } 2>/dev/null | head -3 || true)
    if [ -n "$STALE" ]; then
      if [ "${ALLOW_STALE:-0}" = "1" ]; then
        { echo "ALLOW_STALE=1: deliberately deploying the old zip (sources are newer):"
          echo "$STALE" | sed "s|^$HERE/|  |"; } >&2
        { echo "ALLOW_STALE=1 set - deliberately deploying the old zip:"; echo "$STALE" | sed "s|^$HERE/|  |"; } >> "$LOG"
      else
        { echo "sources newer than $(basename "$ZIP") - building fresh automatically (build step):"
          echo "$STALE" | sed "s|^$HERE/|  |"; } | tee -a "$LOG" >&2
        do_build "$VER_DEFAULT"
      fi
    fi
  fi
fi
[ -n "$ZIP" ] && [ -f "$ZIP" ] || { echo "deploy.sh: no release zip found" >&2; exit 1; }
ZIP=$(cd "$(dirname "$ZIP")" && pwd)/$(basename "$ZIP")
BASE=$(basename "$ZIP")
TOP=$(python3 -c "import zipfile,sys; print(zipfile.ZipFile(sys.argv[1]).namelist()[0].split('/')[0])" "$ZIP")
# Whitelist BASE and TOP before ANY use: both are embedded into the remote ssh command
# string below (inside single quotes) - any other character could break out of the quoting
# and inject commands that would run as root on the device.
case "$BASE" in
  ""|*[!A-Za-z0-9._-]*) echo "deploy.sh: refusing unsafe zip file name '$BASE' (allowed chars: A-Z a-z 0-9 . _ -)" >&2; exit 1 ;;
esac
case "$TOP" in
  ""|*[!A-Za-z0-9._-]*) echo "deploy.sh: refusing unsafe zip top-level dir '$TOP' (allowed chars: A-Z a-z 0-9 . _ -)" >&2; exit 1 ;;
esac

{
  echo "== JamClock $MODE =="
  echo "time  : $(date '+%Y-%m-%d %H:%M:%S')"
  echo "target: root@$IP"
  echo "zip   : $ZIP"
} | tee -a "$LOG"

# 1) print sha256 of the zip + optionally check it against a known value
SHA=$(sha256sum "$ZIP" | cut -d' ' -f1)
echo "sha256(zip): $SHA" | tee -a "$LOG"
if [ -n "${JAMCLOCK_EXPECTED_SHA256:-}" ]; then
  [ "$SHA" = "$JAMCLOCK_EXPECTED_SHA256" ] || { echo "deploy.sh: sha256 does NOT match JAMCLOCK_EXPECTED_SHA256 (check deploy log)" >&2; exit 1; }
  echo "sha256 verified against JAMCLOCK_EXPECTED_SHA256: OK" | tee -a "$LOG"
fi

# 2) check the zip's internal SHA256SUMS locally (extract into a temp dir)
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$ZIP" "$TMP"
if ! ( cd "$TMP/$TOP" && sha256sum -c SHA256SUMS >/dev/null ); then
  echo "local internal SHA256SUMS check FAILED" >&2
  exit 1
fi
echo "local internal SHA256SUMS check: OK" | tee -a "$LOG"

# 3) upload the zip + verify and install on the device
scp $SSH_OPTS "$ZIP" "root@$IP:/tmp/$BASE"
{
  echo "--- device: fresh temp dir + zip-slip guard + unzip + sha256sum -c + $MODE ---"
  # BusyBox on the MPC has no --quiet for unzip (only -c[sw]) -> plain unzip.
  # The installer inside the zip is install.sh (deploy) resp. uninstall.sh (rollback).
  # BASE/TOP/IP are whitelisted above, so embedding them into this string is safe.
  if [ "$MODE" = rollback ]; then DEVICE_SCRIPT=uninstall.sh; else DEVICE_SCRIPT=install.sh; fi
  ssh $SSH_OPTS "root@$IP" "set -e; rm -rf /tmp/jamclock-deploy && mkdir -p /tmp/jamclock-deploy && cd /tmp/jamclock-deploy; if unzip -l \"/tmp/$BASE\" | grep -qF '..'; then echo 'zip contains .. paths, refusing' >&2; exit 1; fi; unzip -o \"/tmp/$BASE\" && cd \"$TOP\" && sha256sum -c SHA256SUMS && echo 'SHA256SUMS on device: OK' && sh $DEVICE_SCRIPT -y"
} 2>&1 | tee -a "$LOG"

# 4) upload the test script as well (allowed on the device), unless SKIP_TESTS=1
if [ "${SKIP_TESTS:-0}" != "1" ] && [ -f "$HERE/test/on_device_wp2.sh" ]; then
  scp $SSH_OPTS "$HERE/test/on_device_wp2.sh" "root@$IP:/tmp/on_device_wp2.sh"
  echo "test/on_device_wp2.sh -> /tmp/on_device_wp2.sh on the device" | tee -a "$LOG"
fi

{
  echo
  echo "Done ($MODE). Log: $LOG"
  echo "Please back up the project on the MPC (the installer restarts acvs) and add results to the deploy log by hand (set the date)."
  echo "On-device measurement:  sh /tmp/on_device_wp2.sh <ref_bpm> <duration_s> \"<app-port e.g. 128:0>\""
  echo "Rollback: $0 rollback $IP $ZIP"
} | tee -a "$LOG"
