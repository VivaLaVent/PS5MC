# Validation matrix

The same kind of run EVO Player documents: every codec and resolution in the
table below, played in full at real time, with the numbers taken from Kodi's
log rather than from impressions. Capture the log with klogsrv
(`nc <console-ip> 3232 | tee validation.txt`), play each clip for at least 60
seconds, stop it, and read the lines below.

## Where the numbers come from

- **Hardware decoding** (H.264, HEVC, VP9): one line per stream when it
  closes, always logged:
  `CDVDVideoCodecPS5: HEVC Main10 3840x2160 at 23.976 fps, zero-copy: 1439
  pictures in 60.0 s (24.0/s), decode 6.1 ms average / 11.2 ms longest, 0 over
  one frame period`
  Real time means the achieved rate matches the stream's rate and the
  over-budget count stays at 0 (a decode longer than one frame period is a
  frame the player would have to drop).
- **Software decoding** (AV1, MPEG-2, anything FFmpeg takes): Kodi's player
  info overlay during playback (codec info, `O` on a keyboard) shows dropped
  and skipped frames; note them at the end of the clip.
- **Presentation** (with `kodi-debug`): every 5 seconds
  `PS5 presentation (kodi-debug): 50.0 frames/s (…), paced at 50.000 Hz, VRR
  link, render 3.2 ms average / 5.8 ms longest` - the display rate and the
  frame cost, for 4K rendering and the VRR pacing.
- **Audio**: `CAESinkPS5: opened main port, 48000 Hz, 8 ch (FL FR FC LFE BL BR
  SL SR) …` for the port, and your ears for the speaker placement (a channel
  test clip helps). Passthrough: the receiver's own display ("Dolby Digital",
  "DTS") is the proof.

## Video

| Codec | Clip | Resolution / fps | Decoder | Achieved rate | Over budget | Dropped (OSD) | Result |
| --- | --- | --- | --- | --- | --- | --- | --- |
| H.264 High | | 1080p / 24 | hardware | | | | |
| H.264 High | | 1080p / 60 | hardware | | | | |
| H.264 High | | 4K / 30 | hardware | | | | |
| H.264 High | | 4K / 60 | hardware | | | | |
| H.264 High 10 | | 1080p | hardware or FFmpeg | | | | |
| HEVC Main | | 1080p / 24 | hardware | | | | |
| HEVC Main | | 4K / 60 | hardware | | | | |
| HEVC Main 10 (HDR10) | | 1080p / 24 | hardware | | | | |
| HEVC Main 10 (HDR10) | | 4K / 24 | hardware | | | | |
| HEVC Main 10 (HLG) | | 4K / 25 | hardware | | | | |
| HEVC 1080i (TV recording) | | 1080i / 50 | hardware + bwdif | | | | |
| VP9 Profile 0 | | 1080p / 30 | hardware | | | | |
| VP9 Profile 0 | | 4K / 60 | hardware | | | | |
| VP9 Profile 2 (HDR) | | 4K / 60 | hardware | | | | |
| AV1 | | 1080p / 24 | dav1d | | | | |
| AV1 | | 4K / 24 | dav1d | | | | |
| AV1 (Dolby Vision profile 10) | | 4K / 24 | dav1d | | | | |
| MPEG-2 | | 1080i / 25 | FFmpeg | | | | |

## Audio

Kodi's *Number of channels* at 7.1 for the surround rows.

| Format | Clip | Channels | Decoder / path | Port opened | Speakers correct | Result |
| --- | --- | --- | --- | --- | --- | --- |
| AAC | | 5.1 | FFmpeg -> 8-channel PCM | | | |
| Dolby Digital (AC-3) | | 5.1 | FFmpeg -> 8-channel PCM | | | |
| Dolby Digital Plus | | 7.1 | FFmpeg -> 8-channel PCM | | | |
| Dolby Digital Plus + Atmos | | 7.1 bed | FFmpeg -> 8-channel PCM | | | |
| Dolby TrueHD | | 7.1 | FFmpeg -> 8-channel PCM | | | |
| Dolby TrueHD + Atmos | | 7.1 bed | FFmpeg -> 8-channel PCM | | | |
| DTS | | 5.1 | FFmpeg -> 8-channel PCM | | | |
| DTS-HD MA | | 7.1 | FFmpeg -> 8-channel PCM | | | |
| DTS:X | | 7.1 bed | FFmpeg -> 8-channel PCM | | | |
| FLAC | | 5.1 | FFmpeg -> 8-channel PCM | | | |
| AC-3 passthrough | | 5.1 | IEC 61937 (Kodi *Allow passthrough*) | | receiver shows Dolby Digital | |
| DTS passthrough | | 5.1 | IEC 61937 | | receiver shows DTS | |
| E-AC-3 passthrough | | 7.1 | IEC 61937 at 192 kHz | | receiver shows Dolby Digital Plus | |
| TrueHD passthrough | | 7.1 | IEC 61937 HBR (8 ch, 192 kHz) | | receiver shows TrueHD | |
| DTS-HD MA passthrough | | 7.1 | IEC 61937 HBR (8 ch, 192 kHz) | | receiver shows DTS-HD MA | |

## Result

Filled-in tables belong in a GitHub issue or the Discord, with the console's
firmware, loader and the log file. Rows that fail are bugs to fix, not
footnotes.
