# PS5MC: Kodi forked, PS5 as a platform, Kodi 21 and 22 supported

Decided 2026-10-05. PS5MC is a real GitHub fork of `xbmc/xbmc`
(**https://github.com/VivaLaVent/PS5MC**). This repository (`kodi-ps5`) is
frozen after its last build and points there.

## Why a fork

The overlay+patches shape (19 patches, overlay/, manifest) was right for a few
files and is not maintainable at the current size. A fork gives upstream
rebasing, PS5 as a first-class platform in Kodi's own build, and a plausible
path to upstream.

## Naming

- Project: **PS5MC (Kodi)**. Repo: `VivaLaVent/PS5MC`.
- Branches: `ps5mc-piers` (Kodi 22, primary - what we ship) and `ps5mc-omega`
  (Kodi 21). `master` is Kodi's; we never commit to it.
- Code identifiers stay technical and unchanged: `TARGET_PS5`,
  `xbmc/platform/ps5/`, `cmake/platform/ps5/`. Renaming those buys nothing
  and would touch every file. Branding is in docs, branches, the title name.
- Build infrastructure (scripts, shims, pacbrew recipes, toolchain, tools,
  docs) lives under `ps5/` in the fork.

## One platform series, two bases

Upstream branches (confirmed 2026-10-05): `Omega` = 21, `Piers` = 22 (at
`22.0rc1-Piers`; stable is imminent), `master` = 23-dev. We do not track
`master`: every rebase would drag in 23 churn.

The PS5 work is **one linear series of commits** rebased onto each base: the
19 patches as commits, the platform files as 4 grouped commits, the infra
under `ps5/`. Where 21 and 22 genuinely differ, the difference is a small
guard, not a second series.

**Verified offline (2026-10-05):** all 19 patches apply on `upstream/Piers`
(even at HEAD) unchanged. On `upstream/Omega`, 10 apply unchanged and 8 need
Omega forms (`patches/omega/`: 4 pure offset drift, 4 real ports -
`TimeUtils` inverted condition, no FBO depth-buffer block,
`std::unique_lock<CCriticalSection>` spelling, Python init in
`OnScriptInitialized()`). `tools/migrate-to-fork.sh` reproduces both branches
and was executed end to end against a real Kodi clone: 24 commits per branch,
trees identical to the hand-built ones.

Binary add-on ABI versions differ per branch (dev-kit `versions.h`):
screensaver `2.2.0` on both; **inputstream `3.3.0` on Omega vs `3.4.0` on
Piers**. inputstream.adaptive's manifest and build must be versioned per
branch.

Honest expectation: `ps5mc-omega` lags. 21 uses FFmpeg 6 against our
sysroot's 7.1 and an older Python integration; the first configure may
surface build-system differences the patches do not cover.

## What carries over, non-negotiably

The verification discipline that caught real errors becomes CI rather than
manifest greps:

- build stamp = the fork's `git describe` (branch, upstream tag, distance,
  hash) - provenance of every eboot;
- the sysroot-library relink guard (cmake does not track sysroot `.a` as a
  `kodi.bin` dependency);
- the eboot link checks for required wrappers and `EXTERN` symbols
  (`__register_frame`, `ps5_agc_gate2_run`);
- the FSELF converter constraint: **no added defined-data object in the eboot
  link** (bisected at length; see `xbmc/platform/ps5/elf/README.md`). Binary
  add-on imports are resolved from the eboot's own GOT at run time;
- `docs/WORKFLOW.md`: zip byte-size verify, batching, FTP deploy, nc capture.
  To be merged into the shape of `ps5-homebrew-dev-protocol`, whose
  close/launch payloads over elfldr:9021 remove the manual "quit, re-register,
  launch" steps.

## Sandbox / paths: the interface contract

One class owns it; nothing else branches on "escaped". Non-escaped mode stays
supported (etaHEN whitelists; a hard stop would lock out the most common
loader).

| | escaped | not escaped |
|---|---|---|
| app files (RO) | `/mnt/sandbox/<id>_000/app0` | `/app0` |
| writable home | `/data/kodi` | `/download0/.kodi` |
| `KODI_PS5_TITLE_ROOT` | set | unset |

- Escape attempted at startup, **klog-only** logging (CLog is not up that
  early; CLog there kernel-panics, and buffered CLog silently ate the ELF
  loader's diagnostics across three crashes).
- On failure: warn prominently (storage cap from `downloadDataSize`; logs
  unreachable while the title is closed), run degraded. Hard requirement only
  once etaHEN support exists.
- `KODI_PS5_TITLE_ROOT` discovered by **matching `eboot.bin`'s inode under
  `/mnt/sandbox`** - no title-ID or `_000` guessing (from
  `shims/native-app/sandbox_paths.c`, whose path-wrapping otherwise retires).
- Switch file `kodi-no-escape` forces legacy mode so both modes are testable.
- Constraints: a title reaches the filesystem by path through only 12
  libkernel calls, with no `*at()` variants; `chroot`/`fchdir`/`nmount` are
  `libkernel_sys` only (titles do not get it); a failed
  `CLangInfo::SetLanguage` takes Kodi down via `exit()` dying in SIGSYS.

## Work streams (keep them in separate files)

| stream | files |
|---|---|
| fork, build system, CI | `ps5/scripts`, `cmake/platform/ps5` |
| sandbox escape, paths class, `main.cpp` refactor, USB | `xbmc/platform/ps5/main.cpp`, paths class |
| video: codec, decoder, renderer, HDR | `xbmc/platform/ps5/video`, `windowing/ps5` |
| ELF loader, binary add-ons, Python runtime | `xbmc/platform/ps5/elf`, `pacbrew/python3` |

Branch model: short-lived feature branches off `ps5mc-piers`, rebased not
merged.

## In flight (independent of the migration)

- Binary add-on loader: Kodi calls `CPS5AddonLoader` on hardware; it faults
  in `ps5elf::resolve` at a fixed address. The loader now writes an unbuffered
  klog trace before each risky step, so the next run names the step.
- Built, untested: mid-stream decoder resync; `video decoded N presented M`
  counters; length-prefixed NAL conversion; staged hardware/software recovery.
