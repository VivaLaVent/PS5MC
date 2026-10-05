<p align="center">
  <img src="title/sce_sys/icon0.png" width="160" alt="PS5MC (Kodi) for PlayStation 5">
</p>

# PS5MC (Kodi)

Welcome to Kodi for PS5! This is a native port of [Kodi](https://kodi.tv) 22,
the free and open source media center, to jailbroken PlayStation 5 consoles.
It runs as a regular home-screen title, renders its interface at 4K on the PS5's
GPU, decodes H.264, HEVC and VP9 on the console's video hardware, and plays your
library from the network or local storage.

It is built on the open-source [ps5-payload-dev](https://github.com/ps5-payload-dev)
toolchain and [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl), and
is installed with ShadowMountPlus or a compatible loader.

[![Release](https://img.shields.io/github/v/release/VivaLaVent/kodi-ps5?label=release)](https://github.com/VivaLaVent/kodi-ps5/releases)
[![Kodi 22](https://img.shields.io/badge/Kodi-22%20BETA2-17b2e7)](https://kodi.tv)
[![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue)](LICENSE)
[![Discord](https://img.shields.io/badge/Discord-join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/YB58bUZrqu)

> **Unofficial.** This project is not affiliated with or endorsed by Team Kodi,
> the XBMC Foundation or Sony. "Kodi" and the Kodi logo are trademarks of the
> XBMC Foundation. The repository contains no exploit, no Sony SDK code and no
> firmware files; the Sony library prototypes in `overlay/xbmc/platform/ps5/sce/`
> are clean-room declarations of the handful of functions Kodi needs.

## Give the code a test drive

Ready-to-install builds are on the [releases page](https://github.com/VivaLaVent/kodi-ps5/releases).

**Required on the console:**

- a HEN that gives homebrew kernel access, such as **etaHEN**;
- **ShadowMountPlus** (or a compatible folder-title loader), to register and
  mount the title folder;
- an **FTP server**, to copy the files (most HEN setups start one, usually on
  port 2121).

**Optional:**

- **PS5-Lapy-JB-Daemon**, for USB drives and `/data` (see *USB drives* below);
- **klogsrv**, to capture Kodi's log over the network (see *Debugging*).

1. Extract the release zip into `/data/homebrew/` on the console over FTP; it
   creates `/data/homebrew/PPSA99420/`.
2. Register the title with ShadowMountPlus, then start Kodi from the home screen.
3. Add your media under *Videos → Files → Add videos… → Browse*. Each **USB
   drive** appears on its own (see below). The console filesystem is not
   offered as an automatic source: probing the loader's loopback FTP server on
   the GUI thread stalls navigation, so it was removed. To browse console files
   anyway, add `ftp://127.0.0.1:2121/` (or `:1337`) by hand via *Add network
   location…*. For network shares choose
   **Add network location…**, protocol **Windows network (SMB)** or **NFS**,
   and enter the server's **IP address** (Windows/NetBIOS names are not
   resolved). SMB2 and SMB3 work, SMB1 does not.

### USB drives

A title runs in a sandbox that hides USB drives and `/data`. A resident
jailbreak daemon can open it on request, and Kodi asks by itself right after
start-up:

1. Load the **PS5-Lapy-JB-Daemon** payload once per console boot, **before**
   starting Kodi - with your payload loader, or automatically through its
   autoload list. etaHEN answers the same request where its
   jailbreak-on-demand is available.
2. Plug in a drive formatted **exFAT** or **FAT32**.
3. Start Kodi. The log reports `PS5 sandbox: opened by the jailbreak daemon`,
   and the drive appears under *Videos → Files → Add videos… → Browse* as
   `/mnt/usb0` and so on.

Without a daemon, Kodi runs as before with network sources only. The request is
a file (`{"PID":"<pid>"}` in the title's `/download0/etahen_jailbreak`), which
the daemon consumes.

### Updating

**Close Kodi before replacing its files** over FTP: overwriting a running
title's files can crash the console. Usually only `eboot.bin` changes (plus `sce_sys/` when the title
metadata changes, `share/` when Kodi's data files change; 0.9 adds Python's
standard library under `share/kodi/python/`, so copy the whole folder once).
Kodi keeps its data in the title's save data (`/download0/.kodi`), which
updates leave alone; `kodi-reset` clears it (see *Switches*).

## What works

- **Interface:** Kodi's Estuary skin, rendered natively at 3840x2160 with
  OpenGL 4.6 on the PS5's GPU.
- **Hardware video decoding** on the console's decoder: H.264 (8-bit), HEVC
  Main and Main 10, VP9 Profile 0 and 2 - shown zero-copy (the GPU reads the
  decoder's frames directly). Interlaced streams (1080i TV recordings) are
  decoded in hardware and deinterlaced with bwdif.
- **Everything else in software** (FFmpeg), including AV1 (dav1d; Dolby Vision
  profile 10 files are AV1).
- **HDR output** for HDR10 (PQ) and HLG video when the TV link runs in HDR: a
  10-bit BT.2020 PQ picture (HLG converted to PQ in the shader), with Kodi's
  on-screen display composited into it.
- **VRR during playback**, matched to the video's frame rate; the menus at
  60 Hz (see *Display*).
- **Audio:** stereo, 5.1 and 7.1 PCM to the console (8-channel port).
- **Sources:** SMB2/3 and NFS network shares, UPnP; USB drives and `/data`
  with PS5-Lapy-JB-Daemon loaded (see *USB drives*).
- **Library:** thumbnails, databases and settings persist between sessions.
- **Internet:** Kodi's add-on repository, add-on installs and updates,
  artwork and scraper downloads (HTTPS, verified against Kodi's CA bundle).
- **Python add-ons** (CPython 3.14, built into Kodi): scrapers, weather,
  plugins and services, including their own HTTPS requests.
- **Hardware decoding recovery:** a file the decoder refuses before its first
  picture is replayed from the start, first in hardware with a cleaned
  bitstream, then in software, instead of hanging on the busy spinner.
- **DualSense** navigation (buttons mapped to Kodi's keyboard actions), and
  the PS5's own on-screen keyboard for text entry.

## What doesn't yet

- **Binary add-ons**, and Python add-ons that need a compiled module: a title
  cannot load libraries at run time. This covers `inputstream.adaptive`
  (DASH/HLS/DRM streams, e.g. many broadcaster plugins), `script.module.pil`
  (Pillow; e.g. the Open-Meteo weather add-on) and `pycryptodome`. Pure-Python
  add-ons work.
- **Add-on installs are slow:** tens of seconds per package; downloads
  themselves are fast. Being investigated.
- **Hardware decoding** of HEVC 4:2:2/4:4:4 and 12-bit video: FFmpeg decodes
  them, which is slow at high resolutions. H.264 High 10 is offered to the
  hardware decoder and falls back to FFmpeg if the decoder refuses it.
- **Dolby Vision** is not mapped: a DV file's base layer plays but the picture
  can look wrong or black, because the DV dynamic-metadata (RPU) is ignored.
  Files that also carry an HDR10 or HLG layer are best set to play as HDR10.
- **4K60 video** may stutter: the decoder runs at pipeline depth 1, which
  measured just short of a 60 fps frame at 4K. Deeper pipelines produced black
  pictures after every seek on this hardware, so depth 1 stays.
- **Fixed 24/25/50 Hz output:** the PS5 refuses explicit refresh rates from
  titles, so without VRR everything plays at 59.94 Hz.
- **Dolby/DTS passthrough** (including TrueHD and DTS-HD at 8 channels and
  192 kHz): offered to Kodi's *Allow passthrough*, but not yet confirmed to
  reach a receiver intact.
- **DualSense as a game controller** (joystick add-on).
- **The Media tab:** Kodi is a Games title (the GL driver fails in the Media
  category's sandbox).
- **The player debug overlay (L3)** raises the output to ~120 Hz during VRR.

## Settings that matter

### Display

Two PS5 settings (*Settings → Screen and Video → Video Output*) and two Kodi
settings (*Settings → Player → Videos*, settings level Advanced or Expert)
decide the output:

| PS5 **VRR** | Kodi *Adjust display refresh rate* | Menus | During a video |
| --- | --- | --- | --- |
| Off | any | 59.94 Hz fixed | 59.94 Hz fixed |
| On | Off | 60 Hz (paced on the VRR link) | 60 Hz |
| On | **On start/stop** | 60 Hz | **VRR at the video's rate** until it stops, also with the menus on top |
| On | **Always** | 60 Hz | **VRR at the video's rate** while it is fullscreen; 60 Hz with the menus on top |
| On | **On start** | 60 Hz until the first video | **VRR at the video's rate**, kept after it stops (Kodi does not switch back) |

With the PS5's VRR on, the system runs Kodi on a VRR link and the TV refreshes
whenever Kodi presents a frame. Kodi paces its frames: 59.94 per second in the
menus, and during a video the lowest multiple of the frame rate within the
PS5's VRR range of 48–120 Hz:

| Video | VRR rate | Frames shown |
| --- | --- | --- |
| 23.976 fps (films) | 71.93 Hz | each 3× |
| 24 fps | 48 Hz | each 2× |
| 25 fps (PAL) | 50 Hz | each 2× |
| 29.97 / 30 fps | 59.94 / 60 Hz | each 2× |
| 50 / 59.94 / 60 fps | 50 / 59.94 / 60 Hz | each 1× |

Every frame is on screen equally long: no 3:2 judder, no speed change.
Stopping the video returns to 60 Hz (with *On start*, Kodi keeps the video's
rate until the next video).

- **Enable 120 Hz Output** on the PS5 should be **Automatic**: the system
  builds its VRR link from the high-refresh mode Kodi declares.
- **HDR.** The title declares HDR capability, so with the PS5's HDR at *On When
  Supported* the TV runs in HDR for the whole session: the PS5 maps the menus
  and SDR video into the HDR signal itself (brightness per its *Adjust HDR*
  calibration), and Kodi outputs HDR10 (PQ) video natively and HLG converted
  to PQ - whenever the display link actually runs in HDR (read from VideoOut).
  For an SDR title
  instead, build with `KODI_HDR_TITLE=0 bash scripts/30-deploy.sh` (or set the
  PS5's HDR to *Off*): the TV stays SDR and HDR video is tone mapped by Kodi
  (video OSD: *Tone mapping*).
- **Sync playback to display** never changes the output rate. On the fixed
  59.94 Hz output it adjusts playback speed (and audio pitch) to the display's
  vblank clock; on the VRR link the display follows Kodi, so Kodi keeps its
  own clock and the setting has nothing to correct.
- The TV's own overlay (on an LG: the Game Dashboard) shows the rate: about
  60 in the menus, the table's rate during a video. Kodi's log records every
  decision (`[PS5] 25 fps: VRR at 2x = … 50.000Hz`, `display mode …: VRR on;
  presenting at 50.000 Hz on the VRR link`).

### Audio

Set *Settings → System → Audio → Number of channels* to **5.1** or **7.1** for
surround tracks; Kodi's default of 2.0 downmixes them (without the LFE). Kodi
opens an 8-channel port in the console's channel order (FL FR FC LFE BL BR SL
SR); the PS5 downmixes further to what the display or receiver takes.

Dolby Digital, Dolby Digital Plus and DTS **passthrough** is offered to Kodi's
*Allow passthrough* setting (off by default) as IEC 61937 inside PCM. It needs
the PS5's *Audio Format (Priority)* at **Linear PCM** and a bit-exact path; try
it at low volume first, because a receiver that does not recognise the packets
plays them as noise.

### Switches

Create an empty file with one of these names in `/data/homebrew/PPSA99420/` and
start Kodi. The console protects files a title creates from outside processes,
so FTP cannot delete Kodi's data; the first two let Kodi do it. There are no
feature switches: this is an alpha, every feature is on, and the logs decide
what gets fixed.

| File | Effect |
| --- | --- |
| `kodi-reset` | wipe Kodi's save data once, then start fresh |
| `kodi-uninstall` | wipe Kodi's save data and quit |
| `kodi-debug` | debug-level logging (slower; remove when done) |
| `kodi-home-data` | *(development)* keep Kodi's data in `/data/kodi` instead of the save data, when the sandbox is open |

## Building

Kodi is not forked. This repository is an **overlay**: a `ps5` platform directory
copied on top of a stock Kodi checkout, nineteen small Kodi patches, C shims that
fill gaps in what a title's system libraries provide, and the scripts that set
up the cross toolchain, configure, build and package.

**Host:** Linux, or Windows 11 with WSL2 (Ubuntu 24.04). 8+ cores and ~40 GB free
disk recommended; the toolchain and library set take 1–3 hours to build once,
Kodi itself about as long again. Builds live on the Linux filesystem.

```bash
git clone https://github.com/VivaLaVent/kodi-ps5.git ~/kodi-ps5-src
cd ~/kodi-ps5-src

# 0. Toolchain: ps5-payload-sdk + pacbrew libraries + ps5-opengl + native-app template (1–3 h)
bash scripts/00-setup-wsl.sh
#    If a pacbrew package fails on a flaky download, resume from it:
#    bash scripts/01-pacbrew-resume.sh <package>

# 1. Kodi source (master / 22.x; the patches are maintained against it)
git clone https://github.com/xbmc/xbmc.git ~/kodi

# 2. Host tools and the libraries nobody packages
bash scripts/10-build-host-tools.sh      # TexturePacker + JsonSchemaBuilder, native
bash scripts/11-build-tinyxml.sh         # TinyXML 2.6.2 into the sysroot
bash scripts/21-rebuild-libiconv.sh    # add CP437 to pacbrew libiconv (extra encodings); needed for add-on zips
bash scripts/12-build-libuuid-shim.sh    # small libuuid (crossguid) + libprocstat stub (exiv2)
bash scripts/13-build-brotli.sh          # brotli for Kodi's internal exiv2
bash scripts/14-sysroot-pc-files.sh      # .pc files pacbrew does not install (sqlite3)
bash scripts/15-build-dav1d.sh           # dav1d AV1 decoder (needs nasm on the host)
bash scripts/16-build-ffmpeg.sh          # FFmpeg 7.1 with libdav1d (Kodi needs >= 7.1)
bash scripts/17-build-sce-stubs.sh       # link stubs: libSceVideodec2, extended libSceVideoOut
bash scripts/18-build-ps5-opengl.sh      # ps5-opengl SDK with Kodi's additions (patches/ps5-opengl)
bash scripts/19-build-python.sh          # optional: static CPython 3.14 -> Python add-ons (needs java; configure builds SWIG)

# 3. Configure (applies overlay + patches), build, package
bash scripts/20-configure-kodi.sh
cmake --build ~/kodi-ps5-build -j$(nproc)
bash scripts/30-deploy.sh                # -> ~/kodi-ps5-stage/app/dist/PPSA99420/
```

The overlay is copied with a content check, the patch folder must match
`patches/kodi/manifest.txt` (a release zip extracted over an older checkout
never deletes files), and packaging refuses stale builds.

<details>
<summary><b>Repository layout</b></summary>

```
toolchain/ps5-kodi.cmake        wraps the SDK's prospero.cmake, selects CORE_SYSTEM_NAME=ps5
overlay/                        copied onto a Kodi checkout by scripts/20-configure-kodi.sh
  cmake/platform/ps5/           platform selection and dependency exclusions
  cmake/scripts/ps5/            ArchSetup / PathSetup / Install / Macros for the ps5 core system
  cmake/treedata/ps5/           which xbmc/ subdirectories are compiled
  cmake/installdata/ps5/        extra files installed (system/advancedsettings.xml: PS5 defaults)
  system/advancedsettings.xml   Kodi defaults for the console (curl IPv4-only)
  xbmc/platform/ps5/            main.cpp, CPlatformPS5, CPU/GPU info, klog log sink, strptime
    audio/ input/ network/ storage/    AESinkPS5, PS5PadInput + the native keyboard (PS5ImeDialog), NetworkPS5, PS5StorageProvider
    filesystem/                 smb:// over libsmb2 (SMB2Session, CSMB2File, CSMB2Directory)
    video/                      hardware decoder (CVideoDec2, CDVDVideoCodecPS5), zero-copy buffers and renderer
    sce/                        clean-room prototypes of the Sony libraries used
  xbmc/windowing/ps5/           CWinSystemPS5, CWinSystemPS5GLContext (EGL), VRR pacing, HDR output
patches/kodi/                   nineteen Kodi patches (charset, SMB hooks, log sink, renderer, refresh, HDR framebuffer, HLG shader, native keyboard, curl idle-close off-thread and waits, binary add-on loader seam, Python init reporting) + manifest
patches/ps5-opengl/             Kodi's additions to the GL driver/runtime (zero-copy textures, HDR scanout switch), written against the ps5-opengl revision in PS5-OPENGL-COMMIT
patches/                        fix for older native-app template converters
shims/native-app/               C library gaps, compiled into the title
shims/libuuid/ shims/libprocstat/   minimal libraries for crossguid and exiv2
shims/sce_stubs/                link stub for libSceVideodec2 (the SDK has none)
pacbrew/ffmpeg/ pacbrew/dav1d/  PKGBUILDs for FFmpeg 7.1 and dav1d (scripts/15, 16)
scripts/                        00 setup · 01 pacbrew resume · 10–18 dependencies · 20 configure · 30 package
title/sce_sys/                  Kodi's icon
```
</details>

<details>
<summary><b>How it works</b></summary>

| Piece | What it does |
| --- | --- |
| Graphics | OpenGL 4.6 Core via ps5-opengl's Mesa/Gallium build; EGL default display |
| Video decoding | `CDVDVideoCodecPS5` on the hardware decoder (libSceVideodec2). Frames are shown zero-copy: GL textures lie over the decoder's frames (driver additions), a frame returns to the decoder when Kodi releases the picture. 10-bit output arrives lower-aligned in 16-bit words |
| HDR output | for PQ video the scanout buffers switch to the platform's HDR 10-bit format in place (`sceVideoOutSubmitChangeBufferAttribute2`); Kodi renders into a 10-bit target that a final pass packs into the 8-bit framebuffer; the GUI is composited in PQ with Kodi's own compositing path |
| Display timing | the system rate (59.94 Hz) always; on the PS5's VRR link Kodi paces presentation, at the video's VRR rate during playback |
| A/V sync | Kodi's own model, unchanged: audio is the master clock, tied to the audio hardware by the sink's blocking writes and its delay report (the playing block's remaining time plus what is assembled); video is scheduled against that clock and late frames are dropped by Kodi's render manager. Hardware pictures are stamped in display order with a bounded timestamp set (a picture the decoder skips cannot leave video permanently behind). On the paced VRR link the window system reports the presentation latency (one period, plus each frame's wait for its tick) so Kodi schedules against the moment a frame actually reaches the screen. Zero-copy and copying pictures, hardware and software decoding, all take the same path |
| Audio | `AESinkPS5`: 48 kHz, 2 or 8 channels on the system audio port; the blocking write is the clock. Passthrough = IEC 61937 in 16-bit stereo PCM at 48/192 kHz |
| Input | `PS5PadInput`: DualSense polled at 125 Hz, mapped to Kodi keyboard events |
| Network sources | `smb://` on libsmb2, NFS on libnfs, UPnP |
| Logging | every log line goes to klog (`PS5InterfaceForCLog`) as well as `kodi.log` |
| C library gaps | `shims/native-app/`: resolver (`getaddrinfo` on `sceNetResolver`), locale, directory reading, time, 8 MiB thread stacks, direct-memory heap |
| Packaging | ps5-opengl's native-app template: `eboot.bin` + `sce_module/` + `sce_sys/` + Kodi's data in `share/` |
</details>

<details>
<summary><b>Platform notes</b> (things that differ from a FreeBSD desktop)</summary>

- **Missing modules.** Titles do not get `libScePosixForWebKit` or
  `libkernel_sys`; anything only they export jumps to address 0. Their link
  stubs are removed so such symbols fail at link time and get a shim instead.
- **Memory.** A title's *flexible* memory (`mmap`) is only 448 MiB; the malloc
  heap is carved out of *direct* memory instead (`heap_dmem.c`). Thread stacks
  default to 64 KiB and are raised to 8 MiB (`thread_stack.c`): 1 MiB overflowed
  during thumbnail extraction.
- **ABI mismatches with the system C library:** `struct lconv` field order
  (own `localeconv`), 16-bit `wchar_t` on the PlayStation compiler target
  (patch 0005), directories that need 64 KiB `getdents` buffers (own
  `opendir`/`readdir`).
- **Sandbox.** `lstat` on the title's own mount points fails (SQLite gets a
  patched system call); files the title creates cannot be deleted from outside;
  USB drives and `/data` are not visible to a jailed title. A resident jailbreak
  daemon opens the sandbox on request (`{"PID":"<pid>"}` in
  `/download0/etahen_jailbreak`); system modules must be loaded, and graphics
  and VideoOut brought up, before that - afterwards those fail.
- **Media category.** Media apps get half the page tables and a stricter
  sandbox in which the GL driver fails (`EGL_BAD_ALLOC`), so Kodi is a Games title.
- **Display.** The GL driver's render size is a build profile (2160p60 by
  default, `PS5_SCANOUT_HEIGHT` in `scripts/18-build-ps5-opengl.sh`). Explicit
  refresh rates through the mode API are refused (`UNSUPPORTED_OUTPUT_MODE`), so
  fixed 24/25/50 Hz are not available to titles. With the PS5's VRR on, the
  system keeps the title on a ~120 Hz VRR link that follows the title's
  presentation, so Kodi's VRR is frame pacing on that link (checked every 2
  seconds). The high-refresh preset's unpeg (`sceVideoOutVrrUnpegFromFixedRate`)
  is refused (`0x8029001c`), so Kodi does not use that route.
- **HDR.** Registering scanout buffers in the HDR format needs HDR-capable
  title metadata (`attribute` in `param.json`); with it, the PS5 keeps the TV in
  HDR for the whole session. Re-registering buffers in place is refused
  (`SLOT_OCCUPIED`); `sceVideoOutSubmitChangeBufferAttribute2` switches the
  format at the next flip.
- **GL driver.** 2D R8/RG8 textures are tiled and uploaded pixel by pixel, so
  video frames use rectangle textures (patch 0008); the driver reports wrong
  buffer ages, so Kodi redraws the whole screen each frame. Zero-copy video uses
  linear 2D textures over the decoder's memory (row pitch a multiple of 256
  bytes), which the driver additions make available as EGL images.
- **Dynamic linking.** A title cannot resolve symbols by name
  (`sceKernelDlsym` fails), and the loader leaves weak imports empty; functions
  the SDK's stubs lack (e.g. the VRR unpeg) come from an extended link stub.
- **C library.** A title's C library is the template's clean-room `libc.prx`,
  not FreeBSD's, while the SDK headers are FreeBSD's. Its `FILE` layout
  differs, so C code must not use the headers' inline stdio macros
  (`getc_unlocked`, `fileno`; Python is built without them); it has no
  `getcwd`, and `fopen` does not set POSIX errno values (both shimmed).
- **Python's sockets** run on `libSceNet`: the socket module is redirected
  at compile time (`pacbrew/python3/ps5_pysocket.h`), because the title may
  not set sockets non-blocking with `ioctl(FIONBIO)` and `sceNetSocket`
  rejects the `SOCK_CLOEXEC` flag. Kodi's own networking (curl, SMB) is
  untouched.
</details>

## Debugging

```bash
nc <console-ip> 3232 | tee kodi-klog.txt          # capture while launching (klogsrv on the console)
grep -a "\[kodi" kodi-klog.txt | tail -100          # Kodi's log and the port's startup markers
```

A crash prints a report with `# backtrace:`. To turn its addresses into
function names (the build keeps symbols):

```bash
N=$(grep -a -n "# backtrace:" kodi-klog.txt | tail -1 | cut -d: -f1)
sed -n "$((N+1)),$((N+40))p" kodi-klog.txt | grep -a -o "^# [0-9a-f]\{16\}" | awk '{print $2}' |
  while read a; do printf '0x%x\n' $((0x$a - 0x400000 - 1)); done |
  llvm-symbolizer-18 --obj=$HOME/kodi-ps5-stage/app/build/llvm-pie.elf --demangle --inlining=false -p
```

## Roadmap

1. Binary add-ons: an in-process ELF loader (`overlay/xbmc/platform/ps5/elf/`,
   host-tested) for `inputstream.adaptive`; Pillow built into the interpreter.
2. Add-on install speed.
3. Thread stacks out of the title's small flexible-memory pool (it also
   holds the hardware decoder's workspace).
4. The player debug overlay (L3) during VRR: keep the paced rate
   (`kodi-debug` logs presented frames, pacing and render time every 5 s).
5. GL driver: cheaper clears and draws at 4K, runtime-selected render size.
6. A DualSense joystick driver.
7. Passthrough confirmation: whether the PS5 passes IEC 61937 PCM through
   bit-exactly (2 channels first, then the 8-channel HBR formats).

## Comparison with upstream

`docs/upstream-comparison.md` sets the PS5 implementation of each feature next
to Kodi's own platforms (GBM, VAAPI, ALSA, the Linux storage provider): which
differences the console forces, which were accidental, and what was done.

## Validation

`docs/validation.md` is the test matrix, one row per codec and resolution,
with the log lines each number is read from. Kodi's hardware decoder logs a
summary line per stream (pictures decoded, achieved rate, decode times, decodes
over one frame period), so a run through the matrix produces the table.

## Contributing

Bug reports with a klog capture (see Debugging) and your firmware/loader
versions are the most useful thing. Pull requests welcome. Keep line endings
LF (enforced by `.gitattributes`); the scripts are bash and break on CRLF.

Join the [Discord server](https://discord.gg/YB58bUZrqu) for help, test reports
and development news.

## Acknowledgements

- John Törnblom and contributors: [ps5-payload-dev](https://github.com/ps5-payload-dev)
  SDK, pacbrew-repo, ftpsrv, klogsrv.
- BlackBearReloaded: [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl),
  the PS5 native-app template, and the PS5 hardware video and audio decoding
  research (console-proven VideoDec2 modes, VP9, low-aligned 10-bit surfaces,
  the AudioOut formats).
- ProsperoLight: reference for direct-memory allocation, the high-refresh
  entitlement, VRR and the HDR scanout format on a PS5 title.
- The prosper project, for the VP9 codec value.
- [EVO Player](https://github.com/sainsaji/EVO-PLAYER-PS5): the hardware-verified
  8-channel order, the sandbox-open protocol and what must precede it, and the
  in-place HDR switch (and why not to re-register buffers).
- Ronnie Sahlberg: [libsmb2](https://github.com/sahlberg/libsmb2).
- The PS5 SDL backend, whose observations of the audio and pad libraries the
  `sce/` headers restate.
- Team Kodi, for Kodi itself.

## License

GPL-2.0-or-later, the same license as Kodi (see [LICENSE](LICENSE)). Files under
`overlay/` carry Kodi's standard file header because they are written to be
upstreamed into Kodi's tree.

This project is for running free software on hardware you own. It does not
enable, and must not be used for, copyright infringement of any kind.
