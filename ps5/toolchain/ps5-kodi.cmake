# Kodi cross-compile toolchain for PlayStation 5 homebrew.
#
# Wraps the ps5-payload-sdk toolchain (target x86_64-sie-ps5, FreeBSD-derived
# userland) and points Kodi at:
#   * the pacbrew-built libraries inside the SDK sysroot (/user/homebrew), and
#   * the ps5-opengl SDK (Mesa/Gallium OpenGL 4.6 Core + EGL) for graphics.
#
# Usage (from an empty build dir):
#   cmake -DCMAKE_TOOLCHAIN_FILE=/path/to/ps5-kodi.cmake [options] /path/to/kodi
#
# Environment overrides: PS5_PAYLOAD_SDK, PS5_OPENGL_PREFIX

if(NOT DEFINED PS5_PAYLOAD_SDK)
  if(DEFINED ENV{PS5_PAYLOAD_SDK})
    set(PS5_PAYLOAD_SDK "$ENV{PS5_PAYLOAD_SDK}")
  else()
    set(PS5_PAYLOAD_SDK "/opt/ps5-payload-sdk")
  endif()
endif()
set(PS5_PAYLOAD_SDK "${PS5_PAYLOAD_SDK}" CACHE PATH "ps5-payload-sdk install directory")

if(NOT EXISTS "${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")
  message(FATAL_ERROR "ps5-payload-sdk not found at ${PS5_PAYLOAD_SDK} (set PS5_PAYLOAD_SDK)")
endif()

# John Törnblom's toolchain: sets CMAKE_SYSTEM_NAME FreeBSD, the sysroot,
# prospero-clang/++ wrappers, static-only linking and PIE.
include("${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")

# The SDK toolchain prefers a library's own CMake package over Find modules.
# Kodi's Find modules must run (they create the kodi::<Dep> targets the build
# links against), so restore CMake's default module-first search here.
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG FALSE)

# --- Kodi platform selection -------------------------------------------------
set(CORE_SYSTEM_NAME   ps5 CACHE STRING "Kodi core system name" FORCE)
set(CORE_PLATFORM_NAME ps5 CACHE STRING "Kodi platform" FORCE)
set(APP_RENDER_SYSTEM  gl  CACHE STRING "Kodi render system" FORCE)

# NOTE: nothing in this file may force CMAKE_INSTALL_PREFIX, CMAKE_CXX_STANDARD
# or CMAKE_PREFIX_PATH: Kodi passes this same toolchain file to the CMake
# sub-builds of its internal libraries, which install into the build tree and
# get their own -D arguments. The Kodi install prefix (/app0) is given by
# scripts/20-configure-kodi.sh.

# CMake 3.28+ scans C++20 targets for modules with clang-scan-deps, which the
# SDK's compiler wrapper does not provide. Kodi does not use modules.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# --- Libraries built by pacbrew-repo (inside the sysroot) ---------------------
set(PS5_HBROOT "${PS5_PAYLOAD_SDK}/target/user/homebrew")
list(APPEND CMAKE_FIND_ROOT_PATH "${PS5_HBROOT}")
set(ENV{PKG_CONFIG_LIBDIR}      "${PS5_HBROOT}/lib/pkgconfig:${PS5_PAYLOAD_SDK}/target/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${PS5_PAYLOAD_SDK}/target")

# --- ps5-opengl SDK -----------------------------------------------------------
if(NOT DEFINED PS5_OPENGL_PREFIX)
  if(DEFINED ENV{PS5_OPENGL_PREFIX})
    set(PS5_OPENGL_PREFIX "$ENV{PS5_OPENGL_PREFIX}")
  else()
    set(PS5_OPENGL_PREFIX "/opt/ps5-opengl-gl46")
  endif()
endif()
set(PS5_OPENGL_PREFIX "${PS5_OPENGL_PREFIX}" CACHE PATH "ps5-opengl relocatable SDK prefix")
set(PS5OpenGL_DIR "${PS5_OPENGL_PREFIX}/lib/cmake/PS5OpenGL" CACHE PATH "PS5OpenGL CMake config dir")
list(APPEND CMAKE_FIND_ROOT_PATH "${PS5_OPENGL_PREFIX}")
set(ENV{PKG_CONFIG_LIBDIR} "$ENV{PKG_CONFIG_LIBDIR}:${PS5_OPENGL_PREFIX}/lib/pkgconfig")

