/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*
 * Clean-room declarations for the PS5 audio output library (libSceAudioOut).
 * Behaviour, as observed by the public PS5 SDL backend
 * (github.com/ps5-payload-dev/SDL, src/audio/ps5):
 *   - the port runs at 48 kHz; `len` is frames per sceAudioOutOutput() call
 *     and must be one of 256, 512, 768, 1024, 1280, 1536, 1792, 2048;
 *   - sceAudioOutOutput() blocks until the previously queued block has been
 *     consumed, so it doubles as the pacing clock;
 *   - sceAudioOutOutput(handle, nullptr) waits for the queue to drain
 *     (PS4 SDK semantics; verify on hardware).
 */

#include <cstdint>

extern "C"
{
  int32_t sceAudioOutInit(void);
  int32_t sceAudioOutOpen(int32_t userId,
                          int32_t type,
                          int32_t index,
                          uint32_t len,
                          uint32_t freq,
                          uint32_t param);
  int32_t sceAudioOutOutput(int32_t handle, const void* buffer);
  // flag: one bit per channel (L, R, C, LFE, LS, RS, LE, RE); vol: 8 entries
  int32_t sceAudioOutSetVolume(int32_t handle, int32_t flag, int32_t* vol);
  int32_t sceAudioOutClose(int32_t handle);

  // HDMI bitstream output (audio passthrough). Not in the SDK headers, but the
  // payload SDK's libSceAudioOut stub exports them; they must be linked, not
  // looked up (sceKernelDlsym is refused in the title sandbox). Found and
  // verified on hardware by sainsaji: github.com/sainsaji/PS5-Audio-Passthrough-Research
  // (GPL-3.0; only the documented calls and constants are used here).
  //   Ex*: Dolby Digital, DTS, Dolby Digital Plus, DTS-HD (the citroncore path)
  //   Sys*: Dolby TrueHD as MAT (the Blu-ray player's path)
  int32_t sceAudioOutExOpen(int32_t userId, int32_t mode); // S16 stereo port
  int32_t sceAudioOutExClose(int32_t handle);
  // zero must be 0; target 1 is what Sony's own player passes
  int32_t sceAudioOutExConfigureOutput(int32_t zero, uint32_t flags, int32_t mode,
                                       int32_t target, uint64_t option);
  int32_t sceAudioOutSysOpen(int32_t userId, int32_t mode); // mode 5: S16 8-channel
  int32_t sceAudioOutSysClose(int32_t handle);
  // type 1 = HDMI
  int32_t sceAudioOutSysConfigureOutput(int32_t type, uint32_t flags, int32_t mode,
                                        int32_t target, uint64_t option);
  // type 1 = HDMI; size must be AUDIO_OUT_HDMI_MONITOR_INFO_SIZE
  int32_t sceAudioOutSysGetHdmiMonitorInfo(int32_t type, void* info, uint32_t size);
}

namespace KODI::PLATFORM::PS5
{

constexpr int32_t AUDIO_OUT_PORT_TYPE_MAIN = 0;
// Open the port on behalf of the system user so no login is required.
constexpr int32_t AUDIO_OUT_USER_ID_SYSTEM = 0xFF;

// `param` values for sceAudioOutOpen() (PS4 numbering; stereo verified by the
// SDL port, 8-channel by EVO Player).
constexpr uint32_t AUDIO_OUT_FORMAT_S16_MONO = 0;
constexpr uint32_t AUDIO_OUT_FORMAT_S16_STEREO = 1;
constexpr uint32_t AUDIO_OUT_FORMAT_S16_8CH = 2;
constexpr uint32_t AUDIO_OUT_FORMAT_FLOAT_MONO = 3;
constexpr uint32_t AUDIO_OUT_FORMAT_FLOAT_STEREO = 4;
constexpr uint32_t AUDIO_OUT_FORMAT_FLOAT_8CH = 5;
// 8 channels interleave FL FR FC LFE BL BR SL SR (verified on hardware)

constexpr uint32_t AUDIO_OUT_SAMPLE_RATE = 48000;
// the other rate the port accepts (BlackBearReloaded's audio research)
constexpr uint32_t AUDIO_OUT_SAMPLE_RATE_HIGH = 192000;
constexpr int32_t AUDIO_OUT_VOLUME_0DB = 0x8000;
constexpr int32_t AUDIO_OUT_VOLUME_ALL_CHANNELS = 0xff;

// HDMI bitstream output
constexpr int32_t AUDIO_OUT_HDMI = 1;                // Sys* "type", HDMI monitor info
constexpr int32_t AUDIO_OUT_BITSTREAM_TARGET = 1;    // what Sony's own player passes
constexpr int32_t AUDIO_OUT_MODE_DEFAULT = 0xFF;     // back to normal (PCM) output
constexpr uint32_t AUDIO_OUT_HDMI_MONITOR_INFO_SIZE = 0x180;
// in the monitor info: number of short audio descriptors (32-bit), then the
// 8-byte descriptors {CEA-861 coding type, channels, rate mask, ...}
constexpr uint32_t AUDIO_OUT_HDMI_SAD_COUNT_OFFSET = 0x94;
constexpr uint32_t AUDIO_OUT_HDMI_SAD_OFFSET = 0x98;
constexpr uint32_t AUDIO_OUT_HDMI_NAME_OFFSET = 0x0B;

} // namespace KODI::PLATFORM::PS5
