#!/usr/bin/env bash
# Build Kodi's two host-side build tools natively. Kodi cannot run target
# binaries on the host, so the configure step needs these pre-built:
#   -DWITH_TEXTUREPACKER=<path to TexturePacker>
#   -DWITH_JSONSCHEMABUILDER=<path to JsonSchemaBuilder>
#
# The tools must come from the SAME Kodi tree as the target: Kodi 22 changed
# the texture-bundle format (XBTF version 3), which Kodi 21 cannot read. One
# shared TexturePacker gave the Kodi 21 build a skin bundle it rejected - every
# icon, button and highlight missing. 20-configure-kodi.sh therefore calls this
# with PREFIX=$NATIVE/kodi<major> for the tree it is configuring.
set -euo pipefail

KODI_SRC="${KODI_SRC:-$HOME/kodi}"
NATIVE="${NATIVE:-$HOME/kodi-ps5-native}"
PREFIX="${PREFIX:-$NATIVE}"   # where bin/ goes; per Kodi major when called by 20-configure
JOBS="${JOBS:-$(nproc)}"

for tool in TexturePacker JsonSchemaBuilder; do
  echo "==> $tool"
  src="$KODI_SRC/tools/depends/native/$tool/src"
  build="$PREFIX/build-$tool"
  rm -rf "$build"   # always configure from a clean cache
  # KODI_SOURCE_DIR: TexturePacker's CMakeLists compares it unquoted and errors
  # when unset; pointing it at the Kodi tree makes it use its own FindLzo2.
  # APP_NAME_LC: JsonSchemaBuilder installs as ${APP_NAME_LC}-JsonSchemaBuilder,
  # and Kodi's find module looks for kodi-JsonSchemaBuilder.
  # ARCH_DEFINES: the tools are built for the Linux host; without TARGET_POSIX
  # TexturePacker's cmdlineargs.h includes windows.h.
  cmake -S "$src" -B "$build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
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

ls -l "$PREFIX/bin"
echo "Pass: -DWITH_TEXTUREPACKER=$PREFIX/bin/TexturePacker -DWITH_JSONSCHEMABUILDER=$PREFIX/bin/JsonSchemaBuilder"
