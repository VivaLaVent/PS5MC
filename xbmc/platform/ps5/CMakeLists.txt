set(SOURCES CPUInfoPS5.cpp
            JitProbe.cpp
            GPUInfoPS5.cpp
            MemUtils.cpp
            PlatformPS5.cpp
            PS5InterfaceForCLog.cpp
            SandboxPS5.cpp
            VideoOutInfo.cpp
            strptime.cpp)

set(HEADERS CPUInfoPS5.h
            JitProbe.h
            GPUInfoPS5.h
            PlatformPS5.h
            PS5InterfaceForCLog.h
            SandboxPS5.h
            VideoOutInfo.h
            sce/SceAudioOut.h
            sce/SceImeDialog.h
            sce/ScePad.h
            sce/SceUserService.h)

core_add_library(platform_ps5)
