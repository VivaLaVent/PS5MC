# PS5 implementation vs. upstream Kodi

Where the PS5 platform code differs from what Kodi's own platforms do, and
whether the difference is forced by the console or accidental. Accidental
differences are bugs; the last column says what was done about them.

| Area | Upstream reference | PS5 | Difference | Verdict |
| --- | --- | --- | --- | --- |
| Window system | `CWinSystemGbm` / `CWinSystemGbmGLContext` (Linux DRM) | `CWinSystemPS5` / `CWinSystemPS5GLContext` (EGL on ps5-opengl) | One fixed output mode at the system rate; VRR by pacing presentation on the console's VRR link instead of KMS mode sets (the PS5 refuses explicit refresh rates from titles). | Forced |
| Refresh-rate selection | `CResolutionUtils::ChooseBestResolution` (whitelist, fixed rates) | patch 0012: a VRR mode for the video whenever *Adjust display refresh rate* is not Off; Kodi's own switch-back semantics kept | Kodi's rule "pick the mode matching the fps" cannot apply to a display that has no fixed modes; the PS5 rule picks the VRR pacing rate instead. | Forced |
| Display latency | GBM: Kodi's estimate `(buffers+1)/fps`; no platform implements `GetFrameLatencyAdjustment` | one pacing period on the VRR link, plus each frame's wait for its tick (`GetFrameLatencyAdjustment`); Kodi's estimate for fixed-rate output | Uses hooks upstream defines but does not use. Needed because the pacer adds a per-frame wait upstream displays do not have. | Extension |
| HDR output | GBM: DRM connector properties (EOTF PQ/HLG, metadata), GUI compositing in PQ (`CGuiCompositeShaderGL`) | scanout format switch in place (`sceVideoOutSubmitChangeBufferAttribute2`), 10-bit intermediate target packed for the 8-bit framebuffer (patch 0013), the same GUI compositor; HLG converted to PQ in the YUV shader (patch 0014), since the platform's HDR format is PQ only | Mechanism forced by the platform; the compositor is upstream's. `IsHdrComposite()` (compensated GUI blending) and `GetDisplayHDRCapabilities()` were missing. | Fixed: both added |
| Video decoding | `CDVDVideoCodecVAAPI`, `MediaCodec`: hardware decoders with Kodi's `CVideoBuffer` pool, drop requests honoured (`DVP_FLAG_DROPPED`), reopen on hint change (player's job) | `CDVDVideoCodecPS5` on libSceVideodec2: same interface, drop flag honoured, pictures in display order stamped with the smallest pending timestamp (the API carries none) | Timestamp inference is forced (no pts in the output). Unbounded pending set could lag video permanently after a skipped picture. | Fixed: bounded, logged |
| Zero-copy rendering | `CRendererVAAPIGL` (EGL images over VA surfaces), `CRendererDRMPRIME` | `CRendererPS5`: EGL images over the decoder's frames (driver additions), fences before frame reuse, `NeedBuffer`/`ReleaseBuffer` as VAAPI | Same shape; PS5 needs the driver additions (`patches/ps5-opengl`) since Mesa's DMA-BUF import does not exist there. `CreateTexture` did not reset `loaded` as the base does. | Fixed earlier |
| Deinterlacing | VAAPI: VPP deinterlacer; software: FFmpeg filters | hardware decode of both fields, FFmpeg `bwdif` | The PS5 decoder has no post-processing unit accessible to a title. | Forced |
| Audio sink | `CAESinkALSA`, `CAESinkAUDIOTRACK`: delay from the driver, `Drain` waits for the last sample, passthrough as IEC 61937 in PCM | `CAESinkPS5`: blocking `sceAudioOutOutput` is the clock; delay = the playing block's remaining time + assembled frames; `Drain` pads and waits; passthrough as IEC 61937 including HBR | The delay report ignored the time elapsed since the last write (up to one block of jitter in the audio clock). | Fixed |
| Channel layout | ALSA: the device's layout; AudioTrack: fixed | 8 channels in the console's verified order (FL FR FC LFE BL BR SL SR) | Order verified on hardware by EVO Player, not by Kodi's own means (none exist). | Verified |
| Storage | `CLinuxStorageProvider` + udev/UDisks: mounts, hot-plug events | `CPS5StorageProvider`: fixed mount points `/mnt/usbN`, `/mnt/extN`, `/data`; hot-plug by polling existence every 2 s | No udev on the console. `PumpDriveChangeEvents` returned false, so a drive plugged in after start did not appear. | Fixed: events added |
| Sandbox | Linux: none (full filesystem) | opened on request through the jailbreak daemon's file-drop protocol, after graphics and module loads | Console specific. | Forced |
| Network | `CNetworkPosix` (`getifaddrs`, resolv.conf, ICMP) | `CNetworkPS5`: interface data from `libSceNetCtl`, name resolution via `sceNetResolver` in the libc shim, ping via TCP connect | No `getifaddrs`, no `/etc/resolv.conf`, no raw sockets in a title; the interface (`CNetworkBase`) is fully implemented. | Forced |
| SMB | libsmbclient (`CSMBFile`, patched hooks) | libsmb2 sessions behind Kodi's `smb://` hooks (patch 0006) | libsmbclient does not build for the console. | Forced |
| Input | joystick peripheral bus + button maps | DualSense polled, mapped to keyboard actions | Interim; a joystick add-on is on the roadmap. | Roadmap |
| Python | depends: static CPython 3.14, `PYTHONHOME` from the environment | the same recipe (`pacbrew/python3`), no subprocesses (as tvOS), `PYTHONHOME` set in `main.cpp` | The cross-build whitelist patch and the module list are the only additions. | Pending build |
| Logging | Linux: `CLinuxInterfaceForCLog` (files) | files plus every line to klog (`PS5InterfaceForCLog`) | Remote log capture is how the console is debugged. | Extension |
| Thread stacks | libc default (8 MiB on Linux) | 8 MiB, via `--wrap=pthread_create` (the console's default is 64 KiB) | Same size as Linux; the console needs the wrapper. | Forced |
| Tone mapping | Kodi default: off | Kodi default: off | Unchanged. | Same |

## Method

Each row was checked by reading the upstream class next to ours: the virtual
interface (every override present, same semantics), the behaviour the player
relies on (drop requests, drain, delay, buffer lifetime), and the settings
Kodi shows depending on what the window system reports.
