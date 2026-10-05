/*
 * Route ONLY Python's socket module through libSceNet.
 *
 * A PS5 title's sandbox denies the raw BSD socket()/connect() syscalls
 * (EACCES); Sony's sceNet* API is the permitted path (it's what Kodi's curl
 * uses). An earlier attempt wrapped these names process-wide with --wrap and
 * broke Kodi's own curl, which also uses sceNet. Instead this header is
 * force-included into socketmodule.c ONLY (via CPPFLAGS in the PKGBUILD), so
 * just Python's socket calls are redirected; Kodi, curl and libc are
 * untouched. The ps5_* functions live in shims/native-app/libc_socket.c,
 * linked into the eboot.
 *
 * SOCKETCLOSE in socketmodule.c maps to close(); redirect it to ps5_close so
 * Python's socket fds are closed via sceNet, while the process-wide close()
 * stays the normal one.
 */
#pragma once
#ifdef PS5_PYSOCKET

#include <sys/types.h>
struct sockaddr;

int ps5_socket(int domain, int type, int protocol);
int ps5_connect(int s, const struct sockaddr* addr, unsigned int len);
int ps5_bind(int s, const struct sockaddr* addr, unsigned int len);
int ps5_listen(int s, int backlog);
long ps5_send(int s, const void* buf, unsigned long len, int flags);
long ps5_recv(int s, void* buf, unsigned long len, int flags);
long ps5_sendto(int s, const void* buf, unsigned long len, int flags, const struct sockaddr* to, unsigned int tolen);
long ps5_recvfrom(int s, void* buf, unsigned long len, int flags, struct sockaddr* from, unsigned int* fromlen);
int ps5_setsockopt(int s, int level, int opt, const void* val, unsigned int len);
int ps5_getsockopt(int s, int level, int opt, void* val, unsigned int* len);
int ps5_getsockname(int s, struct sockaddr* name, unsigned int* len);
int ps5_getpeername(int s, struct sockaddr* name, unsigned int* len);
int ps5_shutdown(int s, int how);
int ps5_close(int fd);
int ps5_ioctl(int fd, unsigned long req, ...);

#define socket      ps5_socket
#define connect     ps5_connect
#define bind        ps5_bind
#define listen      ps5_listen
#define send        ps5_send
#define recv        ps5_recv
#define sendto      ps5_sendto
#define recvfrom    ps5_recvfrom
#define setsockopt  ps5_setsockopt
#define getsockopt  ps5_getsockopt
#define getsockname ps5_getsockname
#define getpeername ps5_getpeername
#define shutdown    ps5_shutdown
#define SOCKETCLOSE ps5_close
#define ioctl       ps5_ioctl

#endif /* PS5_PYSOCKET */
