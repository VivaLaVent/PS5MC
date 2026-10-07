#.rst:
# FindCurl
# --------
# Finds the Curl library
#
# This will define the following variables::
#
# CURL_FOUND - system has Curl
# CURL_INCLUDE_DIRS - the Curl include directory
# CURL_LIBRARIES - the Curl libraries
# CURL_DEFINITIONS - the Curl definitions
#
# and the following imported targets::
#
#   Curl::Curl   - The Curl library

if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_CURL libcurl QUIET)
endif()

find_path(CURL_INCLUDE_DIR NAMES curl/curl.h
                           PATHS ${PC_CURL_INCLUDEDIR})
find_library(CURL_LIBRARY NAMES curl libcurl libcurl_imp
                          PATHS ${PC_CURL_LIBDIR})

set(CURL_VERSION ${PC_CURL_VERSION})

set(CURL_LIB_TYPE SHARED)
set(CURL_LDFLAGS ${PC_CURL_LDFLAGS})

# check if curl is statically linked
if(${CURL_LIBRARY} MATCHES ".+\.a$" AND PC_CURL_STATIC_LDFLAGS)
  set(CURL_LIB_TYPE STATIC)
  set(CURL_LDFLAGS ${PC_CURL_STATIC_LDFLAGS})

  pkg_check_modules(PC_NGHTTP2 libnghttp2 QUIET)
  find_library(NGHTTP2_LIBRARY NAMES libnghttp2 nghttp2
                               PATHS ${PC_NGHTTP2_LIBDIR})
  # ps5: a static curl built without HTTP/2 has no nghttp2 to link; Kodi 22
  # only adds it when curl uses it. Do not propagate a NOTFOUND into the link.
  if(NOT NGHTTP2_LIBRARY)
    unset(NGHTTP2_LIBRARY CACHE)
    set(NGHTTP2_LIBRARY "")
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Curl
                                  REQUIRED_VARS CURL_LIBRARY CURL_INCLUDE_DIR
                                  VERSION_VAR CURL_VERSION)

if(CURL_FOUND)
  set(CURL_INCLUDE_DIRS ${CURL_INCLUDE_DIR})
  set(CURL_LIBRARIES ${CURL_LIBRARY} ${NGHTTP2_LIBRARY})
  # ps5: a static curl's own dependencies (zstd, libpsl, ssl, ...) are in
  # pkg-config's Libs.private. Kodi 21 computes them (CURL_LDFLAGS, above) but
  # never links them; Kodi 22 gets them through pkg-config's imported target.
  # Appended after libcurl.a, as a static link needs.
  if(CURL_LIB_TYPE STREQUAL STATIC AND CORE_SYSTEM_NAME STREQUAL ps5)
    list(APPEND CURL_LIBRARIES ${CURL_LDFLAGS})
  endif()

  if(NOT TARGET Curl::Curl)
    add_library(Curl::Curl ${CURL_LIB_TYPE} IMPORTED)
    set_target_properties(Curl::Curl PROPERTIES
                                     IMPORTED_LOCATION "${CURL_LIBRARY}"
                                     INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIR}")
    if(HAS_CURL_STATIC)
        set_target_properties(Curl::Curl PROPERTIES
                                         INTERFACE_COMPILE_DEFINITIONS HAS_CURL_STATIC=1)
    endif()
  endif()
endif()

mark_as_advanced(CURL_INCLUDE_DIR CURL_LIBRARY CURL_LDFLAGS)
