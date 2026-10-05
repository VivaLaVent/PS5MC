#!/usr/bin/env bash
# Build and install CPython 3.14 (static) into the sysroot for Kodi's Python
# add-ons; pacbrew has no package for it. Recipe: pacbrew/python3/PKGBUILD.
# Once installed, scripts/20-configure-kodi.sh enables Kodi's Python interface.
set -euo pipefail

export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
export MAKEFLAGS="${MAKEFLAGS:--j$(nproc)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${WORK:-$HOME/ps5-work}"
REPO="$WORK/pacbrew-repo"          # for pacman.conf
BUILD="$WORK/kodi-pacbrew/python3"

[ -f "$REPO/pacman.conf" ] || { echo "pacbrew-repo not found at $REPO (run 00-setup-wsl.sh first)"; exit 1; }
mkdir -p "$BUILD"
cp "$HERE"/pacbrew/python3/* "$BUILD/"
cd "$BUILD"
rm -rf src pkg ./*.pkg.tar.gz
makepkg -c -f -C -d
sudo pacman --config "$REPO/pacman.conf" --noconfirm -U ./ps5-payload-python3-*.pkg.tar.gz
ls -l "$PS5_PAYLOAD_SDK/target/user/homebrew/lib/libpython3.14.a"
