#!/usr/bin/env bash
# Kodi still requires TinyXML 1 (2.6.2) next to TinyXML2. pacbrew-repo has no
# package for it and Kodi has no internal recipe, so build it here into the
# SDK sysroot with STL support (Kodi's find module expects -DTIXML_USE_STL).
set -euo pipefail

export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
WORK="${WORK:-$HOME/ps5-work}"
DEST="$PS5_PAYLOAD_SDK/target/user/homebrew"
mkdir -p "$WORK" && cd "$WORK"

if [ ! -d tinyxml ]; then
  # Debian's mirror of the upstream 2.6.2 tarball (SourceForge is unreliable for scripts).
  wget -q -O tinyxml_2.6.2.orig.tar.gz \
    http://deb.debian.org/debian/pool/main/t/tinyxml/tinyxml_2.6.2.orig.tar.gz
  tar xzf tinyxml_2.6.2.orig.tar.gz
fi
cd tinyxml

# Kodi (like every distro package) expects TIXML_USE_STL to be defined by the
# header itself; it does not add the define on its own.
grep -q "^#define TIXML_USE_STL" tinyxml.h || sed -i '1i #ifndef TIXML_USE_STL\n#define TIXML_USE_STL\n#endif' tinyxml.h

$CXX -O2 -fPIC -DTIXML_USE_STL -c tinyxml.cpp tinyxmlerror.cpp tinyxmlparser.cpp tinystr.cpp
$AR rcs libtinyxml.a tinyxml.o tinyxmlerror.o tinyxmlparser.o tinystr.o

sudo install -d "$DEST/lib/pkgconfig" "$DEST/include"
sudo install -m644 libtinyxml.a "$DEST/lib/"
sudo install -m644 tinyxml.h tinystr.h "$DEST/include/"
sudo tee "$DEST/lib/pkgconfig/tinyxml.pc" >/dev/null <<PC
prefix=/user/homebrew
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: TinyXML
Description: A simple, small, C++ XML parser
Version: 2.6.2
Libs: -L\${libdir} -ltinyxml
Cflags: -I\${includedir} -DTIXML_USE_STL
PC
echo "installed tinyxml 2.6.2 into $DEST"
