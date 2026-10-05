/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/AudioEngine/Interfaces/AESink.h"
#include "cores/AudioEngine/Utils/AEDeviceInfo.h"

#include <cstdint>
#include <memory>
#include <string>
#include <chrono>
#include <vector>

/*!
 * \brief Audio sink on top of libSceAudioOut (main port, 48 kHz, stereo).
 *
 * The hardware port consumes fixed blocks of `grain` frames; sceAudioOutOutput()
 * blocks until the previously queued block has played, so writing to it paces
 * the ActiveAE sink thread the same way ALSA's blocking write does. With
 * kodi-multichannel the port is 8-channel (5.1/7.1 content); passthrough is a
 * follow-up.
 */
class CAESinkPS5 : public IAESink
{
public:
  const char* GetName() override { return "PS5"; }

  CAESinkPS5() = default;
  ~CAESinkPS5() override;

  static void Register();
  static std::unique_ptr<IAESink> Create(std::string& device, AEAudioFormat& desiredFormat);
  static void EnumerateDevicesEx(AEDeviceInfoList& list, bool force);

  bool Initialize(AEAudioFormat& format, std::string& device) override;
  void Deinitialize() override;

  double GetCacheTotal() override;
  double GetLatency() override;
  unsigned int AddPackets(uint8_t** data, unsigned int frames, unsigned int offset) override;
  void GetDelay(AEDelayStatus& status) override;
  void Drain() override;

private:
  bool Output(const uint8_t* block);
  bool OpenPort(AEAudioFormat& format, bool eight);
  bool OpenPassthroughPort(AEAudioFormat& format);

  static constexpr unsigned int GRAIN_FRAMES = 1024; // ~21.3 ms at 48 kHz
  static constexpr unsigned int QUEUE_DEPTH = 2; // blocks the port holds

  int32_t m_handle{-1};
  unsigned int m_channels{2};
  unsigned int m_sampleRate{48000}; // 192000 for E-AC3 passthrough
  unsigned int m_frameSize{0};
  bool m_hbrSwap{false}; // 8-channel passthrough: words 2 and 3 swapped (FC/LFE slots)
  std::vector<uint8_t> m_block; // one grain being assembled
  unsigned int m_blockFrames{0}; // frames currently in m_block
  std::chrono::steady_clock::time_point m_blockStarted{}; // when the playing block began
};
