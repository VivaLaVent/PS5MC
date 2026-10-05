#include "platform/ps5/elf/ElfLoader.h"

#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

// ELF64 (x86-64) types and constants, defined here so the file is identical on
// the Linux host and the PS5 (FreeBSD) sysroot, whose <elf.h> differ.
namespace
{
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

struct Ehdr { u8 e_ident[16]; u16 e_type, e_machine; u32 e_version; u64 e_entry, e_phoff, e_shoff;
              u32 e_flags; u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; };
struct Phdr { u32 p_type, p_flags; u64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; };
struct Dyn  { i64 d_tag; u64 d_val; };
struct Sym  { u32 st_name; u8 st_info, st_other; u16 st_shndx; u64 st_value, st_size; };
struct Rela { u64 r_offset; u64 r_info; i64 r_addend; };

constexpr u16 ET_DYN = 3;
constexpr u16 EM_X86_64 = 62;
constexpr u32 PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, PT_GNU_EH_FRAME = 0x6474e550;
constexpr u32 PF_X = 1, PF_W = 2, PF_R = 4;

constexpr i64 DT_NULL=0, DT_HASH=4, DT_STRTAB=5, DT_SYMTAB=6, DT_RELA=7, DT_RELASZ=8,
              DT_RELAENT=9, DT_SYMENT=11, DT_INIT=12, DT_PLTGOT=3, DT_PLTRELSZ=2,
              DT_JMPREL=23, DT_INIT_ARRAY=25, DT_INIT_ARRAYSZ=27, DT_GNU_HASH=0x6ffffef5;

constexpr u16 SHN_UNDEF = 0;
constexpr u8 STB_WEAK = 2;

// x86-64 relocation types (ABI-fixed).
constexpr u32 R_X86_64_64=1, R_X86_64_GLOB_DAT=6, R_X86_64_JUMP_SLOT=7, R_X86_64_RELATIVE=8,
              R_X86_64_DTPMOD64=16, R_X86_64_DTPOFF64=17, R_X86_64_TPOFF64=18;

inline u32 R_SYM(u64 i) { return static_cast<u32>(i >> 32); }
inline u32 R_TYPE(u64 i) { return static_cast<u32>(i & 0xffffffff); }

size_t page() { long p = sysconf(_SC_PAGESIZE); return p > 0 ? static_cast<size_t>(p) : 0x4000; }
u64 trunc_page(u64 x, size_t pg) { return x & ~static_cast<u64>(pg - 1); }
u64 round_page(u64 x, size_t pg) { return (x + pg - 1) & ~static_cast<u64>(pg - 1); }
} // namespace

// Progress trace to the kernel log. Deliberately NOT CLog: a page fault while
// loading kills the process before CLog flushes, and three rounds of guards
// here produced no output at all for that reason. sceKernelDebugOutText is
// unbuffered, so a line written before a step survives a crash in that step.
extern "C" int sceKernelDebugOutText(int channel, const char* text);
namespace
{
// Per-symbol tracing fires for every relocation (hundreds per add-on): only
// with the kodi-debug switch, which main.cpp exports as KODI_PS5_DEBUG. Reading
// an env var keeps this independent of where the switch file lives.
bool verbose()
{
  static const bool v = std::getenv("KODI_PS5_DEBUG") != nullptr;
  return v;
}

void klog(const char* fmt, ...)
{
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = std::vsnprintf(buf, sizeof buf - 2, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > static_cast<int>(sizeof buf) - 2) n = sizeof buf - 2;
  buf[n] = '\n'; buf[n + 1] = 0;
  sceKernelDebugOutText(0, buf);
}
} // namespace

namespace ps5elf
{

struct Image
{
  u8* base = nullptr;      // load bias (mapping - trunc(min_vaddr))
  u8* map = nullptr;       // actual mmap start
  size_t span = 0;         // mmap length
  const Phdr* phdr = nullptr;
  u16 phnum = 0;
  const Sym* symtab = nullptr;
  const char* strtab = nullptr;
  u32 symcount = 0;      // from DT_HASH nchain / GNU hash; 0 = unknown
  size_t strsz = 0;      // DT_STRSZ, to bound st_name
  void (*init)() = nullptr;
  void (**init_array)() = nullptr;
  size_t init_arrayn = 0;
  HostResolver resolver = nullptr;
  void* user = nullptr;
  u8* eh_frame_hdr = nullptr; // PT_GNU_EH_FRAME (.eh_frame_hdr), for the unwinder
  bool eh_registered = false;
  Stats st{};
};

namespace
{
u32 gnu_hash_symcount(const u32* gh)
{
  // Derive the highest symbol index + 1 from a GNU hash table.
  const u32 nbuckets = gh[0];
  const u32 symoffset = gh[1];
  const u32 bloom_size = gh[2];
  const u32* buckets = gh + 4 + bloom_size * 2; // 64-bit bloom words
  const u32* chain = buckets + nbuckets;
  u32 last = 0;
  for (u32 i = 0; i < nbuckets; ++i)
    if (buckets[i] > last)
      last = buckets[i];
  if (last < symoffset)
    return symoffset;
  // walk the chain of the highest bucket to its terminator (low bit set)
  u32 idx = last;
  while (!(chain[idx - symoffset] & 1))
    ++idx;
  return idx + 1;
}

// Resolve the symbol a relocation refers to. Returns false on hard failure.
bool resolve(Image* img, u32 symidx, u64* out, char* err, size_t errlen)
{
  // A wrong relocation count or entry size would walk past .rela.dyn and hand
  // us a nonsense index; dereferencing it faults in unmapped memory. Fail with
  // a message instead (seen on console: page fault inside resolve()).
  if (!img->symtab || !img->strtab || img->symcount == 0 || symidx >= img->symcount)
  {
    std::snprintf(err, errlen,
                  "bad symbol index %u (symbol table has %u entries%s)", symidx, img->symcount,
                  img->symcount == 0 ? ": DT_HASH/DT_GNU_HASH missing or unparsed" : "");
    return false;
  }
  // Trace before touching the table: if THIS faults, the klog line names
  // the index and pointer, which is what three silent crashes never told us.
  if (verbose())
    klog("[ps5elf] resolve sym %u of %u (entry %p)", symidx, img->symcount,
         static_cast<const void*>(&img->symtab[symidx]));
  const Sym& s = img->symtab[symidx];
  if (s.st_shndx != SHN_UNDEF)
  {
    *out = reinterpret_cast<u64>(img->base) + s.st_value; // defined here
    return true;
  }
  if (img->strsz && s.st_name >= img->strsz)
  {
    std::snprintf(err, errlen, "symbol %u: name offset %u past the string table (%zu bytes)",
                  symidx, s.st_name, img->strsz);
    return false;
  }
  const char* name = img->strtab + s.st_name;
  if (verbose())
    klog("[ps5elf]   -> name @%p (st_name %u, shndx %u, value %#llx)",
         static_cast<const void*>(name), s.st_name, s.st_shndx,
         static_cast<unsigned long long>(s.st_value));
  void* h = img->resolver ? img->resolver(name, img->user) : nullptr;
  if (h)
  {
    *out = reinterpret_cast<u64>(h);
    return true;
  }
  if ((s.st_info >> 4) == STB_WEAK)
  {
    *out = 0; // unresolved weak -> 0, legal
    return true;
  }
  std::snprintf(err, errlen, "undefined symbol: %s", name);
  return false;
}

bool apply_rela(Image* img, const Rela* r, size_t n, char* err, size_t errlen)
{
  const u64 bias = reinterpret_cast<u64>(img->base);
  for (size_t i = 0; i < n; ++i)
  {
    const u32 type = R_TYPE(r[i].r_info);
    const u32 sym = R_SYM(r[i].r_info);
    u64* where = reinterpret_cast<u64*>(bias + r[i].r_offset);
    switch (type)
    {
      case R_X86_64_RELATIVE:
        *where = bias + static_cast<u64>(r[i].r_addend);
        img->st.relative++;
        break;
      case R_X86_64_64:
      case R_X86_64_GLOB_DAT:
      {
        u64 v = 0;
        if (!resolve(img, sym, &v, err, errlen))
          return false;
        *where = v + static_cast<u64>(r[i].r_addend);
        img->st.glob_dat++;
        break;
      }
      case R_X86_64_JUMP_SLOT:
      {
        u64 v = 0;
        if (!resolve(img, sym, &v, err, errlen))
          return false;
        *where = v;
        img->st.jump_slot++;
        break;
      }
      case R_X86_64_DTPMOD64:
      case R_X86_64_DTPOFF64:
      case R_X86_64_TPOFF64:
        img->st.tls++; // not yet supported; left as-is, reported by stats
        break;
      default:
        img->st.other++;
        break;
    }
  }
  return true;
}
} // namespace

Image* load(const void* image, size_t image_len, HostResolver resolver, void* user,
            char* err, size_t errlen)
{
  auto fail = [&](const char* m) -> Image* {
    std::snprintf(err, errlen, "%s", m);
    klog("[ps5elf] load failed: %s", m);
    return nullptr;
  };
  if (image_len < sizeof(Ehdr))
    return fail("image too small");
  const auto* e = static_cast<const Ehdr*>(image);
  if (std::memcmp(e->e_ident, "\x7f""ELF", 4) != 0)
    return fail("not an ELF file");
  if (e->e_ident[4] != 2 /*ELFCLASS64*/ || e->e_ident[5] != 1 /*little-endian*/)
    return fail("not ELF64 LE");
  if (e->e_type != ET_DYN)
    return fail("not a shared object (ET_DYN)");
  if (e->e_machine != EM_X86_64)
    return fail("not x86-64");

  const size_t pg = page();
  const auto* ph = reinterpret_cast<const Phdr*>(static_cast<const u8*>(image) + e->e_phoff);

  // span of all PT_LOAD
  u64 min_v = ~0ull, max_v = 0;
  const Phdr* dynph = nullptr;
  bool has_tls = false;
  u64 eh_frame_hdr_v = 0; // PT_GNU_EH_FRAME vaddr; resolved to a pointer after base is known
  for (u16 i = 0; i < e->e_phnum; ++i)
  {
    if (ph[i].p_type == PT_LOAD)
    {
      min_v = ph[i].p_vaddr < min_v ? ph[i].p_vaddr : min_v;
      u64 end = ph[i].p_vaddr + ph[i].p_memsz;
      max_v = end > max_v ? end : max_v;
    }
    else if (ph[i].p_type == PT_GNU_EH_FRAME)
      eh_frame_hdr_v = ph[i].p_vaddr;
    else if (ph[i].p_type == PT_DYNAMIC)
      dynph = &ph[i];
    else if (ph[i].p_type == PT_TLS)
      has_tls = true;
  }
  if (min_v == ~0ull || !dynph)
    return fail("no PT_LOAD or no PT_DYNAMIC");

  const u64 base_v = trunc_page(min_v, pg);
  const size_t span = round_page(max_v, pg) - base_v;
  void* m = mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED)
    return fail("reservation mmap failed");

  auto* img = new Image();
  img->map = static_cast<u8*>(m);
  img->span = span;
  img->base = static_cast<u8*>(m) - base_v; // load bias
  if (eh_frame_hdr_v) img->eh_frame_hdr = img->base + eh_frame_hdr_v;
  img->resolver = resolver;
  img->user = user;
  (void)has_tls;

  // Copy each PT_LOAD in, RW (route B: write first, flip to X later).
  for (u16 i = 0; i < e->e_phnum; ++i)
  {
    if (ph[i].p_type != PT_LOAD)
      continue;
    u8* seg = img->base + ph[i].p_vaddr;
    u8* segpg = reinterpret_cast<u8*>(trunc_page(reinterpret_cast<u64>(seg), pg));
    size_t seglen = round_page(reinterpret_cast<u64>(seg) + ph[i].p_memsz, pg)
                    - reinterpret_cast<u64>(segpg);
    if (mprotect(segpg, seglen, PROT_READ | PROT_WRITE) != 0)
    {
      std::snprintf(err, errlen, "mprotect RW failed: %s", std::strerror(errno));
      munmap(m, span);
      delete img;
      return nullptr;
    }
    std::memcpy(seg, static_cast<const u8*>(image) + ph[i].p_offset, ph[i].p_filesz);
    if (ph[i].p_memsz > ph[i].p_filesz) // .bss
      std::memset(seg + ph[i].p_filesz, 0, ph[i].p_memsz - ph[i].p_filesz);
  }

  // Parse PT_DYNAMIC.
  const auto* dyn = reinterpret_cast<const Dyn*>(img->base + dynph->p_vaddr);
  const Rela* rela = nullptr; size_t relasz = 0;
  const Rela* jmprel = nullptr; size_t pltrelsz = 0;
  const u32* sysv_hash = nullptr; const u32* gnu_hash = nullptr;
  for (const Dyn* d = dyn; d->d_tag != DT_NULL; ++d)
  {
    switch (d->d_tag)
    {
      case DT_SYMTAB: img->symtab = reinterpret_cast<const Sym*>(img->base + d->d_val); break;
      case 10: img->strsz = static_cast<size_t>(d->d_val); break; // DT_STRSZ
      case DT_STRTAB: img->strtab = reinterpret_cast<const char*>(img->base + d->d_val); break;
      case DT_RELA:   rela = reinterpret_cast<const Rela*>(img->base + d->d_val); break;
      case DT_RELASZ: relasz = d->d_val; break;
      case DT_JMPREL: jmprel = reinterpret_cast<const Rela*>(img->base + d->d_val); break;
      case DT_PLTRELSZ: pltrelsz = d->d_val; break;
      case DT_HASH:   sysv_hash = reinterpret_cast<const u32*>(img->base + d->d_val); break;
      case DT_GNU_HASH: gnu_hash = reinterpret_cast<const u32*>(img->base + d->d_val); break;
      case DT_INIT:   img->init = reinterpret_cast<void(*)()>(img->base + d->d_val); break;
      case DT_INIT_ARRAY: img->init_array = reinterpret_cast<void(**)()>(img->base + d->d_val); break;
      case DT_INIT_ARRAYSZ: img->init_arrayn = d->d_val / sizeof(void*); break;
      default: break;
    }
  }
  // Every table pointer must land inside the mapping we just made. A d_val the
  // image did not intend (or a base we computed wrong) otherwise faults on the
  // first dereference in resolve(); seen on console as a page fault reading a
  // fixed address. Report the numbers so the cause is visible, then fail.
  {
    const u8* lo = img->map;
    const u8* hi = img->map + img->span;
    auto inside = [&](const void* p) {
      const u8* q = static_cast<const u8*>(p);
      return q >= lo && q < hi;
    };
    if ((img->symtab && !inside(img->symtab)) || (img->strtab && !inside(img->strtab)))
    {
      std::snprintf(err, errlen,
                    "dynamic tables outside the mapping: symtab %p strtab %p, "
                    "mapped %p..%p (base %p)",
                    static_cast<const void*>(img->symtab), static_cast<const void*>(img->strtab),
                    static_cast<const void*>(lo), static_cast<const void*>(hi),
                    static_cast<const void*>(img->base));
      munmap(m, span);
      delete img;
      return nullptr;
    }
  }
  if (!img->symtab || !img->strtab)
  {
    munmap(m, span); delete img; return fail("missing DT_SYMTAB/DT_STRTAB");
  }
  if (sysv_hash)
    img->symcount = sysv_hash[1]; // nchain
  else if (gnu_hash)
    img->symcount = gnu_hash_symcount(gnu_hash);
  // symcount only needed by symbol(); relocations index directly.

  // Apply relocations.
  klog("[ps5elf] parsed: %u dyn syms, %zu rela, %zu jmprel, strsz %zu, map %p..%p, base %p, "
       "symtab %p, strtab %p",
       img->symcount, relasz / sizeof(Rela), pltrelsz / sizeof(Rela), img->strsz,
       static_cast<void*>(img->map), static_cast<void*>(img->map + img->span),
       static_cast<void*>(img->base), static_cast<const void*>(img->symtab),
       static_cast<const void*>(img->strtab));

  // Refuse up front when the symbol count is unknown: resolve() would
  // otherwise index the symbol table unchecked (page fault seen on console).
  if (img->symcount == 0 && (relasz || pltrelsz))
  {
    std::snprintf(err, errlen,
                  "no usable symbol count (DT_HASH/DT_GNU_HASH missing); %zu rela + %zu jmprel "
                  "relocations need symbol lookups",
                  relasz / sizeof(Rela), pltrelsz / sizeof(Rela));
    munmap(m, span);
    delete img;
    return nullptr;
  }
  klog("[ps5elf] applying %zu .rela.dyn relocations", relasz / sizeof(Rela));
  if (rela && !apply_rela(img, rela, relasz / sizeof(Rela), err, errlen))
  {
    munmap(m, span); delete img; return nullptr;
  }
  klog("[ps5elf] applying %zu .rela.plt relocations", pltrelsz / sizeof(Rela));
  if (jmprel && !apply_rela(img, jmprel, pltrelsz / sizeof(Rela), err, errlen))
  {
    munmap(m, span); delete img; return nullptr;
  }

  // Final protections per p_flags (route B flip: code becomes R+X now).
  for (u16 i = 0; i < e->e_phnum; ++i)
  {
    if (ph[i].p_type != PT_LOAD)
      continue;
    int prot = ((ph[i].p_flags & PF_R) ? PROT_READ : 0) |
               ((ph[i].p_flags & PF_W) ? PROT_WRITE : 0) |
               ((ph[i].p_flags & PF_X) ? PROT_EXEC : 0);
    u8* seg = img->base + ph[i].p_vaddr;
    u8* segpg = reinterpret_cast<u8*>(trunc_page(reinterpret_cast<u64>(seg), pg));
    size_t seglen = round_page(reinterpret_cast<u64>(seg) + ph[i].p_memsz, pg)
                    - reinterpret_cast<u64>(segpg);
    if (mprotect(segpg, seglen, prot) != 0)
    {
      std::snprintf(err, errlen, "final mprotect failed: %s", std::strerror(errno));
      munmap(m, span); delete img; return nullptr;
    }
  }
  return img;
}

// libgcc's unwinder keeps a registry of .eh_frame sections. A JIT-mapped
// object is invisible to it until registered, so a C++ exception thrown inside
// a binary add-on would otherwise reach std::terminate instead of its own
// catch. __register_frame takes the .eh_frame; we locate it from the
// PT_GNU_EH_FRAME header (.eh_frame_hdr), whose 4-byte prologue is followed by
// an encoded pointer to .eh_frame.
extern "C" void __register_frame(const void*) __attribute__((weak));
extern "C" void __deregister_frame(const void*) __attribute__((weak));

namespace
{
// Decode the eh_frame_hdr's eh_frame_ptr (DW_EH_PE encoding in byte 1).
// ISA's toolchain emits pcrel|sdata4 (0x1b), the GNU default; handle that and
// the absolute encodings, and give up (no registration) on anything else.
u8* eh_frame_from_hdr(u8* hdr)
{
  if (!hdr) return nullptr;
  const u8 eh_frame_ptr_enc = hdr[1];
  const u8* p = hdr + 4; // version(1) + eh_frame_ptr_enc(1) + fde_count_enc(1) + table_enc(1)
  const u8 app = eh_frame_ptr_enc & 0x70;
  const u8 fmt = eh_frame_ptr_enc & 0x0f;
  long long val = 0;
  const u8* vp = p;
  switch (fmt)
  {
    case 0x03: { int32_t v; std::memcpy(&v, vp, 4); val = v; break; }           // sdata4
    case 0x0b: { uint32_t v; std::memcpy(&v, vp, 4); val = (int32_t)v; break; } // udata4 (treat signed)
    case 0x01: { uintptr_t v; std::memcpy(&v, vp, sizeof v); val = (long long)v; break; } // uleb-ish/abs ptr
    default: return nullptr;
  }
  if (app == 0x10) // DW_EH_PE_pcrel: relative to the location of eh_frame_ptr
    return reinterpret_cast<u8*>(reinterpret_cast<uintptr_t>(p) + (intptr_t)val);
  if (app == 0x00) // absolute
    return reinterpret_cast<u8*>((uintptr_t)val);
  return nullptr;
}
} // namespace

void run_init(Image* img)
{
  if (!img) return;
  if (img->eh_frame_hdr && __register_frame && !img->eh_registered)
  {
    if (u8* eh = eh_frame_from_hdr(img->eh_frame_hdr))
    {
      __register_frame(eh);
      img->eh_registered = true;
    }
  }
  if (img->init) img->init();
  for (size_t i = 0; i < img->init_arrayn; ++i)
    if (img->init_array[i]) img->init_array[i]();
}

void* symbol(Image* img, const char* name)
{
  if (!img || !img->symtab || !img->strtab) return nullptr;
  for (u32 i = 0; i < img->symcount; ++i)
  {
    const Sym& s = img->symtab[i];
    if (s.st_shndx != SHN_UNDEF && std::strcmp(img->strtab + s.st_name, name) == 0)
      return img->base + s.st_value;
  }
  return nullptr;
}

Stats stats(const Image* img) { return img ? img->st : Stats{}; }

static void deregister_eh(Image* img)
{
  if (img && img->eh_registered && __deregister_frame)
  {
    if (u8* eh = eh_frame_from_hdr(img->eh_frame_hdr))
      __deregister_frame(eh);
    img->eh_registered = false;
  }
}

void unload(Image* img)
{
  deregister_eh(img);
  if (!img) return;
  if (img->map) munmap(img->map, img->span);
  delete img;
}

} // namespace ps5elf
