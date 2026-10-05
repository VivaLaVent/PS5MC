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
