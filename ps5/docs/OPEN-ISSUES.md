# PS5MC — open issues

Kept current; one line of status and the next action for each. History and
reasoning live in `ARCHITECTURE-LESSONS.md`.

## Fix in hand, confirm on the console

**Stop froze the picture; hardware decoding dead until a reboot (green
screen after)** - Kodi 21, 1.2, after stopping a 4K 10-bit film. Logs: the
stop completed in Kodi, then the UI thread never logged again; after a
relaunch `sceVideodec2CreateDecoder failed (0x811d0100)` and the software
fallback showed green. Defect found in the code (both Kodi versions): Kodi
deletes the renderer without UnInit, and our destructor released the
zero-copy pictures - possibly the decoder's last owners, freeing its frame
memory - before any glFinish and before destroying the textures/EGL images
over that memory. Fixed: GPU finish, then textures, then pictures (destructor,
UnInit, and any ReleaseBuffer that holds a decoder's last reference); fences
waited (bounded) on direct releases; the texture cache is keyed by decoder
instance. *Confirm:* stop several 4K 10-bit films; the klog shows
`[ps5vdec] decoder N: closing / deleted / compute queue released / closed`.
If it still freezes, the last `[ps5vdec]` line names the call that blocked.
Also seen in the same session: `sceVideodec2Decode failed (0x811d0303)` runs
(12 in a row) mid-stream, recovered by the resync; watch whether they precede
a freeze.

**Audio passthrough** (1.3): HDMI bitstream ports per sainsaji's research -
Ex modes 0 (AC-3), 2 (DTS), 3 (E-AC-3/Atmos), 4 (DTS-HD, experimental) and Sys
mode 5 (TrueHD as MAT); Kodi's own IEC 61937 packing. *Confirm per format:*
the receiver names it; the klog shows our `HDMI bitstream: <format> via Ex|Sys
mode N` and the console's `[AvControl] audio: ... fmt:BITSTREAM <format>`
(`ENCODE_DOLBY` would mean re-encoding, not passthrough). Watch for: a receiver
stuck on the previous format after many switches (unsolved in the research
too; settle times are in place), TrueHD shown as "Dolby Audio" rather than
"Dolby TrueHD" (the research's simplified MAT did that - Kodi's MAT packer is
the full one), the Atmos indicator, and A/V offset. Still open: a route for
DTS-HD Master Audio (8-channel high bit rate); Kodi sends its core meanwhile.

**Kodi 21: skin icons, buttons and highlights missing.** Cause: both builds
used one host TexturePacker built from Kodi 22, which writes texture bundles in
XBTF version 3; Kodi 21 reads only version 2 and rejects the skin's
`Textures.xbt`. Host tools are now built per Kodi major from the tree being
configured, and the deploy refuses a bundle the target cannot read.
*Confirm:* the 21 configure log shows `host tools for Kodi 21`, the deploy
`texture bundles: N, XBTF version 2..2 ok`, and the skin renders fully.

**Loose end from that investigation.** Before the cause was known, a
diagnostic build saw `GL_INVALID_VALUE` on two of three 1920x1080 skin-image
uploads on Kodi 21. *Next:* once the 21 skin renders, check that no
full-screen image is missing; if one is, instrument that upload.

## Open

**Closing an idle HTTP(S) connection can stall** (patch 0016 moved it off the
GUI thread: it froze the GUI for ~35 s). Possibly also delays the start of
HTTP playback after a HEAD request. liujiny's kodi-ps5 fork patches libcurl to
read with `MSG_DONTWAIT` there, on the theory that `fcntl(O_NONBLOCK)` has no
effect on PS5 sockets - simulated on a PC, not verified on hardware.
*Measure first:* with `kodi-debug` on, the network self-test logs
`sockets: fcntl(O_NONBLOCK) ... -> non-blocking|BLOCKED`, the same for
`MSG_DONTWAIT` and sceNet `SO_NBIO`, and a `sockets verdict:` line. If
`fcntl` is BLOCKED but `SO_NBIO` works, fix it once in the `fcntl` shim (set
`SO_NBIO` too: covers curl, its TLS shutdown, libsmb2, UPnP); if `fcntl`
works, the stall is elsewhere (TLS shutdown).

**UPnP discovery** (fixed in 1.3, from liujiny's fork): Neptune now lists
interfaces with `getifaddrs`. *Confirm:* the self-test line `interfaces:
ioctl(SIOCGIFCONF) REFUSED ...` shows the old path failed; Add videos >
Browse > UPnP devices lists a DLNA server on the network.

**Intermittent black screen on some 4K HDR films** (sound plays, menus
invisible). No reproducer yet: the one reported file turned out to be Dolby
Vision profile 5 (now decoded in software). Diagnostics in place: a GL error
pending at the HDR pack is logged (sampled once a second), and `kodi-debug`
logs decoded/presented frame counts. *Next:* when it happens, capture the klog
with `kodi-debug` on (the switch also works from the `/data` home, see the
README).

**Binary add-ons** (inputstream.adaptive, Pillow, pycryptodome) do not load.
The in-process ELF loader runs and faulted in `ps5elf::resolve`; it writes a
klog trace before each step. The test add-on ships in `ps5/addons/test.binary.ps5`
(stage with `ps5/tools/ps5-stage2-binary-addon.sh`). *Next:* enable it, capture
`[ps5elf]` lines, fix the failing step.

**Add-on installs are slow.** Downloads are fast; curl, session teardown,
SQLite and dependency resolution were measured and ruled out. The unpack/write
phase was unmeasured. *Measuring (1.3, patch 0037, always on):* every install
logs `CAddonInstallJob[id]: installed in N ms: download/verify N ms, unload N
ms, install (dependencies + files) N ms, reload N ms, post-install N ms` and
`CFilesystemInstaller[id]: N files, N KiB: unpacked in N ms, moved in N ms,
old version removed in N ms`. Install two or three add-ons of different
sizes; time per file vs per KiB says whether it is file creation on /data or
the copy itself. Then fix that step.

**Film at 59.94 Hz without VRR (3:2 judder).** EVO Player's hardware research
(docs/hardware/refresh-rate-modes.md in its repo, FW 12.70, PS5 Pro): an app
gets only two output modes through `sceVideoOutConfigureOutput(handle, mode,
NULL, NULL, 0)` - 0x1 (system default) and 0xf (119.88 Hz); 23.976/24/50 Hz
(modes 2/3/9) are refused (`0x8029001e`), the Blu-ray player's Ex call needs a
system-app capability. EVO falls back to 119.88 Hz for film (5 refreshes per
frame: no judder) when the display has 120 Hz. We already have both pieces
(`kOutputModeHighRefresh` = 0xf, `SetOutputMode`). Careful: our patch 0010 once
chose 119.88 Hz for film; 0012 replaced it with VRR-only, and why is not
recorded (the "blanked the TV" note in WinSystemPS5 concerns the VRR unpeg,
not 0xf). Also, `RefreshLinkState` treats any rate above 100 Hz as a VRR link
and re-checks every 2 s - a fixed 119.88 Hz would be misread. *Next:* a
switch-gated experiment (fixed 0xf for 23.976/24 fps when there is no VRR
link and the display supports 120 Hz; its own state, excluded from the VRR
link check; verify the measured rate and fall back to 0x1 otherwise), tested
on the console before it becomes a setting.

**Dolby Vision profile 5** has no HDR10 base layer and shows a green/purple
tint. Since 1.2 it is routed to FFmpeg (software), but software decoding does
not apply Dolby Vision's per-frame colour reshaping either, so the colours are
most likely still wrong - *unverified on the console*. *Next:* play a profile 5
file on 1.2; if it is still tinted, route it back to the hardware decoder (same
colours, smoother at 4K). A real fix reshapes it per frame on the GPU from the
RPU (Nuvio-PS5 does this; it is GPL-3, so reimplement from the specification
rather than copy). Large feature, not started.

**Thread stacks and flexible memory.** Every thread gets an 8 MiB stack
(`shims/native-app/thread_stack.c`; 1 MiB overflowed during thumbnail
extraction). A title's flexible-memory pool is 448 MiB and shared with the
video decoder's workspace; if thread stacks are committed from it, ~40 threads
would take most of it. *Unmeasured.* *Next:* log flexible memory available at
startup and during 4K playback (`sceKernelAvailableFlexibleMemorySize`, if the
SDK stubs export it). Only if it is short: move stacks to direct memory -
which then means owning stack lifetime for detached threads.

**SMB account lockout** is now reported as such (no password re-prompt, no
retry storm). *Next:* confirm on the console after unlocking the account on
the server.

## Upstream status (2026-10-09)

- **Kodi:** `Omega` (21) unchanged since June - 21.3 is current. `Piers` (22)
  has 71 commits since our base (backports towards 22.0: SMB seek, an audio
  crash, Python/OpenSSL/expat, image orientation). One file both sides change,
  `xbmc/windowing/Resolution.cpp` (our refresh patches 0010/0012); the trial
  rebase is clean. Take it at the 22.0 tag: `ONLY=22 ONTO=22.0-Piers bash
  ps5/scripts/50-upstream-update.sh --apply`.
- **ps5-opengl:** 58 commits since our pin `122aa899` (2026-09-25): SDK 1.0.1
  (vertex-buffer references), scanout pool flush and re-arm on shutdown,
  compute/geometry/draw paths, EGL context creation, signed releases. All nine
  Kodi additions still find their places; the three files they patch changed
  by ~1600 lines, so it needs a build and the GPU checks on the console before
  the pin moves (`bash ps5/scripts/51-try-ps5-opengl.sh`). Worth offering our
  two additions (EGL images over decoder memory, scanout format switch)
  upstream: then updates need no patching.

## Closed recently (for reference)

- Memory figure in System info (real heap figure).
- Build stamp in every log; contentVersion rises per release.
- Kodi 21 black skin background and splash: `GL_BGRA` uploads (0033).
- Release publish gate counted 4 assets as an error (zip + exFAT per variant).
- A failed build left `BuildStamp.h` modified and the wrong branch checked out.
- `kodi-reset`/`kodi-uninstall` on one build also wiped the other build's
  library.
