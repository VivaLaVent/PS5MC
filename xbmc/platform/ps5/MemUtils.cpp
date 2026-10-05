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

#include <sys/types.h>

#include <sys/sysctl.h>

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

  buffer->totalPhys = physmem;
  buffer->availPhys = physmem / 2; // TODO: derive from the process' actual allocation
}

} // namespace MEMORY
} // namespace KODI
