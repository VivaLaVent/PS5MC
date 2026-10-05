#!/usr/bin/env bash
# Rebuild pacbrew's GNU libiconv with --enable-extra-encodings and reinstall
# libiconv.a into the SDK sysroot.
#
# Why: pacbrew's libiconv PKGBUILD configures plain (--enable-static
# --disable-shared), which omits GNU libiconv's "extra" encodings - CP437
# among them. Kodi's zip reader converts add-on-package entry names from CP437
# (the codepage of names in a zip without the UTF-8 flag), so without it every
# add-on install fails with "iconv_open() for CP437 failed" and "invalid
# package". CP437 is also the missing piece for several legacy subtitle
# codepages. Rebuilding with the flag adds CP437 (and CP437's OEM siblings)
# to the same libiconv.a Kodi already links; only the static library changes,
# so a Kodi relink (scripts/30-deploy.sh) picks it up - no Kodi rebuild.
#
# Idempotent: re-running reconfigures and reinstalls. Uses the libiconv source
# pacbrew already downloaded during scripts/00-setup-wsl.sh; falls back to
# re-running the pacbrew package build if that tree is gone.
set -euo pipefail

export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
export MAKEFLAGS="${MAKEFLAGS:--j$(nproc)}"
WORK="${WORK:-$HOME/ps5-work}"
REPO="$WORK/pacbrew-repo"
LA="$PS5_PAYLOAD_SDK/target/user/homebrew/lib/libiconv.a"
NM="$PS5_PAYLOAD_SDK/bin/llvm-nm"; [ -x "$NM" ] || NM="$(command -v llvm-nm-18 || command -v llvm-nm)"

[ -f "$PS5_PAYLOAD_SDK/toolchain/prospero.sh" ] || { echo "!! ps5-payload-sdk not found at $PS5_PAYLOAD_SDK"; exit 1; }
[ -d "$REPO/libiconv" ] || { echo "!! $REPO/libiconv not found (run scripts/00-setup-wsl.sh first)"; exit 1; }

# Prefer the already-extracted source tree from the first pacbrew build.
SRC="$(find "$REPO/libiconv" -maxdepth 2 -type d -name 'libiconv-*' 2>/dev/null | head -1)"

if [ -z "$SRC" ]; then
  echo "==> no extracted libiconv source; rebuilding the pacbrew package with the flag"
  # Patch the PKGBUILD's configure line in place and run it through makepkg.
  PB="$REPO/libiconv/PKGBUILD"
  grep -q -- "--enable-extra-encodings" "$PB" || \
    sed -i 's/--enable-static --disable-shared/--enable-static --disable-shared --enable-extra-encodings/' "$PB"
  grep -q -- "--enable-extra-encodings" "$PB" || { echo "!! could not add --enable-extra-encodings to $PB"; exit 1; }
  if [ "${FORCE_EXTRA:-0}" = 1 ]; then
    # Belt-and-suspenders: define ENABLE_EXTRA on the compiler command line too,
    # so the DOS/extra converters compile even if configure's AC_DEFINE is lost.
    grep -q 'CPPFLAGS=.*ENABLE_EXTRA' "$PB" || \
      sed -i 's#\./configure #CPPFLAGS="${CPPFLAGS:-} -DENABLE_EXTRA=1" ./configure #' "$PB"
  fi
  ( cd "$REPO/libiconv" && rm -f ./*.pkg.tar.gz && rm -rf src pkg && makepkg -c -f -C \
      && sudo pacman --config "$REPO/pacman.conf" --noconfirm -U ./ps5-payload-libiconv-*.pkg.tar.gz )
else
  echo "==> reconfiguring $SRC with --enable-extra-encodings"
  # shellcheck disable=SC1090
  source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
  cd "$SRC"
  make distclean >/dev/null 2>&1 || true
  [ "${FORCE_EXTRA:-0}" = 1 ] && export CPPFLAGS="${CPPFLAGS:-} -DENABLE_EXTRA=1"
  ./configure --prefix="${PS5_HBROOT}" --host=x86_64-pc-freebsd \
              --enable-static --disable-shared --enable-extra-encodings
  ${MAKE:-make} ${MAKEFLAGS}
  DESTDIR="$PS5_PAYLOAD_SDK/target" make install
  [ -n "${PS5_CROSS_FIX_ROOT:-}" ] && ${PS5_CROSS_FIX_ROOT} "${PS5_PAYLOAD_SDK}/target/${PS5_HBROOT}" 2>/dev/null || true
fi

echo "==> verifying CP437 is now in $LA"
# The reliable signal is the converter symbols GNU libiconv compiles CP437 to
# (cp437_mbtowc / cp437_wctomb). Check the archive directly; fall back to
# nm on the extracted iconv.o if the toolchain nm can't index the .a.
have_cp437=0
if $NM "$LA" 2>/dev/null | grep -qi "cp437_mbtowc"; then
  have_cp437=1
else
  tmpd="$(mktemp -d)"; ( cd "$tmpd" && "$AR" x "$LA" iconv.o 2>/dev/null ) || true
  if [ -f "$tmpd/iconv.o" ] && $NM "$tmpd/iconv.o" 2>/dev/null | grep -qi "cp437_mbtowc"; then
    have_cp437=1
  elif [ -f "$tmpd/iconv.o" ] && strings "$tmpd/iconv.o" | grep -qx "csPC8CodePage437"; then
    have_cp437=1   # unambiguous CP437-only alias string
  fi
  rm -rf "$tmpd"
fi
if [ "$have_cp437" = 1 ]; then
  echo "    OK: CP437 converter present (cp437_mbtowc/cp437_wctomb)"
else
  echo "!! CP437 still not in libiconv.a."
  echo "   Check that config.h has ENABLE_EXTRA:"
  echo "     find ~/ps5-work/pacbrew-repo/libiconv -name config.h -path '*libiconv-1.17*' -exec grep -H ENABLE_EXTRA {} +"
  echo "   If it shows '#undef ENABLE_EXTRA', re-run with: FORCE_EXTRA=1 bash scripts/21-rebuild-libiconv.sh"
  exit 1
fi
echo
echo "Done. Relink Kodi against the new library (no Kodi rebuild needed):"
echo "  bash scripts/30-deploy.sh"

# a rebuilt sysroot library must be relinked into kodi.bin (see scripts/lib/sysroot-changed.sh)
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/sysroot-changed.sh"; sysroot_changed
