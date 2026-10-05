# PS5 install rules. `make install DESTDIR=<stage>` produces
#   <stage>/app0/lib/kodi/kodi.elf          (main executable, becomes eboot.bin)
#   <stage>/app0/share/kodi/{addons,media,system,userdata}
# The staged tree is then assembled into a ShadowMountPlus folder title by
# scripts/30-deploy.sh (see the port README).
set(APP_BINARY ${APP_NAME_LC}.elf)
set(APP_PREFIX ${prefix})
set(APP_LIB_DIR ${libdir}/${APP_NAME_LC})
set(APP_DATA_DIR ${datarootdir}/${APP_NAME_LC})
set(APP_INCLUDE_DIR ${includedir}/${APP_NAME_LC})

configure_file(${CMAKE_SOURCE_DIR}/cmake/KodiConfig.cmake.in
               ${CORE_BUILD_DIR}/scripts/${APP_NAME}Config.cmake @ONLY)

install(TARGETS ${APP_NAME_LC} DESTINATION ${libdir}/${APP_NAME_LC}
        RENAME ${APP_BINARY} COMPONENT kodi-bin)

# Python's standard library, found by PYTHONHOME (set in main.cpp)
if(ENABLE_PYTHON)
  install(DIRECTORY ${CMAKE_SYSROOT}/user/homebrew/lib/python3.14
          DESTINATION ${datarootdir}/${APP_NAME_LC}/python/lib
          COMPONENT kodi
          PATTERN "__pycache__" EXCLUDE)
endif()

foreach(_dir addons media system userdata)
  install(DIRECTORY ${CMAKE_BINARY_DIR}/${_dir}
          DESTINATION ${datarootdir}/${APP_NAME_LC}
          COMPONENT kodi
          PATTERN "*.so" EXCLUDE)
endforeach()
unset(_dir)

message(STATUS "PS5 build: install with DESTDIR to stage the title image")
