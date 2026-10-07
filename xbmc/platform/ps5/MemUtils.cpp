/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/MemUtils.h"

#include <cstdarg>
#include <cstdio>

extern "C" int sceKernelDebugOutText(int, const char*);
namespace { void Klogf(const char* fmt, ...) { char b[256]; va_list ap; va_start(ap, fmt); std::vsnprintf(b, sizeof b, fmt, ap); va_end(ap); sceKernelDebugOutText(0, b); } }

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

#include <sys/types.h>

#include <sys/sysctl.h>


// Kodi's allocations live in one direct-memory heap managed by the ps5-opengl
// template's app_heap.c, which counts its live bytes. The deploy appends these
// two accessors to it; the weak versions here keep the plain CMake link of
// kodi.bin (which does not include the template) working, and lose to the
// template's strong ones in the eboot.
// The direct-memory heap (ps5-opengl's app_heap.c) is linked into the eboot by
// the native-app template and defines these as STRONG symbols; the weak
// fallbacks here keep a plain kodi.bin link (and tests) working and lose to the
// strong ones in the eboot. Resolved at LINK time - no constructor (one ran
// during C++ static init and crashed in ios_base::Init) and no runtime lookup.
extern "C" __attribute__((weak)) size_t ps5_heap_capacity_value()
{
  return 0;
}
extern "C" __attribute__((weak)) size_t ps5_heap_live_bytes_value()
{
  return 0;
}

namespace KODI
{
namespace MEMORY
{

void* AlignedMalloc(size_t s, size_t alignTo)
{
  void* p = nullptr;
  int res = posix_memalign(&p, alignTo, s);
  if (res == EINVAL)
    throw std::runtime_error("Failed to align memory, alignment is not a multiple of 2");
  else if (res == ENOMEM)
    throw std::runtime_error("Failed to align memory, insufficient memory available");
  return p;
}

void AlignedFree(void* p)
{
  if (!p)
    return;
  free(p);
}

void GetMemoryStatus(MemoryStatus* buffer)
{
  if (!buffer)
    return;

  // 16 GiB GDDR6 shared with the GPU. There is no public API for the budget a
  // homebrew title is actually granted, so report physical memory when the
  // kernel answers and a conservative figure otherwise. Kodi only uses this
  // for the system-info screen and cache-size heuristics.
  // Asked once: Kodi's GUI polls this several times a second, and a refused
  // sysctl ("hw.physmem is not approved") is logged by the kernel every time.
  static uint64_t physmem = 0;
  if (physmem == 0)
  {
    size_t len = sizeof(physmem);
    if (sysctlbyname("hw.physmem", &physmem, &len, nullptr, 0) != 0 || physmem == 0)
      physmem = 5ULL * 1024 * 1024 * 1024;
  }

  // Report Kodi's heap: its size and how much of it is in use. (The fixed
  // "half of 5 GiB" this replaced always showed 2560/5120 MB.)
  const size_t capacity = ps5_heap_capacity_value();
  if (capacity != 0)
  {
    const size_t live = ps5_heap_live_bytes_value();
    buffer->totalPhys = capacity;
    buffer->availPhys = live < capacity ? capacity - live : 0;
    static bool logged = false;
    if (!logged)
    {
      logged = true;
      Klogf("[kodi-ps5] memory: heap %llu MiB, %llu MiB live\n",
            static_cast<unsigned long long>(capacity >> 20),
            static_cast<unsigned long long>(live >> 20));
    }
    return;
  }
  buffer->totalPhys = physmem; // no heap counters registered: a static estimate
  buffer->availPhys = physmem / 2;
  static bool loggedEstimate = false;
  if (!loggedEstimate)
  {
    loggedEstimate = true;
    Klogf("[kodi-ps5] memory: no heap counters registered; estimating from physmem %llu MiB\n",
          static_cast<unsigned long long>(physmem >> 20));
  }
}

} // namespace MEMORY
} // namespace KODI
