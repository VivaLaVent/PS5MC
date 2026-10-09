#!/usr/bin/env bash
# 51-try-ps5-opengl.sh - build another ps5-opengl revision SIDE BY SIDE, to
# test a driver update without touching the working one.
#
#   bash ps5/scripts/51-try-ps5-opengl.sh [revision]     (default: origin/main)
#
# - its own source checkout ($WORK/ps5-opengl-try) and install prefix
#   (/opt/ps5-opengl-gl46-try); the pinned driver in $WORK/ps5-opengl and
#   /opt/ps5-opengl-gl46 stay exactly as they are
# - full build (Mesa, shader compiler: make source-fetch && make sdk-gl46 -
#   long, like the first setup), then Kodi's additions, runtime rebuild and
#   install through 18-build-ps5-opengl.sh, with its checks
# - stops if any of Kodi's additions no longer finds its place
#
# Then build Kodi against it (one variant is enough for a first look):
#   PS5_OPENGL_PREFIX=/opt/ps5-opengl-gl46-try ONLY=22 bash ps5/scripts/40-release.sh 1.3 --no-publish
# and test on the console: zero-copy 8/10-bit video, HDR on/off, stopping
# playback repeatedly, the Kodi 21 skin, 4K performance. If all is well,
# adopt it: put the revision in ps5/patches/ps5-opengl/PS5-OPENGL-COMMIT.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${WORK:-$HOME/ps5-work}"
REV="${1:-origin/main}"
TRY_SRC="${PS5_OPENGL_TRY_SRC:-$WORK/ps5-opengl-try}"
TRY_PREFIX="${PS5_OPENGL_TRY_PREFIX:-/opt/ps5-opengl-gl46-try}"
DEFAULT_PREFIX="/opt/ps5-opengl-gl46"
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"

[ "$TRY_PREFIX" != "$DEFAULT_PREFIX" ] || { echo "!! the try prefix must not be the working driver's ($DEFAULT_PREFIX)"; exit 1; }
[ "$TRY_SRC" != "$WORK/ps5-opengl" ] || { echo "!! the try source must not be the working checkout"; exit 1; }

if [ ! -d "$TRY_SRC/.git" ]; then
  echo "==> cloning ps5-opengl into $TRY_SRC"
  git clone -q --no-checkout https://github.com/blackbearreloaded/ps5-opengl.git "$TRY_SRC"
fi
git -C "$TRY_SRC" fetch -q origin
COMMIT="$(git -C "$TRY_SRC" rev-parse --verify "$REV^{commit}")" || { echo "!! unknown revision: $REV"; exit 1; }
PIN="$(tr -d '[:space:]' < "$HERE/patches/ps5-opengl/PS5-OPENGL-COMMIT")"
echo "==> revision $(git -C "$TRY_SRC" log -1 --format='%h %ad %s' --date=short "$COMMIT" | cut -c1-90)"
echo "    pinned:  $(git -C "$TRY_SRC" log -1 --format='%h %ad' --date=short "$PIN" 2>/dev/null || echo "$PIN")"
echo "    commits since the pin: $(git -C "$TRY_SRC" rev-list --count "$PIN..$COMMIT" 2>/dev/null || echo '?')"
git -C "$TRY_SRC" checkout -q -f "$COMMIT"
git -C "$TRY_SRC" clean -qfd -e build

echo "==> do Kodi's additions still find their places? (checked on a copy)"
CHECK="$(mktemp -d)"
for f in src/platform/ps5_agc_native_runtime.c src/gallium/ps5/ps5_screen.c src/egl/ps5_egl.c; do
  mkdir -p "$CHECK/$(dirname "$f")"; cp "$TRY_SRC/$f" "$CHECK/$f"
done
python3 "$HERE/patches/ps5-opengl/kodi-additions.py" "$CHECK" | tee "$CHECK/report.txt" | sed 's/^/   /'
rm -rf "$CHECK/src"
if grep -qi "skipped" "$CHECK/report.txt"; then
  echo "!! some additions no longer find their place in $REV: port them in patches/ps5-opengl/kodi-additions.py first"
  exit 1
fi

echo "==> full driver build in $TRY_SRC (long: Mesa and the shader compiler)"
( cd "$TRY_SRC" && make source-fetch && make sdk-gl46 ) > "$WORK/ps5-opengl-try-build.log" 2>&1 || {
  echo "!! driver build failed, last lines of $WORK/ps5-opengl-try-build.log:"; tail -25 "$WORK/ps5-opengl-try-build.log"; exit 1; }
sudo mkdir -p "$TRY_PREFIX"
sudo cp -a "$TRY_SRC/build/sdk/ps5-opengl-gl46/." "$TRY_PREFIX/"

echo "==> Kodi's additions, runtime rebuild and install into $TRY_PREFIX"
PS5_OPENGL_SRC="$TRY_SRC" PS5_OPENGL_PREFIX="$TRY_PREFIX" bash "$HERE/scripts/18-build-ps5-opengl.sh"

cat <<MSG

==> ps5-opengl $(git -C "$TRY_SRC" log -1 --format=%h "$COMMIT") is installed in $TRY_PREFIX (the working driver is unchanged).
Build Kodi against it:
  PS5_OPENGL_PREFIX=$TRY_PREFIX ONLY=22 bash ps5/scripts/40-release.sh 1.3 --no-publish
Normal builds keep using $DEFAULT_PREFIX. To adopt it after testing:
  echo $COMMIT > ps5/patches/ps5-opengl/PS5-OPENGL-COMMIT
MSG
