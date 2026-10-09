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
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace KODI::PLATFORM::PS5;

namespace
{
// 8-channel output (Kodi remaps 5.1/7.1 into it, and the PS5 downmixes to
// what the display or receiver takes). The sink declares the port's own
// channel order, which is the order Kodi writes samples in: FL FR FC LFE BL
// BR SL SR - FFmpeg's 7.1 order, verified on hardware with a 5.1 system (EVO
// Player).
//
// Passthrough (Kodi's "Allow passthrough" setting, off by default): Kodi packs
// Dolby/DTS as IEC 61937 bursts in 16-bit samples; the PS5 sends them to the
// receiver untouched over HDMI through a bitstream port, the way its own media
// apps do. Sequence and modes from sainsaji's hardware research
// (github.com/sainsaji/PS5-Audio-Passthrough-Research): open the bitstream
// port first, then switch HDMI to that bitstream; while it is on, the
// process's PCM is muted; switch back to normal output on every exit path.
// (Writing IEC 61937 to an ordinary PCM port, as this sink did before, makes
// the receiver play the bursts as noise.)

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

// One route per format Kodi packs for: the IEC rate and channel count Kodi
// hands the sink (CAEBitstreamPacker::GetOutputRate/GetOutputChannelMap) and
// the PS5 port that carries it.
struct BitstreamPort
{
  CAEStreamInfo::DataType type;
  unsigned int iecRate; // Kodi's IEC 61937 rate
  unsigned int channels; // 2, or 8 for TrueHD (high bit rate)
  bool sys; // Sys functions (TrueHD/MAT) instead of Ex
  int32_t mode;
  unsigned int grainFrames; // frames per sceAudioOutOutput
  unsigned int portRate; // rate the port consumes frames at
  const char* name;
};

// Status on hardware (PS5 Pro, FW 12.70, per the research): AC-3, DTS core
// and E-AC-3 (incl. Atmos/JOC) verified; TrueHD plays; DTS-HD (Ex mode 4,
// "DTS_HD_HR") sent but not confirmed - offered only when the receiver lists
// DTS-HD, and only used when Kodi's "DTS-HD capable receiver" is enabled.
// DTS-HD MA (8-channel high bit rate) has no known port: Kodi falls back to
// its DTS core (audiooutput.dtshdcorefallback, on by default).
constexpr BitstreamPort kBitstreamPorts[] = {
    {CAEStreamInfo::STREAM_TYPE_AC3, 48000, 2, false, 0, 256, 48000, "Dolby Digital"},
    {CAEStreamInfo::STREAM_TYPE_DTS_512, 48000, 2, false, 2, 256, 48000, "DTS"},
    {CAEStreamInfo::STREAM_TYPE_DTS_1024, 48000, 2, false, 2, 256, 48000, "DTS"},
    {CAEStreamInfo::STREAM_TYPE_DTS_2048, 48000, 2, false, 2, 256, 48000, "DTS"},
    {CAEStreamInfo::STREAM_TYPE_DTSHD_CORE, 48000, 2, false, 2, 256, 48000, "DTS (core)"},
    {CAEStreamInfo::STREAM_TYPE_EAC3, 192000, 2, false, 3, 1024, 192000, "Dolby Digital Plus"},
    {CAEStreamInfo::STREAM_TYPE_DTSHD, 192000, 2, false, 4, 1024, 192000, "DTS-HD"},
    // the Sys port's 16-byte frames are consumed at 192 kHz, not its 768 kHz label
    {CAEStreamInfo::STREAM_TYPE_TRUEHD, 192000, 8, true, 5, 1024, 192000, "Dolby TrueHD"},
};

const BitstreamPort* FindBitstreamPort(CAEStreamInfo::DataType type, unsigned int iecRate,
                                       unsigned int channels)
{
  for (const BitstreamPort& port : kBitstreamPorts)
    if (port.type == type && port.iecRate == iecRate && port.channels == channels)
      return &port;
  return nullptr;
}

// CEA-861 audio coding types in the HDMI short audio descriptors
constexpr uint8_t kCodingAc3 = 0x02;
constexpr uint8_t kCodingDts = 0x07;
constexpr uint8_t kCodingEac3 = 0x0A;
constexpr uint8_t kCodingDtsHd = 0x0B;
constexpr uint8_t kCodingMat = 0x0C; // Dolby TrueHD (MAT)

// The audio formats the TV or receiver on HDMI decodes, from its EDID. Empty
// when the console cannot tell (then every route is offered and Kodi's own
// per-format passthrough settings decide).
std::vector<uint8_t> HdmiAudioCodings(std::string& deviceName)
{
  std::vector<uint8_t> codings;
  uint8_t info[AUDIO_OUT_HDMI_MONITOR_INFO_SIZE] = {};
  const int32_t rc =
      sceAudioOutSysGetHdmiMonitorInfo(AUDIO_OUT_HDMI, info, AUDIO_OUT_HDMI_MONITOR_INFO_SIZE);
  if (rc < 0)
  {
    CLog::Log(LOGINFO, "CAESinkPS5: HDMI audio capabilities unavailable ({:#x})",
              static_cast<uint32_t>(rc));
    return codings;
  }
  for (uint32_t i = AUDIO_OUT_HDMI_NAME_OFFSET; i < AUDIO_OUT_HDMI_NAME_OFFSET + 16; ++i)
  {
    if (info[i] < 0x20 || info[i] > 0x7e)
      break;
    deviceName += static_cast<char>(info[i]);
  }
  uint32_t count = 0;
  std::memcpy(&count, info + AUDIO_OUT_HDMI_SAD_COUNT_OFFSET, sizeof(count));
  constexpr uint32_t maxCount = (AUDIO_OUT_HDMI_MONITOR_INFO_SIZE - AUDIO_OUT_HDMI_SAD_OFFSET) / 8;
  for (uint32_t i = 0; i < std::min(count, maxCount); ++i)
    codings.push_back(info[AUDIO_OUT_HDMI_SAD_OFFSET + i * 8]);
  return codings;
}

// A receiver needs time to follow each HDMI format change; switching back to
// back can leave it on the previous format. Values from the research's
// hardware tests (they did not cure every receiver, so failures are logged).
constexpr unsigned int kLeadInMs = 400; // pause bursts after the switch
constexpr unsigned int kLeadOutMs = 500; // pause bursts before the port closes
constexpr auto kAfterClose = std::chrono::milliseconds(250); // close -> reset
constexpr auto kSinceReset = std::chrono::milliseconds(1000); // reset -> next switch
std::chrono::steady_clock::time_point g_lastHdmiReset{};

void RestoreHdmiOutput()
{
  const int32_t rc = sceAudioOutExConfigureOutput(0, 0, AUDIO_OUT_MODE_DEFAULT,
                                                  AUDIO_OUT_MODE_DEFAULT, 0);
  g_lastHdmiReset = std::chrono::steady_clock::now();
  if (rc < 0)
    CLog::Log(LOGWARNING, "CAESinkPS5: restoring normal HDMI audio output failed: {:#x}",
              static_cast<uint32_t>(rc));
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
  // Passthrough: the formats a bitstream port carries (Kodi packs the IEC 61937
  // bursts itself, at 48 or 192 kHz), limited to what the TV or receiver on
  // HDMI decodes. Kodi's per-format passthrough settings decide the rest.
  info.m_deviceType = AE_DEVTYPE_HDMI;
  info.m_wantsIECPassthrough = true;
  info.m_dataFormats.push_back(AE_FMT_RAW);
  info.m_sampleRates.push_back(AUDIO_OUT_SAMPLE_RATE_HIGH);

  std::string deviceName;
  const std::vector<uint8_t> codings = HdmiAudioCodings(deviceName);
  auto decodes = [&codings](uint8_t coding)
  { return codings.empty() || std::find(codings.begin(), codings.end(), coding) != codings.end(); };
  std::string offered;
  auto offer = [&info, &offered](std::initializer_list<CAEStreamInfo::DataType> types,
                                 const char* name)
  {
    info.m_streamTypes.insert(info.m_streamTypes.end(), types.begin(), types.end());
    offered += offered.empty() ? name : std::string(", ") + name;
  };
  if (decodes(kCodingAc3))
    offer({CAEStreamInfo::STREAM_TYPE_AC3}, "Dolby Digital");
  if (decodes(kCodingEac3))
    offer({CAEStreamInfo::STREAM_TYPE_EAC3}, "Dolby Digital Plus");
  if (decodes(kCodingDts))
    offer({CAEStreamInfo::STREAM_TYPE_DTS_512, CAEStreamInfo::STREAM_TYPE_DTS_1024,
           CAEStreamInfo::STREAM_TYPE_DTS_2048, CAEStreamInfo::STREAM_TYPE_DTSHD_CORE},
          "DTS");
  if (decodes(kCodingMat))
    offer({CAEStreamInfo::STREAM_TYPE_TRUEHD}, "Dolby TrueHD");
  // DTS-HD only when the receiver says so: unconfirmed on hardware
  if (!codings.empty() && decodes(kCodingDtsHd))
    offer({CAEStreamInfo::STREAM_TYPE_DTSHD}, "DTS-HD (experimental)");

  std::string listed;
  for (const uint8_t coding : codings)
    listed += (listed.empty() ? "" : " ") + std::to_string(coding);
  CLog::Log(LOGINFO, "CAESinkPS5: HDMI {}: audio codings [{}] -> passthrough offered: {}",
            deviceName.empty() ? "device" : "'" + deviceName + "'",
            codings.empty() ? "unknown" : listed, offered.empty() ? "none" : offered);
  list.push_back(info);
}

bool CAESinkPS5::Initialize(AEAudioFormat& format, std::string& device)
{
  if (format.m_dataFormat == AE_FMT_RAW)
    return OpenBitstreamPort(format);
  const bool eight = format.m_channelLayout.Count() > 2;
  if (!OpenPort(format, eight) && (!eight || !OpenPort(format, false)))
    return false;
  if (eight && m_channels != 8)
    CLog::Log(LOGWARNING, "CAESinkPS5: the 8-channel port did not open, using stereo");
  return true;
}

bool CAESinkPS5::OpenBitstreamPort(AEAudioFormat& format)
{
  const unsigned int channels = format.m_channelLayout.Count();
  const BitstreamPort* port =
      FindBitstreamPort(format.m_streamInfo.m_type, format.m_sampleRate, channels);
  if (!port)
  {
    CLog::Log(LOGWARNING, "CAESinkPS5: no HDMI bitstream route for {} at {} Hz, {} ch",
              CAEUtil::StreamTypeToStr(format.m_streamInfo.m_type), format.m_sampleRate,
              channels);
    return false;
  }

  // give the receiver time to follow the previous format change
  const auto sinceReset = std::chrono::steady_clock::now() - g_lastHdmiReset;
  if (sinceReset < kSinceReset)
    std::this_thread::sleep_for(kSinceReset - sinceReset);

  const int32_t initResult = sceAudioOutInit();
  if (initResult < 0)
    CLog::Log(LOGDEBUG, "CAESinkPS5: sceAudioOutInit returned {:#x} (already initialised is fine)",
              static_cast<uint32_t>(initResult));

  // 1. the bitstream port first (Sony's order), 2. then switch HDMI to it
  m_handle = port->sys ? sceAudioOutSysOpen(AUDIO_OUT_USER_ID_SYSTEM, port->mode)
                       : sceAudioOutExOpen(AUDIO_OUT_USER_ID_SYSTEM, port->mode);
  if (m_handle < 0)
  {
    CLog::Log(LOGWARNING, "CAESinkPS5: {} bitstream port (mode {}) failed: {:#x}", port->name,
              port->mode, static_cast<uint32_t>(m_handle));
    m_handle = -1;
    return false;
  }
  const int32_t switchRc =
      port->sys ? sceAudioOutSysConfigureOutput(AUDIO_OUT_HDMI, 0, port->mode,
                                                AUDIO_OUT_BITSTREAM_TARGET, 0)
                : sceAudioOutExConfigureOutput(0, 0, port->mode, AUDIO_OUT_BITSTREAM_TARGET, 0);
  if (switchRc < 0)
  {
    CLog::Log(LOGWARNING, "CAESinkPS5: switching HDMI to {} (mode {}) failed: {:#x}", port->name,
              port->mode, static_cast<uint32_t>(switchRc));
    if (port->sys)
      sceAudioOutSysClose(m_handle);
    else
      sceAudioOutExClose(m_handle);
    m_handle = -1;
    RestoreHdmiOutput();
    return false;
  }

  m_bitstream = true;
  m_bitstreamSys = port->sys;
  m_streamInfo = format.m_streamInfo;
  m_channels = port->channels;
  m_sampleRate = port->portRate;
  m_frameSize = m_channels * sizeof(int16_t);
  m_grainFrames = port->grainFrames;
  format.m_frames = m_grainFrames;
  format.m_frameSize = m_frameSize;
  m_block.assign(static_cast<size_t>(m_grainFrames) * m_frameSize, 0);
  m_blockFrames = 0;
  CLog::Log(LOGINFO,
            "CAESinkPS5: HDMI bitstream: {} via {} mode {} ({} ch, {} frames at {} Hz), "
            "port {:#x}, switch {:#x}",
            port->name, port->sys ? "Sys" : "Ex", port->mode, m_channels, m_grainFrames,
            m_sampleRate, static_cast<uint32_t>(m_handle), static_cast<uint32_t>(switchRc));

  // 3. let the receiver lock onto the format before the first frame
  WritePause(kLeadInMs);
  return true;
}

void CAESinkPS5::WritePause(unsigned int millis)
{
  // Kodi's own IEC 61937 pause bursts, in short chunks: its packer holds at
  // most one 61440-byte packet, about 20 ms of 8-channel TrueHD
  constexpr unsigned int chunkMs = 20;
  m_pausePacker.PackPause(m_streamInfo, chunkMs, true);
  const uint8_t* pause = m_pausePacker.GetBuffer();
  const unsigned int pauseFrames = m_pausePacker.GetSize() / m_frameSize;
  if (!pause || pauseFrames == 0)
    return;
  for (unsigned int done = 0; done < millis; done += chunkMs)
  {
    uint8_t* data[1] = {const_cast<uint8_t*>(pause)};
    if (AddPackets(data, pauseFrames, 0) < pauseFrames)
      return;
  }
}

void CAESinkPS5::CloseBitstreamPort()
{
  // 4. finish the stream, close the port, give HDMI back to normal output.
  // The pause bursts follow the last audio directly; only the final partial
  // grain is padded.
  WritePause(kLeadOutMs);
  if (m_blockFrames > 0)
  {
    std::memset(m_block.data() + static_cast<size_t>(m_blockFrames) * m_frameSize, 0,
                static_cast<size_t>(m_grainFrames - m_blockFrames) * m_frameSize);
    Output(m_block.data());
    m_blockFrames = 0;
  }
  sceAudioOutOutput(m_handle, nullptr);
  const int32_t closeRc = m_bitstreamSys ? sceAudioOutSysClose(m_handle)
                                         : sceAudioOutExClose(m_handle);
  m_handle = -1;
  std::this_thread::sleep_for(kAfterClose);
  RestoreHdmiOutput();
  m_bitstream = false;
  m_bitstreamSys = false;
  CLog::Log(LOGINFO, "CAESinkPS5: HDMI bitstream closed ({:#x}), normal output restored",
            static_cast<uint32_t>(closeRc));
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
  m_grainFrames = PCM_GRAIN_FRAMES;
  m_bitstream = false;
  format.m_frames = m_grainFrames;
  format.m_frameSize = m_frameSize;

  const int32_t initResult = sceAudioOutInit();
  if (initResult < 0)
    CLog::Log(LOGDEBUG, "CAESinkPS5: sceAudioOutInit returned {:#x} (already initialised is fine)",
              static_cast<uint32_t>(initResult));

  m_handle = sceAudioOutOpen(AUDIO_OUT_USER_ID_SYSTEM, AUDIO_OUT_PORT_TYPE_MAIN, 0, m_grainFrames,
                             AUDIO_OUT_SAMPLE_RATE, param);
  if (m_handle <= 0)
  {
    CLog::Log(eight ? LOGWARNING : LOGERROR, "CAESinkPS5: sceAudioOutOpen ({} ch, format {}) failed: {:#x}",
              m_channels, param, static_cast<uint32_t>(m_handle));
    m_handle = -1;
    return false;
  }
  m_block.assign(static_cast<size_t>(m_grainFrames) * m_frameSize, 0);
  m_blockFrames = 0;
  CLog::Log(LOGINFO, "CAESinkPS5: opened main port, {} Hz, {} ch{}, {} frames/block, {}",
            AUDIO_OUT_SAMPLE_RATE, m_channels,
            eight ? " (FL FR FC LFE BL BR SL SR)" : "",
            m_grainFrames, format.m_dataFormat == AE_FMT_FLOAT ? "float" : "s16");
  return true;
}

void CAESinkPS5::Deinitialize()
{
  if (m_bitstream && m_handle >= 0)
    CloseBitstreamPort();
  else if (m_handle > 0)
  {
    sceAudioOutClose(m_handle);
    m_handle = -1;
  }
  m_blockStarted = {};
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
  return static_cast<double>(m_grainFrames * QUEUE_DEPTH) / m_sampleRate;
}

double CAESinkPS5::GetLatency()
{
  // Unknown mixer latency downstream of the port; the AE sync loop measures
  // the rest through GetDelay().
  return 0.0;
}

unsigned int CAESinkPS5::AddPackets(uint8_t** data, unsigned int frames, unsigned int offset)
{
  if (m_handle < 0)
    return 0;

  const uint8_t* src = data[0] + static_cast<size_t>(offset) * m_frameSize;
  unsigned int remaining = frames;

  while (remaining > 0)
  {
    const unsigned int space = m_grainFrames - m_blockFrames;
    const unsigned int n = std::min(space, remaining);
    uint8_t* dst = m_block.data() + static_cast<size_t>(m_blockFrames) * m_frameSize;
    std::memcpy(dst, src, static_cast<size_t>(n) * m_frameSize);
    m_blockFrames += n;
    src += static_cast<size_t>(n) * m_frameSize;
    remaining -= n;

    if (m_blockFrames == m_grainFrames)
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
    playing = std::max(0.0, static_cast<double>(m_grainFrames) / m_sampleRate - elapsed);
  }
  status.SetDelay(playing + static_cast<double>(m_blockFrames) / m_sampleRate);
}

void CAESinkPS5::Drain()
{
  if (m_handle < 0)
    return;

  if (m_blockFrames > 0)
  {
    std::memset(m_block.data() + static_cast<size_t>(m_blockFrames) * m_frameSize, 0,
                static_cast<size_t>(m_grainFrames - m_blockFrames) * m_frameSize);
    Output(m_block.data());
    m_blockFrames = 0;
  }
  // A null buffer waits for the queue to run dry (PS4 semantics, see header).
  sceAudioOutOutput(m_handle, nullptr);
}
