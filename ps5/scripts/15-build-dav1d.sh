#!/usr/bin/env bash
# Build and install the dav1d AV1 decoder into the sysroot (pacbrew has no
# package for it); FFmpeg (scripts/16) is then built with --enable-libdav1d.
# Recipe: pacbrew/dav1d/PKGBUILD. Needs nasm on the host for the assembly.
set -euo pipefail

export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
export MAKEFLAGS="${MAKEFLAGS:--j$(nproc)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${WORK:-$HOME/ps5-work}"
REPO="$WORK/pacbrew-repo"          # for pacman.conf
BUILD="$WORK/kodi-pacbrew/dav1d"

[ -f "$REPO/pacman.conf" ] || { echo "pacbrew-repo not found at $REPO (run 00-setup-wsl.sh first)"; exit 1; }
command -v nasm >/dev/null || sudo apt-get install -y nasm
mkdir -p "$BUILD"
cp "$HERE/pacbrew/dav1d/PKGBUILD" "$BUILD/PKGBUILD"
cd "$BUILD"
rm -rf src pkg ./*.pkg.tar.gz
makepkg -c -f -C -d   # -d: nasm is a host tool, not a pacbrew package
sudo pacman --config "$REPO/pacman.conf" --noconfirm -U ./ps5-payload-dav1d-*.pkg.tar.gz
"$PS5_PAYLOAD_SDK/bin/prospero-pkg-config" --modversion dav1d
