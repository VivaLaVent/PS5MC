/*
 *  LibraryLoader backed by the in-process ELF loader (ps5elf), for binary
 *  add-ons on PS5. A title has no system dynamic loader, so the stock
 *  SoLoader (dlopen) cannot load an add-on's shared object. This maps and
 *  relocates the .so in-process instead, resolving its C/C++ runtime imports
 *  against the eboot via HostExports, and exposes its ADDON_* entry points
 *  through ResolveExport - so CAddonDll/DllAddon work unchanged.
 */
#pragma once

#include "cores/DllLoader/LibraryLoader.h"

namespace ps5elf { struct Image; }

class CPS5AddonLoader : public LibraryLoader
{
public:
  explicit CPS5AddonLoader(const std::string& so);
  ~CPS5AddonLoader() override;

  bool Load() override;
  void Unload() override;
  int ResolveExport(const char* symbol, void** ptr, bool logging = true) override;
  bool IsSystemDll() override;
  HMODULE GetHModule() override;
  bool HasSymbols() override;

private:
  ps5elf::Image* m_img = nullptr;
  bool m_loaded = false;
};
