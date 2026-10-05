#!/usr/bin/env bash
# Rebuild the PS5 OpenGL SDK with Kodi's additions (patches/ps5-opengl/) and
# reinstall it into $PS5_OPENGL_PREFIX. Safe to run repeatedly; afterwards
# rebuild Kodi (the SDK is linked into it).
#
# Display profile (the driver's build-time render/presentation size):
#   PS5_SCANOUT_HEIGHT  2160 (default), 1440 or 1080 - match the system output
#                       Kodi reports at start ("PS5 video out: ... system output")
#   PS5_SCANOUT_FPS     60 (default) or 120
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${WORK:-$HOME/ps5-work}"
SRC="$WORK/ps5-opengl"
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
export PS5_OPENGL_PREFIX="${PS5_OPENGL_PREFIX:-/opt/ps5-opengl-gl46}"

[ -d "$SRC/src/platform" ] || { echo "!! ps5-opengl source not found at $SRC"; exit 1; }

export PS5_SCANOUT_HEIGHT="${PS5_SCANOUT_HEIGHT:-2160}"
export PS5_SCANOUT_FPS="${PS5_SCANOUT_FPS:-60}"
case "$PS5_SCANOUT_HEIGHT" in 1080|1440|2160) ;; *) echo "!! PS5_SCANOUT_HEIGHT must be 1080, 1440 or 2160"; exit 1 ;; esac
case "$PS5_SCANOUT_FPS" in 60|120) ;; *) echo "!! PS5_SCANOUT_FPS must be 60 or 120"; exit 1 ;; esac
PROFILE="${PS5_SCANOUT_HEIGHT}p${PS5_SCANOUT_FPS}"

echo "==> applying Kodi additions to $SRC"
python3 "$HERE/patches/ps5-opengl/kodi-additions.py" "$SRC"

# Our additions live in the runtime (src/platform, src/egl), which the SDK
# installer rebuilds itself; Mesa and the shader compiler are unchanged and
# already built by 00-setup-wsl.sh. So run the installer directly rather than
# "make sdk-gl46", whose host-side compiler self-tests are not needed here.
# The runtime's make rules do not track compiler flags: when the display
# profile changes, rebuild the runtime objects from scratch.
STAMP="$SRC/build/.kodi-display-profile"
if [ "$(cat "$STAMP" 2>/dev/null)" != "$PROFILE" ]; then
  echo "==> display profile $PROFILE (was: $(cat "$STAMP" 2>/dev/null || echo "upstream default 1080p60")): clean runtime rebuild"
  rm -rf "$SRC/build/core33-native-runtime"
fi
echo "==> rebuilding the runtime and packaging the SDK ($PROFILE)"
( cd "$SRC" && bash toolchain/install-ps5-opengl-gl46.sh build/sdk/ps5-opengl-gl46 ) \
  > "$WORK/ps5-opengl-build.log" 2>&1 || {
  echo "!! SDK build failed, last lines of $WORK/ps5-opengl-build.log:"; tail -25 "$WORK/ps5-opengl-build.log"; exit 1; }
( cd "$SRC" && python3 tests/ps5/verify_gl46_link_surface.py ) >> "$WORK/ps5-opengl-build.log" 2>&1 \
  || echo "   note: verify_gl46_link_surface.py reported a problem (see the log)"

mkdir -p "$SRC/build" && echo "$PROFILE" > "$STAMP"
echo "==> installing into $PS5_OPENGL_PREFIX"
sudo cp -a "$SRC/build/sdk/ps5-opengl-gl46/." "$PS5_OPENGL_PREFIX/"

# lib/libPS5OpenGL.a is a linker script (GROUP of the real archives); the
# runtime, and so our additions, live in lib/libps5_opengl_core33.a.
found=""
for lib in "$PS5_OPENGL_PREFIX"/lib/*.a; do
  # no "grep -q": it stops reading early, llvm-nm then dies of SIGPIPE and
  # pipefail reports the match as a failure
  if [ "$("$PS5_PAYLOAD_SDK/bin/llvm-nm" "$lib" 2>/dev/null | grep -c " T ps5_opengl_video_out_handle")" -gt 0 ]; then
    found="$lib"; break
  fi
done
if ! grep -q "PS5_OPENGL_NATIVE_HEIGHT $PS5_SCANOUT_HEIGHT" "$PS5_OPENGL_PREFIX/include/ps5_opengl_display.h"; then
  echo "!! installed SDK does not report the $PROFILE profile"; exit 1
fi
if [ -n "$found" ]; then
  echo "SDK ready ($PROFILE): ps5_opengl_video_out_handle exported (in $(basename "$found"))"
else
  echo "!! no installed archive in $PS5_OPENGL_PREFIX/lib exports ps5_opengl_video_out_handle"; exit 1
fi
