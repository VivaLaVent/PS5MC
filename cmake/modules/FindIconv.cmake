#.rst:
# FindIconv
# --------
# Finds the ICONV library
#
# This will define the following targets:
#
#   ${APP_NAME_LC}::Iconv - An alias of the Iconv::Iconv target
#   LIBRARY::Iconv - An alias of the Iconv::Iconv target
#   Iconv::Iconv - The ICONV library

if(NOT TARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})

  # We do this dance to utilise cmake system FindIconv. Saves us dealing with it
  set(_temp_CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH})
  unset(CMAKE_MODULE_PATH)

  if(Iconv_FIND_REQUIRED)
    set(ICONV_REQUIRED "REQUIRED")
  endif()

  if(CORE_SYSTEM_NAME STREQUAL "ps5")
    # A PS5 title has no iconv in its C library: the SDK's libSceLibcInternal
    # exports none, and the plain iconv_open a title binds at run time is the
    # console's system libc, which knows only the Unicode encodings and fails
    # (EINVAL) on CP437 - the encoding of zip entry names in add-on packages,
    # and of every legacy subtitle/filename codepage Kodi offers. pacbrew's
    # GNU libiconv 1.17 (built by scripts/00-setup-wsl.sh, in the SDK sysroot)
    # handles them all, but CMake's stock FindIconv test-compiles a plain
    # iconv_open call, which *links* in this cross build, so it concludes iconv
    # is built into libc (Iconv_IS_BUILT_IN) and links neither the library nor
    # its header. Force the discovery path instead: GNU libiconv's own
    # <iconv.h> then renames iconv_open -> libiconv_open, which resolves in
    # libiconv.a rather than at address 0. No shim, no interposition. NOTE:
    # pacbrew's libiconv must be built with --enable-extra-encodings
    # (scripts/21-rebuild-libiconv.sh), or it links fine but has no CP437
    # converter and iconv_open("UTF-8","CP437") still fails at run time - the
    # add-on-zip case this whole path exists for.
    set(Iconv_IS_BUILT_IN OFF CACHE BOOL "iconv is not in the PS5 libc" FORCE)
  endif()

  find_package(Iconv ${ICONV_REQUIRED} ${SEARCH_QUIET})

  # Back to our normal module paths
  set(CMAKE_MODULE_PATH ${_temp_CMAKE_MODULE_PATH})

  if(ICONV_FOUND OR Iconv_FOUND)
    # We still want to Alias its "standard" target to our APP_NAME_LC based target
    # for integration into our core dep packaging
    add_library(${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME} ALIAS Iconv::Iconv)
    add_library(LIBRARY::${CMAKE_FIND_PACKAGE_NAME} ALIAS Iconv::Iconv)

    # Required for external searches. Not used internally
    set(Iconv_FOUND ON CACHE BOOL "Iconv found")
    mark_as_advanced(Iconv_FOUND)
  else()
    if(Iconv_FIND_REQUIRED)
      message(FATAL_ERROR "Iconv libraries were not found.")
    endif()
  endif()
endif()
