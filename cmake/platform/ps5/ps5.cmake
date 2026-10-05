# PlayStation 5 homebrew platform.
# Graphics: OpenGL 4.6 Core through the ps5-opengl SDK (Mesa/Gallium on AGC),
# presented on a single fullscreen EGL surface. No windowed mode.

set(APP_RENDER_SYSTEM gl)

# The ps5-opengl SDK ships GL and EGL together as one static package with a
# CMake config. Kodi's FindOpenGl/FindEGL modules would otherwise probe the
# sysroot with find_library(GL)/find_library(EGL), which finds pacbrew's
# software osmesa build instead. Both modules start with
#   if(NOT TARGET ${APP_NAME_LC}::<Dep>)
# so pre-creating the targets here makes them skip their probing.
#
# APP_NAME_LC is not defined yet at this point of the configure run (it comes
# from core_find_versions()), so derive it from version.txt the same way.
file(STRINGS "${CMAKE_SOURCE_DIR}/version.txt" _ps5_app_name_line REGEX "^APP_NAME ")
string(REGEX REPLACE "^APP_NAME +" "" _ps5_app_name "${_ps5_app_name_line}")
string(TOLOWER "${_ps5_app_name}" _ps5_app_lc)
if(NOT _ps5_app_lc)
  set(_ps5_app_lc kodi)
endif()

find_package(PS5OpenGL CONFIG REQUIRED)

if(NOT TARGET ${_ps5_app_lc}::OpenGl)
  add_library(${_ps5_app_lc}::OpenGl INTERFACE IMPORTED)
  set_target_properties(${_ps5_app_lc}::OpenGl PROPERTIES
                        INTERFACE_LINK_LIBRARIES PS5OpenGL::OpenGL
                        INTERFACE_COMPILE_DEFINITIONS HAS_GL)
  set(OPENGL_FOUND TRUE CACHE INTERNAL "")
endif()

if(NOT TARGET ${_ps5_app_lc}::EGL)
  add_library(${_ps5_app_lc}::EGL INTERFACE IMPORTED)
  set_target_properties(${_ps5_app_lc}::EGL PROPERTIES
                        INTERFACE_LINK_LIBRARIES PS5OpenGL::OpenGL
                        INTERFACE_COMPILE_DEFINITIONS "HAS_EGL;EGL_NO_X11;MESA_EGL_NO_X11_HEADERS")
  set(EGL_FOUND TRUE CACHE INTERNAL "")
endif()

# Kodi 21 (Omega) names the guard targets differently: its FindOpenGl /
# FindEGL check OpenGL::GL and EGL::EGL, define HAS_GL=1 / HAS_EGL=1, and
# register the targets on INTERNAL_DEPS_PROP for the link step. Pre-create
# those too, so the same platform file satisfies both versions; on 22 they are
# simply unused extra targets.
if(NOT TARGET OpenGL::GL)
  add_library(OpenGL::GL INTERFACE IMPORTED)
  set_target_properties(OpenGL::GL PROPERTIES
                        INTERFACE_LINK_LIBRARIES PS5OpenGL::OpenGL
                        INTERFACE_COMPILE_DEFINITIONS HAS_GL=1)
  set_property(GLOBAL APPEND PROPERTY INTERNAL_DEPS_PROP OpenGL::GL)
endif()
if(NOT TARGET EGL::EGL)
  add_library(EGL::EGL INTERFACE IMPORTED)
  set_target_properties(EGL::EGL PROPERTIES
                        INTERFACE_LINK_LIBRARIES PS5OpenGL::OpenGL
                        INTERFACE_COMPILE_DEFINITIONS "HAS_EGL=1;EGL_NO_X11;MESA_EGL_NO_X11_HEADERS")
  set_property(GLOBAL APPEND PROPERTY INTERNAL_DEPS_PROP EGL::EGL)
endif()

unset(_ps5_app_name_line)
unset(_ps5_app_name)
unset(_ps5_app_lc)

set(PLATFORM_REQUIRED_DEPS OpenGl EGL)

# Nothing of this exists on the console.
set(PLATFORM_OPTIONAL_DEPS_EXCLUDE Alsa Avahi Bluetooth CAP CEC DBus LircClient
                                   Pipewire PulseAudio Sndio UDEV)
