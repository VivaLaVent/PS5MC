/*
 *  Backing store for the title's malloc heap (ps5-opengl's app_heap.c mspace).
 *  The template reserves it with mmap(), which draws from the title's
 *  *flexible* memory - only 448 MiB here - so a Kodi-sized heap cannot be
 *  created and malloc silently falls back to the small system heap (the first
 *  shader compile then ran out of memory). Take it from *direct* memory
 *  instead (~11 GiB for a title), with the allocation parameters ProsperoLight
 *  uses on hardware.
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int memory_type, int64_t* start);
int sceKernelMapDirectMemory(void** address, size_t length, int protection, int flags,
                             int64_t start, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t start, size_t length);
int sceKernelDebugOutText(int channel, const char* text);

static int64_t g_heap_start = -1;
static size_t g_heap_length;

static void heap_log(const char* fmt, long long a, long long b, long long c)
{
  char buf[192];
  snprintf(buf, sizeof(buf), fmt, a, b, c);
  sceKernelDebugOutText(0, buf);
}

void* ps5_heap_map(size_t size)
{
  static const int types[] = {12, 0};           /* 12: as ProsperoLight */
  static const int protections[] = {0x03, 0x33}; /* CPU RW, then CPU+GPU RW */
  const int64_t limit = sceKernelGetDirectMemorySize();
  for (unsigned t = 0; t < sizeof(types) / sizeof(types[0]); ++t)
  {
    int64_t start = -1;
    int r = sceKernelAllocateDirectMemory(0, limit, size, 0x4000, types[t], &start);
    if (r != 0)
    {
      heap_log("[kodi-ps5] heap: allocate %lld MiB direct memory (type %lld) failed: 0x%llx\n",
               (long long)(size >> 20), types[t], (unsigned)r);
      continue;
    }
    for (unsigned p = 0; p < sizeof(protections) / sizeof(protections[0]); ++p)
    {
      void* address = NULL;
      r = sceKernelMapDirectMemory(&address, size, protections[p], 0, start, 0x4000);
      if (r == 0 && address)
      {
        g_heap_start = start;
        g_heap_length = size;
        heap_log("[kodi-ps5] heap: %lld MiB of direct memory at 0x%llx (type/prot 0x%llx)\n",
                 (long long)(size >> 20), (long long)(uintptr_t)address,
                 (long long)((types[t] << 8) | protections[p]));
        return address;
      }
      heap_log("[kodi-ps5] heap: map direct memory (prot 0x%llx) failed: 0x%llx (%lld)\n",
               protections[p], (unsigned)r, 0);
    }
    sceKernelReleaseDirectMemory(start, size);
  }
  sceKernelDebugOutText(0, "[kodi-ps5] heap: no direct memory - malloc falls back to the system heap\n");
  return MAP_FAILED;
}

int ps5_heap_unmap(void* address, size_t size)
{
  (void)address;
  if (g_heap_start >= 0)
  {
    sceKernelReleaseDirectMemory(g_heap_start, g_heap_length ? g_heap_length : size);
    g_heap_start = -1;
  }
  return 0;
}
