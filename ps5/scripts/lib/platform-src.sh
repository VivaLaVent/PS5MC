# Resolve where the PS5 platform sources live.
#   fork mode   : this ps5/ directory sits inside a Kodi checkout that carries
#                 the platform series, so the sources are at <kodi>/xbmc/...
#   legacy mode : the old overlay/ tree next to scripts/.
# Sets PLATFORM_SRC (root to prefix "xbmc/platform/ps5/..." onto) and, if
# unset, KODI_SRC. Source this after HERE is defined.
if [ -z "${KODI_SRC:-}" ] && [ -f "$HERE/../xbmc/platform/ps5/main.cpp" ]; then
  KODI_SRC="$(cd "$HERE/.." && pwd)"
fi
if [ -f "${KODI_SRC:-/nonexistent}/xbmc/platform/ps5/main.cpp" ] && \
   git -C "$KODI_SRC" ls-files --error-unmatch xbmc/platform/ps5/main.cpp >/dev/null 2>&1; then
  PLATFORM_SRC="$KODI_SRC"
else
  PLATFORM_SRC="$HERE/overlay"
fi
