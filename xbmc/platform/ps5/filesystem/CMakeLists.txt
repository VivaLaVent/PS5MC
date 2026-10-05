# libsmb2 (pacbrew-repo) headers: <sysroot>/user/homebrew/include/smb2
include_directories(SYSTEM ${CMAKE_SYSROOT}/user/homebrew/include)

set(SOURCES SMB2Session.cpp
            SMB2File.cpp
            SMB2Directory.cpp)

set(HEADERS SMB2Session.h
            SMB2File.h
            SMB2Directory.h)

core_add_library(platform_ps5_filesystem)
