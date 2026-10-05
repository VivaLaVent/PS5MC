#!/usr/bin/env bash
# Kodi's internal exiv2 build requires brotli (BMFF/HEIF metadata), and Kodi
# only builds brotli itself for depends-based or Windows builds. pacbrew-repo
# has no brotli package, so cross-build the upstream release into the sysroot.
set -euo pipefail

export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
WORK="${WORK:-$HOME/ps5-work}"
VER=1.1.0
mkdir -p "$WORK" && cd "$WORK"

if [ ! -d "brotli-$VER" ]; then
  wget -q -O "brotli-$VER.tar.gz" "https://github.com/google/brotli/archive/refs/tags/v$VER.tar.gz"
  tar xzf "brotli-$VER.tar.gz"
fi

rm -rf brotli-build
cmake -S "brotli-$VER" -B brotli-build -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/user/homebrew \
      -DBUILD_SHARED_LIBS=OFF \
      -DBROTLI_DISABLE_TESTS=ON \
      -DBROTLI_BUILD_TOOLS=OFF
cmake --build brotli-build -j"$(nproc)"
sudo env DESTDIR="$PS5_PAYLOAD_SDK/target" cmake --install brotli-build
echo "installed brotli $VER into $PS5_PAYLOAD_SDK/target/user/homebrew"
ls "$PS5_PAYLOAD_SDK/target/user/homebrew/lib/pkgconfig/" | grep -i brotli
