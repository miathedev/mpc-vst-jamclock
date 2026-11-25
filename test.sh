#!/usr/bin/env bash
# Offline x86 test of the JamClock port under ASan/UBSan (tools/test_port.sh of mpc-vst-plugins):
#   MPC_VST=/path/to/mpc-vst-plugins ./test.sh [aarch64]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
MPC_VST="${MPC_VST:-$HERE/../mpc-vst-plugins}"
exec "$MPC_VST/tools/test_port.sh" "$HERE/vst/vst.json" "${1:-}"
