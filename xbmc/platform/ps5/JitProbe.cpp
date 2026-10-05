/*
 *  PS5 executable-memory probe.
 *
 *  Decides whether this title can obtain memory it can execute from - the
 *  prerequisite for an in-process code loader (binary add-ons via a userland
 *  dlopen). It does NOT load anything; it writes six bytes of machine code
 *  (mov eax,0x1337 ; ret) into a page and tries to call them, by three routes:
 *
 *    A  mmap(PROT_READ|PROT_WRITE|PROT_EXEC)                 - RWX in one step
 *    B  mmap(PROT_READ|PROT_WRITE) then mprotect(+PROT_EXEC) - W^X flip
 *    C  sceKernelJitCreateSharedMemory + a writable alias    - the "blessed"
 *       (RX handle) mapped RX, bytes written through the RW    JIT path
 *       alias, then executed through the RX mapping
 *
 *  A mapping call can succeed yet the CPU still fault on execute if W^X is
 *  enforced at fault time, so each call runs under a SIGSEGV/SIGBUS/SIGILL
 *  handler that siglongjmp()s back: a fault is reported as "faulted", never a
 *  crash. Every result is logged over klog. Gated by the kodi-jitprobe switch.
 *
 *  The Sce JIT symbols are declared weak: if the title's libkernel does not
 *  export them the pointers are null and route C reports "unavailable" instead
 *  of failing the link.
 */

#include <cstdarg>
#include <cstdio>
#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>

#include "platform/ps5/JitProbe.h"

// Log straight to klog, exactly as main.cpp's markers do. Self-contained so
// this file has no cross-TU link dependency (main.cpp's Klog has internal
// linkage). sceKernelDebugOutText is the title's only reliable early sink.
extern "C" void sceKernelDebugOutText(int channel, const char* text);

namespace
{
void Klog(const char* text)
{
  sceKernelDebugOutText(0, text);
}
void Klogf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Klogf(const char* fmt, ...)
{
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Klog(buf);
}
} // namespace

// Sce JIT API. Signatures per the PS5 homebrew SDK; declared weak so an
// absent symbol leaves the pointer null rather than breaking the link.
extern "C" __attribute__((weak)) int sceKernelJitCreateSharedMemory(const char* name,
                                                                    std::size_t len,
                                                                    int maxProt,
                                                                    int* fd);
extern "C" __attribute__((weak)) int sceKernelJitCreateAliasOfSharedMemory(int fd,
                                                                           int prot,
                                                                           int* aliasFd);
extern "C" __attribute__((weak)) int sceKernelJitMapSharedMemory(int fd,
                                                                 int prot,
                                                                 void** startOut,
                                                                 void* start,
                                                                 std::size_t len);

namespace
{

// mov eax, 0x1337 ; ret  -> returns 0x1337 when called as int(void).
const unsigned char kStub[] = {0xB8, 0x37, 0x13, 0x00, 0x00, 0xC3};
constexpr int kStubResult = 0x1337;
using Fn = int (*)();

sigjmp_buf g_jmp;
volatile sig_atomic_t g_faulted = 0;

void FaultHandler(int)
{
  g_faulted = 1;
  siglongjmp(g_jmp, 1);
}

// Call fn under fault protection. Returns true and sets *result on a clean
// call; returns false if the call faulted (page not really executable).
bool CallGuarded(Fn fn, int* result)
{
  struct sigaction seg{}, bus{}, ill{}, oseg{}, obus{}, oill{};
  seg.sa_handler = &FaultHandler;
  bus.sa_handler = &FaultHandler;
  ill.sa_handler = &FaultHandler;
  sigaction(SIGSEGV, &seg, &oseg);
  sigaction(SIGBUS, &bus, &obus);
  sigaction(SIGILL, &ill, &oill);

  bool ok = false;
  g_faulted = 0;
  if (sigsetjmp(g_jmp, 1) == 0)
  {
    int r = fn();
    ok = (r == kStubResult);
    if (result)
      *result = r;
  }

  sigaction(SIGSEGV, &oseg, nullptr);
  sigaction(SIGBUS, &obus, nullptr);
  sigaction(SIGILL, &oill, nullptr);
  return ok;
}

std::size_t PageSize()
{
  long p = sysconf(_SC_PAGESIZE);
  return p > 0 ? static_cast<std::size_t>(p) : 0x4000; // PS5: 16 KiB
}

void RouteA_RWX()
{
  const std::size_t len = PageSize();
  void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED)
  {
    Klogf("[kodi-ps5] jitprobe A (mmap RWX): mmap failed, errno %d (%s)\n", errno, std::strerror(errno));
    return;
  }
  std::memcpy(p, kStub, sizeof kStub);
  int r = 0;
  const bool ok = CallGuarded(reinterpret_cast<Fn>(p), &r);
  if (ok)
    Klog("[kodi-ps5] jitprobe A (mmap RWX): EXECUTED OK\n");
  else if (g_faulted)
    Klog("[kodi-ps5] jitprobe A (mmap RWX): mapped, but FAULTED on execute (W^X enforced)\n");
  else
    Klogf("[kodi-ps5] jitprobe A (mmap RWX): ran but wrong result 0x%x\n", r);
  munmap(p, len);
}

void RouteB_Mprotect()
{
  const std::size_t len = PageSize();
  void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED)
  {
    Klogf("[kodi-ps5] jitprobe B (mprotect +X): mmap RW failed, errno %d (%s)\n", errno, std::strerror(errno));
    return;
  }
  std::memcpy(p, kStub, sizeof kStub);
  if (mprotect(p, len, PROT_READ | PROT_EXEC) != 0)
  {
    Klogf("[kodi-ps5] jitprobe B (mprotect +X): mprotect failed, errno %d (%s)\n", errno, std::strerror(errno));
    munmap(p, len);
    return;
  }
  int r = 0;
  const bool ok = CallGuarded(reinterpret_cast<Fn>(p), &r);
  if (ok)
    Klog("[kodi-ps5] jitprobe B (mprotect +X): EXECUTED OK\n");
  else if (g_faulted)
    Klog("[kodi-ps5] jitprobe B (mprotect +X): mprotect returned 0, but FAULTED on execute\n");
  else
    Klogf("[kodi-ps5] jitprobe B (mprotect +X): ran but wrong result 0x%x\n", r);
  munmap(p, len);
}

void RouteC_Jit()
{
  if (!sceKernelJitCreateSharedMemory || !sceKernelJitMapSharedMemory)
  {
    Klog("[kodi-ps5] jitprobe C (SceJit): unavailable (symbols not exported to this title)\n");
    return;
  }
  const std::size_t len = PageSize();
  int rxFd = -1;
  // maxProt RX (0x05 = PROT_READ|PROT_EXEC).
  int rc = sceKernelJitCreateSharedMemory("", len, PROT_READ | PROT_EXEC, &rxFd);
  if (rc != 0 || rxFd < 0)
  {
    Klogf("[kodi-ps5] jitprobe C (SceJit): JitCreateSharedMemory rc 0x%x fd %d\n", rc, rxFd);
    return;
  }
  // Writable alias to populate the code (the RX handle itself is not writable).
  void* writable = nullptr;
  int rwFd = -1;
  bool haveAlias = false;
  if (sceKernelJitCreateAliasOfSharedMemory)
  {
    rc = sceKernelJitCreateAliasOfSharedMemory(rxFd, PROT_READ | PROT_WRITE, &rwFd);
    if (rc == 0 && rwFd >= 0)
    {
      rc = sceKernelJitMapSharedMemory(rwFd, PROT_READ | PROT_WRITE, &writable, nullptr, len);
      haveAlias = (rc == 0 && writable);
    }
  }
  if (!haveAlias)
  {
    Klogf("[kodi-ps5] jitprobe C (SceJit): could not map a writable alias (rc 0x%x); cannot populate\n", rc);
    return;
  }
  std::memcpy(writable, kStub, sizeof kStub);

  void* exec = nullptr;
  rc = sceKernelJitMapSharedMemory(rxFd, PROT_READ | PROT_EXEC, &exec, nullptr, len);
  if (rc != 0 || !exec)
  {
    Klogf("[kodi-ps5] jitprobe C (SceJit): map RX failed rc 0x%x\n", rc);
    munmap(writable, len);
    return;
  }
  int r = 0;
  const bool ok = CallGuarded(reinterpret_cast<Fn>(exec), &r);
  if (ok)
    Klog("[kodi-ps5] jitprobe C (SceJit): EXECUTED OK  <-- in-process code loading is possible\n");
  else if (g_faulted)
    Klog("[kodi-ps5] jitprobe C (SceJit): mapped RX, but FAULTED on execute\n");
  else
    Klogf("[kodi-ps5] jitprobe C (SceJit): ran but wrong result 0x%x\n", r);
  munmap(exec, len);
  munmap(writable, len);
}

} // namespace

void XBMC_PS5_RunJitProbe()
{
  Klog("[kodi-ps5] jitprobe: testing whether this title can execute its own memory\n");
  RouteA_RWX();
  RouteB_Mprotect();
  RouteC_Jit();
  Klog("[kodi-ps5] jitprobe: done. Any 'EXECUTED OK' means an in-process loader is feasible;\n");
  Klog("[kodi-ps5] jitprobe: all 'faulted/failed/unavailable' means binary add-ons need static linking.\n");
}
