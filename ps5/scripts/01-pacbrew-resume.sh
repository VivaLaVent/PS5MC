#!/usr/bin/env bash
# Build pacbrew packages in upstream's ci-libs.sh order, from a given package
# (through an optional last one). ci-libs.sh has no resume of its own: after
# any failure it starts over from the SDK.
#   01-pacbrew-resume.sh sdk                   everything (what 00-setup-wsl.sh runs)
#   01-pacbrew-resume.sh miniupnpc             resume: miniupnpc through the end
#   01-pacbrew-resume.sh glu glew              a range: glu through glew only
#   SKIP="glu glew llvm mesa love" ...         packages to leave out (Kodi needs
#          none of pacbrew's GL stack; upstream also lists glu/glew before mesa)
#   REUSE=1 01-pacbrew-resume.sh <package>    install the package's already-built
#          .pkg.tar.gz instead of rebuilding it (e.g. llvm built fine but the
#          `sudo pacman -U` step failed) - later packages are built as usual.
set -uo pipefail
WORK="${WORK:-$HOME/ps5-work}"
REPO="$WORK/pacbrew-repo"
START="${1:?usage: $0 <first-package> [last-package]  (names as in ci-libs.sh)}"
END="${2:-}"
SKIP="${SKIP:-}"
export MAKEFLAGS="${MAKEFLAGS:--j$(nproc)}"
cd "$REPO"

# Same sudo keep-alive as 00-setup-wsl.sh (llvm/mesa outlast the 15 min cache).
sudo -v
( while kill -0 "$$" 2>/dev/null; do sudo -n true 2>/dev/null; sleep 60; done ) &
SUDO_KEEPALIVE=$!
trap 'kill "$SUDO_KEEPALIVE" 2>/dev/null' EXIT

# The ordered package list, pulled out of ci-libs.sh itself (comments dropped).
PKGS=$(sed -n '/^PKGS=(/,/^ *)/p' ci-libs.sh | sed -e 's/^PKGS=(//' -e 's/#.*//' -e 's/)//' | tr -s ' \n' '\n' | sed '/^$/d')

FOUND=0
for PKG in $PKGS; do
  [ "$PKG" = "$START" ] && FOUND=1
  [ "$FOUND" -eq 1 ] || continue
  case " $SKIP " in *" $PKG "*) echo "==> $PKG (skipped)"; continue;; esac
  echo "==> $PKG"
  if [ "$PKG" = "$START" ] && [ "${REUSE:-0}" = 1 ] && ls "$PKG"/ps5-payload-*.pkg.tar.gz >/dev/null 2>&1; then
    echo "    reusing already-built package"
    ( cd "$PKG" && sudo pacman --config "$REPO/pacman.conf" --noconfirm -U ./ps5-payload-*.pkg.tar.gz ) \
      || { echo "!! installing $PKG failed"; exit 1; }
    continue
  fi
  ( cd "$PKG" && rm -f ./*.pkg.tar.gz && rm -rf src pkg && makepkg -c -f -C \
      && sudo pacman --config "$REPO/pacman.conf" --noconfirm -U ./ps5-payload-*.pkg.tar.gz ) \
    || { echo "!! $PKG failed; fix and re-run: $0 $PKG${END:+ $END}   (REUSE=1 if only the install step failed)"; exit 1; }
  [ "$PKG" = "$END" ] && break
done
[ "$FOUND" -eq 1 ] || { echo "'$START' is not in ci-libs.sh's package list"; exit 1; }
echo "==> pacbrew done${SKIP:+ (skipped: $SKIP)}"
