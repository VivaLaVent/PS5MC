/* pipe() for a PS5 title: the kernel exports pipe, but the call fails inside
 * the sandbox (curl's resolver wake-up, which is a pipe, could never start).
 * An AF_UNIX socketpair does everything a pipe is used for here: read, write,
 * poll, close - so it is the fallback whenever the real call fails.
 * Linked with --wrap=pipe (scripts/30-deploy.sh). */
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int __real_pipe(int fds[2]);

int __wrap_pipe(int fds[2])
{
  if (__real_pipe(fds) == 0)
    return 0;
  return socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
}

/* fcntl() for a PS5 title: close-on-exec is meaningless here (a title cannot
 * exec), and the kernel may refuse F_SETFD/F_GETFD on some descriptors, which
 * would fail callers that only set FD_CLOEXEC after a pipe or socketpair
 * (curl's resolver wake-up). Those two commands are answered as success;
 * everything else goes to the real call. Linked with --wrap=fcntl. */
#include <fcntl.h>
#include <stdarg.h>

int __real_fcntl(int fd, int cmd, ...);

int __wrap_fcntl(int fd, int cmd, ...)
{
  if (cmd == F_SETFD)
    return 0;
  if (cmd == F_GETFD)
    return 0;
  va_list ap;
  va_start(ap, cmd);
  long arg = va_arg(ap, long);
  va_end(ap);
  return __real_fcntl(fd, cmd, arg);
}
