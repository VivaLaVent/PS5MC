# Kodi 21 (Omega) forms of the platform patches

The 19 patches in `kodi/` were written against Kodi 22 (Piers). On Omega, 10
apply unchanged; these 8 are the Omega forms, exported from the verified
`ps5-omega` commits, so the migration script applies them without fuzz:

| patch | why it differs on 21 |
|---|---|
| 0002 timeutils | Omega inverts the condition (`CLOCK_MONOTONIC_RAW && !ANDROID`); add `&& !TARGET_PS5` |
| 0013 default-framebuffer-hdr | Omega's FBO has no depth-buffer block; that hunk is dropped |
| 0016 curl-idle-close | Omega spells `std::unique_lock<CCriticalSection>` (Piers uses CTAD) |
| 0018 xbpython-init | Omega initializes Python in `OnScriptInitialized()`, not the ctor; returns false on failure |
| 0006/0007/0008/0014 | pure line-offset drift (content identical) |

`tools/migrate-to-fork.sh` prefers a file here over `kodi/` when building `ps5-omega`.
| 0020 cdio | Kodi 21 requires Cdio unconditionally (22 only with `ENABLE_OPTICAL`); moved under that guard |
| 0028 22-compat | headers at their Kodi 22 paths (`jobs/Job.h`, `jobs/JobManager.h`, `FileItemList.h`) forwarding to the 21 locations; `CGuiCompositeShaderGL` + `gl_gui_composite` shaders backported verbatim from 22 (HDR GUI compositing) |
| 0027 optical off | 21 includes CDDA/cddb headers unconditionally in FileFactory, DirectoryFactory, MusicDatabase and builds the CDDA tag loader always; guarded as Kodi 22 does |
| 0026 force internal | TagLib/PCRE/RapidJSON had the same linux/freebsd-only force clause: a re-run found the previous run's internal copy by version, took the system path, and its re-rooted find_library failed. ps5 added |
| 0025 spdlog fmt_DIR | spdlog's sub-build finds fmt via `CMAKE_PREFIX_PATH`, which is re-rooted under the sysroot when cross-compiling, so it got fmt 12 even with internal 9.1 built; `fmt_DIR` names the config dir and is not re-rooted (latent on 22, masked by matching versions) |
| 0024 fmt/spdlog | 21 pins fmt 9.1 + spdlog 1.10; the sysroot's fmt 12 satisfies 21's ≥9.1 check but breaks spdlog 1.10 (`fmt::basic_runtime` gone in fmt 10). ps5 now forces the internal pair, as upstream does on linux/freebsd |
| 0023 libdvd | 21 builds libdvdread/nav/css with autotools and `--host=x86_64-ps5`, which `config.sub` rejects (22 uses meson); ps5 maps to `x86_64-unknown-freebsd` |
| 0022 findcurl | 21 links `NGHTTP2_LIBRARY` unconditionally when curl is static; ours has no HTTP/2 → NOTFOUND broke generation. Now empty when absent (22 behaviour) |
| 0021 treedata | `RetroPlayer/shaders/gl` and `posix/filesystem/test` are 22-only directories; dropped from `cmake/treedata/ps5` |

Omega-only build facts (not patches): `lzo2` must be in the sysroot
(`scripts/22-build-kodi21-deps.sh`); KissFFT, PCRE v1 and RapidJSON build
internally (`ArchSetup.cmake`); `FindOpenGl`/`FindEGL` guard on
`OpenGL::GL`/`EGL::EGL` (handled in `platform/ps5/ps5.cmake` for both versions).

API renames that cannot be shimmed (they are in `override` signatures) are
guarded in the shared platform sources with `PS5_KODI_MAJOR`, which
`ArchSetup.cmake` reads from the tree's `version.txt`:
`CacheType`/`DIR_CACHE_TYPE` (SMB2Directory.h), `IOControl`/`EIoControl` (SMB2File.h).
