/* Symbols libpython references that the SDK's stub libraries do not provide,
 * so the kodi.bin link (which sees only the SDK stubs) can resolve them.
 *
 * The final eboot is linked by scripts/30-deploy.sh with the native-app shims
 * (shims/native-app/libc_posix.c) as plain objects, whose getentropy() takes
 * precedence over this archive's copy: an archive member is only extracted
 * for a symbol still undefined, so there is no duplicate. explicit_bzero has
 * no other provider and comes from here in both links. */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

static int kernel_random(void* buf, size_t len)
{
  unsigned char* p = (unsigned char*)buf;
  while (len > 0)
  {
    int mib[2] = {CTL_KERN, KERN_ARND};
    size_t chunk = len > 256 ? 256 : len;
    size_t got = chunk;
    if (sysctl(mib, 2, p, &got, NULL, 0) != 0 || got == 0)
      break;
    p += got;
    len -= got;
  }
  if (len == 0)
    return 0;
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  while (len > 0)
  {
    ssize_t n = read(fd, p, len);
    if (n <= 0)
      break;
    p += n;
    len -= (size_t)n;
  }
  close(fd);
  return len == 0 ? 0 : -1;
}

int getentropy(void* buf, size_t len)
{
  if (len > 256 || kernel_random(buf, len) != 0)
  {
    errno = EIO;
    return -1;
  }
  return 0;
}

/* Must not be optimized away: write through a volatile pointer. */
void explicit_bzero(void* buf, size_t len)
{
  volatile unsigned char* p = (volatile unsigned char*)buf;
  while (len--)
    *p++ = 0;
}

/* ------------------------------------------------------------------------
 * POSIX functions libpython calls that a PS5 title does not get.
 *
 * Python's configure link-tests against the SDK's full stub set, which
 * includes libkernel_sys and libScePosixForWebKit; scripts/30-deploy.sh
 * removes those two from the eboot link because a title never loads those
 * modules (a call would jump to address 0). So these exist for the kodi.bin
 * link but not the eboot link. Provided here so both links resolve.
 *
 * Load-bearing ones are implemented on primitives libkernel does export:
 *   clock_nanosleep -> nanosleep/clock_gettime   (time.sleep uses it)
 *   fstatat         -> stat/lstat at AT_FDCWD    (os.lstat -> shutil/pathlib)
 *   utimensat       -> utimes at AT_FDCWD        (os.utime -> shutil.copy2)
 *   futimens        -> futimes
 *   openat          -> open at AT_FDCWD
 *   closefrom       -> close() loop
 * All of these are WEAK: the kodi.bin link (cmake) still links the payload
 * SDK's libc.a, which has real versions - strong definitions here would be
 * duplicates once this object is pulled in for getentropy. Weak ones yield to
 * libc.a there, and become the definitions at the eboot link, where no other
 * provider exists (the stubs that had them are removed by 30-deploy.sh).
 * The rest fail with ENOSYS (or a harmless default), which Python raises
 * as OSError - the right outcome for chroot/chown/mknod/tty speeds/uid
 * changes in a sandboxed title.
 * ---------------------------------------------------------------------- */
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <termios.h>
#include <time.h>

/* float.h FLT_ROUNDS on FreeBSD calls __flt_rounds(); 1 = round to nearest */
__attribute__((weak)) int __flt_rounds(void) { return 1; }

#define ENOSYS_RET(type, v) do { errno = ENOSYS; return (type)(v); } while (0)

static void ts_to_tv(const struct timespec* ts, struct timeval* tv)
{
  tv->tv_sec = ts->tv_sec;
  tv->tv_usec = ts->tv_nsec / 1000;
}

/* --- time.sleep() --- */
__attribute__((weak)) int clock_nanosleep(clockid_t clk, int flags, const struct timespec* req, struct timespec* rem)
{
  struct timespec rel = *req;
  if (flags & TIMER_ABSTIME)
  {
    struct timespec now;
    if (clock_gettime(clk, &now) != 0)
      return errno;
    rel.tv_sec = req->tv_sec - now.tv_sec;
    rel.tv_nsec = req->tv_nsec - now.tv_nsec;
    if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000L; rel.tv_sec--; }
    if (rel.tv_sec < 0)
      return 0; /* deadline already passed */
    rem = NULL; /* absolute sleeps report no remainder */
  }
  if (nanosleep(&rel, rem) != 0)
    return errno;
  return 0;
}

/* --- os.stat/lstat with dir_fd / follow_symlinks --- */
__attribute__((weak)) int fstatat(int dirfd, const char* path, struct stat* st, int flags)
{
  if (dirfd != AT_FDCWD)
    ENOSYS_RET(int, -1);
  return (flags & AT_SYMLINK_NOFOLLOW) ? lstat(path, st) : stat(path, st);
}

/* --- os.utime --- */
__attribute__((weak)) int utimensat(int dirfd, const char* path, const struct timespec times[2], int flags)
{
  (void)flags; /* no lutimes on this libc: symlinks are followed */
  if (dirfd != AT_FDCWD)
    ENOSYS_RET(int, -1);
  if (!times)
    return utimes(path, NULL);
  struct timeval tv[2];
  ts_to_tv(&times[0], &tv[0]);
  ts_to_tv(&times[1], &tv[1]);
  return utimes(path, tv);
}

__attribute__((weak)) int futimens(int fd, const struct timespec times[2])
{
  if (!times)
    return futimes(fd, NULL);
  struct timeval tv[2];
  ts_to_tv(&times[0], &tv[0]);
  ts_to_tv(&times[1], &tv[1]);
  return futimes(fd, tv);
}

/* --- os.open with dir_fd --- */
__attribute__((weak)) int openat(int dirfd, const char* path, int flags, ...)
{
  if (dirfd != AT_FDCWD)
    ENOSYS_RET(int, -1);
  mode_t mode = 0;
  if (flags & O_CREAT)
  {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  return open(path, flags, mode);
}

/* --- os.closerange --- */
__attribute__((weak)) void closefrom(int lowfd)
{
  for (int fd = lowfd; fd < 1024; ++fd)
    close(fd);
}

/* --- dir_fd-only variants: ENOSYS (Python only reaches them with dir_fd) --- */
__attribute__((weak)) int faccessat(int d, const char* p, int m, int f) { (void)d;(void)p;(void)m;(void)f; ENOSYS_RET(int, -1); }
__attribute__((weak)) int fchmodat(int d, const char* p, mode_t m, int f) { (void)d;(void)p;(void)m;(void)f; ENOSYS_RET(int, -1); }
__attribute__((weak)) int fchownat(int d, const char* p, uid_t u, gid_t g, int f) { (void)d;(void)p;(void)u;(void)g;(void)f; ENOSYS_RET(int, -1); }
__attribute__((weak)) int linkat(int d1, const char* p1, int d2, const char* p2, int f) { (void)d1;(void)p1;(void)d2;(void)p2;(void)f; ENOSYS_RET(int, -1); }
__attribute__((weak)) int mkdirat(int d, const char* p, mode_t m) { (void)d;(void)p;(void)m; ENOSYS_RET(int, -1); }
__attribute__((weak)) int mkfifoat(int d, const char* p, mode_t m) { (void)d;(void)p;(void)m; ENOSYS_RET(int, -1); }
__attribute__((weak)) int mknodat(int d, const char* p, mode_t m, dev_t dv) { (void)d;(void)p;(void)m;(void)dv; ENOSYS_RET(int, -1); }
__attribute__((weak)) ssize_t readlinkat(int d, const char* p, char* b, size_t n) { (void)d;(void)p;(void)b;(void)n; ENOSYS_RET(ssize_t, -1); }
__attribute__((weak)) int renameat(int d1, const char* p1, int d2, const char* p2) { (void)d1;(void)p1;(void)d2;(void)p2; ENOSYS_RET(int, -1); }
__attribute__((weak)) int symlinkat(const char* t, int d, const char* p) { (void)t;(void)d;(void)p; ENOSYS_RET(int, -1); }
__attribute__((weak)) int unlinkat(int d, const char* p, int f) { (void)d;(void)p;(void)f; ENOSYS_RET(int, -1); }

/* --- things a sandboxed title cannot do --- */
__attribute__((weak)) int chown(const char* p, uid_t u, gid_t g) { (void)p;(void)u;(void)g; ENOSYS_RET(int, -1); }
__attribute__((weak)) int lchown(const char* p, uid_t u, gid_t g) { (void)p;(void)u;(void)g; ENOSYS_RET(int, -1); }
__attribute__((weak)) int lchmod(const char* p, mode_t m) { (void)p;(void)m; ENOSYS_RET(int, -1); }
__attribute__((weak)) int lchflags(const char* p, unsigned long f) { (void)p;(void)f; ENOSYS_RET(int, -1); }
__attribute__((weak)) int chroot(const char* p) { (void)p; ENOSYS_RET(int, -1); }
__attribute__((weak)) int fchdir(int fd) { (void)fd; ENOSYS_RET(int, -1); }
__attribute__((weak)) int mkfifo(const char* p, mode_t m) { (void)p;(void)m; ENOSYS_RET(int, -1); }
__attribute__((weak)) int mknod(const char* p, mode_t m, dev_t d) { (void)p;(void)m;(void)d; ENOSYS_RET(int, -1); }
__attribute__((weak)) int posix_fallocate(int fd, off_t o, off_t l) { (void)fd;(void)o;(void)l; return ENOSYS; }
__attribute__((weak)) int posix_openpt(int f) { (void)f; ENOSYS_RET(int, -1); }
__attribute__((weak)) int killpg(pid_t pg, int s) { (void)pg;(void)s; ENOSYS_RET(int, -1); }
__attribute__((weak)) int setgid(gid_t g) { (void)g; ENOSYS_RET(int, -1); }
__attribute__((weak)) pid_t setsid(void) { ENOSYS_RET(pid_t, -1); }
__attribute__((weak)) int setpgid(pid_t p, pid_t g) { (void)p;(void)g; ENOSYS_RET(int, -1); }
__attribute__((weak)) int setresgid(gid_t r, gid_t e, gid_t s) { (void)r;(void)e;(void)s; ENOSYS_RET(int, -1); }
__attribute__((weak)) int setresuid(uid_t r, uid_t e, uid_t s) { (void)r;(void)e;(void)s; ENOSYS_RET(int, -1); }
__attribute__((weak)) pid_t getpgid(pid_t p) { (void)p; return getpid(); }
__attribute__((weak)) pid_t getpgrp(void) { return getpid(); }
__attribute__((weak)) int getresuid(uid_t* r, uid_t* e, uid_t* s) { uid_t u = getuid(); if (r) *r = u; if (e) *e = u; if (s) *s = u; return 0; }
__attribute__((weak)) int getresgid(gid_t* r, gid_t* e, gid_t* s) { gid_t g = getgid(); if (r) *r = g; if (e) *e = g; if (s) *s = g; return 0; }
__attribute__((weak)) int getloadavg(double a[], int n) { (void)a;(void)n; return -1; }
__attribute__((weak)) clock_t times(struct tms* t) { (void)t; ENOSYS_RET(clock_t, -1); }
__attribute__((weak)) long fpathconf(int fd, int n) { (void)fd;(void)n; errno = EINVAL; return -1; }
__attribute__((weak)) long pathconf(const char* p, int n) { (void)p;(void)n; errno = EINVAL; return -1; }

/* --- termios speeds (no tty in a title) --- */
__attribute__((weak)) speed_t cfgetispeed(const struct termios* t) { (void)t; return 0; }
__attribute__((weak)) speed_t cfgetospeed(const struct termios* t) { (void)t; return 0; }
__attribute__((weak)) int cfsetispeed(struct termios* t, speed_t s) { (void)t;(void)s; return 0; }
__attribute__((weak)) int cfsetospeed(struct termios* t, speed_t s) { (void)t;(void)s; return 0; }

/* --- pwd / netdb lookups: not available --- */
struct passwd;
__attribute__((weak)) int getpwnam_r(const char* n, struct passwd* p, char* b, size_t l, struct passwd** r)
{ (void)n;(void)p;(void)b;(void)l; if (r) *r = NULL; return ENOSYS; }
struct hostent;
__attribute__((weak)) struct hostent* gethostbyaddr(const void* a, unsigned int l, int t) { (void)a;(void)l;(void)t; return NULL; }
struct servent;
__attribute__((weak)) struct servent* getservbyname(const char* n, const char* p) { (void)n;(void)p; return NULL; }
__attribute__((weak)) const char* hstrerror(int e) { (void)e; return "Unknown host error"; }
__attribute__((weak)) char* strsignal(int s) { (void)s; return (char*)"Unknown signal"; }
