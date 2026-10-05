/*
 * Stage-2 binary add-on for the PS5 in-process ELF loader.
 *
 * It is NOT linked against Kodi: like every Kodi binary add-on it exports the
 * C entry points the add-on ABI defines (ADDON_Create / ADDON_Destroy), and it
 * reaches Kodi only through the function table Kodi hands it at create time
 * (the add-on dev-kit's "interface" pointer). That keeps the symbol surface
 * tiny - exactly what the loader's export table must satisfy - and is why the
 * toolchain probe showed only libc/libc++ imports, nothing Kodi-specific.
 *
 * What it proves on the console, logged through the interface's log callback:
 *   1. the loader mapped and relocated this .so and ran its constructor,
 *   2. a call from Kodi into the .so works (ADDON_Create),
 *   3. a call from the .so back into Kodi works (the log callback),
 *   4. a C++ exception thrown and caught inside the .so works (needs the
 *      loader's .eh_frame registration),
 *   5. heap and a vtable work (operator new, virtual dispatch).
 */
#include <exception>
#include <stdexcept>
#include <string>

// Minimal slice of the add-on ABI we need: the create-time context carries a
// logger. The real kodi-dev-kit header defines far more; we declare only what
// this test calls, by the same layout, so we need not compile against it.
extern "C"
{
  typedef void (*KodiLog)(void* kodiBase, int level, const char* msg);
  struct AddonToKodiFuncTable_Addon // prefix matches the dev-kit; only addon_log used
  {
    void* kodiBase;
    void* (*free_string)(void*, char*);
    void* (*free_string_array)(void*, char**, int);
    void* (*get_addon_path)(void*);
    void* (*get_lib_path)(void*);
    KodiLog addon_log;
  };
  struct AddonGlobalInterface
  {
    const char* libPath;
    const char* dataPath;
    AddonToKodiFuncTable_Addon* toKodi;
    void* toAddon;
  };
}

namespace
{
int g_ctorRan = 0;
struct Ctor { Ctor() { g_ctorRan = 0xC0FFEE; } };
Ctor g_ctor;

struct Base { virtual int tag() const { return 1; } virtual ~Base() = default; };
struct Derived : Base { int tag() const override { return 0x5151; } };

void Log(AddonGlobalInterface* g, const char* msg)
{
  if (g && g->toKodi && g->toKodi->addon_log)
    g->toKodi->addon_log(g->toKodi->kodiBase, 1 /*INFO*/, msg); // call back into Kodi
}
} // namespace

extern "C" int ADDON_Create(void* instance, const char* /*globalApiVersion*/, void* /*unused*/)
{
  auto* g = static_cast<AddonGlobalInterface*>(instance);
  Log(g, "[test.binary.ps5] ADDON_Create: the loader called into the .so");
  Log(g, g_ctorRan == 0xC0FFEE ? "[test.binary.ps5] global constructor ran"
                               : "[test.binary.ps5] FAIL: constructor did not run");

  int caught = 0;
  try { throw std::runtime_error("intentional"); }
  catch (const std::exception& e) { caught = 1; }
  Log(g, caught ? "[test.binary.ps5] exception thrown and caught inside the .so (eh_frame OK)"
                : "[test.binary.ps5] FAIL: exception not caught");

  Base* b = new Derived();            // heap + vtable
  int t = b->tag();
  delete b;
  Log(g, t == 0x5151 ? "[test.binary.ps5] heap + vtable OK"
                     : "[test.binary.ps5] FAIL: vtable dispatch wrong");

  Log(g, "[test.binary.ps5] STAGE 2 PASS");
  return 0; // ADDON_STATUS_OK
}

extern "C" void ADDON_Destroy() {}

// Kodi's binary loader resolves these alongside ADDON_Create (DllAddon.h):
// ADDON_GetTypeVersion is required, ADDON_GetTypeMinVersion optional. Return
// the ABI version string Kodi expects for the add-on type; for this loader
// test the value only has to be non-null and stable.
extern "C" const char* ADDON_GetTypeVersion(int /*type*/)
{
  return "1.0.0";
}
extern "C" const char* ADDON_GetTypeMinVersion(int /*type*/)
{
  return "1.0.0";
}
