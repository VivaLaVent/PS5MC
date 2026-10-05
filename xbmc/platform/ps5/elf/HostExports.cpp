#include "platform/ps5/elf/HostExports.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <elf.h>

// The SDK's <link.h> does not define glibc's ElfW() macro, and _DYNAMIC
// may not be declared. The target is always ELF64; use the concrete types
// and declare _DYNAMIC ourselves (every PIE/-shared image defines it).
extern "C" Elf64_Dyn _DYNAMIC[];

// The SDK's <elf.h> spells the PLT relocation R_X86_64_JUMP_SLO (no trailing T)
// and GLOB_DAT may vary too; pin both to their ABI values regardless.
#ifndef R_X86_64_JUMP_SLOT
#define R_X86_64_JUMP_SLOT 7
#endif
#ifndef R_X86_64_GLOB_DAT
#define R_X86_64_GLOB_DAT 6
#endif

// Resolve a binary add-on's undefined C/C++ runtime imports (memcpy, malloc,
// pthread_*, __cxa_atexit, _Unwind_*, ...) against the SAME definitions the
// eboot uses. On this platform those symbols are not defined in the eboot -
// they live in the clean-room libc.prx and are bound at load time - so the
// eboot's .dynsym lists them UND (address 0). The real addresses are in the
// eboot's GOT: every such symbol has an R_X86_64_GLOB_DAT (and often a
// JUMP_SLOT) relocation whose r_offset is a GOT slot the loader fills with the
// libc.prx address at startup. So: walk the eboot's own relocations, match the
// symbol by name through .dynsym/.dynstr, and return the CONTENTS of its GOT
// slot. The add-on then calls the identical libc.prx function the eboot does.
//
// Nothing is added to the link (the FSELF converter crashes on any added data
// object), and no runtime loader/dlsym is needed - we read the eboot's own
// relocated GOT, which the kernel has already populated.

namespace
{
struct Self
{
  const Elf64_Sym* symtab = nullptr;
  const char* strtab = nullptr;
  const Elf64_Rela* rela = nullptr;   // DT_RELA (.rela.dyn)
  size_t rela_n = 0;
  const Elf64_Rela* jmprel = nullptr; // DT_JMPREL (.rela.plt)
  size_t jmprel_n = 0;
  uintptr_t base = 0;
  bool ready = false;
};
Self g;

uintptr_t page_down(uintptr_t a) { return a & ~uintptr_t(0xfff); }

const Elf64_Ehdr* find_ehdr(uintptr_t from)
{
  for (uintptr_t p = page_down(from); p; p -= 0x1000)
  {
    const auto* m = reinterpret_cast<const unsigned char*>(p);
    if (m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F')
      return reinterpret_cast<const Elf64_Ehdr*>(p);
    if (from - p > (256u << 20))
      break;
  }
  return nullptr;
}

void init()
{
  if (g.ready)
    return;
  g.ready = true;

  const uintptr_t dyn_run = reinterpret_cast<uintptr_t>(&_DYNAMIC[0]);
  const Elf64_Ehdr* eh = find_ehdr(dyn_run);
  if (!eh)
    return;
  const uintptr_t imgbase = reinterpret_cast<uintptr_t>(eh);
  const Elf64_Phdr* ph = reinterpret_cast<const Elf64_Phdr*>(imgbase + eh->e_phoff);
  uintptr_t dyn_vaddr = 0;
  for (unsigned i = 0; i < eh->e_phnum; ++i)
    if (ph[i].p_type == PT_DYNAMIC)
    {
      dyn_vaddr = ph[i].p_vaddr;
      break;
    }
  const uintptr_t bias = dyn_vaddr ? dyn_run - dyn_vaddr : imgbase;
  g.base = bias;

  // DT_* d_ptr is link-time on a static eboot; some runtimes pre-relocate it.
  auto fix = [&](uintptr_t p) -> uintptr_t {
    return (p >= imgbase && p < imgbase + (uintptr_t(1) << 33)) ? p : bias + p;
  };
  size_t relasz = 0, relaent = sizeof(Elf64_Rela);
  size_t pltrelsz = 0;
  for (const Elf64_Dyn* d = _DYNAMIC; d->d_tag != DT_NULL; ++d)
  {
    switch (d->d_tag)
    {
      case DT_SYMTAB:   g.symtab = reinterpret_cast<const Elf64_Sym*>(fix(d->d_un.d_ptr)); break;
      case DT_STRTAB:   g.strtab = reinterpret_cast<const char*>(fix(d->d_un.d_ptr)); break;
      case DT_RELA:     g.rela = reinterpret_cast<const Elf64_Rela*>(fix(d->d_un.d_ptr)); break;
      case DT_RELASZ:   relasz = d->d_un.d_val; break;
      case DT_RELAENT:  relaent = d->d_un.d_val; break;
      case DT_JMPREL:   g.jmprel = reinterpret_cast<const Elf64_Rela*>(fix(d->d_un.d_ptr)); break;
      case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
      default: break;
    }
  }
  if (relaent == 0)
    relaent = sizeof(Elf64_Rela);
  g.rela_n = relasz / relaent;
  g.jmprel_n = pltrelsz / relaent;
}

// If this relocation names `name`, return the resolved address from its GOT
// slot (the slot content, not its address). 0 if not a match / not filled.
void* from_rela(const Elf64_Rela* r, const char* name)
{
  const uint32_t type = ELF64_R_TYPE(r->r_info);
  if (type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT)
    return nullptr;
  const uint32_t sym = ELF64_R_SYM(r->r_info);
  if (std::strcmp(g.strtab + g.symtab[sym].st_name, name) != 0)
    return nullptr;
  void** slot = reinterpret_cast<void**>(g.base + r->r_offset);
  return *slot; // the loader filled this with the libc.prx address
}
} // namespace

void* host_export_resolver(const char* name, void*)
{
  init();
  if (!g.symtab || !g.strtab)
    return nullptr;
  // Prefer GLOB_DAT (.rela.dyn, always filled at load) then JUMP_SLOT.
  for (size_t i = 0; i < g.rela_n; ++i)
    if (void* a = from_rela(&g.rela[i], name))
      return a;
  for (size_t i = 0; i < g.jmprel_n; ++i)
    if (void* a = from_rela(&g.jmprel[i], name))
      return a;
  return nullptr;
}
