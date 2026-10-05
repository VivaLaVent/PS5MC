#!/usr/bin/env bash
# Sysroot dependencies that only Kodi 21 (Omega) needs.
#
# Kodi 22 builds these internally (ENABLE_INTERNAL_*); Kodi 21's find-modules
# for them have no internal-build path and want them in the sysroot:
#   lzo2  2.10  required; tiny pure-C library -> cross-compiled here
# (KissFFT, PCRE v1 and RapidJSON are also 21-only, but 21 CAN build those
#  internally; cmake/scripts/ps5/ArchSetup.cmake turns that on.)
#
# Version and SHA-512 are taken verbatim from Kodi 21's own depends recipe
# (tools/depends/target/liblzo2), so the sysroot gets exactly what
# upstream 21 was tested with, and every download is verified.
#
# Run once (after scripts/00); re-running is a no-op if installed.
set -euo pipefail
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="$PS5_PAYLOAD_SDK/target/user/homebrew"
WORK="${WORK:-$HOME/ps5-work}/kodi21-deps"; mkdir -p "$WORK"
MIRROR="${KODI_MIRROR:-http://mirrors.kodi.tv}/build-deps/sources"
JOBS="${JOBS:-$(nproc)}"

fetch() { # $1 = file, $2 = sha512
  if [ ! -f "$WORK/$1" ]; then curl -fL --retry 3 -o "$WORK/$1" "$MIRROR/$1"; fi
  echo "$2  $WORK/$1" | sha512sum -c --quiet || { echo "!! checksum mismatch for $1"; rm -f "$WORK/$1"; exit 1; }
}

# ---------------------------------------------------------------- lzo2 2.10 ---
if [ -f "$PREFIX/lib/liblzo2.a" ] && [ -f "$PREFIX/include/lzo/lzo1x.h" ]; then
  echo "==> lzo2: already in the sysroot"
else
  echo "==> lzo2 2.10"
  fetch lzo-2.10.tar.gz a3dae5e4a6b93b1f5bf7435e8ab114a9be57252e9efc5dd444947d7a2d031b0819f34bcaeb35f60b5629a01b1238d738735a64db8f672be9690d3c80094511a4
  rm -rf "$WORK/lzo-2.10" && tar -C "$WORK" -xzf "$WORK/lzo-2.10.tar.gz"
  # lzo ships CMake; cross-compile with the same toolchain file Kodi uses.
  cmake -S "$WORK/lzo-2.10" -B "$WORK/lzo-build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain/ps5-kodi.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON >/dev/null
  cmake --build "$WORK/lzo-build" -j"$JOBS" >/dev/null
  sudo cmake --install "$WORK/lzo-build" >/dev/null
  [ -f "$PREFIX/lib/liblzo2.a" ] || { echo "!! lzo2 did not install"; exit 1; }
  echo "    installed $PREFIX/lib/liblzo2.a"
fi

# a rebuilt/added sysroot library must be relinked into kodi.bin
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/sysroot-changed.sh"; sysroot_changed
echo "==> done: Kodi 21 sysroot deps ready"
