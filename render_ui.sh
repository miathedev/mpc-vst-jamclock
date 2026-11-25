#!/usr/bin/env bash
# render_ui.sh - render the plugin UI locally from vst/layout.conf into preview PNGs.
#
#   ./render_ui.sh [OUTDIR]        default OUTDIR: vst/build/preview/  (git-ignored)
#
# Pipeline (like tools/build_port.sh, only local): build the skin via gen_vst.py into
# vst/build/skin/ (TUI.json + PNGs; "warning: skin:" lines = TOUCH/EDGE overlaps)
# -> studio.py preview paints one page_N.png per tab (background + controls + live text).
#
# Renderer per vst.json:
#   "art": "html"  -> tools/html_art.py (headless Chromium, real fonts) in the Docker image
#                     mpc-vst-html-art (built on demand, like build_port.sh)
#   otherwise      -> shadow_art via host gcc (bitmap font font8x8)
#
# Needs: python3 with Pillow (pip install pillow); for "art": "html" additionally Docker.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
MPC_VST="${MPC_VST:-$HERE/../mpc-vst-plugins}"
VST="$HERE/vst"
BUILD="$VST/build"
OUT="${1:-$BUILD/preview}"
ROOT="$HERE"                     # plugin root (vst.json "build.root"), mounted as /w in the container
PORT="vst"                       # gen_vst.py --shell: PORT=vst

python3 -c "import PIL" 2>/dev/null || { echo "render_ui.sh: Pillow missing (pip install pillow)" >&2; exit 1; }
[ -d "$MPC_VST/tools" ] || { echo "render_ui.sh: MPC_VST does not point to mpc-vst-plugins (tools/ missing): $MPC_VST" >&2; exit 1; }

mkdir -p "$BUILD" "$OUT"

# 1) build the skin (gen_vst.py); its stderr/stdout pass through ("warning: skin:" etc.)
if grep -Eq '"art"[[:space:]]*:[[:space:]]*"html"' "$VST/vst.json"; then
  docker image inspect mpc-vst-html-art >/dev/null 2>&1 || \
    docker build -q -t mpc-vst-html-art "$MPC_VST/tools/html_art"
  GEN_OUT=$(docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$ROOT":/w -v "$MPC_VST":/mv:ro -w /w mpc-vst-html-art \
    python3 /mv/tools/gen_vst.py "$PORT/vst.json")   # -> /w/vst/vst.json
else
  command -v gcc >/dev/null 2>&1 || { echo "render_ui.sh: gcc missing" >&2; exit 1; }
  gcc -O2 -I"$MPC_VST/tools/vendor/force-shadow/tools" -o "$BUILD/shadow_art" "$MPC_VST/tools/shadow_art.c" -lm
  GEN_OUT=$(python3 "$MPC_VST/tools/gen_vst.py" "$VST/vst.json")
fi
printf '%s\n' "$GEN_OUT"

# 2) find the skin dir (regardless of whether gen_vst printed a host or a container path)
PSDIR=$(find "$BUILD/skin" -type d -name "Plugin Skins" 2>/dev/null | head -1)
[ -n "$PSDIR" ] || { echo "render_ui.sh: skin dir not found under $BUILD/skin" >&2; exit 1; }

# 3) render the preview PNGs (one per tab page)
python3 "$MPC_VST/tools/studio.py" preview "$PSDIR" -o "$OUT/page_%d.png"

echo
echo "Preview: $OUT"
ls -1 "$OUT"
