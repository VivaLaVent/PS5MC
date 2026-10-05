# Python add-ons on PS5 — status (2026-10-01)

## Result: WORKING end to end
CPython 3.14.6 is statically linked into the eboot. Pure-Python add-ons install,
run, and make HTTPS requests. Verified on console with weather.gismeteo:
full TLS round-trip to services.gismeteo.net, forecast payload received,
add-on caching working, script completes in ~4 s.

## What it took (each was a distinct platform gap, none a Kodi bug)
| Layer | Problem | Fix |
|---|---|---|
| Build | configure probes poisoned by force-included header | ps5_compat.h includes nothing (uses `__SIZE_TYPE__`) |
| Build | getentropy/pthread names undeclared in SDK headers | prototype for getentropy; disable pthread_*name_np via cache |
| Build | OpenSSL link test fails standalone (needs Kodi's getentropy shim) | force `ac_cv_working_openssl_*=yes`; `--with-openssl`; keep `_sha2` |
| Build | "no processes" cache inconsistent (fexecv typo; sched.h off, funcs on) | fix `fexecve`; disable all `sched_*` |
| Link | libpython needs getentropy/explicit_bzero + 51 POSIX funcs a title lacks | `libkodishim.a` (weak POSIX shims, real impls for clock_nanosleep/fstatat/utimensat) |
| Link | `_ZTH…upcallTls` weak TLS-init ref rejected by converter | no-op definition in weak_shims.c |
| Runtime | clean-room libc.prx lacks getcwd (Python getpath crashed) | getcwd/realpath/chdir-wrap shims |
| Runtime | fopen errno not POSIX (getpath pyvenv.cfg probe → "error evaluating path") | `--wrap=fopen` normalizes errno via stat |
| Runtime | FreeBSD stdio inline macros read FILE internals (C only; libc.prx layout differs) → tokenizer GPF | `#undef` the macros in Python.h (`PS5_CLEANROOM_STDIO`); `getc_unlocked=no` |
| Runtime | Python init failure exit()'d mid-startup, error invisible | patch 0018: `Py_InitializeFromConfig` + CLog |
| Network | `ioctl(FIONBIO)` denied by the sandbox (EACCES); Python's socket module moved to libSceNet | `ps5_pysocket.h` redirects **only socketmodule.c** to `ps5_*` over libSceNet |
| Network | sceNetSocket rejects SOCK_CLOEXEC flag (0x8041012b) | mask flag bits |
| Network | ioctl(FIONBIO) raw syscall denied | `ps5_ioctl` → `sceNetSetsockopt(SO_NBIO)` |

Key design lesson: process-wide `--wrap` of socket names broke Kodi's own curl
(also on sceNet). Scoping the redirect to one translation unit via a header
`#define` was the correct, collision-free approach.

Build-system lesson: cmake does not track a sysroot `.a` as a kodi.bin
dependency; script 12 now deletes kodi.bin after installing libkodishim so a
rebuilt shim is actually relinked (two "failed" iterations were stale builds).

## Known limits
- Add-ons needing binary modules (PIL/Pillow, cryptography, lxml) still fail
  with ModuleNotFoundError — needs the binary-add-on ELF loader (separate work).
- `select()` in a title is slow; Kodi curl uses `curl_multi_wait` (patch 0019).
  Python's `selectors` uses kqueue/poll — not yet characterized.
- Diagnostic patches 0020/0021 (curl/install timing) are still applied; revert
  once the install-slowness investigation concludes.

## Open: add-on install slowness (NOT network)
Downloads are fast (curl healthy). Measured and ruled out: curl easy_reset,
session teardown, SQLite, CheckDependencies. Remaining suspect: the
unpack/write phase into /download0 (save-data image). Not yet instrumented.
