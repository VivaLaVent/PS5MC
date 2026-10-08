# PS5MC — open issues

Kept current; one line of status and the next action for each. History and
reasoning live in `ARCHITECTURE-LESSONS.md`.

## Fix in hand, confirm on the console

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
phase is unmeasured. *Next:* one timer around it (debug logging on), then fix.

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

## Closed recently (for reference)

- Memory figure in System info (real heap figure).
- Build stamp in every log; contentVersion rises per release.
- Kodi 21 black skin background and splash: `GL_BGRA` uploads (0033).
- Release publish gate counted 4 assets as an error (zip + exFAT per variant).
- A failed build left `BuildStamp.h` modified and the wrong branch checked out.
- `kodi-reset`/`kodi-uninstall` on one build also wiped the other build's
  library.
