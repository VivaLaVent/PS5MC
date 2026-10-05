set(SOURCES dll.cpp
            DllLoaderContainer.cpp
            LibraryLoader.cpp)

set(HEADERS dll.h
            DllLoaderContainer.h
            LibraryLoader.h)

if(NOT CORE_SYSTEM_NAME STREQUAL windows AND NOT CORE_SYSTEM_NAME STREQUAL windowsstore)
  list(APPEND SOURCES SoLoader.cpp)
  list(APPEND HEADERS SoLoader.h)
else()
  list(APPEND SOURCES Win32DllLoader.cpp)
  list(APPEND HEADERS Win32DllLoader.h)
endif()

if(CORE_SYSTEM_NAME STREQUAL ps5)
  # In-process ELF loader for binary add-ons (no system dynamic loader on PS5).
  list(APPEND SOURCES ${CMAKE_SOURCE_DIR}/xbmc/platform/ps5/elf/ElfLoader.cpp
                      ${CMAKE_SOURCE_DIR}/xbmc/platform/ps5/elf/HostExports.cpp
                      ${CMAKE_SOURCE_DIR}/xbmc/platform/ps5/elf/PS5AddonLoader.cpp)
  list(APPEND HEADERS ${CMAKE_SOURCE_DIR}/xbmc/platform/ps5/elf/ElfLoader.h
                      ${CMAKE_SOURCE_DIR}/xbmc/platform/ps5/elf/HostExports.h
                      ${CMAKE_SOURCE_DIR}/xbmc/platform/ps5/elf/PS5AddonLoader.h)
endif()

add_definitions(-DAPI_DEBUG)

core_add_library(dllloader)
