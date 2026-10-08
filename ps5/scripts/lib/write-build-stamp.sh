# Write the real build stamp into BuildStamp.h. MUST run after 20-configure and
# BEFORE cmake --build (main.cpp -> kodi.bin is compiled in that build step).
# Usage: KODI_SRC=... bash scripts/lib/write-build-stamp.sh
set -euo pipefail
SRC="${KODI_SRC:-$HOME/kodi}"
H="$SRC/xbmc/platform/ps5/BuildStamp.h"
[ -f "$H" ] || { echo "   (no BuildStamp.h; skipping stamp)"; exit 0; }
if git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1; then
  B="$(git -C "$SRC" rev-parse --abbrev-ref HEAD 2>/dev/null)"
  D="$(git -C "$SRC" describe --always --tags --dirty 2>/dev/null || echo unknown)"
  STAMP="${B}-${D}"
else
  STAMP="unknown"
fi
sed -i "s/#define KODI_PS5_BUILD_STAMP \"[^\"]*\"/#define KODI_PS5_BUILD_STAMP \"$STAMP\"/" "$H"
echo "==> build stamp: $STAMP"
