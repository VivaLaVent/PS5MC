/*
 * Tee stdout/stderr to klog. A title's fd 1/2 go nowhere, which hides exactly
 * the messages that matter most at startup: Python's fatal init errors
 * ("Fatal Python error: ...") are written with write(2), before Kodi's own
 * stdio redirection is in place, as are C-level asserts and abort messages.
 * Linked with --wrap=write (scripts/30-deploy.sh): fd 1 and 2 are copied to
 * the kernel debug log with a tag, then passed through unchanged. Every other
 * fd is a straight passthrough (one integer compare on the hot path).
 */
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

extern void sceKernelDebugOutText(int channel, const char* text);
ssize_t __real_write(int fd, const void* buf, size_t n);

ssize_t __wrap_write(int fd, const void* buf, size_t n)
{
  if ((fd == 1 || fd == 2) && buf && n > 0)
  {
    char line[480];
    const char* tag = fd == 2 ? "[stderr] " : "[stdout] ";
    const size_t taglen = strlen(tag);
    const char* p = (const char*)buf;
    size_t left = n;
    while (left > 0)
    {
      size_t chunk = left < sizeof line - taglen - 2 ? left : sizeof line - taglen - 2;
      memcpy(line, tag, taglen);
      memcpy(line + taglen, p, chunk);
      size_t end = taglen + chunk;
      if (line[end - 1] != '\n')
        line[end++] = '\n';
      line[end] = '\0';
      sceKernelDebugOutText(0, line);
      p += chunk;
      left -= chunk;
    }
  }
  return __real_write(fd, buf, n);
}
