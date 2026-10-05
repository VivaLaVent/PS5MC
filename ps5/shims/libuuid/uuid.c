/*
 *  Minimal libuuid-compatible implementation for the PS5 payload SDK sysroot.
 *  Random (version 4) UUIDs only. Entropy comes from /dev/urandom; if the
 *  title sandbox refuses it, a splitmix64 generator seeded from the clocks,
 *  the pid and a stack address is used instead - good enough for the
 *  identifiers Kodi generates (UPnP ids, temp names), not for cryptography.
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "uuid.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t g_state;

static uint64_t splitmix64(void)
{
  uint64_t z = (g_state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

static void fill_random(unsigned char *buf, size_t len)
{
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0)
  {
    size_t got = 0;
    while (got < len)
    {
      ssize_t n = read(fd, buf + got, len - got);
      if (n <= 0)
        break;
      got += (size_t)n;
    }
    close(fd);
    if (got == len)
      return;
  }
  if (g_state == 0)
  {
    struct timespec rt = {0, 0}, mono = {0, 0};
    clock_gettime(CLOCK_REALTIME, &rt);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    g_state = ((uint64_t)rt.tv_sec << 32) ^ (uint64_t)rt.tv_nsec ^
              ((uint64_t)mono.tv_nsec << 20) ^ ((uint64_t)getpid() << 48) ^
              (uintptr_t)&rt;
  }
  for (size_t i = 0; i < len; i += 8)
  {
    uint64_t r = splitmix64();
    size_t n = len - i < 8 ? len - i : 8;
    memcpy(buf + i, &r, n);
  }
}

void uuid_generate_random(uuid_t out)
{
  fill_random(out, 16);
  out[6] = (unsigned char)((out[6] & 0x0F) | 0x40); /* version 4 */
  out[8] = (unsigned char)((out[8] & 0x3F) | 0x80); /* RFC 4122 variant */
}

void uuid_generate(uuid_t out) { uuid_generate_random(out); }
void uuid_clear(uuid_t uu) { memset(uu, 0, 16); }
void uuid_copy(uuid_t dst, const uuid_t src) { memcpy(dst, src, 16); }
int  uuid_compare(const uuid_t a, const uuid_t b) { return memcmp(a, b, 16); }

int uuid_is_null(const uuid_t uu)
{
  for (int i = 0; i < 16; ++i)
    if (uu[i])
      return 0;
  return 1;
}

static void unparse(const uuid_t uu, char *out, const char *fmt)
{
  snprintf(out, UUID_STR_LEN, fmt, uu[0], uu[1], uu[2], uu[3], uu[4], uu[5], uu[6], uu[7], uu[8],
           uu[9], uu[10], uu[11], uu[12], uu[13], uu[14], uu[15]);
}

void uuid_unparse_lower(const uuid_t uu, char *out)
{
  unparse(uu, out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x");
}

void uuid_unparse_upper(const uuid_t uu, char *out)
{
  unparse(uu, out, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X");
}

void uuid_unparse(const uuid_t uu, char *out) { uuid_unparse_lower(uu, out); }

int uuid_parse(const char *in, uuid_t uu)
{
  if (strlen(in) != 36)
    return -1;
  unsigned v[16];
  if (sscanf(in, "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x", &v[0], &v[1], &v[2], &v[3],
             &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14],
             &v[15]) != 16)
    return -1;
  for (int i = 0; i < 16; ++i)
    uu[i] = (unsigned char)v[i];
  return 0;
}
