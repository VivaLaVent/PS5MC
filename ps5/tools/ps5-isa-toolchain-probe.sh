#!/usr/bin/env bash
# Stage 1 of binary-add-on bring-up: prove our ELF loader handles a shared
# object built by the PS5 toolchain the way inputstream.adaptive is built,
# BEFORE anything runs on the console.
#
# It builds a .so with prospero-clang (C++, exceptions on, a vtable, a
# global constructor, a std::string and a throw/catch - the C++ features ISA
# uses), then loads it with the host build of ps5elf and checks:
#   - every relocation in the .so is a kind the loader applies (no "other"),
#   - the object needs no TLS relocations (the one kind not yet supported),
#   - its constructor ran and an exported function returns the right value,
#   - its list of undefined symbols (what the export table must provide).
#
# Output is a report; it does not touch the console. Run after scripts/00.
set -uo pipefail
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$HERE/scripts/lib/platform-src.sh"
CLANGXX="$PS5_PAYLOAD_SDK/bin/prospero-clang++"
WORK="${WORK:-$HOME/ps5-work}/isa-probe"
mkdir -p "$WORK"; cd "$WORK"

[ -x "$CLANGXX" ] || { echo "!! $CLANGXX not found (run scripts/00 first)"; exit 1; }

# --- a .so that exercises the C++ surface ISA relies on ----------------------
cat > probe.cpp <<'CPP'
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// a global constructor (DT_INIT_ARRAY) with a non-trivial type
static std::string g_tag;
struct Ctor { Ctor() { g_tag = "isa-probe"; } };
static Ctor g_ctor;

// a vtable (R_X86_64_RELATIVE into .data.rel.ro) + a virtual call
struct Base { virtual int value() const { return 1; } virtual ~Base() = default; };
struct Derived : Base { int value() const override { return 41; } };

extern "C" int probe_entry()
{
  // throw/catch across the .so's own frames (needs unwind tables)
  int extra = 0;
  try { throw std::runtime_error("x"); }
  catch (const std::exception&) { extra = 1; }

  std::vector<int> v;               // operator new/delete from the host
  for (int i = 0; i < 10; ++i) v.push_back(i);
  int sum = 0; for (int i : v) sum += i;   // 45

  Base* b = new Derived();          // vtable dispatch
  int r = b->value();               // 41
  delete b;

  // 41 + 45 + 1 - 46 == 41, and prove the constructor ran
  return r + sum + extra - 46 + (g_tag == "isa-probe" ? 5 : -100);
}
CPP

echo "==> building probe.so with prospero-clang++ (exceptions, PIC, no system loader)"
"$CLANGXX" -shared -fPIC -fexceptions -frtti -O2 -std=c++17 \
  -o probe.so probe.cpp 2>probe.build.log || { echo "!! build failed"; cat probe.build.log; exit 1; }

# Find the SDK's llvm binutils (names vary: prospero-llvm-nm, llvm-nm-NN, or
# the multicall prospero-llvm/llvm driver). Fall back to the host's.
find_tool() {
  local base="$1"; shift
  for c in "$PS5_PAYLOAD_SDK/bin/prospero-$base" "$PS5_PAYLOAD_SDK/bin/$base" \
           "$(command -v "$base" 2>/dev/null)" \
           "$(command -v "${base}-18" 2>/dev/null)" "$(command -v "${base}-17" 2>/dev/null)"; do
    [ -n "$c" ] && [ -x "$c" ] && { echo "$c"; return 0; }
  done
  return 1
}
NM="$(find_tool llvm-nm || true)"
READOBJ="$(find_tool llvm-readelf || find_tool readelf || true)"

echo "==> undefined (imported) symbols the export table must provide:"
if [ -n "$NM" ]; then
  "$NM" -D --undefined-only probe.so 2>/dev/null | awk '$1=="U"||$2=="U"{print "     " $NF}' | sort -u | tee undefined.txt
  echo "    ($(wc -l < undefined.txt) symbols, via $NM)"
else
  echo "     (no llvm-nm found; skipping)"; : > undefined.txt
fi

echo "==> relocation types present in probe.so:"
if [ -n "$READOBJ" ]; then
  "$READOBJ" -r probe.so 2>/dev/null | grep -oE 'R_X86_64_[A-Z0-9_]+' | sort | uniq -c | sed 's/^/     /'
else
  echo "     (no readelf found; skipping)"
fi

# --- host harness: load probe.so with ps5elf, resolve its imports, run it ----
cat > harness.cpp <<'CPP'
#include "ElfLoader.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>

// The host provides the C++ runtime the .so imports. For the probe these are
// the real host libstdc++/libc symbols; dlsym(RTLD_DEFAULT) reaches them,
// which stands in for the generated export table used on the console.
#include <dlfcn.h>
#include <cstdio>
#include <cerrno>
// BSD libc spellings glibc does not export; the add-on imports these and on the
// console they resolve from the eboot. For the host test, map them to glibc's
// equivalents so the probe completes. (A stand-in for the generated table.)
extern "C" { FILE* host__stderrp() { return stderr; } int* host__error() { return &errno; } }
static void* resolve(const char* name, void*)
{
  if (!std::strcmp(name, "__stderrp")) return (void*)host__stderrp();
  if (!std::strcmp(name, "__error"))   return (void*)host__error();
  return dlsym(RTLD_DEFAULT, name);
}

int main(int argc, char** argv)
{
  std::ifstream f(argv[1], std::ios::binary);
  std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  char err[256] = {};
  auto* img = ps5elf::load(buf.data(), buf.size(), resolve, nullptr, err, sizeof err);
  if (!img) { printf("LOAD FAILED: %s\n", err); return 2; }
  auto s = ps5elf::stats(img);
  printf("relocations: relative=%u glob_dat=%u jump_slot=%u tls=%u other=%u\n",
         s.relative, s.glob_dat, s.jump_slot, s.tls, s.other);
  ps5elf::run_init(img);
  auto fn = (int(*)())ps5elf::symbol(img, "probe_entry");
  if (!fn) { printf("no probe_entry\n"); return 3; }
  int r = fn();
  printf("probe_entry() = %d (expect 46)\n", r);
  int ok = (r == 46) && (s.other == 0) && (s.tls == 0);
  printf("%s\n", ok ? "STAGE 1 PASS" : "STAGE 1 FAIL");
  return ok ? 0 : 1;
}
CPP

echo "==> building the host harness against ps5elf"
# ElfLoader.cpp includes "platform/ps5/elf/ElfLoader.h", so compile from an
# include root that contains that path, and point the harness include at it too.
INCROOT="$WORK/inc"
mkdir -p "$INCROOT/platform/ps5/elf"
cp "$PLATFORM_SRC/xbmc/platform/ps5/elf/ElfLoader.h" "$INCROOT/platform/ps5/elf/"
sed -i 's|#include "ElfLoader.h"|#include "platform/ps5/elf/ElfLoader.h"|' harness.cpp
g++ -std=c++17 -O2 -I"$INCROOT" \
  harness.cpp "$PLATFORM_SRC/xbmc/platform/ps5/elf/ElfLoader.cpp" \
  -ldl -o harness 2>harness.build.log || { echo "!! harness build failed"; cat harness.build.log; exit 1; }

echo "==> loading probe.so through ps5elf:"
./harness probe.so
