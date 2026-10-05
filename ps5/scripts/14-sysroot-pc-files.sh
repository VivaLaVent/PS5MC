#!/usr/bin/env bash
# Some pacbrew-repo packages install a library without its pkg-config file,
# and Kodi's find modules only look through pkg-config / CMake configs.
# Write the missing .pc files into the sysroot. Idempotent.
set -euo pipefail

export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
PC="$PS5_PAYLOAD_SDK/target/user/homebrew/lib/pkgconfig"
sudo install -d "$PC"

# sqlite: pacbrew runs `make lib_install`, which skips sqlite3.pc
if [ ! -f "$PC/sqlite3.pc" ]; then
  ver=$(grep -m1 '#define SQLITE_VERSION ' "$PS5_PAYLOAD_SDK/target/user/homebrew/include/sqlite3.h" | sed 's/.*"\(.*\)".*/\1/')
  sudo tee "$PC/sqlite3.pc" >/dev/null <<PCFILE
prefix=/user/homebrew
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: SQLite
Description: SQL database engine (pacbrew static build)
Version: ${ver:-3.46.1}
Libs: -L\${libdir} -lsqlite3
Libs.private: -lz
Cflags: -I\${includedir}
PCFILE
  echo "wrote sqlite3.pc (${ver:-3.46.1})"
fi
echo "sysroot pkg-config files ok"
