/*
 *  Definitions for weak references that the native-app converter cannot leave
 *  unresolved (it requires every undefined symbol to come from a Sony stub).
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stddef.h>

/* zstd tracing hooks (weak in libzstd; 0 = tracing disabled). */
typedef unsigned long long ZSTD_TraceCtx;
ZSTD_TraceCtx ZSTD_trace_compress_begin(const void* cctx) { (void)cctx; return 0; }
void ZSTD_trace_compress_end(ZSTD_TraceCtx ctx, const void* trace) { (void)ctx; (void)trace; }
ZSTD_TraceCtx ZSTD_trace_decompress_begin(const void* dctx) { (void)dctx; return 0; }
void ZSTD_trace_decompress_end(ZSTD_TraceCtx ctx, const void* trace) { (void)ctx; (void)trace; }

/*
 * __cxa_thread_atexit_impl: the glibc hook libc++abi uses (weakly) to register
 * destructors of C++ thread_local objects. The console libc has none, so
 * provide it: a per-thread LIFO list run from a pthread key destructor when
 * the thread exits. Destructors that create new thread_locals re-arm the key,
 * and pthread calls the destructor again (PTHREAD_DESTRUCTOR_ITERATIONS).
 * The main thread's list is not run at process exit - the process is gone.
 */
#include <pthread.h>
#include <stdlib.h>

struct ps5_tls_dtor
{
  void (*fn)(void*);
  void* obj;
  struct ps5_tls_dtor* next;
};

static pthread_key_t g_tls_dtor_key;
static pthread_once_t g_tls_dtor_once = PTHREAD_ONCE_INIT;

static void ps5_run_tls_dtors(void* head)
{
  struct ps5_tls_dtor* node = (struct ps5_tls_dtor*)head;
  while (node)
  {
    struct ps5_tls_dtor* next = node->next;
    node->fn(node->obj);
    free(node);
    node = next;
  }
}

static void ps5_tls_dtor_key_init(void)
{
  pthread_key_create(&g_tls_dtor_key, ps5_run_tls_dtors);
}

int __cxa_thread_atexit_impl(void (*fn)(void*), void* obj, void* dso_symbol)
{
  (void)dso_symbol;
  pthread_once(&g_tls_dtor_once, ps5_tls_dtor_key_init);
  struct ps5_tls_dtor* node = (struct ps5_tls_dtor*)malloc(sizeof(*node));
  if (!node)
    return -1;
  node->fn = fn;
  node->obj = obj;
  node->next = (struct ps5_tls_dtor*)pthread_getspecific(g_tls_dtor_key);
  pthread_setspecific(g_tls_dtor_key, node);
  return 0;
}

/*
 * dlopen family: the SDK libc's dlopen()/dlsym()/... forward to __dlopen etc.,
 * which live in the payload runtime (crt1.o's rtld). A native title is linked
 * with the boilerplate's startup code instead, so there is no runtime loader:
 * every open fails cleanly. Kodi phase 1 loads no binary add-ons.
 */
static int g_dl_tried = 0;

void* __dlopen(const char* filename, int flags)
{
  (void)filename;
  (void)flags;
  g_dl_tried = 1;
  return NULL;
}

void* __dlsym(void* handle, const char* symbol)
{
  (void)handle;
  (void)symbol;
  g_dl_tried = 1;
  return NULL;
}

int __dlclose(void* handle)
{
  (void)handle;
  return -1;
}

int __dladdr(void* addr, void* info)
{
  (void)addr;
  (void)info;
  return 0;
}

char* __dlerror(void)
{
  if (!g_dl_tried)
    return NULL;
  g_dl_tried = 0;
  return (char*)"dynamic loading is not available in this PS5 title";
}

/*
 * C++ thread_local init functions (_ZTH<mangled-name>): the compiler references
 * them weakly from every TU that touches a thread_local, and they resolve to
 * null when the variable has no dynamic initializer (the defining TU emits
 * nothing). The native-app converter rejects any undefined symbol, weak or
 * not, so give them a no-op body: the TLS wrapper calls it, nothing happens,
 * the variable keeps its static (zero) initialization - which is correct
 * precisely because no dynamic init exists.
 *
 *   XBMCAddon::xbmcgui::InterceptorBase::upcallTls  (a plain thread_local
 *   pointer in xbmc/interfaces/legacy/Window.cpp; Python GUI bindings)
 */
void _ZTHN9XBMCAddon7xbmcgui15InterceptorBase9upcallTlsE(void)
{
}
