# Sourced by scripts that install into the sysroot (12, 17, 21, 22). cmake does
# not track sysroot archives as dependencies of kodi.bin, so after replacing
# one, every build directory's kodi.bin must go - or the next build silently
# keeps the old library linked in (see docs/ARCHITECTURE-LESSONS.md).
# Covers the legacy build dir, the PS5MC per-version dirs, and $KODI_BUILD.
sysroot_changed() {
  local kb
  for kb in "${KODI_BUILD:-}" "$HOME/kodi-ps5-build" "$HOME"/ps5mc-build-*; do
    [ -n "$kb" ] && [ -f "$kb/kodi.bin" ] || continue
    rm -f "$kb/kodi.bin"
    echo "==> sysroot changed: removed $kb/kodi.bin so the next build relinks against the new libraries"
  done
}
