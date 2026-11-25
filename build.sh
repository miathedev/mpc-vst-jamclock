#!/usr/bin/env bash
# Build the JamClock port for the MPC (upstream toolchain; needs Docker with QEMU binfmt for armv7):
#   MPC_VST=/path/to/mpc-vst-plugins ./build.sh [armv7|aarch64|all]
# On this host (no binfmt) use plugins/build-gen1.sh jamclock <version> instead: it cross-compiles
# armv7 without QEMU and writes the release zip. Offline test: ./test.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
MPC_VST="${MPC_VST:-$HERE/../mpc-vst-plugins}"
exec "$MPC_VST/tools/build_port.sh" "$HERE/vst/vst.json" "${1:-armv7}"
