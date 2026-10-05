#include "platform/ps5/elf/PS5AddonLoader.h"

#include "filesystem/File.h"
#include "platform/ps5/elf/ElfLoader.h"
#include "platform/ps5/elf/HostExports.h"
#include "utils/log.h"

#include <cstdio>
#include <vector>

extern "C" int sceKernelDebugOutText(int channel, const char* text);

CPS5AddonLoader::CPS5AddonLoader(const std::string& so) : LibraryLoader(so)
{
}

CPS5AddonLoader::~CPS5AddonLoader()
{
  Unload();
}

bool CPS5AddonLoader::Load()
{
  if (m_loaded)
    return true;

  // Read the whole .so into memory; the ELF loader maps from a buffer (it must
  // write and mprotect its own pages - it cannot use the file's mapping).
  const std::string path = GetFileName();
  XFILE::CFile file;
  std::vector<uint8_t> buf;
  if (file.LoadFile(path, buf) <= 0 || buf.empty()) // LoadFile returns bytes read, <0 on error
  {
    CLog::Log(LOGERROR, "PS5AddonLoader: cannot read add-on binary {}", path);
    return false;
  }

  char err[256] = {0};
  m_img = ps5elf::load(buf.data(), buf.size(), &host_export_resolver, nullptr, err, sizeof err);
  if (!m_img)
  {
    char line[400];
    std::snprintf(line, sizeof line, "[ps5elf] %s: %s\n", path.c_str(), err);
    sceKernelDebugOutText(0, line); // unbuffered: survives a crash CLog would not
    CLog::Log(LOGERROR, "PS5AddonLoader: failed to load {}: {}", path, err);
    return false;
  }

  const ps5elf::Stats st = ps5elf::stats(m_img);
  if (st.tls != 0)
    CLog::Log(LOGWARNING,
              "PS5AddonLoader: {} uses {} TLS relocation(s), which are not yet applied; "
              "the add-on may misbehave if it relies on thread-local storage",
              path, st.tls);
  if (st.other != 0)
    CLog::Log(LOGWARNING, "PS5AddonLoader: {} has {} unhandled relocation(s)", path, st.other);

  // Run the add-on's constructors (C++ static initializers) before use.
  {
    char line[200];
    std::snprintf(line, sizeof line, "[ps5elf] relocated OK (rel %u glob %u jmp %u); running constructors\n",
                  st.relative, st.glob_dat, st.jump_slot);
    sceKernelDebugOutText(0, line);
  }
  ps5elf::run_init(m_img);
  sceKernelDebugOutText(0, "[ps5elf] constructors done; add-on ready\n");
  m_loaded = true;
  CLog::Log(LOGDEBUG,
            "PS5AddonLoader: loaded {} (reloc: relative={} glob/64={} jmp={})",
            path, st.relative, st.glob_dat, st.jump_slot);
  return true;
}

void CPS5AddonLoader::Unload()
{
  if (m_img)
  {
    ps5elf::unload(m_img);
    m_img = nullptr;
  }
  m_loaded = false;
}

int CPS5AddonLoader::ResolveExport(const char* symbol, void** ptr, bool logging)
{
  if (!m_loaded && !Load())
  {
    if (logging)
      CLog::Log(LOGWARNING, "PS5AddonLoader: cannot resolve {} in {}: not loaded", symbol,
                GetName());
    return 0;
  }
  void* s = ps5elf::symbol(m_img, symbol);
  if (!s)
  {
    if (logging)
      CLog::Log(LOGWARNING, "PS5AddonLoader: unable to resolve {} in {}", symbol, GetName());
    return 0;
  }
  *ptr = s;
  return 1;
}

bool CPS5AddonLoader::IsSystemDll()
{
  return false;
}

HMODULE CPS5AddonLoader::GetHModule()
{
  return m_img;
}

bool CPS5AddonLoader::HasSymbols()
{
  return false;
}
