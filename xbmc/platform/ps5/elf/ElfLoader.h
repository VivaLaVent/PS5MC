/*
 *  Minimal in-process loader for ELF64 shared objects (x86-64), the memory +
 *  relocation core for binary add-ons on PS5. It maps an add-on's PT_LOAD
 *  segments writable, applies relocations, resolves undefined symbols through
 *  a host callback, flips code pages to execute-only-after-write (the W^X
 *  route the on-console probe confirmed: map RW, write, mprotect RX), then
 *  runs the object's constructors. No dependency on a system dynamic loader,
 *  which a PS5 title does not have.
 *
 *  This header is deliberately free of <elf.h>: the same source builds on the
 *  Linux host (for tests) and the PS5 sysroot, whose elf headers differ.
 */
#pragma once
#include <cstddef>
#include <cstdint>

namespace ps5elf
{

// Resolve an undefined symbol by name to its address in the host (the running
// Kodi). Returns nullptr if unknown; the loader treats that as an error unless
// the reference is weak. `user` is passed through from elf_load.
using HostResolver = void* (*)(const char* name, void* user);

struct Image; // opaque

// Load a shared object from an in-memory image. Returns nullptr on failure and
// writes a reason into errbuf. Does not run constructors (call run_init).
Image* load(const void* image, size_t image_len, HostResolver resolver, void* user,
            char* errbuf, size_t errlen);

// Run DT_INIT then DT_INIT_ARRAY (C++ static constructors). Call once, after
// load, before using the object.
void run_init(Image* img);

// Address of an exported symbol by name, or nullptr.
void* symbol(Image* img, const char* name);

// Diagnostics: counts of relocation kinds applied (for the test harness/logs).
struct Stats
{
  unsigned relative;
  unsigned glob_dat;   // GLOB_DAT + R_X86_64_64
  unsigned jump_slot;
  unsigned tls;        // TLS relocs seen (not yet supported; reported)
  unsigned other;      // unhandled types seen
};
Stats stats(const Image* img);

void unload(Image* img);

} // namespace ps5elf
