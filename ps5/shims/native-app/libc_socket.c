/*
 * Python-only BSD socket layer over Sony's libSceNet.
 *
 * The PS5 sandbox denies the raw socket()/connect() syscalls (Python got
 * EACCES on every connection); sceNet* is the allowed path (Kodi's curl uses
 * it). These ps5_* functions are called ONLY from Python's socket module,
 * which pacbrew/python3/ps5_pysocket.h redirects into here at compile time -
 * so nothing else in the process (Kodi, curl, libc) is affected. Every fd
 * these return is a sceNet socket, so no fd-type tracking is needed.
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/filio.h>   /* FIONBIO */
#include <stdarg.h>

typedef struct SceNetSockaddr
{
  uint8_t sa_len;
  uint8_t sa_family;
  char sa_data[14];
} SceNetSockaddr;

int sceNetSocket(const char* name, int family, int type, int protocol);
int sceNetSocketClose(int s);
int sceNetConnect(int s, const SceNetSockaddr* addr, int addrlen);
int sceNetBind(int s, const SceNetSockaddr* addr, int addrlen);
int sceNetListen(int s, int backlog);
int sceNetSendto(int s, const void* buf, size_t len, int flags, const SceNetSockaddr* to, int tolen);
int sceNetRecvfrom(int s, void* buf, size_t len, int flags, SceNetSockaddr* from, int* fromlen);
int sceNetSetsockopt(int s, int level, int optname, const void* optval, int optlen);
int sceNetGetsockopt(int s, int level, int optname, void* optval, int* optlen);
int sceNetGetsockname(int s, SceNetSockaddr* name, int* namelen);
int sceNetGetpeername(int s, SceNetSockaddr* name, int* namelen);
int sceNetShutdown(int s, int how);
int* sceNetErrnoLoc(void);
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);

/* sceNet must be initialized before sockets can be created. The resolver shim
 * (libc_net.c) already relies on a net pool; do the library init once here so
 * the first Python socket() works. sceNetInit is idempotent-ish: if already
 * initialized it returns an "already" error, which we ignore. */
extern void sceKernelDebugOutText(int channel, const char* text);
static int g_net_ready = 0;
static void net_init_once(void)
{
  if (g_net_ready)
    return;
  int r = sceNetInit();                 /* 0 or "already initialized" both fine */
  int pool = sceNetPoolCreate("kodi-py", 0x10000, 0); /* a heap for socket bufs */
  (void)r; (void)pool;
  g_net_ready = 1;
}

struct sockaddr;

static void sce_log(const char* call, int se)
{
  char b[128];
  /* minimal formatter: sceNet error is hex */
  const char* hex = "0123456789abcdef";
  char code[11]; int n = 0; unsigned v = (unsigned)se;
  code[n++]='0'; code[n++]='x';
  for (int sh=28; sh>=0; sh-=4) code[n++]=hex[(v>>sh)&0xf];
  code[n]=0;
  int i=0; const char* p="[kodi-ps5] pysock ";
  while (*p && i<120) b[i++]=*p++;
  const char* c=call; while (*c && i<120) b[i++]=*c++;
  b[i++]=' '; const char* q=code; while (*q && i<125) b[i++]=*q++;
  b[i++]='\n'; b[i]=0;
  sceKernelDebugOutText(0, b);
}
static int sce_fail_tagged(const char* call)
{
  int* e = sceNetErrnoLoc();
  int se = e ? *e : 0;
  sce_log(call, se);
  switch (se & 0xff)
  {
    case 0x20: errno = EPIPE; break;
    case 0x23: errno = EWOULDBLOCK; break;
    case 0x24: errno = EINPROGRESS; break;
    case 0x25: errno = EALREADY; break;
    case 0x28: errno = EISCONN; break;
    case 0x27: errno = ENOTCONN; break;
    case 0x3d: errno = ECONNRESET; break;
    case 0x3e: errno = ECONNREFUSED; break;
    case 0x40: errno = ETIMEDOUT; break;
    default:   errno = se ? EIO : ECONNREFUSED; break;
  }
  return -1;
}
static int sce_fail(void) { return sce_fail_tagged("?"); }

static int to_sce(const struct sockaddr* sa, unsigned int len, SceNetSockaddr* out, int* outlen)
{
  if (!sa || len > sizeof(SceNetSockaddr))
    return -1;
  memcpy(out, sa, len);
  out->sa_len = (uint8_t)len;
  *outlen = (int)len;
  return 0;
}
static void from_sce(const SceNetSockaddr* in, int inlen, struct sockaddr* sa, unsigned int* len)
{
  if (!sa || !len)
    return;
  int n = inlen < (int)*len ? inlen : (int)*len;
  memcpy(sa, in, n);
  *len = (unsigned int)n;
}

int ps5_socket(int domain, int type, int protocol)
{
  net_init_once();
  /* Python ORs BSD flag bits into the type (SOCK_CLOEXEC = 0x10000000,
     SOCK_NONBLOCK = 0x20000000). sceNet only accepts the bare socket type and
     rejects the flags with EPROTONOSUPPORT (0x8041012b). Strip them: CLOEXEC
     is meaningless in a title (no exec), and Python sets non-blocking mode
     itself afterwards via its own fcntl/ioctl path when it needs it. */
  int bare_type = type & ~(0x10000000 | 0x20000000);
  int s = sceNetSocket("python", domain, bare_type, protocol);
  if (s < 0)
  {
    return sce_fail_tagged("socket");
  }
  return s;
}
int ps5_close(int fd)
{
  return sceNetSocketClose(fd) < 0 ? sce_fail() : 0;
}
int ps5_connect(int s, const struct sockaddr* addr, unsigned int len)
{
  SceNetSockaddr sa; int sl;
  if (to_sce(addr, len, &sa, &sl)) { errno = EINVAL; return -1; }
  return sceNetConnect(s, &sa, sl) < 0 ? sce_fail_tagged("connect") : 0;
}
int ps5_bind(int s, const struct sockaddr* addr, unsigned int len)
{
  SceNetSockaddr sa; int sl;
  if (to_sce(addr, len, &sa, &sl)) { errno = EINVAL; return -1; }
  return sceNetBind(s, &sa, sl) < 0 ? sce_fail() : 0;
}
int ps5_listen(int s, int backlog)
{
  return sceNetListen(s, backlog) < 0 ? sce_fail() : 0;
}
long ps5_sendto(int s, const void* buf, unsigned long len, int flags, const struct sockaddr* to, unsigned int tolen)
{
  SceNetSockaddr sa; int sl = 0; const SceNetSockaddr* pto = NULL;
  if (to && to_sce(to, tolen, &sa, &sl) == 0) pto = &sa;
  int r = sceNetSendto(s, buf, (size_t)len, flags, pto, sl);
  return r < 0 ? sce_fail_tagged("send") : r;
}
long ps5_recvfrom(int s, void* buf, unsigned long len, int flags, struct sockaddr* from, unsigned int* fromlen)
{
  SceNetSockaddr sa; int sl = (int)sizeof sa;
  int r = sceNetRecvfrom(s, buf, (size_t)len, flags, from ? &sa : NULL, from ? &sl : NULL);
  if (r < 0) return sce_fail_tagged("recv");
  if (from) from_sce(&sa, sl, from, fromlen);
  return r;
}
long ps5_send(int s, const void* buf, unsigned long len, int flags)
{
  return ps5_sendto(s, buf, len, flags, NULL, 0);
}
long ps5_recv(int s, void* buf, unsigned long len, int flags)
{
  return ps5_recvfrom(s, buf, len, flags, NULL, NULL);
}
int ps5_setsockopt(int s, int level, int opt, const void* val, unsigned int len)
{
  return sceNetSetsockopt(s, level, opt, val, (int)len) < 0 ? sce_fail_tagged("setsockopt") : 0;
}
int ps5_getsockopt(int s, int level, int opt, void* val, unsigned int* len)
{
  int l = (int)*len; int r = sceNetGetsockopt(s, level, opt, val, &l); *len = (unsigned int)l;
  return r < 0 ? sce_fail() : 0;
}
int ps5_getsockname(int s, struct sockaddr* name, unsigned int* len)
{
  SceNetSockaddr sa; int l = (int)sizeof sa;
  if (sceNetGetsockname(s, &sa, &l) < 0) return sce_fail();
  from_sce(&sa, l, name, len); return 0;
}
int ps5_getpeername(int s, struct sockaddr* name, unsigned int* len)
{
  SceNetSockaddr sa; int l = (int)sizeof sa;
  if (sceNetGetpeername(s, &sa, &l) < 0) return sce_fail();
  from_sce(&sa, l, name, len); return 0;
}
int ps5_shutdown(int s, int how)
{
  return sceNetShutdown(s, how) < 0 ? sce_fail() : 0;
}

/* Python sets a socket non-blocking with ioctl(fd, FIONBIO, &on) - a raw
 * libkernel syscall the sandbox denies (EACCES), and the one call between
 * socket() and connect() our redirect did not cover, which is why connect()
 * was never reached. sceNet exposes non-blocking as a socket option:
 * setsockopt(SOL_SOCKET=0xffff, SCE_NET_SO_NBIO=0x1200, int). Route FIONBIO
 * to that; any other ioctl on a socket is unsupported here (ENOTTY). */
#define SCE_NET_SOL_SOCKET 0xffff
#define SCE_NET_SO_NBIO    0x1200
int ps5_ioctl(int fd, unsigned long req, ...)
{
  va_list ap;
  va_start(ap, req);
  void* arg = va_arg(ap, void*);
  va_end(ap);
  if (req == FIONBIO)
  {
    int on = arg ? (*(int*)arg ? 1 : 0) : 0;
    if (sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, (int)sizeof on) < 0)
      return sce_fail_tagged("ioctl-nbio");
    return 0;
  }
  errno = ENOTTY;
  return -1;
}
