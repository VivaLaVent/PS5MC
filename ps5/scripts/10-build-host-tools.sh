#!/usr/bin/env bash
# Build Kodi's two host-side build tools natively. Kodi cannot run target
# binaries on the host, so the configure step needs these pre-built:
#   -DWITH_TEXTUREPACKER=<dir>   containing kodi-TexturePacker or TexturePacker
#   -DWITH_JSONSCHEMABUILDER=<dir> containing JsonSchemaBuilder
set -euo pipefail

KODI_SRC="${KODI_SRC:-$HOME/kodi}"
NATIVE="${NATIVE:-$HOME/kodi-ps5-native}"
JOBS="${JOBS:-$(nproc)}"

for tool in TexturePacker JsonSchemaBuilder; do
  echo "==> $tool"
  src="$KODI_SRC/tools/depends/native/$tool/src"
  build="$NATIVE/build-$tool"
  rm -rf "$build"   # always configure from a clean cache
  # KODI_SOURCE_DIR: TexturePacker's CMakeLists compares it unquoted and errors
  # when unset; pointing it at the Kodi tree makes it use its own FindLzo2.
  # APP_NAME_LC: JsonSchemaBuilder installs as ${APP_NAME_LC}-JsonSchemaBuilder,
  # and Kodi's find module looks for kodi-JsonSchemaBuilder.
  # ARCH_DEFINES: the tools are built for the Linux host; without TARGET_POSIX
  # TexturePacker's cmdlineargs.h includes windows.h.
  cmake -S "$src" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$NATIVE" \
        -DKODI_SOURCE_DIR="$KODI_SRC" \
        -DAPP_NAME_LC=kodi \
        -DARCH_DEFINES="-DTARGET_POSIX;-DTARGET_LINUX;-D_GNU_SOURCE"
  cmake --build "$build" -j"$JOBS"
  cmake --install "$build"
done

# Kodi builds a few more host tools itself (flatc). With NATIVEPREFIX set it
# looks for this file and uses it as the toolchain for those builds; without
# it they would be cross-compiled with the PS5 toolchain and crash on the host.
mkdir -p "$NATIVE/share"
cat > "$NATIVE/share/Toolchain-Native.cmake" <<'TC'
# Host toolchain for Kodi's native build tools.
set(CMAKE_C_COMPILER /usr/bin/cc)
set(CMAKE_CXX_COMPILER /usr/bin/c++)
set(CMAKE_BUILD_TYPE Release)
TC

ls -l "$NATIVE/bin"
echo "Pass: -DWITH_TEXTUREPACKER=$NATIVE/bin -DWITH_JSONSCHEMABUILDER=$NATIVE/bin"
