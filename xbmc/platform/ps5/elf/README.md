# In-process ELF loader (binary add-ons)

A PS5 title has no system dynamic loader, so binary add-ons (shared objects
Kodi normally `dlopen`s) cannot be loaded the usual way. The on-console
`kodi-jitprobe` established that a title *can* execute memory it writes if it
respects W^X: map a page RW, write code, `mprotect` it R+X, then call. (RWX in
one step faults; the Sce JIT path returned EINVAL. Route B - RW then +X - is
the one that works, and is what this loader uses.)

## Status

- [x] **Memory + relocation core** (`ElfLoader.{h,cpp}`). Maps `PT_LOAD`
      segments RW, applies `R_X86_64_RELATIVE`, `_64`, `_GLOB_DAT`,
      `_JUMP_SLOT`, resolves undefined symbols via a host callback, flips code
      to R+X, runs `DT_INIT`/`DT_INIT_ARRAY`. Verified on the host against a
      synthetic `.so` exercising every reloc kind, a data global, a
      host-imported function, and a constructor (all checks pass).
- [ ] **TLS relocations** (`DTPMOD64`/`DTPOFF64`/`TPOFF64`): counted and
      reported by `Stats`, not yet applied. Needed for add-ons that use
      `__thread`. Requires allocating a TLS block and wiring the module id.
- [x] **Host symbol table** (`HostExports.{h,cpp}` + `tools/ps5-gen-addon-exports.py`).
      Confirmed add-ons call the host through the `AddonGlobalInterface`
      function-pointer struct (resolved at `ADDON_Create`), *not* named imports
      - so the only named symbols an add-on needs from the eboot are the C/C++
      runtime + libc. The generator turns an add-on's undefined-symbol list into
      an **assembly** export table (asm can name mangled C++ symbols like
      `_Znwm`), linked into the eboot so addresses bind at link time - no
      `--export-dynamic` and no runtime dynsym query needed, and any missing
      symbol is a link error, not a runtime crash. Verified on the host: an
      add-on importing `operator new/delete` and `memcpy` binds them to the
      host's own instances (shared allocator) and runs correctly.
      `HostExports.cpp` ships a **weak, empty** `g_host_exports` so the eboot
      links and runs before any add-on table exists (Python needs none). The
      generated assembly table defines a **strong** `g_host_exports` that
      overrides it once a binary add-on is built.
- [x] **Wired into the loader factory** (`PS5AddonLoader.{h,cpp}` +
      `patches/kodi/0017-ps5-binary-addon-inprocess-loader.patch`).
      `CPS5AddonLoader` implements `LibraryLoader` over `ps5elf`; the
      `DllLoaderContainer` factory selects it on PS5 instead of the dlopen
      `SoLoader`, so `CAddonDll`/`DllAddon` reach it unchanged. It reads the
      add-on .so, loads+relocates it with the `HostExports` resolver, runs the
      constructors, and returns `ADDON_*` through `ResolveExport`. Compiles
      against the real Kodi interfaces; runtime-testable once an add-on .so is
      cross-built.
- [ ] **First target**: build `inputstream.adaptive` for the PS5 sysroot and
      load it through the chain end to end.

## Testing

The core is host-testable because ELF64/x86-64 relocation logic and
`mmap`+`mprotect` are identical on the build host and PS5. See the harness in
the project notes; it loads a synthetic `.so` and checks a computed result.

## Stage 1 (toolchain probe) + exception handling — DONE (host-verified)
`tools/ps5-isa-toolchain-probe.sh` builds a C++ .so with prospero-clang
(exceptions, RTTI, a vtable, a global constructor, operator new/delete) and
loads it through ps5elf. The loader now **registers the add-on's .eh_frame**
with libgcc's unwinder in run_init (__register_frame, from the PT_GNU_EH_FRAME
header), so a C++ exception thrown inside a loaded add-on reaches its own catch
instead of std::terminate — the first blocker stage 1 surfaced, since
inputstream.adaptive's JSON parser throws. Host harness result: relocations
all handled (relative/glob_dat/jump_slot), tls=0, other=0, throw/catch works,
constructor ran. __deregister_frame on unload.

Next: stage 2, a tiny binary add-on bundled in the title, to prove the
DllAddon seam and two-way calls on the console.

## Import resolution — self-dynsym (final design)
The generated export-table object was abandoned: the FSELF converter
(ps5-native-tool) segfaults on ANY added defined-data object in the eboot
(bisected - a 1 KB blob in the same link slot also crashed it; it was never
relocations, the symbol name, or a constructor). So nothing is added to the
link. Instead host_export_resolver (HostExports.cpp) reads the eboot's OWN
dynamic symbol table at run time - walk _DYNAMIC -> DT_SYMTAB/STRTAB, count via
DT_HASH nchain or the GNU-hash tail, linear-scan by name - and returns the
address of the eboot's own copy of each C/C++ runtime symbol the add-on
imports. No generated object, no new data, nothing for the converter to choke
on. DT pointers are handled whether link-time (static eboot) or pre-relocated.
Host-verified against a program's own dynsym; tools/ps5-gen-addon-exports.py
and host_exports.list are retired.

## Diagnostics: klog, not CLog (2026-10-05)
Three rounds of guards in `load()`/`resolve()` produced no output on the
console because every message went through CLog, which buffers; a page fault
right after kills the process before it flushes. The loader now writes an
unbuffered `sceKernelDebugOutText` trace BEFORE each risky step: parse summary
(dyn syms, rela/jmprel counts, strsz, map range, base, symtab/strtab), each
relocation pass, and (with the kodi-debug switch, via `KODI_PS5_DEBUG`) every
symbol resolve with its index and pointers. A crash now leaves the last line
naming the step. Host-verified: the trace prints and the probe still passes.
