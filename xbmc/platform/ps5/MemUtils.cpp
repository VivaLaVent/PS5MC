/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/MemUtils.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

#include <sys/sysctl.h>
#include <sys/types.h>

// Kodi's allocations live in one direct-memory heap managed by the ps5-opengl
// template's app_heap.c, which counts its live bytes. scripts/30-deploy.sh
// appends these two accessors to it as strong symbols, which replace the weak
// ones below when the eboot is linked. The weak versions keep a plain kodi.bin
// link (built without the template) working: they report "no counters", and
// GetMemoryStatus then falls back to physical memory.
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

  // Kodi's allocations live in the title's direct-memory heap; report its size
  // and how much of it is in use. Kodi's GUI polls this several times a second,
  // so this path stays allocation- and syscall-free.
  const size_t capacity = ps5_heap_capacity_value();
  if (capacity != 0)
  {
    const size_t live = ps5_heap_live_bytes_value();
    buffer->totalPhys = capacity;
    buffer->availPhys = live < capacity ? capacity - live : 0;
    return;
  }

  // No heap counters (a plain kodi.bin without the native-app template): fall
  // back to physical memory. Asked once - a title is refused hw.physmem and the
  // kernel logs that refusal on every call.
  static uint64_t physmem = 0;
  if (physmem == 0)
  {
    size_t len = sizeof(physmem);
    if (sysctlbyname("hw.physmem", &physmem, &len, nullptr, 0) != 0 || physmem == 0)
      physmem = 5ULL * 1024 * 1024 * 1024;
  }
  buffer->totalPhys = physmem;
  buffer->availPhys = physmem / 2;
}

} // namespace MEMORY
} // namespace KODI
