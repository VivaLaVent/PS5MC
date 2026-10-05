/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AESinkPS5.h"

#include "cores/AudioEngine/AESinkFactory.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "utils/log.h"

#include "platform/ps5/sce/SceAudioOut.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <utility>

using namespace KODI::PLATFORM::PS5;

namespace
{
// 8-channel output (Kodi remaps 5.1/7.1 into it, and the PS5 downmixes to
// what the display or receiver takes). The sink declares the port's own
// channel order, which is the order Kodi writes samples in: FL FR FC LFE BL
// BR SL SR - FFmpeg's 7.1 order, verified on hardware with a 5.1 system (EVO
// Player).
//
// Passthrough (Kodi's "Allow passthrough" setting, off by default): Dolby/DTS
// as IEC 61937 packets inside 16-bit PCM - 2 channels at 48/192 kHz, and for
// TrueHD and DTS-HD (HBR) 8 channels at 192 kHz. Only works if the PS5 passes
// the PCM through bit-exactly (audio output set to Linear PCM); otherwise the
// receiver plays the packets as noise. The HBR word-to-slot mapping is a
// hypothesis: the PS5 orders its channels FL FR FC LFE ..., HDMI slots FL FR
// LFE FC ..., so words 2 and 3 are swapped.

CAEChannelInfo EightChannelLayout()
{
  CAEChannelInfo layout;
  layout += AE_CH_FL;
  layout += AE_CH_FR;
  layout += AE_CH_FC;
  layout += AE_CH_LFE;
  layout += AE_CH_BL;
  layout += AE_CH_BR;
  layout += AE_CH_SL;
  layout += AE_CH_SR;
  return layout;
}
} // namespace

CAESinkPS5::~CAESinkPS5()
{
  Deinitialize();
}

void CAESinkPS5::Register()
{
  AE::AESinkRegEntry entry;
  entry.sinkName = "PS5";
  entry.createFunc = CAESinkPS5::Create;
  entry.enumerateFunc = CAESinkPS5::EnumerateDevicesEx;
  AE::CAESinkFactory::RegisterSink(entry);
}

std::unique_ptr<IAESink> CAESinkPS5::Create(std::string& device, AEAudioFormat& desiredFormat)
{
  auto sink = std::make_unique<CAESinkPS5>();
  if (sink->Initialize(desiredFormat, device))
    return sink;
  return {};
}

void CAESinkPS5::EnumerateDevicesEx(AEDeviceInfoList& list, bool force)
{
  CAEDeviceInfo info;
  info.m_deviceName = "main";
  info.m_displayName = "PlayStation 5";
  info.m_displayNameExtra = "HDMI / headset (system mixer)";
  info.m_deviceType = AE_DEVTYPE_PCM;
  info.m_wantsIECPassthrough = false;
  info.m_channels = EightChannelLayout();
  info.m_sampleRates.push_back(AUDIO_OUT_SAMPLE_RATE);
  info.m_dataFormats.push_back(AE_FMT_FLOAT);
  info.m_dataFormats.push_back(AE_FMT_S16NE);
  // the IEC 61937 formats at the port's two rates (48/192 kHz)
  info.m_deviceType = AE_DEVTYPE_HDMI;
  info.m_wantsIECPassthrough = true;
  info.m_dataFormats.push_back(AE_FMT_RAW);
  info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_AC3);
  info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_EAC3);
  info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_DTS_512);
  info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_DTS_1024);
  info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_DTS_2048);
  info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_DTSHD_CORE);
  info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_TRUEHD);
  info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_DTSHD);
  info.m_streamTypes.push_back(CAEStreamInfo::STREAM_TYPE_DTSHD_MA);
  info.m_sampleRates.push_back(AUDIO_OUT_SAMPLE_RATE_HIGH);
  list.push_back(info);
}

bool CAESinkPS5::Initialize(AEAudioFormat& format, std::string& device)
{
  if (format.m_dataFormat == AE_FMT_RAW)
    return OpenPassthroughPort(format);
  const bool eight = format.m_channelLayout.Count() > 2;
  if (!OpenPort(format, eight) && (!eight || !OpenPort(format, false)))
    return false;
  if (eight && m_channels != 8)
    CLog::Log(LOGWARNING, "CAESinkPS5: the 8-channel port did not open, using stereo");
  return true;
}

bool CAESinkPS5::OpenPassthroughPort(AEAudioFormat& format)
{
  // Kodi hands over IEC 61937 packets as 16-bit samples at the stream's IEC
  // rate: 2 channels at 48 kHz for AC3/DTS and 192 kHz for E-AC3; 8 channels
  // at 192 kHz for the HBR formats (TrueHD, DTS-HD)
  const unsigned int rate = format.m_sampleRate;
  if (rate != AUDIO_OUT_SAMPLE_RATE && rate != AUDIO_OUT_SAMPLE_RATE_HIGH)
  {
    CLog::Log(LOGWARNING, "CAESinkPS5: passthrough at {} Hz is not possible (48/192 kHz only)",
              rate);
    return false;
  }
  const bool hbr = format.m_channelLayout.Count() > 2;
  m_channels = hbr ? 8 : 2;
  m_sampleRate = rate;
  m_frameSize = m_channels * sizeof(int16_t);
  m_hbrSwap = hbr;
  format.m_channelLayout = hbr ? EightChannelLayout() : CAEChannelInfo(AE_CH_LAYOUT_2_0);
  format.m_frames = GRAIN_FRAMES;
  format.m_frameSize = m_frameSize;

  const int32_t initResult = sceAudioOutInit();
  if (initResult < 0)
    CLog::Log(LOGDEBUG, "CAESinkPS5: sceAudioOutInit returned {:#x} (already initialised is fine)",
              static_cast<uint32_t>(initResult));
  m_handle = sceAudioOutOpen(AUDIO_OUT_USER_ID_SYSTEM, AUDIO_OUT_PORT_TYPE_MAIN, 0, GRAIN_FRAMES,
                             rate, hbr ? AUDIO_OUT_FORMAT_S16_8CH : AUDIO_OUT_FORMAT_S16_STEREO);
  if (m_handle <= 0)
  {
    CLog::Log(LOGWARNING, "CAESinkPS5: passthrough port ({} ch, {} Hz) failed: {:#x}", m_channels,
              rate, static_cast<uint32_t>(m_handle));
    m_handle = -1;
    return false;
  }
  // bit-exact at best only unattenuated
  int32_t volumes[8];
  std::fill(std::begin(volumes), std::end(volumes), AUDIO_OUT_VOLUME_0DB);
  const int32_t volumeRc = sceAudioOutSetVolume(m_handle, AUDIO_OUT_VOLUME_ALL_CHANNELS, volumes);
  m_block.assign(static_cast<size_t>(GRAIN_FRAMES) * m_frameSize, 0);
  m_blockFrames = 0;
  CLog::Log(LOGINFO,
            "CAESinkPS5: passthrough port opened: {} (IEC 61937 in 16-bit {}), {} Hz, "
            "volume 0 dB ({:#x}){}",
            CAEUtil::StreamTypeToStr(format.m_streamInfo.m_type),
            hbr ? "8-channel HBR" : "stereo", rate, static_cast<uint32_t>(volumeRc),
            m_hbrSwap ? ", words 2/3 swapped for the HDMI slot order" : "");
  return true;
}

bool CAESinkPS5::OpenPort(AEAudioFormat& format, bool eight)
{
  format.m_sampleRate = AUDIO_OUT_SAMPLE_RATE;
  m_sampleRate = AUDIO_OUT_SAMPLE_RATE;
  format.m_channelLayout = eight ? EightChannelLayout() : CAEChannelInfo(AE_CH_LAYOUT_2_0);
  m_channels = eight ? 8 : 2;

  uint32_t param;
  if (format.m_dataFormat == AE_FMT_S16NE)
  {
    param = eight ? AUDIO_OUT_FORMAT_S16_8CH : AUDIO_OUT_FORMAT_S16_STEREO;
    m_frameSize = m_channels * sizeof(int16_t);
  }
  else
  {
    format.m_dataFormat = AE_FMT_FLOAT;
    param = eight ? AUDIO_OUT_FORMAT_FLOAT_8CH : AUDIO_OUT_FORMAT_FLOAT_STEREO;
    m_frameSize = m_channels * sizeof(float);
  }
  format.m_frames = GRAIN_FRAMES;
  format.m_frameSize = m_frameSize;

  const int32_t initResult = sceAudioOutInit();
  if (initResult < 0)
    CLog::Log(LOGDEBUG, "CAESinkPS5: sceAudioOutInit returned {:#x} (already initialised is fine)",
              static_cast<uint32_t>(initResult));

  m_handle = sceAudioOutOpen(AUDIO_OUT_USER_ID_SYSTEM, AUDIO_OUT_PORT_TYPE_MAIN, 0, GRAIN_FRAMES,
                             AUDIO_OUT_SAMPLE_RATE, param);
  if (m_handle <= 0)
  {
    CLog::Log(eight ? LOGWARNING : LOGERROR, "CAESinkPS5: sceAudioOutOpen ({} ch, format {}) failed: {:#x}",
              m_channels, param, static_cast<uint32_t>(m_handle));
    m_handle = -1;
    return false;
  }
  m_block.assign(static_cast<size_t>(GRAIN_FRAMES) * m_frameSize, 0);
  m_blockFrames = 0;
  CLog::Log(LOGINFO, "CAESinkPS5: opened main port, {} Hz, {} ch{}, {} frames/block, {}",
            AUDIO_OUT_SAMPLE_RATE, m_channels,
            eight ? " (FL FR FC LFE BL BR SL SR)" : "",
            GRAIN_FRAMES, format.m_dataFormat == AE_FMT_FLOAT ? "float" : "s16");
  return true;
}

void CAESinkPS5::Deinitialize()
{
  m_blockStarted = {};
  if (m_handle > 0)
  {
    sceAudioOutClose(m_handle);
    m_handle = -1;
  }
  m_block.clear();
  m_blockFrames = 0;
}

bool CAESinkPS5::Output(const uint8_t* block)
{
  // blocks until the previous block has finished playing; this one starts then
  const int32_t result = sceAudioOutOutput(m_handle, block);
  m_blockStarted = std::chrono::steady_clock::now();
  if (result < 0)
  {
    CLog::Log(LOGERROR, "CAESinkPS5: sceAudioOutOutput failed: {:#x}", static_cast<uint32_t>(result));
    return false;
  }
  return true;
}

double CAESinkPS5::GetCacheTotal()
{
  return static_cast<double>(GRAIN_FRAMES * QUEUE_DEPTH) / m_sampleRate;
}

double CAESinkPS5::GetLatency()
{
  // Unknown mixer latency downstream of the port; the AE sync loop measures
  // the rest through GetDelay().
  return 0.0;
}

unsigned int CAESinkPS5::AddPackets(uint8_t** data, unsigned int frames, unsigned int offset)
{
  if (m_handle <= 0)
    return 0;

  const uint8_t* src = data[0] + static_cast<size_t>(offset) * m_frameSize;
  unsigned int remaining = frames;

  while (remaining > 0)
  {
    const unsigned int space = GRAIN_FRAMES - m_blockFrames;
    const unsigned int n = std::min(space, remaining);
    uint8_t* dst = m_block.data() + static_cast<size_t>(m_blockFrames) * m_frameSize;
    std::memcpy(dst, src, static_cast<size_t>(n) * m_frameSize);
    if (m_hbrSwap)
    {
      // HBR: the packet words must land in HDMI slot order; the PS5's FC (word
      // 2) goes to slot 4 and its LFE (word 3) to slot 3
      auto* words = reinterpret_cast<int16_t*>(dst);
      for (unsigned int f = 0; f < n; ++f)
        std::swap(words[f * 8 + 2], words[f * 8 + 3]);
    }
    m_blockFrames += n;
    src += static_cast<size_t>(n) * m_frameSize;
    remaining -= n;

    if (m_blockFrames == GRAIN_FRAMES)
    {
      // Blocks until the previous block has been consumed: this is our clock.
      if (!Output(m_block.data()))
        return frames - remaining;
      m_blockFrames = 0;
    }
  }
  return frames;
}

void CAESinkPS5::GetDelay(AEDelayStatus& status)
{
  // What is left of the block playing now (it started when the last blocking
  // write returned), plus what we have assembled but not yet handed over.
  // Kodi derives its audio clock from this, so it must track the real
  // buffered audio at the moment it asks, not the moment we last wrote.
  double playing = 0.0; // nothing has been written yet
  if (m_blockStarted.time_since_epoch().count() != 0)
  {
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_blockStarted).count();
    playing = std::max(0.0, static_cast<double>(GRAIN_FRAMES) / m_sampleRate - elapsed);
  }
  status.SetDelay(playing + static_cast<double>(m_blockFrames) / m_sampleRate);
}

void CAESinkPS5::Drain()
{
  if (m_handle <= 0)
    return;

  if (m_blockFrames > 0)
  {
    std::memset(m_block.data() + static_cast<size_t>(m_blockFrames) * m_frameSize, 0,
                static_cast<size_t>(GRAIN_FRAMES - m_blockFrames) * m_frameSize);
    Output(m_block.data());
    m_blockFrames = 0;
  }
  // A null buffer waits for the queue to run dry (PS4 semantics, see header).
  sceAudioOutOutput(m_handle, nullptr);
}
