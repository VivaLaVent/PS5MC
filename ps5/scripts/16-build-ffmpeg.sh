#!/usr/bin/env bash
# Rebuild pacbrew's ffmpeg package at a version Kodi master accepts (>= 7.1)
# and upgrade it in the sysroot. Recipe: pacbrew/ffmpeg/PKGBUILD in this repo.
set -euo pipefail

export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
export MAKEFLAGS="${MAKEFLAGS:--j$(nproc)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${WORK:-$HOME/ps5-work}"
REPO="$WORK/pacbrew-repo"          # for pacman.conf
BUILD="$WORK/kodi-pacbrew/ffmpeg"

[ -f "$REPO/pacman.conf" ] || { echo "pacbrew-repo not found at $REPO (run 00-setup-wsl.sh first)"; exit 1; }
mkdir -p "$BUILD"
cp "$HERE/pacbrew/ffmpeg/PKGBUILD" "$BUILD/PKGBUILD"
cd "$BUILD"
rm -rf src pkg ./*.pkg.tar.gz
makepkg -c -f -C
sudo pacman --config "$REPO/pacman.conf" --noconfirm -U ./ps5-payload-ffmpeg-*.pkg.tar.gz
"$PS5_PAYLOAD_SDK/bin/prospero-pkg-config" --modversion libavcodec
