/*
 *  libc functions the title's clean-room runtime (libc.prx) does not export
 *  but Kodi's dependencies reference. Each is either implemented or fails the
 *  way the real function fails when the feature is unavailable.
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <stddef.h>

/* curl: interface-scoped IPv6 literals (fe80::1%eth0) - no named interfaces. */
unsigned int if_nametoindex(const char* ifname)
{
  (void)ifname;
  errno = ENXIO;
  return 0;
}

/* OpenSSL DTLS batching: callers fall back to sendmsg/recvmsg on ENOSYS. */
struct mmsghdr;
struct timespec;
int sendmmsg(int s, struct mmsghdr* msgvec, unsigned int vlen, int flags)
{
  (void)s; (void)msgvec; (void)vlen; (void)flags;
  errno = ENOSYS;
  return -1;
}
int recvmmsg(int s, struct mmsghdr* msgvec, unsigned int vlen, int flags, struct timespec* timeout)
{
  (void)s; (void)msgvec; (void)vlen; (void)flags; (void)timeout;
  errno = ENOSYS;
  return -1;
}

/* OpenSSL DSO path lookup: no dynamic loader in a title. */
int dladdr(const void* addr, void* info)
{
  (void)addr; (void)info;
  return 0;
}

/*
 * FreeBSD's MB_CUR_MAX macro calls ___mb_cur_max(). The title runs in the C
 * locale, where a character is one byte.
 */
size_t ___mb_cur_max(void)
{
  return 1;
}

/*
 * _setjmp/_longjmp (no signal mask), System V x86-64. FreeBSD's jmp_buf is
 * 13 longs; we use 9 slots: rip, rbx, rsp, rbp, r12-r15, x87 CW + MXCSR.
 */
__asm__(
    ".text\n"
    ".globl _setjmp\n"
    ".type _setjmp,@function\n"
    "_setjmp:\n"
    "  movq (%rsp), %rdx\n"
    "  movq %rdx, 0(%rdi)\n"
    "  movq %rbx, 8(%rdi)\n"
    "  leaq 8(%rsp), %rdx\n"
    "  movq %rdx, 16(%rdi)\n"
    "  movq %rbp, 24(%rdi)\n"
    "  movq %r12, 32(%rdi)\n"
    "  movq %r13, 40(%rdi)\n"
    "  movq %r14, 48(%rdi)\n"
    "  movq %r15, 56(%rdi)\n"
    "  fnstcw 64(%rdi)\n"
    "  stmxcsr 68(%rdi)\n"
    "  xorl %eax, %eax\n"
    "  ret\n"
    ".size _setjmp,.-_setjmp\n"
    ".globl _longjmp\n"
    ".type _longjmp,@function\n"
    "_longjmp:\n"
    "  movl %esi, %eax\n"
    "  testl %eax, %eax\n"
    "  jnz 1f\n"
    "  incl %eax\n"
    "1:\n"
    "  movq 8(%rdi), %rbx\n"
    "  movq 16(%rdi), %rsp\n"
    "  movq 24(%rdi), %rbp\n"
    "  movq 32(%rdi), %r12\n"
    "  movq 40(%rdi), %r13\n"
    "  movq 48(%rdi), %r14\n"
    "  movq 56(%rdi), %r15\n"
    "  fldcw 64(%rdi)\n"
    "  ldmxcsr 68(%rdi)\n"
    "  jmp *0(%rdi)\n"
    ".size _longjmp,.-_longjmp\n");
