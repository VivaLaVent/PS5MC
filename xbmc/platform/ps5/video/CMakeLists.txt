# FFmpeg headers live in the sysroot's homebrew prefix
include_directories(SYSTEM ${CMAKE_SYSROOT}/user/homebrew/include)

set(SOURCES VideoDec2.cpp
            DVDVideoCodecPS5.cpp
            VideoBufferPS5.cpp
            RendererPS5.cpp)

set(HEADERS VideoDec2.h
            DVDVideoCodecPS5.h
            VideoBufferPS5.h
            RendererPS5.h
            VideoCodecRegistration.h)

core_add_library(platform_ps5_video)
