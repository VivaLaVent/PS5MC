# Source this after installing anything into the PS5 sysroot.
#
# cmake tracks kodi.bin's source objects but NOT the sysroot libraries it links
# against, so after a rebuilt libkodishim / libiconv / SDK stub is installed,
# `cmake --build` sees nothing changed and does not relink: the eboot ships the
# OLD library. This cost two full test iterations before it was noticed (the
# Python socket shim "fix that didn't work" was never actually linked in).
# Removing kodi.bin forces the relink; nothing else is affected.
sysroot_changed() {
  local kb="${KODI_BUILD:-$HOME/kodi-ps5-build}"
  if [ -f "$kb/kodi.bin" ]; then
    rm -f "$kb/kodi.bin"
    echo "==> sysroot changed: removed $kb/kodi.bin so the next cmake --build relinks against the new libraries"
  fi
}
