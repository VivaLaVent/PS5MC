/*
 *  POSIX/BSD functions the console's system libc (SceLibcInternal) and kernel
 *  library do not export, implemented on top of what they do export.
 *  Compiled against the SDK's FreeBSD headers, so types and flag values are
 *  the ones Kodi and its libraries were built with.
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <netdb.h>
#include <pwd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* ---- time --------------------------------------------------------------- */

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

time_t timegm(struct tm* tm)
{
  /* normalise month into range, carrying into the year, like mktime() */
  int64_t year = (int64_t)tm->tm_year + 1900;
  int64_t mon = tm->tm_mon;
  year += mon / 12;
  mon %= 12;
  if (mon < 0)
  {
    mon += 12;
    --year;
  }
  int64_t days = days_from_civil(year, (unsigned)mon + 1, 1) + (tm->tm_mday - 1);
  int64_t secs = days * 86400 + (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec;
  time_t t = (time_t)secs;
  struct tm norm;
  gmtime_r(&t, &norm);
  *tm = norm;
  return t;
}

struct tm* gmtime_r(const time_t* timep, struct tm* result)
{
  int64_t t = (int64_t)*timep;
  int64_t days = t / 86400;
  int64_t rem = t % 86400;
  if (rem < 0)
  {
    rem += 86400;
    --days;
  }
  result->tm_hour = (int)(rem / 3600);
  result->tm_min = (int)(rem % 3600 / 60);
  result->tm_sec = (int)(rem % 60);
  result->tm_wday = (int)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday */

  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = y + (m <= 2);

  result->tm_year = (int)(year - 1900);
  result->tm_mon = (int)m - 1;
  result->tm_mday = (int)d;
  result->tm_yday = (int)(days - days_from_civil(year, 1, 1));
  result->tm_isdst = 0;
  result->tm_gmtoff = 0;
  result->tm_zone = (char*)"UTC";
  return result;
}

struct tm* localtime_r(const time_t* timep, struct tm* result)
{
  /* The system libc's localtime() knows the console's time zone; its struct
     starts with the nine standard fields, so copy those and derive the rest. */
  const struct tm* lt = localtime(timep);
  if (!lt)
    return gmtime_r(timep, result);
  result->tm_sec = lt->tm_sec;
  result->tm_min = lt->tm_min;
  result->tm_hour = lt->tm_hour;
  result->tm_mday = lt->tm_mday;
  result->tm_mon = lt->tm_mon;
  result->tm_year = lt->tm_year;
  result->tm_wday = lt->tm_wday;
  result->tm_yday = lt->tm_yday;
  result->tm_isdst = lt->tm_isdst;
  struct tm tmp = *result;
  result->tm_gmtoff = (long)(timegm(&tmp) - *timep);
  result->tm_zone = (char*)(result->tm_gmtoff ? "LOCAL" : "UTC");
  return result;
}

/* ---- temporary files ---------------------------------------------------- */

static uint64_t g_tmp_seed;

static void fill_template(char* x, size_t n)
{
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  if (g_tmp_seed == 0)
  {
    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_tmp_seed = ((uint64_t)ts.tv_sec << 30) ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)getpid() << 48) ^ (uintptr_t)x;
  }
  for (size_t i = 0; i < n; ++i)
  {
    uint64_t z = (g_tmp_seed += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    x[i] = alphabet[(z ^ (z >> 31)) % (sizeof(alphabet) - 1)];
  }
}

/* Locate the run of trailing X's (before `suffix` chars); at least six. */
static char* template_x(char* tmpl, int suffix, size_t* nx)
{
  size_t len = strlen(tmpl);
  if (suffix < 0 || (size_t)suffix > len)
    return NULL;
  char* end = tmpl + len - suffix;
  char* p = end;
  while (p > tmpl && p[-1] == 'X')
    --p;
  *nx = (size_t)(end - p);
  return *nx >= 6 ? p : NULL;
}

static int make_temp(char* tmpl, int suffix, int flags, int dir)
{
  size_t nx = 0;
  char* x = template_x(tmpl, suffix, &nx);
  if (!x)
  {
    errno = EINVAL;
    return -1;
  }
  for (int attempt = 0; attempt < 100; ++attempt)
  {
    fill_template(x, nx);
    if (dir)
    {
      if (mkdir(tmpl, 0700) == 0)
        return 0;
    }
    else
    {
      int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL | flags, 0600);
      if (fd >= 0)
        return fd;
    }
    if (errno != EEXIST)
      return -1;
  }
  errno = EEXIST;
  return -1;
}

int mkstemps(char* tmpl, int suffixlen) { return make_temp(tmpl, suffixlen, 0, 0); }
int mkostemp(char* tmpl, int flags) { return make_temp(tmpl, 0, flags & (O_APPEND | O_CLOEXEC | O_SYNC), 0); }
char* mkdtemp(char* tmpl) { return make_temp(tmpl, 0, 0, 1) == 0 ? tmpl : NULL; }

/* ---- files and memory ----------------------------------------------------- */

int pipe2(int* fds, int flags)
{
  if (flags & ~(O_CLOEXEC | O_NONBLOCK))
  {
    errno = EINVAL;
    return -1;
  }
  if (pipe(fds) != 0)
    return -1;
  for (int i = 0; i < 2; ++i)
  {
    if ((flags & O_CLOEXEC) && fcntl(fds[i], F_SETFD, FD_CLOEXEC) != 0)
      goto fail;
    if (flags & O_NONBLOCK)
    {
      int fl = fcntl(fds[i], F_GETFL);
      if (fl < 0 || fcntl(fds[i], F_SETFL, fl | O_NONBLOCK) != 0)
        goto fail;
    }
  }
  return 0;
fail:
  close(fds[0]);
  close(fds[1]);
  return -1;
}

/* Advice only; ignoring it is always correct. */
int posix_fadvise(int fd, off_t offset, off_t len, int advice)
{
  (void)fd; (void)offset; (void)len; (void)advice;
  return 0;
}
int posix_madvise(void* addr, size_t len, int advice)
{
  (void)addr; (void)len; (void)advice;
  return 0;
}

/* ---- identity, services, system ----------------------------------------- */

/* No user database on the console. */
struct passwd* getpwnam(const char* name)
{
  (void)name;
  return NULL;
}
int getpwuid_r(uid_t uid, struct passwd* pwd, char* buf, size_t len, struct passwd** result)
{
  (void)uid; (void)pwd; (void)buf; (void)len;
  *result = NULL; /* "not found" */
  return 0;
}

/* No /etc/protocols or /etc/services. */
struct protoent* getprotobyname(const char* name)
{
  (void)name;
  return NULL;
}
struct servent* getservbyport(int port, const char* proto)
{
  (void)port; (void)proto;
  return NULL;
}

/* uname() is an inline around __xuname() in FreeBSD's <sys/utsname.h>. */
int __xuname(int namesize, void* namebuf)
{
  static const char* const fields[5] = {"FreeBSD", "ps5", "PS5", "PlayStation 5", "amd64"};
  char* p = (char*)namebuf;
  for (int i = 0; i < 5; ++i, p += namesize)
  {
    strncpy(p, fields[i], (size_t)namesize - 1);
    p[namesize - 1] = '\0';
  }
  return 0;
}

char* nl_langinfo(nl_item item)
{
  /* Kodi and libiconv only ask for the codeset; filenames and text on the
     console are UTF-8. */
  return (char*)(item == CODESET ? "UTF-8" : "");
}

void openlog(const char* ident, int option, int facility)
{
  (void)ident; (void)option; (void)facility;
}

/* ---- tsearch(3): unbalanced binary search tree ---------------------------- */

typedef struct ps5_tnode
{
  const void* key; /* first member: callers dereference the node as the key */
  struct ps5_tnode* left;
  struct ps5_tnode* right;
} ps5_tnode;

typedef int (*ps5_cmp)(const void*, const void*);

void* tsearch(const void* key, void** rootp, ps5_cmp compar)
{
  if (!rootp)
    return NULL;
  ps5_tnode** link = (ps5_tnode**)rootp;
  while (*link)
  {
    int c = compar(key, (*link)->key);
    if (c == 0)
      return *link;
    link = c < 0 ? &(*link)->left : &(*link)->right;
  }
  ps5_tnode* node = (ps5_tnode*)malloc(sizeof(*node));
  if (!node)
    return NULL;
  node->key = key;
  node->left = node->right = NULL;
  *link = node;
  return node;
}

void* tfind(const void* key, void* const* rootp, ps5_cmp compar)
{
  if (!rootp)
    return NULL;
  ps5_tnode* node = *(ps5_tnode* const*)rootp;
  while (node)
  {
    int c = compar(key, node->key);
    if (c == 0)
      return node;
    node = c < 0 ? node->left : node->right;
  }
  return NULL;
}

/* Returns the parent of the deleted node, (void*)1 if it was the root. */
void* tdelete(const void* key, void** rootp, ps5_cmp compar)
{
  if (!rootp || !*rootp)
    return NULL;
  ps5_tnode** link = (ps5_tnode**)rootp;
  ps5_tnode* parent = NULL;
  while (*link)
  {
    int c = compar(key, (*link)->key);
    if (c == 0)
      break;
    parent = *link;
    link = c < 0 ? &(*link)->left : &(*link)->right;
  }
  ps5_tnode* node = *link;
  if (!node)
    return NULL;
  if (!node->left)
    *link = node->right;
  else if (!node->right)
    *link = node->left;
  else
  {
    /* replace with the in-order successor */
    ps5_tnode** s = &node->right;
    while ((*s)->left)
      s = &(*s)->left;
    ps5_tnode* succ = *s;
    *s = succ->right;
    succ->left = node->left;
    succ->right = node->right;
    *link = succ;
  }
  free(node);
  return parent ? (void*)parent : (void*)1;
}

/* ======================================================================
 * Functions the SDK stubs bind to libScePosixForWebKit, a system module a
 * native title does not get: the loader leaves such imports at address 0
 * (first Kodi launch crashed in spdlog's isatty()). Defined here, they are
 * no longer imported at all. Networking ones live in libc_net.c.
 * ====================================================================== */

#include <fnmatch.h>
#include <sys/sysctl.h>

int isatty(int fd)
{
  (void)fd; /* titles have no terminals */
  errno = ENOTTY;
  return 0;
}

int mkstemp(char* tmpl) { return make_temp(tmpl, 0, 0, 0); }

/* Kernel randomness: kern.arandom (FreeBSD KERN_ARND), else /dev/urandom. */
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

void arc4random_buf(void* buf, size_t len)
{
  if (kernel_random(buf, len) == 0)
    return;
  /* last resort (should not happen): splitmix64 over the clock */
  unsigned char* p = (unsigned char*)buf;
  fill_template((char*)p, 0); /* seeds g_tmp_seed */
  for (size_t i = 0; i < len; ++i)
  {
    uint64_t z = (g_tmp_seed += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    p[i] = (unsigned char)(z ^ (z >> 31));
  }
}

/* getentropy(3) lives in shims/libkodishim (libkodishim.a), which both the
 * kodi.bin link and this eboot link use, so it has exactly one definition. */

uint32_t arc4random(void)
{
  uint32_t v;
  arc4random_buf(&v, sizeof(v));
  return v;
}

/* ---- fnmatch(3) ----------------------------------------------------------- */

static int fold(int c, int flags)
{
  return (flags & FNM_CASEFOLD) && c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Match one bracket expression at *pp against c; advances *pp past ']'.
   Returns 1 match, 0 no match, -1 malformed (treat '[' literally). */
static int bracket(const char** pp, int c, int flags)
{
  const char* p = *pp;
  int negate = 0, matched = 0;
  if (*p == '!' || *p == '^')
  {
    negate = 1;
    ++p;
  }
  int first = 1;
  while (*p && (first || *p != ']'))
  {
    first = 0;
    int lo = (unsigned char)*p;
    if (lo == '\\' && !(flags & FNM_NOESCAPE) && p[1])
      lo = (unsigned char)*++p;
    if (lo == '/' && (flags & FNM_PATHNAME))
      return -1;
    ++p;
    int hi = lo;
    if (*p == '-' && p[1] && p[1] != ']')
    {
      ++p;
      hi = (unsigned char)*p;
      if (hi == '\\' && !(flags & FNM_NOESCAPE) && p[1])
        hi = (unsigned char)*++p;
      ++p;
    }
    int fc = fold(c, flags);
    if ((c >= lo && c <= hi) || (fc >= fold(lo, flags) && fc <= fold(hi, flags)))
      matched = 1;
  }
  if (*p != ']')
    return -1;
  *pp = p + 1;
  return matched != negate;
}

static int fnmatch_at(const char* p, const char* s, const char* start, int flags)
{
  for (;;)
  {
    int c = (unsigned char)*p++;
    int leading_period = *s == '.' && (flags & FNM_PERIOD) &&
                         (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/'));
    switch (c)
    {
      case '\0':
        if ((flags & FNM_LEADING_DIR) && *s == '/')
          return 0;
        return *s ? FNM_NOMATCH : 0;
      case '?':
        if (!*s || ((flags & FNM_PATHNAME) && *s == '/') || leading_period)
          return FNM_NOMATCH;
        ++s;
        break;
      case '*':
        while (*p == '*')
          ++p;
        if (leading_period)
          return FNM_NOMATCH;
        if (!*p)
        {
          if (flags & FNM_PATHNAME)
            return ((flags & FNM_LEADING_DIR) || !strchr(s, '/')) ? 0 : FNM_NOMATCH;
          return 0;
        }
        if (*p == '/' && (flags & FNM_PATHNAME))
        {
          s = strchr(s, '/');
          if (!s)
            return FNM_NOMATCH;
          break;
        }
        for (; *s; ++s)
        {
          if (fnmatch_at(p, s, start, flags & ~FNM_PERIOD) == 0)
            return 0;
          if (*s == '/' && (flags & FNM_PATHNAME))
            break;
        }
        return fnmatch_at(p, s, start, flags & ~FNM_PERIOD) == 0 ? 0 : FNM_NOMATCH;
      case '[':
      {
        if (!*s || ((flags & FNM_PATHNAME) && *s == '/') || leading_period)
          return FNM_NOMATCH;
        const char* q = p;
        int r = bracket(&q, (unsigned char)*s, flags);
        if (r < 0)
        {
          if (*s != '[')
            return FNM_NOMATCH;
        }
        else
        {
          if (!r)
            return FNM_NOMATCH;
          p = q;
        }
        ++s;
        break;
      }
      case '\\':
        if (!(flags & FNM_NOESCAPE) && *p)
          c = (unsigned char)*p++;
        __attribute__((fallthrough));
      default:
        if (fold(c, flags) != fold((unsigned char)*s, flags))
          return FNM_NOMATCH;
        ++s;
        break;
    }
  }
}

int fnmatch(const char* pattern, const char* string, int flags)
{
  return fnmatch_at(pattern, string, string, flags);
}

/* ======================================================================
 * Directory reading. The system libc's opendir() fails inside the title
 * sandbox (Kodi could not list even /app0/share/kodi/addons), so implement
 * the <dirent.h> API directly on the kernel's getdents(2), which returns
 * FreeBSD 11 struct dirent records - the layout the SDK headers declare.
 * ====================================================================== */

#include <dirent.h>

/*
 * The console's filesystems store directories in large blocks (64 KiB on
 * /app0) and, like FreeBSD's msdosfs, reject getdents() with EINVAL when the
 * buffer is smaller than one block - which is also why the system libc's
 * opendir() came back empty. Size the buffer from st_blksize (>= 64 KiB) and
 * grow it on EINVAL.
 */
struct _dirdesc
{
  int fd;
  int len;  /* bytes of valid records in buf */
  int pos;  /* offset of the next record in buf */
  int cap;  /* size of buf */
  long loc; /* entries returned so far (telldir cookie) */
  char* buf;
};

#define PS5_DIRBUF_MIN (64 * 1024)
#define PS5_DIRBUF_MAX (1024 * 1024)

DIR* fdopendir(int fd)
{
  struct stat st;
  if (fstat(fd, &st) != 0)
    return NULL;
  if (!S_ISDIR(st.st_mode))
  {
    errno = ENOTDIR;
    return NULL;
  }
  DIR* d = (DIR*)calloc(1, sizeof(*d));
  if (!d)
  {
    errno = ENOMEM;
    return NULL;
  }
  int cap = PS5_DIRBUF_MIN;
  while (cap < st.st_blksize && cap < PS5_DIRBUF_MAX)
    cap *= 2;
  d->buf = (char*)malloc((size_t)cap);
  if (!d->buf)
  {
    free(d);
    errno = ENOMEM;
    return NULL;
  }
  d->cap = cap;
  d->fd = fd;
  return d;
}

DIR* opendir(const char* path)
{
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0)
    return NULL;
  DIR* d = fdopendir(fd);
  if (!d)
  {
    int saved = errno;
    close(fd);
    errno = saved;
  }
  return d;
}

struct dirent* readdir(DIR* d)
{
  if (!d)
  {
    errno = EBADF;
    return NULL;
  }
  for (;;)
  {
    if (d->pos >= d->len)
    {
      int n = getdents(d->fd, d->buf, d->cap);
      while (n < 0 && errno == EINVAL && d->cap < PS5_DIRBUF_MAX)
      {
        char* bigger = (char*)realloc(d->buf, (size_t)d->cap * 2);
        if (!bigger)
        {
          errno = ENOMEM;
          return NULL;
        }
        d->buf = bigger;
        d->cap *= 2;
        n = getdents(d->fd, d->buf, d->cap);
      }
      if (n < 0 && (errno == ENOSYS || errno == EPERM))
      {
        long base = 0;
        n = getdirentries(d->fd, d->buf, d->cap, &base);
      }
      if (n <= 0)
        return NULL; /* end of directory (errno untouched) or error (errno set) */
      d->len = n;
      d->pos = 0;
    }
    struct dirent* e = (struct dirent*)(d->buf + d->pos);
    if (e->d_reclen == 0 || d->pos + e->d_reclen > d->len)
    {
      d->pos = d->len; /* corrupt record: drop the rest of this block */
      continue;
    }
    d->pos += e->d_reclen;
    if (e->d_fileno == 0)
      continue; /* deleted entry */
    ++d->loc;
    return e;
  }
}

int readdir_r(DIR* d, struct dirent* entry, struct dirent** result)
{
  int saved = errno;
  errno = 0;
  struct dirent* e = readdir(d);
  if (!e)
  {
    *result = NULL;
    int err = errno;
    errno = saved;
    return err;
  }
  memcpy(entry, e, e->d_reclen < sizeof(*entry) ? e->d_reclen : sizeof(*entry));
  *result = entry;
  errno = saved;
  return 0;
}

void rewinddir(DIR* d)
{
  if (!d)
    return;
  lseek(d->fd, 0, SEEK_SET);
  d->len = d->pos = 0;
  d->loc = 0;
}

long telldir(DIR* d) { return d ? d->loc : -1; }

void seekdir(DIR* d, long loc)
{
  if (!d)
    return;
  rewinddir(d);
  while (d->loc < loc && readdir(d))
    ;
}

int dirfd(DIR* d)
{
  if (!d)
  {
    errno = EINVAL;
    return -1;
  }
  return d->fd;
}

int closedir(DIR* d)
{
  if (!d)
  {
    errno = EBADF;
    return -1;
  }
  int r = close(d->fd);
  free(d->buf);
  free(d);
  return r;
}

/* umask(): only libkernel_sys exports it, and titles do not get that module
   (the call jumped to address 0). Record the mask; Kodi's files are opened up
   explicitly by main() (OpenUpTree), so the kernel default is fine. */
static mode_t g_umask = 022;
mode_t umask(mode_t mask)
{
  mode_t old = g_umask;
  g_umask = mask & 0777;
  return old;
}

/* ======================================================================
 * Calls only libkernel_sys exports (a system-process module titles do not
 * get). Each answers the way the real call does when the feature is not
 * available, so callers take their usual fallback path.
 * ====================================================================== */

#include <sys/mount.h>

/* SQLite keeps a new file's owner in line with its directory: irrelevant here. */
int fchown(int fd, uid_t owner, gid_t group)
{
  (void)fd; (void)owner; (void)group;
  return 0;
}

/* A title cannot create processes (Kodi uses this for external programs). */
pid_t fork(void)
{
  errno = ENOSYS;
  return -1;
}

/* Filesystem statistics: Kodi's free-space query and fontconfig's
   filesystem-type checks both handle failure. */
int statfs(const char* path, struct statfs* buf)
{
  (void)path; (void)buf;
  errno = ENOSYS;
  return -1;
}
int fstatfs(int fd, struct statfs* buf)
{
  (void)fd; (void)buf;
  errno = ENOSYS;
  return -1;
}

/* No hard or symbolic links on the title's filesystems. EPERM is what
   fontconfig's cache lock checks for before falling back to a lock folder. */
int link(const char* existing, const char* newpath)
{
  (void)existing; (void)newpath;
  errno = EPERM;
  return -1;
}
int symlink(const char* target, const char* linkpath)
{
  (void)target; (void)linkpath;
  errno = EPERM;
  return -1;
}

/* Nothing is a symbolic link: EINVAL is readlink()'s answer for that. */
ssize_t readlink(const char* restrict path, char* restrict buf, size_t size)
{
  (void)buf; (void)size;
  struct stat st;
  if (stat(path, &st) != 0)
    return -1; /* ENOENT etc. from stat */
  errno = EINVAL;
  return -1;
}

/*
 * getcwd(3) / realpath(3): both come from the clean-room libc.prx in a title,
 * where getcwd is unimplemented - its import resolved to a garbage address
 * and Python's frozen getpath (which computes sys.path at interpreter start)
 * crashed on the first call. Kodi itself never calls either, which is why it
 * went unnoticed. A sandboxed title has no meaningful working directory, so
 * keep one ourselves: it starts as $HOME (the writable save-data area) or "/",
 * and chdir() - a real libkernel syscall - is mirrored so getcwd stays
 * consistent with it. realpath does no symlink resolution (readlink above
 * reports there are none) - it just makes the path absolute.
 */
#include <limits.h>
#include <stdio.h>
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

static char g_cwd[PATH_MAX] = "";

static void cwd_init(void)
{
  if (g_cwd[0])
    return;
  const char* home = getenv("HOME");
  if (home && home[0] == '/' && strlen(home) < sizeof g_cwd)
    strcpy(g_cwd, home);
  else
    strcpy(g_cwd, "/");
}

char* getcwd(char* buf, size_t size)
{
  cwd_init();
  size_t need = strlen(g_cwd) + 1;
  if (!buf)
  {
    if (size == 0)
      size = need;
    if (size < need)
    {
      errno = ERANGE;
      return NULL;
    }
    buf = (char*)malloc(size);
    if (!buf)
    {
      errno = ENOMEM;
      return NULL;
    }
  }
  else if (size < need)
  {
    errno = size == 0 ? EINVAL : ERANGE;
    return NULL;
  }
  memcpy(buf, g_cwd, need);
  return buf;
}

/* Mirror chdir() into our cwd so getcwd() follows it. The real libkernel
 * chdir is called through its wrapped name; see 30-deploy.sh (--wrap=chdir). */
int __real_chdir(const char* path);
int __wrap_chdir(const char* path)
{
  int rc = __real_chdir(path);
  if (rc == 0 && path && path[0])
  {
    cwd_init();
    if (path[0] == '/')
    {
      if (strlen(path) < sizeof g_cwd)
        strcpy(g_cwd, path);
    }
    else
    {
      char tmp[PATH_MAX];
      int n = snprintf(tmp, sizeof tmp, "%s/%s", strcmp(g_cwd, "/") == 0 ? "" : g_cwd, path);
      if (n > 0 && (size_t)n < sizeof tmp)
        strcpy(g_cwd, tmp);
    }
  }
  return rc;
}

char* realpath(const char* restrict path, char* restrict resolved)
{
  if (!path || !path[0])
  {
    errno = ENOENT;
    return NULL;
  }
  char tmp[PATH_MAX];
  if (path[0] == '/')
  {
    if (strlen(path) >= sizeof tmp)
    {
      errno = ENAMETOOLONG;
      return NULL;
    }
    strcpy(tmp, path);
  }
  else
  {
    cwd_init();
    int n = snprintf(tmp, sizeof tmp, "%s/%s", strcmp(g_cwd, "/") == 0 ? "" : g_cwd, path);
    if (n <= 0 || (size_t)n >= sizeof tmp)
    {
      errno = ENAMETOOLONG;
      return NULL;
    }
  }
  /* collapse "/./" and "//" runs; leave ".." alone (no symlinks to worry about
   * but a stat-free normalization must not invent paths) */
  char out[PATH_MAX];
  size_t o = 0;
  for (size_t i = 0; tmp[i]; )
  {
    if (tmp[i] == '/')
    {
      if (o == 0 || out[o - 1] != '/')
        out[o++] = '/';
      i++;
      if (tmp[i] == '.' && (tmp[i + 1] == '/' || tmp[i + 1] == '\0'))
        i++;
      continue;
    }
    out[o++] = tmp[i++];
  }
  if (o > 1 && out[o - 1] == '/')
    o--;
  out[o] = '\0';
  if (!resolved)
  {
    resolved = (char*)malloc(o + 1);
    if (!resolved)
    {
      errno = ENOMEM;
      return NULL;
    }
  }
  memcpy(resolved, out, o + 1);
  return resolved;
}

/*
 * fopen(3) errno normalization. Python's frozen getpath (sys.path setup at
 * interpreter start) probes for pyvenv.cfg with fopen() unconditionally and
 * only tolerates FileNotFoundError/PermissionError, i.e. errno ENOENT or
 * EACCES/EPERM; any other errno (left unset, 0, a Sony code) becomes a bare
 * OSError that escapes and aborts init with "error evaluating path". The
 * clean-room libc.prx's fopen does not reliably set a POSIX errno on failure.
 * Linked with --wrap=fopen: call the real fopen, and if it fails derive errno
 * from stat() (a real libkernel syscall): missing -> ENOENT, directory ->
 * EISDIR, present-but-unopenable -> EACCES. Success is untouched.
 */
extern int sceKernelDebugOutText(int, const char*);
#include <stdio.h>
FILE* __real_fopen(const char* restrict path, const char* restrict mode);
FILE* __real_fopen64(const char* restrict path, const char* restrict mode);

/* Translate an fopen mode string to open(2) flags for the raw-open fallback.
 * Covers the modes Kodi/Python actually use: r, rb, w, wb, a, ab, and '+'. */
static int fopen_mode_to_flags(const char* mode)
{
  if (!mode || !*mode)
    return -1;
  int flags;
  switch (mode[0])
  {
    case 'r': flags = O_RDONLY; break;
    case 'w': flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': flags = O_WRONLY | O_CREAT | O_APPEND; break;
    default: return -1;
  }
  for (const char* p = mode + 1; *p; ++p)
    if (*p == '+')
      flags = (flags & ~(O_RDONLY | O_WRONLY)) | O_RDWR;
  /* 'b' and other stdio-only flags (e.g. 'e', 'x') are ignored here; O_CLOEXEC
   * and O_EXCL are not needed for the files this fallback handles. */
  return flags;
}

static int g_fopen_logged = 0;
FILE* __wrap_fopen(const char* restrict path, const char* restrict mode)
{
  FILE* f = __real_fopen(path, mode);
  if (f || !path)
    return f;
  if (!g_fopen_logged)
  {
    g_fopen_logged = 1;
    char b[256];
    snprintf(b, sizeof b, "[ps5fopen] __wrap_fopen reached, real fopen failed: %s\n", path);
    sceKernelDebugOutText(0, b);
  }
  /* The clean-room libc.prx's fopen() cannot open files on the title's mounted
   * image (/app0) - the same sandbox limitation that made opendir() fail and
   * forced the getdents-based DIR here. Our open()/read() DO work on /app0
   * (the skin XML, fonts and add-ons all load through them), so when the real
   * fopen fails, retry with raw open(2) and wrap the descriptor with fdopen().
   * This is what makes CXBTFReader read skin.estuary/media/Textures.xbt, whose
   * failure left the whole UI without icons. */
  const int flags = fopen_mode_to_flags(mode);
  if (flags >= 0)
  {
    const int fd = open(path, flags, 0666);
    if (fd >= 0)
    {
      FILE* viaFd = fdopen(fd, mode);
      if (viaFd)
      {
        { char b[256]; snprintf(b, sizeof b, "[ps5fopen] fallback OK: %s\n", path); sceKernelDebugOutText(0, b); }
        return viaFd;
      }
      { char b[256]; snprintf(b, sizeof b, "[ps5fopen] open ok, fdopen FAILED: %s (errno %d)\n", path, errno); sceKernelDebugOutText(0, b); }
      close(fd);
    }
    else { char b[256]; snprintf(b, sizeof b, "[ps5fopen] fallback open FAILED: %s (errno %d)\n", path, errno); sceKernelDebugOutText(0, b); }
  }
  /* Still failed: set a POSIX errno Python's path probe tolerates (see above). */
  struct stat st;
  if (stat(path, &st) != 0)
    errno = (errno == ENOTDIR || errno == ENAMETOOLONG) ? errno : ENOENT;
  else if (S_ISDIR(st.st_mode))
    errno = EISDIR;
  else
    errno = EACCES;
  return NULL;
}

/* With -D_FILE_OFFSET_BITS=64, Kodi's fopen() calls compile to fopen64(), so the
 * --wrap=fopen above never fires for them (this is why Textures.xbt still would
 * not open). Wrap fopen64 too, sharing the same /app0 raw-open fallback. */
FILE* __wrap_fopen64(const char* restrict path, const char* restrict mode)
{
  return __wrap_fopen(path, mode);
}
