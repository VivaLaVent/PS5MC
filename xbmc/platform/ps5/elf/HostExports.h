#pragma once
#include "platform/ps5/elf/ElfLoader.h"
// Resolver backed by the generated asm export table (HostExports.S, produced by
// tools/ps5-gen-addon-exports.py and linked into the eboot). Maps the C/C++
// runtime + libc symbol names a binary add-on imports to the eboot's own
// definitions, so add-on and host share one libc/allocator. Pass this as the
// HostResolver to ps5elf::load.
void* host_export_resolver(const char* name, void* user);
