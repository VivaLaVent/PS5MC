# Kodi on PS5 — architecture lessons

Written at the point where the port boots, renders, plays video with correct
format/framerate handling, installs add-ons, and runs Python add-ons with
working HTTPS. This records what the work taught us and turns it into rules,
so the remaining work (binary add-ons, install performance) is done with the
same discipline and fewer wasted iterations.

## 1. The platform model: three runtimes, not one

Every hard bug traced back to one fact. A PS5 native title runs against three
different code bases that a Linux port treats as one "libc":

| What | Provided by | Trust level |
|---|---|---|
| Kernel syscalls (`open`, `stat`, `nanosleep`, sockets…) | `libkernel.sprx` | Real, but the **sandbox denies some** (`ioctl(FIONBIO)` on sockets, for one); plain `socket()`/`connect()` work (libsmb2 uses them) |
| C library (`fopen`, `getc`, `malloc`, `FILE`…) | the boilerplate's **clean-room `libc.prx`** | Incomplete; its `FILE` layout is **not** FreeBSD's |
| Networking | `libSceNet` (`sceNet*`) | The **only** network path a title may use |

The SDK's headers are FreeBSD's. So code compiles against FreeBSD assumptions
and runs against something that only approximates them. Kodi (C++) mostly
dodged this by accident: C++ gets the real libc *functions*, while C code
gets FreeBSD's **inline `FILE`-struct macros** (`getc_unlocked`, `fileno`…),
which read fields laid out for a different libc. That single fact explains the
Python tokenizer GPF, the invisible `stderr`, and why Kodi never saw either.

**Rule:** any newly linked C component must be audited for (a) stdio inline
macros, (b) raw socket/ioctl syscalls, (c) functions the SDK *declares* but
the title doesn't *provide*. These are the three recurring failure classes.

## 2. Scope every interception to the smallest unit that needs it

`--wrap=socket` rewrote every `socket()` in the process — including Kodi's own
curl, which also lives on `sceNet` — and broke all networking at once. The fix
that worked was a header `#define`d into **one translation unit**
(`socketmodule.c`), which cannot collide with anything else by construction.

**Rule:** prefer, in this order: (1) a compile-time redirect scoped to the
consumer, (2) a local object definition that preempts a `.so` export,
(3) `--wrap`. Use `--wrap` only for symbols with exactly one meaning
process-wide (`pipe`, `fopen` errno-fix, `write` tee) — never for anything
two subsystems share.

## 3. Measure before patching — but verify the measurement shipped

Four suspects for the install slowness (curl reset, session teardown, SQLite,
dependency resolution) were each *plausible* from reading source and each
*ruled out* by a 50 ms timer on-console. Guessing would have produced four
wrong patches. Instrumentation was the cheapest tool in the whole effort.

But two "test results" were **stale builds**: cmake does not track a sysroot
`.a` as a dependency of `kodi.bin`, so a rebuilt shim library was never
relinked in, and a correct fix appeared not to work. `scripts/lib/sysroot-changed.sh`
now invalidates `kodi.bin` from every sysroot-installing script (12, 17, 21).

**Rule:** a result only counts after confirming the change is in the artifact
under test (build stamp, `llvm-nm` for a symbol, a log line only the new code
emits). Two builds were wasted learning this.

## 4. Fail at configure time, not 20 minutes later

A misspelled configure-cache key (`fexecv` vs `fexecve`) is a **silent no-op**;
disabling a header while leaving its functions enabled compiles for 20 minutes
and then dies in `posixmodule.c`. The Python PKGBUILD now checks `pyconfig.h`
for both classes immediately after configure.

**Rule:** every "we told the build system X" must have a cheap post-hoc
assertion that X actually took effect. The patch manifest check (which caught
the missing 0017) is the same principle applied to Kodi patches.

## 5. Patches are the *expected* form of a port; keep them honest

There are 19 Kodi patches. None fixes a Kodi bug. Each adapts to a platform
Kodi never targeted: no dynamic loader, a clean-room libc, a sandbox, an
unusual scheduler, HDR/VRR output semantics. Kodi itself carries analogous
sets for Android, webOS and tvOS. This is normal.

What is *not* acceptable is a patch that hides a diagnostic, a guess, or a
symptom fix for an unmeasured cause. Two rules follow:

- **Diagnostic patches are named `*-diagnostic-*` and are temporary.** They
  are removed once the measurement is taken (0020, 0021 removed here).
- **A performance patch names the primitive that is slow**, so the fix lands
  at the layer that owns the defect (0016/0019: `select()` and idle teardown
  in the title's network layer), not scattered across Kodi call sites.

Candidates worth offering upstream once proven on hardware: 0016 (don't tear
down curl sessions on the GUI thread) and 0018 (report Python init failure
instead of `exit()`ing mid-startup). Both improve every platform.

## 6. Things deliberately left alone in this consolidation

- **Video output patches (0011–0014, VRR/HDR/HLG→PQ/PBO)** — working, not
  touched. No lesson above applies to them; changing a working renderer to
  satisfy a tidiness impulse is exactly the risk this pass is meant to avoid.
- **The JIT probe** — kept in that pass as the feasibility test for the
  binary-add-on loader. *Removed in the 2026-10-08 cleanup:* the in-process
  ELF loader now maps executable memory itself and logs its own failures, so
  the standalone probe no longer answers anything the loader does not.
- **`stdio_tee` (`--wrap=write` → klog)** — kept. It is why Python fatal
  errors are now visible; cost is one integer compare per write.
- **Failure-only `pysock` error logging** — kept. Fires only when a `sceNet`
  call fails, and it is how the `SOCK_CLOEXEC` and `FIONBIO` gaps were found.
- **Startup probes in `main.cpp`** (directory-listing checks) — kept. One-time,
  cheap, and they confirm a deploy is real. Removable later if wanted.

## 7. Open, and how to attack it with the above

*Superseded: the current list is `OPEN-ISSUES.md`. The entries below are the
state at the time of this consolidation.*

- **Add-on install slowness (not network).** Downloads are fast; curl is
  healthy. Ruled out: curl reset/teardown, SQLite, `CheckDependencies`.
  Unmeasured: the unpack/write phase into `/download0`. Next step is one
  targeted timer there — *then* a fix.
- **Binary add-ons** (PIL, inputstream.adaptive). Infrastructure exists
  (JIT probe passed via `mprotect`; ELF loader + export table host-tested).
  Apply lesson 1 hard: a binary add-on's `.so` is more C code compiled
  against FreeBSD headers running on the clean-room libc.
- **`select()` is slow in a title.** Kodi's curl avoids it (0019). Python's
  `selectors` uses `kqueue` on this platform and appears fine; untested under
  load.

## 8. Build-speed levers (lesson 3 makes them safe to use)

- `KODI_PS5_CCACHE=1` — opt-in ccache in `20-configure`. Default path unchanged.
- Skip `20-configure` unless `overlay/` or `patches/` changed.
- A shim/link-only change is `12-build-libuuid-shim.sh` (if `libkodishim`)
  + `cmake --build` + `30-deploy.sh`; no configure, no Python rebuild.
- A Python header/patch change needs `19-build-python.sh`; a shim-only change
  does not.

## 9. Corrections and later findings

- **Raw sockets are not blocked.** Kodi's SMB client (libsmb2) uses plain
  `socket()`/`connect()` and works. Python's `EACCES` came from
  `ioctl(FIONBIO)`, the one call between `socket()` and `connect()`; the
  sceNet route was built before that was known. It works and stays, but the
  cause was narrower than first written.
- **VideoDec2 error codes** (named in ProsperoTV's messages): `0x811D0301`
  invalid access unit, `0x811D0302` stream exceeds the configured capacity
  (DPB or coded size), `0x811D0303` no valid video sequence (parameter sets),
  `0x811D0304` fatal bitstream error.
- **`0x811D0303` on a whole file** was an MKV with no codec extradata whose
  frames are length-prefixed: nothing converted them to Annex-B. The
  fingerprint logging (NAL units of the refused frame) found it in one run;
  logging *what the decoder was given* beats guessing *why it refused*.
- **Other PS5 projects confirm the scarce flexible-memory pool** (EVO Player
  shrinks buffers to give memory back to it). Thread stacks come out of it too.

## 10. The Kodi 21 missing-icons hunt (2026-10)

Kodi 21's skin showed text and backgrounds but no icons, buttons or
highlights. Six wrong theories (binary image decoder, DXT upload, texture
size, lzo, a broken `fopen` on `/app0`, path case-folding) each got a patch or
a diagnostic build before the cause was found: **the texture bundle was in a
format Kodi 21 cannot read.** Both builds used one host TexturePacker, built
from the Kodi 22 tree, and Kodi 22 changed the bundle format (XBTF version 2 ->
3). Lessons, cheapest first:

- **Compare against the working twin before theorizing.** Kodi 22 rendered
  the same skin with the same libc, sandbox, `fopen` and path code. Any theory
  that would also have broken Kodi 22 was dead on arrival - that rules out
  four of the six without a single build.
- **A file that exists but "will not open": check what is in it.** `head -c 5
  Textures.xbt` (`XBTF3`) against the reader's expected version would have
  found it in one command.
- **Host tools must come from the same tree as the target.** A format change
  in a build tool is silent: nothing fails until the target reads the output.
  `20-configure-kodi.sh` now builds TexturePacker and JsonSchemaBuilder per
  Kodi major; `30-deploy.sh` rejects any bundle the target cannot read.
- **CMake caches what `find_program()` found.** Pointing an existing build
  directory at a new tool (`-DWITH_TEXTUREPACKER=...`) changes nothing: the
  old path stays in `CMakeCache.txt`. The first per-major build still packed
  with the Kodi 22 tool for exactly that reason. Configure now clears the
  cached tool paths (`-U`) and then checks what CMake actually resolved.
- **A check that runs after the success marker must still decide.** The
  deploy's bundle check fired, but the release script judged the deploy by
  "Build complete" (printed earlier) and packaged the bad build anyway. It now
  uses the deploy's exit status.
- **Read the log for what it says.** The diagnostic printed the bundle path
  *after* translation, and it was correct; a few `/media/Textures.xbt` lines
  were the harmless probes Kodi makes before a skin is loaded. Reading them as
  "the path is corrupted" produced patch 0036, now reverted.
- **A diagnostic must be able to fire on the format under test.** The switch
  file that enabled the `fopen` logs cannot live inside a read-only exFAT
  image, so the logs never ran and their absence was misread as evidence.
  Switches are now also read from the build's `/data` home.
