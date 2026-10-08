# Write the real build stamp into BuildStamp.h. Called by 40-release.sh after
# 20-configure and BEFORE cmake --build (main.cpp is compiled into kodi.bin in
# that step). 40-release resets the file again on exit, so the committed
# placeholder ("unknown") is what stays in git.
# Usage: KODI_SRC=... bash scripts/lib/write-build-stamp.sh
set -euo pipefail
SRC="${KODI_SRC:-$HOME/kodi}"
REL=xbmc/platform/ps5/BuildStamp.h
H="$SRC/$REL"
[ -f "$H" ] || { echo "   (no BuildStamp.h; skipping stamp)"; exit 0; }
if git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1; then
  # a stamp left by an earlier, failed build would make describe say "-dirty"
  git -C "$SRC" checkout -q -- "$REL" 2>/dev/null || true
  B="$(git -C "$SRC" rev-parse --abbrev-ref HEAD 2>/dev/null)"
  D="$(git -C "$SRC" describe --always --tags --dirty 2>/dev/null || echo unknown)"
  STAMP="${B}-${D}"
else
  STAMP="unknown"
fi
sed -i "s/#define KODI_PS5_BUILD_STAMP \"[^\"]*\"/#define KODI_PS5_BUILD_STAMP \"$STAMP\"/" "$H"
echo "==> build stamp: $STAMP"
