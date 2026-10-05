#!/usr/bin/env bash
# Apply the overlay to a Kodi checkout and configure the PS5 build.
#
#   KODI_SRC   Kodi source tree (master / 22.x)         default ~/kodi
#   BUILD      build directory                          default ~/kodi-ps5-build
#   NATIVE     host tools from 10-build-host-tools.sh   default ~/kodi-ps5-native
#   BUILD_TYPE Release (default; built with -O2 -g: optimised, symbols kept
#              for crash backtraces) or Debug (slow, extra assertions).
#              Not RelWithDebInfo: Kodi's dependency helpers then expect the
#              debug-named bundled libraries but link the release ones.
#
# Re-run after editing the overlay; it only copies files, so removing a file
# from the overlay does not remove it from the tree.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$HERE/scripts/lib/platform-src.sh"
KODI_SRC="${KODI_SRC:-$HOME/kodi}"
BUILD="${BUILD:-$HOME/kodi-ps5-build}"
NATIVE="${NATIVE:-$HOME/kodi-ps5-native}"
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"

# our Sony link stubs must be in the sysroot: libSceVideodec2 (the SDK has
# none) and the SDK's libSceVideoOut extended with the VRR unpeg function
NEED_STUBS=0
for stub in "$HERE"/shims/sce_stubs/*.c; do
  [ -f "$PS5_PAYLOAD_SDK/target/lib/$(basename "$stub" .c).so" ] || NEED_STUBS=1
done
[ -f "$PS5_PAYLOAD_SDK/target/lib/libSceVideoOut.so.sdk" ] || NEED_STUBS=1
if [ "$NEED_STUBS" = 1 ]; then
  bash "$HERE/scripts/17-build-sce-stubs.sh" || { echo "!! stub build failed"; exit 1; }
fi
export PS5_OPENGL_PREFIX="${PS5_OPENGL_PREFIX:-/opt/ps5-opengl-gl46}"

[ -f "$KODI_SRC/version.txt" ] || { echo "Kodi source not found at $KODI_SRC"; exit 1; }

# Kodi patches always start from Kodi's own files: restore every file a patch
# touches from git, so a patch that changed between rounds (or one applied
# Two modes:
#  FORK (default when $KODI_SRC/xbmc/platform/ps5 is tracked by git there): the
#       checkout IS the source - patches and overlay already live in it as the
#       ps5 platform series. Nothing is copied or patched; we only verify.
#  LEGACY (the old overlay+patches flow, for the legacy-overlay branch).
if git -C "$KODI_SRC" ls-files --error-unmatch xbmc/platform/ps5/main.cpp >/dev/null 2>&1; then
  MODE=fork
else
  MODE=legacy
fi
echo "==> source mode: $MODE ($KODI_SRC)"

if [ "$MODE" = fork ]; then
  # Verify the series is present and the tree is clean of stray overlay copies.
  if [ ! -f "$KODI_SRC/xbmc/platform/ps5/elf/ElfLoader.cpp" ] || \
     ! grep -q "TARGET_PS5" "$KODI_SRC/xbmc/utils/TimeUtils.cpp"; then
    echo "!! $KODI_SRC does not carry the ps5 platform series (checkout ps5-piers or ps5-omega)"; exit 1
  fi
  if [ -n "$(git -C "$KODI_SRC" status --porcelain 2>/dev/null)" ]; then
    echo "!! $KODI_SRC has uncommitted changes; commit them (the stamp must name a real commit)"; exit 1
  fi
  # Provenance: the Kodi tree's own describe - tag/branch + commit of the series.
  STAMP="$(git -C "$KODI_SRC" describe --always --tags --dirty 2>/dev/null || echo unknown)"
  KODI_BRANCH="$(git -C "$KODI_SRC" rev-parse --abbrev-ref HEAD 2>/dev/null)"
  STAMP="${KODI_BRANCH}-${STAMP}"
  STAMP_H="$KODI_SRC/xbmc/platform/ps5/BuildStamp.h"
  if [ -f "$STAMP_H" ]; then
    sed -i "s/#define KODI_PS5_BUILD_STAMP \"[^\"]*\"/#define KODI_PS5_BUILD_STAMP \"$STAMP\"/" "$STAMP_H"
    # the stamp edit must not dirty the tree for the next provenance check
    git -C "$KODI_SRC" update-index --assume-unchanged "xbmc/platform/ps5/BuildStamp.h" 2>/dev/null || true
    echo "==> build stamp: $STAMP"
  fi
  echo "==> Kodi series: $(git -C "$KODI_SRC" log --oneline | grep -c '^[0-9a-f]* ps5') ps5 commits on $KODI_BRANCH"
else
# only partly before) cannot leave a mixed file behind.
  # The patch folder must match the manifest exactly: extracting a release zip
  # over the folder never deletes files, so a removed or renamed patch would
  # otherwise linger and be applied (or fail) alongside the current one.
  if [ -f "$HERE/patches/kodi/manifest.txt" ]; then
    EXPECTED=$(sort "$HERE/patches/kodi/manifest.txt")
    PRESENT=$(ls "$HERE"/patches/kodi/*.patch | xargs -n1 basename | sort)
    if [ "$EXPECTED" != "$PRESENT" ]; then
      echo "!! patches/kodi does not match patches/kodi/manifest.txt:"
      diff <(echo "$EXPECTED") <(echo "$PRESENT") | sed 's/^</   missing:/; s/^>/   stale (delete it):/' | grep 'missing\|stale'
      exit 1
    fi
  fi
  PATCHED_FILES=$(grep -h '^+++ b/' "$HERE"/patches/kodi/*.patch | sed 's|^+++ b/||; s|\t.*||' | sort -u)
  if git -C "$KODI_SRC" rev-parse --git-dir >/dev/null 2>&1; then
    echo "==> restoring the $(echo "$PATCHED_FILES" | wc -l) Kodi files our patches change"
    for f in $PATCHED_FILES; do
      git -C "$KODI_SRC" checkout -- "$f" 2>/dev/null || echo "   note: $f is not tracked by git"
    done
    CLEAN_BASE=1
  else
    echo "   note: $KODI_SRC is not a git checkout: patches go on top of the current files"
    CLEAN_BASE=0
  fi
  
  # Overlay files are copied only where their content differs, with a fresh
  # timestamp: "cp -a" kept the timestamps from the zip, which can be older than
  # the last build's objects, so ninja skipped changed files.
  echo "==> applying overlay to $KODI_SRC"
  OVERLAY_CHANGED=0
  while IFS= read -r -d '' f; do
    dst="$KODI_SRC/${f#"$HERE"/overlay/}"
    if ! cmp -s "$f" "$dst"; then
      mkdir -p "$(dirname "$dst")"
      cp "$f" "$dst"
      OVERLAY_CHANGED=$((OVERLAY_CHANGED + 1))
    fi
  done < <(find "$HERE/overlay" -type f -print0)
  echo "==> overlay: $OVERLAY_CHANGED files updated"
  
  # Build stamp: the git short-hash (plus -dirty) into BuildStamp.h on the tree,
  # so the build embeds it and every log says exactly which build it is.
  STAMP="$(cd "$HERE" && git describe --always --dirty 2>/dev/null || echo unknown)"
  STAMP_H="$KODI_SRC/xbmc/platform/ps5/BuildStamp.h"
  if [ -f "$STAMP_H" ]; then
    sed -i "s/#define KODI_PS5_BUILD_STAMP \"[^\"]*\"/#define KODI_PS5_BUILD_STAMP \"$STAMP\"/" "$STAMP_H"
    echo "==> build stamp: $STAMP"
  fi
  
  
  # Small patches to Kodi's own files that the overlay cannot express, in order.
  for p in "$HERE"/patches/kodi/*.patch; do
    [ -f "$p" ] || continue
    if [ "$CLEAN_BASE" = 1 ]; then
      ( cd "$KODI_SRC" && patch -p1 --forward --no-backup-if-mismatch -r - --silent < "$p" ) || {
        echo "!! Kodi patch $(basename "$p") does not apply to this Kodi revision"; exit 1; }
    else
      ( cd "$KODI_SRC" && patch -p1 -N --no-backup-if-mismatch -r - --silent < "$p" ) || true
    fi
  done
  echo "==> $(ls "$HERE"/patches/kodi/*.patch | wc -l) Kodi patches applied"
fi

echo "==> configuring in $BUILD"
mkdir -p "$BUILD"

# pkg-config for two worlds: Kodi installs its internally built libraries into
# $BUILD/build and its meson sub-builds (libdvdnav) locate them through
# pkg-config, while pacbrew's .pc files need the SDK wrapper, which hard-wires
# the sysroot search path and would hide the build tree. Try the build tree
# first (no sysroot prefixing), then fall back to the SDK wrapper.
cat > "$BUILD/kodi-pkg-config" <<WRAP
#!/usr/bin/env bash
# Stage 1 only when called by meson for a Kodi sub-build: meson passes the
# cross file's pkg_config_libdir (inside the build tree) in PKG_CONFIG_LIBDIR.
# Kodi's own configure must keep seeing the sysroot only, otherwise libraries
# built in a previous run look "external" and their dependants stop building.
# lib/pkgconfig: CMake/autotools installs; libdata/pkgconfig: meson installs
# for a FreeBSD host machine.
DEPENDS_PC="$BUILD/build/lib/pkgconfig:$BUILD/build/libdata/pkgconfig"
case "\${PKG_CONFIG_LIBDIR:-}" in
  "$BUILD/build/"*)
    env -u PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_LIBDIR="\$DEPENDS_PC" PKG_CONFIG_PATH= pkg-config "\$@" 2>/dev/null && exit 0
    ;;
esac
exec "$PS5_PAYLOAD_SDK/bin/prospero-pkg-config" "\$@"
WRAP
chmod +x "$BUILD/kodi-pkg-config"
# Python add-ons: on once scripts/19-build-python.sh has installed CPython
# into the sysroot. Kodi's bindings generator then needs SWIG and a Java
# runtime on the build machine (scripts/00-setup-wsl.sh installs them).
PY_ROOT="$PS5_PAYLOAD_SDK/target/user/homebrew"
if [ -f "$PY_ROOT/lib/libpython3.14.a" ]; then
  command -v swig >/dev/null && command -v java >/dev/null || {
    echo "!! Python is installed, but Kodi's bindings need swig and java: sudo apt-get install -y swig default-jre-headless"
    exit 1; }
  # Kodi 22 needs SWIG >= 4.5 for the bindings; distro SWIG is older, so build it in-tree.
  PYTHON_ARGS=(-DENABLE_PYTHON=ON -DENABLE_INTERNAL_SWIG=ON -DPYTHON_PATH=/user/homebrew -DPYTHON_VER=3.14
               -DPython3_USE_STATIC_LIBS=ON)
  echo "==> Python 3.14 found in the sysroot: Python add-ons enabled"
else
  PYTHON_ARGS=(-DENABLE_PYTHON=OFF)
fi

# CMake caches pkg-config results; FFmpeg's link flags must be re-read every
# configure, or a rebuilt FFmpeg (new dependencies such as dav1d) links with
# the flags of the old one and kodi.bin fails with undefined symbols.
# Opt-in ccache (KODI_PS5_CCACHE=1): configure's overlay re-copy and patch
# re-apply can touch timestamps, so a full `cmake --build` recompiles far more
# than changed. ccache makes every unchanged file a cache hit. Opt-in so the
# default build path is unchanged; first build after enabling only fills the cache.
CCACHE_ARGS=()
if [ "${KODI_PS5_CCACHE:-0}" = 1 ] && command -v ccache >/dev/null; then
  CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  echo "==> ccache enabled ($(ccache -s 2>/dev/null | grep -iE 'hit rate|cache size' | head -1 | xargs))"
fi

cmake -S "$KODI_SRC" -B "$BUILD" -G Ninja \
  -U "FFMPEG_*" -U "Python3_*" -U "PYTHON_*" \
  -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain/ps5-kodi.cmake" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
  -DCMAKE_C_FLAGS_RELEASE="-O2 -g -DNDEBUG" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O2 -g -DNDEBUG" \
  -DCMAKE_INSTALL_PREFIX=/app0 \
  -DWITH_TEXTUREPACKER="$NATIVE/bin" \
  -DWITH_JSONSCHEMABUILDER="$NATIVE/bin/kodi-JsonSchemaBuilder" \
  # ^ the binary, not its dir: Kodi 21's find-module strips one path component
  #   unconditionally (so a dir became its parent -> "not found"); 22's checks
  #   IS_DIRECTORY first. The binary path satisfies both.
  -DNATIVEPREFIX="$NATIVE" \
  -DPKG_CONFIG_EXECUTABLE="$BUILD/kodi-pkg-config" \
  -DINTERNAL_TEXTUREPACKER_INSTALLABLE=FALSE \
  -DENABLE_INTERNAL_FFMPEG=OFF \
  "${PYTHON_ARGS[@]}" \
  "${CCACHE_ARGS[@]}" \
  -DENABLE_TESTING=OFF \
  -DVERBOSE_FIND=ON \
  "$@"

cat <<MSG

Configured. Build with:
  cmake --build $BUILD -j\$(nproc)
Then stage + deploy with scripts/30-deploy.sh
MSG
