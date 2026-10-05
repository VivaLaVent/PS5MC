/*
 *  Stub libprocstat for the PS5 payload SDK sysroot. exiv2 references the
 *  FreeBSD libprocstat API on any __FreeBSD__ target (only in its library
 *  info dump, which Kodi never calls); the console has no such library.
 *  Every function reports "nothing" and tolerates NULL.
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stddef.h>

struct procstat;
struct kinfo_proc;
struct filestat_list;

struct procstat* procstat_open_sysctl(void) { return NULL; }
struct kinfo_proc* procstat_getprocs(struct procstat* ps, int what, int arg, unsigned int* count)
{
  (void)ps; (void)what; (void)arg;
  if (count) *count = 0;
  return NULL;
}
struct filestat_list* procstat_getfiles(struct procstat* ps, struct kinfo_proc* kp, int mmapped)
{
  (void)ps; (void)kp; (void)mmapped;
  return NULL;
}
void procstat_freefiles(struct procstat* ps, struct filestat_list* head) { (void)ps; (void)head; }
void procstat_freeprocs(struct procstat* ps, struct kinfo_proc* p) { (void)ps; (void)p; }
void procstat_close(struct procstat* ps) { (void)ps; }
