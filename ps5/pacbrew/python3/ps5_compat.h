/*
 * Prototype for getentropy(), which the PS5 target provides (Kodi's C library
 * shim, shims/native-app/libc_posix.c, via kern.arandom) but the SDK's headers
 * do not declare. Python's configure is told it exists
 * (ac_cv_func_getentropy=yes), and it is load-bearing: Python's hash-seed
 * randomization calls it at interpreter startup, and without a working entropy
 * source Python fails to initialize. The call site then needs a prototype or
 * -Werror=implicit-function-declaration rejects it.
 *
 * This header is force-included (CPPFLAGS -include) into every cross-compiled
 * translation unit, INCLUDING configure's own feature tests - so it must pull
 * in NO system headers. Autoconf probes functions by declaring a bogus
 * `char func();` prototype; a real prototype seen first (e.g. clock_gettime
 * via <pthread.h> -> <time.h>) makes that probe fail to compile, and configure
 * wrongly concludes the function is absent. That is exactly what broke
 * HAVE_CLOCK_GETTIME on a previous attempt. Hence __SIZE_TYPE__ (a compiler
 * builtin) instead of <stddef.h>, and no other includes at all.
 *
 * Only getentropy is declared here. Its own configure probe is skipped by the
 * cache entry, so this prototype cannot conflict with any test.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int getentropy(void* buf, __SIZE_TYPE__ buflen);

#ifdef __cplusplus
}
#endif
