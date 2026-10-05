#!/usr/bin/env bash
# Small libraries Kodi's dependencies link against that the console lacks:
#  - libuuid (crossguid needs uuid/uuid.h + -luuid on every UNIX target; pacbrew
#    has no util-linux): shims/libuuid, random v4 UUIDs, nothing more.
#  - libprocstat (exiv2 references it on every __FreeBSD__ target): shims/libprocstat, stubs.
set -euo pipefail

export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
source "$PS5_PAYLOAD_SDK/toolchain/prospero.sh"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$PS5_PAYLOAD_SDK/target/user/homebrew"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

$CC -O2 -fPIC -I"$HERE/shims/libuuid" -c "$HERE/shims/libuuid/uuid.c" -o "$BUILD/uuid.o"
$AR rcs "$BUILD/libuuid.a" "$BUILD/uuid.o"

sudo install -d "$DEST/lib/pkgconfig" "$DEST/include/uuid"
sudo install -m644 "$BUILD/libuuid.a" "$DEST/lib/"
sudo install -m644 "$HERE/shims/libuuid/uuid.h" "$DEST/include/uuid/"
sudo tee "$DEST/lib/pkgconfig/uuid.pc" >/dev/null <<PC
prefix=/user/homebrew
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: uuid
Description: libuuid-compatible shim for the PS5 payload SDK (random v4 UUIDs)
Version: 2.39.0
Libs: -L\${libdir} -luuid
Cflags: -I\${includedir}
PC
echo "installed libuuid shim into $DEST"

# libprocstat stub: exiv2 links it on every __FreeBSD__ target (see shims/libprocstat).
$CC -O2 -fPIC -c "$HERE/shims/libprocstat/procstat.c" -o "$BUILD/procstat.o"
$AR rcs "$BUILD/libprocstat.a" "$BUILD/procstat.o"
sudo install -m644 "$BUILD/libprocstat.a" "$DEST/lib/"
echo "installed libprocstat stub into $DEST"

# libkodishim: getentropy/explicit_bzero for the kodi.bin link (libpython
# references them; the SDK stubs lack them). See shims/libkodishim/kodishim.c.
$CC -O2 -fPIC -c "$HERE/shims/libkodishim/kodishim.c" -o "$BUILD/kodishim.o"
$CC -O2 -fPIC -c "$HERE/shims/native-app/libc_socket.c" -o "$BUILD/libc_socket.o"
$AR rcs "$BUILD/libkodishim.a" "$BUILD/kodishim.o" "$BUILD/libc_socket.o"
sudo install -m644 "$BUILD/libkodishim.a" "$DEST/lib/"
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/sysroot-changed.sh"; sysroot_changed
echo "installed libkodishim into $DEST"
