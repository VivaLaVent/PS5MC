/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDVideoCodecPS5.h"

#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecFFmpeg.h"

#include "RendererPS5.h"

#include "VideoCodecRegistration.h"

#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"

extern "C"
{
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}
#include "cores/VideoPlayer/Process/ProcessInfo.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

using namespace KODI::PLATFORM::PS5;

namespace
{
constexpr unsigned kMaxErrorsInRow = 60; // then give up (Kodi shows an error)
// A stream that decoded fine and then starts failing (a damaged access unit, a
// discontinuity, a stream change) used to freeze: the pre-first-picture
// recovery does not apply once pictures have been shown, and the decoder never
// recovers on its own. After this many failures in a row mid-stream, reset the
// decoder and resync - Kodi feeds a keyframe shortly after, as on a seek.
constexpr unsigned kErrorsBeforeResync = 12;
constexpr unsigned kMaxMidStreamResets = 6; // then hand the rest to FFmpeg
// Before the first picture: this many failed decodes (in total, not in a row)
// hand the stream to FFmpeg. Kept below the 20 decoder frame buffers, because
// a refused access unit can leave its frame with the decoder: after 20 of them
// no frame is free, decoding stalls and the in-a-row count is never reached
// (seen with a 1080p HEVC Main10 file refused with 0x811d0303).
constexpr unsigned kFailuresBeforeSoftware = 8;

bool IsH264Supported(int profile)
{
  // 8-bit 4:2:0 profiles; High 10, 4:2:2 and 4:4:4 go to FFmpeg
  switch (profile)
  {
    case AV_PROFILE_H264_BASELINE:
    case AV_PROFILE_H264_CONSTRAINED_BASELINE:
    case AV_PROFILE_H264_MAIN:
    case AV_PROFILE_H264_EXTENDED:
    case AV_PROFILE_H264_HIGH:
    case AV_PROFILE_UNKNOWN:
      return true;
    default:
      return false;
  }
}
} // namespace

CDVDVideoCodecPS5::CDVDVideoCodecPS5(CProcessInfo& processInfo) : CDVDVideoCodec(processInfo)
{
}

CDVDVideoCodecPS5::~CDVDVideoCodecPS5()
{
  LogStreamSummary();
  ClearQueue();
  CloseDeinterlacer();
  av_bsf_free(&m_bsf);
  av_packet_free(&m_packet);
  // no Close(): zero-copy pictures may still show the decoder's frames; the
  // last owner of m_decoder closes it
  m_decoder.reset();
}

std::unique_ptr<CDVDVideoCodec> CDVDVideoCodecPS5::Create(CProcessInfo& processInfo)
{
  return std::make_unique<CDVDVideoCodecPS5>(processInfo);
}

void CDVDVideoCodecPS5::Register()
{
  CDVDFactoryCodec::RegisterHWVideoCodec("ps5-videodec2", &CDVDVideoCodecPS5::Create);
}

bool CDVDVideoCodecPS5::Open(CDVDStreamInfo& hints, CDVDCodecOptions& options)
{
  VideoDec2Codec codec;
  if (hints.codec == AV_CODEC_ID_H264 && IsH264Supported(hints.profile))
    codec = VideoDec2Codec::H264;
  else if (hints.codec == AV_CODEC_ID_H264 && hints.profile == AV_PROFILE_H264_HIGH_10 &&
           hints.bitsperpixel <= 10)
    codec = VideoDec2Codec::H264High10; // the decoder's query decides; refused -> FFmpeg
  else if (hints.codec == AV_CODEC_ID_HEVC &&
           (hints.profile == AV_PROFILE_HEVC_MAIN || hints.profile == AV_PROFILE_UNKNOWN) &&
           hints.bitsperpixel <= 8)
    codec = VideoDec2Codec::HEVC;
  else if (hints.codec == AV_CODEC_ID_HEVC &&
           (hints.profile == AV_PROFILE_HEVC_MAIN_10 ||
            (hints.profile == AV_PROFILE_UNKNOWN && hints.bitsperpixel == 10)))
    codec = VideoDec2Codec::HEVCMain10;
  else if (hints.codec == AV_CODEC_ID_VP9 &&
           (hints.profile == AV_PROFILE_VP9_0 ||
            (hints.profile == AV_PROFILE_UNKNOWN && hints.bitsperpixel <= 8)))
    codec = VideoDec2Codec::VP9;
  else if (hints.codec == AV_CODEC_ID_VP9 &&
           (hints.profile == AV_PROFILE_VP9_2 ||
            (hints.profile == AV_PROFILE_UNKNOWN && hints.bitsperpixel == 10)))
    codec = VideoDec2Codec::VP9Profile2;
  else
    return false; // FFmpeg takes it (H.264 High 10, HEVC/VP9 4:2:2/4:4:4, 12-bit, AV1, ...)

  if (hints.width <= 0 || hints.height <= 0 || hints.width > 3840 || hints.height > 2176)
    return false;
  // Interlaced streams: decoded in hardware, deinterlaced with bwdif
  const bool interlaced = hints.interlaced;
  if (interlaced)
    CLog::Log(LOGINFO, "CDVDVideoCodecPS5: interlaced {}x{} stream: hardware decode, bwdif",
              hints.width, hints.height);

  // interlaced: bwdif over the decoded frames, which needs them in ordinary
  // memory - so no zero-copy for these
  m_deinterlace = interlaced;
  // zero-copy: pictures are the decoder's own frames
  m_zeroCopy = !interlaced && IsZeroCopyAvailable();
  if (!interlaced && !IsZeroCopyAvailable())
    CLog::Log(LOGWARNING, "CDVDVideoCodecPS5: the GL driver has no zero-copy additions (rebuild "
              "it with scripts/18): copying frames");
  m_decoder->SetPooled(m_zeroCopy);
  if (m_zeroCopy && !m_zeroCopyPool)
    m_zeroCopyPool = std::make_shared<CVideoBufferPoolPS5>();

  std::string error;
  const float fps = hints.fpsscale > 0 ? static_cast<float>(hints.fpsrate) / hints.fpsscale : 0.0f;
  m_streamFps = fps;
  m_streamName = StringUtils::Format(
      "{} {}x{}",
      codec == VideoDec2Codec::H264
          ? "H.264"
          : (codec == VideoDec2Codec::H264High10
                 ? "H.264 High 10"
                 : (m_vp9 ? (m_tenBit ? "VP9 Profile 2" : "VP9")
                          : (m_tenBit ? "HEVC Main10" : "HEVC"))),
      hints.width, hints.height);
  m_streamStart = std::chrono::steady_clock::now();
  m_streamDecodes = 0;
  m_streamDecodeMs = m_streamMaxMs = 0.0;
  m_streamOverBudget = 0;
  m_options = options;
  if (!m_decoder->Open(codec, hints.width, hints.height, error, interlaced, hints.level))
  {
    CLog::Log(LOGWARNING, "CDVDVideoCodecPS5: hardware decoder unavailable ({}), using FFmpeg",
              error);
    m_decoder->Close();
    return false;
  }
  m_hints = hints;
  BuildParameterSets(hints);
  m_prependParameterSets = !m_parameterSets.empty();
  if (!SetupBitstreamFilter(hints))
  {
    m_decoder->Close();
    return false;
  }

  m_width = static_cast<unsigned>(hints.width);
  m_height = static_cast<unsigned>(hints.height);
  m_displayWidth = m_width;
  m_displayHeight = m_height;
  if (hints.aspect > 0.0)
  {
    m_displayWidth = static_cast<unsigned>(std::lrint(m_height * hints.aspect)) & ~3u;
    if (m_displayWidth < m_width)
    {
      m_displayWidth = m_width;
      m_displayHeight = static_cast<unsigned>(std::lrint(m_width / hints.aspect)) & ~3u;
    }
  }
  m_frameDuration = (hints.fpsrate > 0 && hints.fpsscale > 0)
                        ? DVD_TIME_BASE * static_cast<double>(hints.fpsscale) / hints.fpsrate
                        : DVD_TIME_BASE / 25.0;
  m_colorSpace = hints.colorSpace;
  m_colorPrimaries = hints.colorPrimaries;
  m_colorTransfer = hints.colorTransferCharacteristic;
  m_fullRange = hints.colorRange == AVCOL_RANGE_JPEG;
  m_stereoMode = hints.stereo_mode;

  m_vp9 = codec == VideoDec2Codec::VP9 || codec == VideoDec2Codec::VP9Profile2;
  m_tenBit = codec == VideoDec2Codec::HEVCMain10 || codec == VideoDec2Codec::VP9Profile2 ||
             codec == VideoDec2Codec::H264High10;
  m_hevc = codec == VideoDec2Codec::HEVC || codec == VideoDec2Codec::HEVCMain10;
  m_skipRasl = m_hevc;
  m_alignmentKnown = !m_tenBit;
  m_alignmentSamples = 0;
  // 10-bit: lower-aligned until a picture shows otherwise (DetectAlignment)
  m_pixelFormat = m_tenBit ? AV_PIX_FMT_YUV420P10 : AV_PIX_FMT_NV12;
  m_colorBits = m_tenBit ? 10 : 8;
  // HDR10 metadata, for Kodi's tone mapping on an SDR output
  m_hasDisplayMetadata = hints.masteringMetadata != nullptr;
  if (m_hasDisplayMetadata)
    m_displayMetadata = *hints.masteringMetadata;
  m_hasLightMetadata = hints.contentLightMetadata != nullptr;
  if (m_hasLightMetadata)
    m_lightMetadata = *hints.contentLightMetadata;

  m_trace = 30;
  m_processInfo.SetVideoDecoderName(GetName(), true);
  m_processInfo.SetVideoPixelFormat(m_tenBit ? "p010 (lsb)" : "nv12");
  if (m_vp9)
    CLog::Log(LOGINFO, "CDVDVideoCodecPS5: VP9: superframes split, hidden frames not shown");
  if (m_zeroCopy)
    CLog::Log(LOGINFO, "CDVDVideoCodecPS5: zero-copy: pictures are the decoder's own frames");
  m_processInfo.SetVideoDimensions(hints.width, hints.height);
  m_processInfo.SetVideoDeintMethod("none");
  if (hints.fpsrate > 0 && hints.fpsscale > 0)
    m_processInfo.SetVideoFps(static_cast<float>(hints.fpsrate) / hints.fpsscale);

  CLog::Log(LOGINFO, "CDVDVideoCodecPS5: hardware {} decoding {}x{}",
            codec == VideoDec2Codec::H264
                ? "H.264"
                : (codec == VideoDec2Codec::H264High10
                       ? "H.264 High 10"
                       : (m_vp9 ? (m_tenBit ? "VP9 Profile 2" : "VP9")
                                : (m_tenBit ? "HEVC Main10" : "HEVC"))),
            hints.width, hints.height);
  return true;
}

void CDVDVideoCodecPS5::BuildParameterSets(const CDVDStreamInfo& hints)
{
  m_parameterSets.clear();
  const uint8_t* extra = hints.extradata.GetData();
  const size_t size = hints.extradata.GetSize();
  if (!extra || size < 4)
    return;
  static const uint8_t startCode[4] = {0, 0, 0, 1};
  auto addNal = [&](const uint8_t* nal, size_t length)
  {
    m_parameterSets.insert(m_parameterSets.end(), startCode, startCode + 4);
    m_parameterSets.insert(m_parameterSets.end(), nal, nal + length);
  };
  if (extra[0] != 1)
  {
    // already Annex-B (start codes): the parameter sets as they are
    if (extra[0] == 0 && extra[1] == 0)
      m_parameterSets.assign(extra, extra + size);
    return;
  }
  size_t pos = 0;
  if (hints.codec == AV_CODEC_ID_H264)
  {
    // avcC: 5 header bytes, SPS count (low 5 bits), SPS entries, PPS count,
    // PPS entries; each entry a 16-bit length and the NAL unit
    if (size < 7)
      return;
    const unsigned numSps = extra[5] & 0x1f;
    pos = 6;
    for (unsigned i = 0; i < numSps && pos + 2 <= size; ++i)
    {
      const size_t length = (extra[pos] << 8) | extra[pos + 1];
      pos += 2;
      if (pos + length > size)
        return;
      addNal(extra + pos, length);
      pos += length;
    }
    if (pos >= size)
      return;
    const unsigned numPps = extra[pos++];
    for (unsigned i = 0; i < numPps && pos + 2 <= size; ++i)
    {
      const size_t length = (extra[pos] << 8) | extra[pos + 1];
      pos += 2;
      if (pos + length > size)
        return;
      addNal(extra + pos, length);
      pos += length;
    }
  }
  else if (hints.codec == AV_CODEC_ID_HEVC)
  {
    // hvcC: 22 header bytes, then arrays of NAL units (type byte, 16-bit
    // count, entries with 16-bit lengths): VPS, SPS, PPS in order
    if (size < 23)
      return;
    const unsigned numArrays = extra[22];
    pos = 23;
    for (unsigned a = 0; a < numArrays && pos + 3 <= size; ++a)
    {
      const unsigned count = (extra[pos + 1] << 8) | extra[pos + 2];
      pos += 3;
      for (unsigned i = 0; i < count && pos + 2 <= size; ++i)
      {
        const size_t length = (extra[pos] << 8) | extra[pos + 1];
        pos += 2;
        if (pos + length > size)
          return;
        addNal(extra + pos, length);
        pos += length;
      }
    }
  }
}

bool CDVDVideoCodecPS5::SetupBitstreamFilter(const CDVDStreamInfo& hints)
{
  m_packet = av_packet_alloc();
  if (!m_packet)
    return false;

  if (hints.codec == AV_CODEC_ID_VP9)
  {
    // the decoder refuses a compound superframe but takes each of its frames
    const AVBitStreamFilter* split = av_bsf_get_by_name("vp9_superframe_split");
    if (!split || av_bsf_alloc(split, &m_bsf) < 0)
    {
      CLog::Log(LOGERROR, "CDVDVideoCodecPS5: bitstream filter vp9_superframe_split unavailable");
      return false;
    }
    m_bsf->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
    m_bsf->par_in->codec_id = AV_CODEC_ID_VP9;
    if (av_bsf_init(m_bsf) < 0)
    {
      CLog::Log(LOGERROR, "CDVDVideoCodecPS5: cannot initialise vp9_superframe_split");
      av_bsf_free(&m_bsf);
      return false;
    }
    return true;
  }

  // MP4/MKV carry avcC/hvcC (length-prefixed NAL units, parameter sets in
  // extradata); the decoder wants Annex-B. Already Annex-B: no filter.
  const uint8_t* extra = hints.extradata.GetData();
  const size_t extraSize = hints.extradata.GetSize();
  if (!extra || extraSize < 4 || extra[0] != 1)
    return true;

  const char* name = hints.codec == AV_CODEC_ID_H264 ? "h264_mp4toannexb" : "hevc_mp4toannexb";
  const AVBitStreamFilter* filter = av_bsf_get_by_name(name);
  if (!filter || av_bsf_alloc(filter, &m_bsf) < 0)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecPS5: bitstream filter {} unavailable", name);
    return false;
  }
  m_bsf->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
  m_bsf->par_in->codec_id = hints.codec;
  m_bsf->par_in->extradata =
      static_cast<uint8_t*>(av_mallocz(extraSize + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!m_bsf->par_in->extradata)
    return false;
  std::memcpy(m_bsf->par_in->extradata, extra, extraSize);
  m_bsf->par_in->extradata_size = static_cast<int>(extraSize);
  if (av_bsf_init(m_bsf) < 0)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecPS5: cannot initialise {}", name);
    av_bsf_free(&m_bsf);
    return false;
  }
  return true;
}

bool CDVDVideoCodecPS5::AddData(const DemuxPacket& packet)
{
  if (m_software)
  {
    // what the hardware was given first, then the new packets
    while (!m_softwareQueue.empty())
    {
      ReplayPacket& r = m_softwareQueue.front();
      DemuxPacket p;
      p.pData = r.data.data();
      p.iSize = static_cast<int>(r.data.size());
      p.pts = r.pts;
      p.dts = r.dts;
      p.recoveryPoint = r.recoveryPoint;
      if (!m_software->AddData(p))
        return false; // Kodi takes a picture and offers this packet again
      m_softwareQueue.pop_front();
    }
    return m_software->AddData(packet);
  }
  if (m_fatal)
    return false;
  if (!packet.pData || packet.iSize <= 0)
    return true;
  // one decode per packet may leave a picture queued; take it first
  if (m_decoded.size() >= 2)
    return false;
  // zero-copy: every frame shown or held - Kodi releases pictures first
  if (!RetryPending() || !m_decoder->HasFreeFrame())
  {
    // Nothing shown yet, decodes have failed, and now no frame is free: the
    // decoder kept the refused frames and will never return a picture.
    if (m_picturesOut == 0 && m_decoded.empty() && m_failuresBeforeFirst > 0)
    {
      m_recoveryPending = true;
      if (RunRecovery())
        return AddData(packet); // the new state takes this packet
    }
    return false;
  }

  BufferForReplay(packet);
  const bool ok = FeedHardware(packet);
  if (m_recoveryPending)
    return RunRecovery() || !m_fatal; // the replay included this packet
  return ok;
}

bool CDVDVideoCodecPS5::FeedHardware(const DemuxPacket& packet)
{
  const double pts = packet.pts != DVD_NOPTS_VALUE ? packet.pts : packet.dts;
  if (pts != DVD_NOPTS_VALUE)
  {
    m_pts.insert(pts);
    // The decoder returns pictures in display order, and each gets the
    // smallest pending timestamp. If it ever returns fewer pictures than it
    // was given (a skipped or corrupt picture), the pending set would grow and
    // every later picture would be stamped too early - a permanent, growing
    // lag behind the audio. Bound it: the reorder depth plus frames in flight
    // never exceeds this, so anything beyond is a timestamp without a picture.
    constexpr size_t kMaxPending = 24;
    if (m_pts.size() > kMaxPending)
    {
      const size_t excess = m_pts.size() - kMaxPending;
      for (size_t i = 0; i < excess; ++i)
        m_pts.erase(m_pts.begin());
      if (!m_ptsTrimmed)
      {
        m_ptsTrimmed = true;
        CLog::Log(LOGWARNING,
                  "CDVDVideoCodecPS5: the decoder returned fewer pictures than access units; "
                  "{} stale timestamp(s) discarded to keep A/V sync", excess);
      }
    }
  }

  if (!m_bsf)
  {
    const uint8_t* au = packet.pData;
    size_t auSize = static_cast<size_t>(packet.iSize);
    if (!m_vp9 && ToAnnexB(au, auSize, m_annexB))
    {
      if (!m_loggedLengthPrefixed)
      {
        m_loggedLengthPrefixed = true;
        CLog::Log(LOGINFO, "CDVDVideoCodecPS5: {} has length-prefixed NAL units and no codec "
                  "extradata: converting to Annex-B (parameter sets in-band)", m_streamName);
      }
      au = m_annexB.data();
      auSize = m_annexB.size();
    }
    if (!m_pendingAus.empty()) // a replay can outrun the free frames
    {
      m_pendingAus.emplace_back(au, au + auSize);
      return true;
    }
    return DecodeOne(au, auSize) || !m_fatal;
  }

  av_packet_unref(m_packet);
  if (av_new_packet(m_packet, packet.iSize) < 0)
    return true;
  std::memcpy(m_packet->data, packet.pData, static_cast<size_t>(packet.iSize));
  if (av_bsf_send_packet(m_bsf, m_packet) < 0)
    return true; // bad packet: drop it
  while (av_bsf_receive_packet(m_bsf, m_packet) == 0)
  {
    // once one access unit waits for a frame, the following ones queue behind it
    if (!m_pendingAus.empty())
      m_pendingAus.emplace_back(m_packet->data, m_packet->data + m_packet->size);
    else
      DecodeOne(m_packet->data, static_cast<size_t>(m_packet->size));
    av_packet_unref(m_packet);
  }
  return !m_fatal;
}

namespace
{
// VP9 uncompressed header, first bits: frame_marker(2), profile(2, +1 reserved
// for profile 3), show_existing_frame(1), then frame_type(1), show_frame(1).
// Hidden frames (alternate references) still produce a decoder output, which
// must not be shown; a show-existing-frame command is shown.
bool Vp9FrameIsShown(const uint8_t* data, size_t size)
{
  if (size < 1)
    return true;
  unsigned bit = 0;
  auto read = [&](unsigned count)
  {
    unsigned value = 0;
    for (unsigned i = 0; i < count; ++i, ++bit)
    {
      if (bit / 8 >= size)
        return value << (count - i); // truncated: treat as shown further up
      value = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1u);
    }
    return value;
  };
  if (read(2) != 2)
    return true; // not a VP9 frame header: never hide on a guess
  const unsigned profile = read(1) | (read(1) << 1);
  if (profile == 3)
    read(1);
  if (read(1)) // show_existing_frame
    return true;
  read(1); // frame_type
  return read(1) != 0; // show_frame
}
} // namespace

bool CDVDVideoCodecPS5::HevcAccessUnitIsRasl(const uint8_t* data, size_t size)
{
  // Annex-B: the first VCL NAL unit decides. HEVC NAL header: type in bits
  // 1-6 of the first byte; RASL_N = 8, RASL_R = 9; VCL types are 0-31.
  for (size_t i = 0; i + 3 < size; ++i)
  {
    if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
      continue;
    const uint8_t type = (data[i + 3] >> 1) & 0x3f;
    if (type <= 31)
      return type == 8 || type == 9;
    i += 2;
  }
  return false;
}

bool CDVDVideoCodecPS5::DecodeOne(const uint8_t* data, size_t size)
{
  // switched to software mid-packet (the bitstream-filter loop may still hold
  // access units): the hardware decoder is closed, drop the rest
  if (m_software || m_recoveryPending)
    return true; // the stream is replayed from its start
  // After a mid-stream resync the decoder has no reference frames: feeding it
  // mid-GOP pictures just fails again. Drop access units until a keyframe.
  if (m_waitKeyframe)
  {
    std::vector<uint8_t> tmp;
    bool irap = false, hasSlice = false;
    CleanAccessUnit(data, size, tmp, irap, hasSlice);
    if (!irap)
    {
      NextPts(); // its timestamp goes with it
      return true;
    }
    m_waitKeyframe = false;
    CLog::Log(LOGINFO, "CDVDVideoCodecPS5: resynced on a keyframe");
  }

  std::vector<uint8_t> cleaned;
  if (m_cleanStream)
  {
    bool irap = false, hasSlice = false;
    CleanAccessUnit(data, size, cleaned, irap, hasSlice);
    if (!hasSlice || (!m_seenIrap && !irap))
    {
      NextPts(); // nothing to decode here (yet): its timestamp goes with it
      // Never let the cleaned stream swallow everything silently: with nothing
      // decodable (not Annex-B, no slices) or no keyframe for a long run while
      // nothing has been shown, hand the stream to software.
      if (m_picturesOut == 0 && m_decoded.empty() &&
          ((!hasSlice && ++m_cleanNoSlice >= kFailuresBeforeSoftware) ||
           (hasSlice && ++m_cleanWaitIrap >= 300)))
      {
        CLog::Log(LOGWARNING, "CDVDVideoCodecPS5: the cleaned {} stream has {} (nothing shown)",
                  m_streamName, hasSlice ? "no keyframe in 300 access units"
                                         : "no decodable NAL units");
        m_recoveryPending = true;
      }
      return true;
    }
    m_seenIrap = true;
    data = cleaned.data();
    size = cleaned.size();
  }
  if (m_hevc && m_skipRasl)
  {
    if (HevcAccessUnitIsRasl(data, size))
    {
      NextPts(); // its timestamp goes with it
      return true;
    }
    m_skipRasl = false;
  }
  const bool thisShown = !m_vp9 || Vp9FrameIsShown(data, size);
  std::vector<uint8_t> withParameterSets;
  if (m_prependParameterSets)
  {
    m_prependParameterSets = false;
    withParameterSets.reserve(m_parameterSets.size() + size);
    withParameterSets.insert(withParameterSets.end(), m_parameterSets.begin(), m_parameterSets.end());
    withParameterSets.insert(withParameterSets.end(), data, data + size);
    data = withParameterSets.data();
    size = withParameterSets.size();
    if (m_timeDecodes)
      CLog::Log(LOGINFO, "CDVDVideoCodecPS5: parameter sets ({} bytes) prepended to the first access unit",
                m_parameterSets.size());
  }
  bool gotPicture = false;
  const auto decodeStart = std::chrono::steady_clock::now();
  VideoDec2Picture picture;
  std::string error;
  const bool decoded = m_decoder->Decode(data, size, gotPicture, &picture, error);
  Trace("return", picture, decoded && gotPicture);
  if (!decoded)
  {
    if (m_errorsInRow++ < 5)
      CLog::Log(LOGWARNING, "CDVDVideoCodecPS5: {}", error);
    if (m_picturesOut == 0 && m_decoded.empty())
    {
      if (!m_fingerprinted)
      {
        // what the refused stream looks like, once: the NAL units of the
        // parameter sets and of the first access unit that was refused
        m_fingerprinted = true;
        LogFingerprint("extradata", m_parameterSets.data(), m_parameterSets.size());
        LogFingerprint("refused access unit", data, size);
      }
      if (++m_failuresBeforeFirst >= kFailuresBeforeSoftware)
      {
        m_recoveryPending = true; // AddData replays the stream in the next stage
        return false;
      }
    }
    // Mid-stream: pictures have been shown, so the stream itself is decodable.
    // Resync the decoder rather than freeze; if it keeps happening, finish the
    // file in software instead of stuttering through repeated resyncs.
    if (m_picturesOut > 0 && m_errorsInRow >= kErrorsBeforeResync)
    {
      if (++m_midStreamResets > kMaxMidStreamResets)
      {
        CLog::Log(LOGWARNING,
                  "CDVDVideoCodecPS5: {} failed resyncs on {}; continuing in software",
                  m_midStreamResets - 1, m_streamName);
        if (SwitchToSoftware())
          return false;
        m_fatal = true;
        return false;
      }
      CLog::Log(LOGWARNING,
                "CDVDVideoCodecPS5: {} decode errors mid-stream on {}; resetting the decoder "
                "and resyncing (resync {})",
                m_errorsInRow, m_streamName, m_midStreamResets);
      ResetDecoderState();   // drops queued pictures and pending access units
      m_decoder->Reset();    // reclaims the decoder's frame buffers
      m_errorsInRow = 0;
      m_seenIrap = false;    // wait for the next keyframe before decoding again
      m_waitKeyframe = true;
      return false;
    }
    if (m_errorsInRow >= kMaxErrorsInRow)
    {
      CLog::Log(LOGERROR, "CDVDVideoCodecPS5: {} decode errors in a row, giving up",
                m_errorsInRow);
      m_fatal = true;
    }
    return false;
  }
  m_errorsInRow = 0;
  if (!m_decoder->Stalled())
  {
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - decodeStart).count();
    ++m_streamDecodes;
    m_streamDecodeMs += ms;
    m_streamMaxMs = std::max(m_streamMaxMs, ms);
    if (m_streamFps > 0.0f && ms > 1000.0 / static_cast<double>(m_streamFps))
      ++m_streamOverBudget;
    m_decodeTotalMs += ms;
    m_decodeMaxMs = std::max(m_decodeMaxMs, ms);
    ++m_decodeCount;
    if (m_decodeWindow.time_since_epoch().count() == 0)
      m_decodeWindow = now;
    else if (m_timeDecodes && now - m_decodeWindow >= std::chrono::seconds(5))
    {
      CLog::Log(LOGINFO, "CDVDVideoCodecPS5 (kodi-debug): {} decodes in {:.1f} s, {:.1f} ms average, "
                "{:.1f} ms longest",
                m_decodeCount, std::chrono::duration<double>(now - m_decodeWindow).count(),
                m_decodeTotalMs / m_decodeCount, m_decodeMaxMs);
      m_decodeWindow = now;
      m_decodeTotalMs = m_decodeMaxMs = 0.0;
      m_decodeCount = 0;
    }
  }
  if (m_decoder->Stalled())
  {
    // zero-copy: no frame was free; the access unit waits at the front (it is
    // either the first waiting one or a retried one)
    m_pendingAus.emplace_front(data, data + size);
    return true;
  }
  // VP9: whose picture is this? The just-offered frame's (immediate), or the
  // oldest access unit still waiting (frames in flight). The current access
  // unit's flag is queued when its picture has not come out with it.
  bool shown = true;
  if (m_vp9)
  {
    if (gotPicture && picture.immediate)
      shown = thisShown;
    else
    {
      m_vp9PendingShown.push_back(thisShown);
      if (gotPicture)
      {
        shown = m_vp9PendingShown.front();
        m_vp9PendingShown.pop_front();
      }
    }
  }
  if (gotPicture && !shown)
  {
    // VP9 hidden frame: decoded as a reference, never presented
    m_decoder->ReleaseFrame(picture.frameIndex);
    return true;
  }
  if (gotPicture)
    Keep(picture);
  return true;
}

std::string CDVDVideoCodecPS5::LumaSamples(const VideoDec2Picture& picture) const
{
  // five points: top-left, centre, bottom-left, bottom-right, three-quarters
  if (!picture.data || picture.width == 0 || picture.height == 0)
    return "-";
  const unsigned w = std::min(m_width, picture.width), h = std::min(m_height, picture.height);
  const unsigned bytes = picture.bitDepth > 8 ? 2 : 1;
  auto at = [&](unsigned x, unsigned y) -> unsigned
  {
    const uint8_t* p = picture.data + static_cast<size_t>(y) * picture.pitch + static_cast<size_t>(x) * bytes;
    return bytes == 2 ? *reinterpret_cast<const uint16_t*>(p) : *p;
  };
  return StringUtils::Format("{} {} {} {} {}", at(8, 8), at(w / 2, h / 2), at(8, h - 8),
                             at(w - 8, h - 8), at(3 * w / 4, 3 * h / 4));
}

void CDVDVideoCodecPS5::Trace(const char* stage, const VideoDec2Picture& picture, bool gotPicture)
{
  if (!m_timeDecodes || m_trace == 0)
    return;
  --m_trace;
  CLog::Log(LOGINFO,
            "CDVDVideoCodecPS5 (trace {}): offered frame {} ({}), picture {} frame {} (count {}, "
            "immediate {}), luma {}",
            stage, picture.offeredIndex, picture.offeredAccepted ? "accepted" : "refused",
            gotPicture ? "yes" : "no", gotPicture ? picture.frameIndex : -1, picture.pictureCount,
            picture.immediate ? "yes" : "no", gotPicture ? LumaSamples(picture) : "-");
}


void CDVDVideoCodecPS5::LogStreamSummary()
{
  if (m_streamDecodes == 0)
    return;
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - m_streamStart).count();
  CLog::Log(LOGINFO,
            "CDVDVideoCodecPS5: {} at {:.3f} fps, {}: {} pictures in {:.1f} s ({:.1f}/s), decode "
            "{:.1f} ms average / {:.1f} ms longest, {} over one frame period{}",
            m_streamName, m_streamFps, m_zeroCopy ? "zero-copy" : (m_deinterlace ? "bwdif" : "copy"),
            m_streamDecodes, seconds, seconds > 0.0 ? m_streamDecodes / seconds : 0.0,
            m_streamDecodeMs / m_streamDecodes, m_streamMaxMs, m_streamOverBudget,
            m_streamOverBudget == 0 ? "" : " (would drop)");
}

bool CDVDVideoCodecPS5::RetryPending()
{
  while (!m_pendingAus.empty())
  {
    if (!m_decoder->HasFreeFrame())
      return false;
    std::vector<uint8_t> au = std::move(m_pendingAus.front());
    m_pendingAus.pop_front();
    DecodeOne(au.data(), au.size());
    if (!m_pendingAus.empty() && m_decoder->Stalled())
      return false; // it went back to the front: still no frame
  }
  return true;
}

double CDVDVideoCodecPS5::NextPts()
{
  if (!m_pts.empty())
  {
    m_lastPts = *m_pts.begin();
    m_pts.erase(m_pts.begin());
  }
  else
    m_lastPts += m_frameDuration;
  return m_lastPts;
}

void CDVDVideoCodecPS5::DetectAlignment(const VideoDec2Picture& picture, unsigned width,
                                        unsigned height)
{
  // Sample the luma plane. Lower-aligned values (the documented layout for
  // this decoder's Main10 and VP9 Profile 2 output) never set bits 10-15;
  // upper-aligned ones (P010) never set bits 0-5. A dark picture - a fade-in
  // from black - has luma 64 and chroma 512 in the lower layout, both with
  // all low bits zero, so it decides nothing: the lower layout stays assumed
  // and the next pictures are sampled, until one carries evidence.
  uint16_t lowBits = 0, highBits = 0;
  for (unsigned y = 0; y < height; y += std::max(1u, height / 64))
  {
    const auto* row = reinterpret_cast<const uint16_t*>(picture.data + y * picture.pitch);
    for (unsigned x = 0; x < width; x += std::max(1u, width / 64))
    {
      lowBits |= row[x] & 0x003f;
      highBits |= row[x] & 0xfc00;
    }
  }
  ++m_alignmentSamples;
  if (highBits != 0)
  {
    m_pixelFormat = AV_PIX_FMT_YUV420P16;
    m_colorBits = 16;
    m_alignmentKnown = true;
    m_processInfo.SetVideoPixelFormat("p010");
    CLog::Log(LOGINFO, "CDVDVideoCodecPS5: 10-bit samples in the upper 10 bits (P010; high bits "
              "{:#x}): passed as yuv420p16", highBits);
  }
  else if (lowBits != 0 || m_alignmentSamples >= 120)
  {
    m_alignmentKnown = true; // lower layout confirmed (or nothing dark enough to doubt it)
    CLog::Log(LOGINFO, "CDVDVideoCodecPS5: 10-bit samples in the lower 10 bits (low bits {:#x}, "
              "after {} pictures): passed as yuv420p10", lowBits, m_alignmentSamples);
  }
}

bool CDVDVideoCodecPS5::Keep(const VideoDec2Picture& picture)
{
  if (m_timeDecodes && m_trace > 0)
    CLog::Log(LOGINFO, "CDVDVideoCodecPS5 (trace hand-off): frame {}, luma {}", picture.frameIndex,
              LumaSamples(picture));
  // Visible area: the stream's size, never more than what was decoded.
  const unsigned width = std::min(m_width, picture.width);
  const unsigned height = std::min(m_height, picture.height);
  const size_t pitch = picture.pitch;
  const size_t size = pitch * height * 3 / 2;
  if (m_tenBit && !m_alignmentKnown)
    DetectAlignment(picture, width, height);

  if (m_deinterlace)
    return KeepDeinterlaced(picture, width, height);

  if (m_zeroCopy)
  {
    // the picture is the decoder's frame itself; CRendererPS5 shows it
    auto* frame = static_cast<CVideoBufferPS5*>(m_zeroCopyPool->Get());
    frame->Set(m_decoder, picture, m_tenBit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12);
    m_decoded.push_back(Decoded{frame, NextPts()});
    return true;
  }

  CVideoBuffer* buffer =
      m_processInfo.GetVideoBufferManager().Get(m_pixelFormat, static_cast<int>(size), nullptr);
  if (!buffer)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecPS5: no video buffer available");
    NextPts(); // keep timestamps in step
    return false;
  }
  uint8_t* planes[YuvImage::MAX_PLANES];
  const uint8_t* chroma = picture.data + pitch * picture.height; // after the full coded height
  if (!m_tenBit)
  {
    const int strides[YuvImage::MAX_PLANES] = {static_cast<int>(pitch), static_cast<int>(pitch),
                                               0};
    buffer->SetDimensions(static_cast<int>(width), static_cast<int>(height), strides);
    buffer->GetPlanes(planes);
    // luma: `height` visible rows; interleaved chroma
    std::memcpy(planes[0], picture.data, pitch * height);
    std::memcpy(planes[1], chroma, pitch * height / 2);
  }
  else
  {
    // 16-bit planar for Kodi's renderer: luma copied as is, the interleaved
    // U/V samples split into two planes of half the row length.
    const int strides[YuvImage::MAX_PLANES] = {static_cast<int>(pitch),
                                               static_cast<int>(pitch / 2),
                                               static_cast<int>(pitch / 2)};
    buffer->SetDimensions(static_cast<int>(width), static_cast<int>(height), strides);
    buffer->GetPlanes(planes);
    std::memcpy(planes[0], picture.data, pitch * height);
    const unsigned chromaWidth = (width + 1) / 2;
    for (unsigned y = 0; y < (height + 1) / 2; ++y)
    {
      const auto* src = reinterpret_cast<const uint16_t*>(chroma + y * pitch);
      auto* u = reinterpret_cast<uint16_t*>(planes[1] + y * (pitch / 2));
      auto* v = reinterpret_cast<uint16_t*>(planes[2] + y * (pitch / 2));
      for (unsigned x = 0; x < chromaWidth; ++x)
      {
        u[x] = src[2 * x];
        v[x] = src[2 * x + 1];
      }
    }
  }

  m_decoded.push_back(Decoded{buffer, NextPts()});
  return true;
}

bool CDVDVideoCodecPS5::SetupDeinterlacer()
{
  // NV12 from the decoder is split into planar 4:2:0 on the way in (bwdif
  // works on planar formats); fields in top-field-first order, as broadcast
  m_deintGraph = avfilter_graph_alloc();
  m_deintIn = av_frame_alloc();
  m_deintOut = av_frame_alloc();
  if (!m_deintGraph || !m_deintIn || !m_deintOut)
    return false;
  char args[256];
  snprintf(args, sizeof(args),
           "video_size=%ux%u:pix_fmt=%d:time_base=1/%d:pixel_aspect=1/1", m_width, m_height,
           static_cast<int>(AV_PIX_FMT_YUV420P), static_cast<int>(DVD_TIME_BASE));
  const AVFilter* bwdif = avfilter_get_by_name("bwdif");
  AVFilterContext* deint = nullptr;
  if (!bwdif ||
      avfilter_graph_create_filter(&m_deintSource, avfilter_get_by_name("buffer"), "in", args,
                                   nullptr, m_deintGraph) < 0 ||
      avfilter_graph_create_filter(&deint, bwdif, "deint", "mode=send_field:parity=tff:deint=all",
                                   nullptr, m_deintGraph) < 0 ||
      avfilter_graph_create_filter(&m_deintSink, avfilter_get_by_name("buffersink"), "out",
                                   nullptr, nullptr, m_deintGraph) < 0 ||
      avfilter_link(m_deintSource, 0, deint, 0) < 0 || avfilter_link(deint, 0, m_deintSink, 0) < 0 ||
      avfilter_graph_config(m_deintGraph, nullptr) < 0)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecPS5: the bwdif deinterlacer could not be set up");
    return false;
  }
  m_deintIn->format = AV_PIX_FMT_YUV420P;
  m_deintIn->width = static_cast<int>(m_width);
  m_deintIn->height = static_cast<int>(m_height);
  if (av_frame_get_buffer(m_deintIn, 64) < 0)
    return false;
  CLog::Log(LOGINFO, "CDVDVideoCodecPS5: deinterlacing {}x{} with bwdif (one picture per field)",
            m_width, m_height);
  m_processInfo.SetVideoDeintMethod("bwdif");
  return true;
}

void CDVDVideoCodecPS5::CloseDeinterlacer()
{
  avfilter_graph_free(&m_deintGraph); // frees the filter contexts too
  m_deintSource = nullptr;
  m_deintSink = nullptr;
  av_frame_free(&m_deintIn);
  av_frame_free(&m_deintOut);
}

bool CDVDVideoCodecPS5::KeepDeinterlaced(const VideoDec2Picture& picture, unsigned width,
                                         unsigned height)
{
  if (!m_deintGraph && !SetupDeinterlacer())
  {
    CloseDeinterlacer();
    m_deinterlace = false; // show the woven frames rather than nothing
    return Keep(picture);
  }
  if (av_frame_make_writable(m_deintIn) < 0)
    return false;
  // luma rows as they are; interleaved chroma split into U and V
  for (unsigned y = 0; y < height; ++y)
    std::memcpy(m_deintIn->data[0] + static_cast<size_t>(y) * m_deintIn->linesize[0],
                picture.data + static_cast<size_t>(y) * picture.pitch, width);
  const uint8_t* chroma = picture.data + static_cast<size_t>(picture.pitch) * picture.height;
  for (unsigned y = 0; y < height / 2; ++y)
  {
    const uint8_t* src = chroma + static_cast<size_t>(y) * picture.pitch;
    uint8_t* u = m_deintIn->data[1] + static_cast<size_t>(y) * m_deintIn->linesize[1];
    uint8_t* v = m_deintIn->data[2] + static_cast<size_t>(y) * m_deintIn->linesize[2];
    for (unsigned x = 0; x < width / 2; ++x)
    {
      u[x] = src[2 * x];
      v[x] = src[2 * x + 1];
    }
  }
  m_deintIn->pts = m_deintFrames++;
  m_deintIn->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
  m_deintPts.push_back(NextPts());
  if (av_buffersrc_add_frame_flags(m_deintSource, m_deintIn, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
    return false;

  while (av_buffersink_get_frame(m_deintSink, m_deintOut) >= 0)
  {
    // the two fields of the front input: its pts, then half a frame later
    double pts = DVD_NOPTS_VALUE;
    if (!m_deintPts.empty())
    {
      pts = m_deintPts.front() + (m_deintOutputs == 1 ? m_frameDuration / 2 : 0.0);
      if (++m_deintOutputs == 2)
      {
        m_deintPts.pop_front();
        m_deintOutputs = 0;
      }
    }
    const int strides[YuvImage::MAX_PLANES] = {static_cast<int>(width),
                                               static_cast<int>(width / 2),
                                               static_cast<int>(width / 2)};
    const size_t size = static_cast<size_t>(width) * height * 3 / 2;
    CVideoBuffer* buffer = m_processInfo.GetVideoBufferManager().Get(AV_PIX_FMT_YUV420P,
                                                                     static_cast<int>(size), nullptr);
    if (buffer)
    {
      buffer->SetDimensions(static_cast<int>(width), static_cast<int>(height), strides);
      uint8_t* planes[YuvImage::MAX_PLANES];
      buffer->GetPlanes(planes);
      for (int p = 0; p < 3; ++p)
      {
        const unsigned rows = p == 0 ? height : height / 2;
        const unsigned bytes = p == 0 ? width : width / 2;
        for (unsigned y = 0; y < rows; ++y)
          std::memcpy(planes[p] + static_cast<size_t>(y) * strides[p],
                      m_deintOut->data[p] + static_cast<size_t>(y) * m_deintOut->linesize[p],
                      bytes);
      }
      m_decoded.push_back(Decoded{buffer, pts});
    }
    av_frame_unref(m_deintOut);
  }
  return true;
}

void CDVDVideoCodecPS5::ClearQueue()
{
  for (auto& d : m_decoded)
    if (d.buffer)
      d.buffer->Release();
  m_decoded.clear();
}

void CDVDVideoCodecPS5::Reset()
{
  if (m_software)
  {
    m_softwareQueue.clear(); // a seek: the old position is not wanted any more
    m_software->Reset();
    return;
  }
  ResetDecoderState();
  m_replay.clear();
  m_replayBytes = 0;
  m_replayOverflow = false;
  m_failuresBeforeFirst = 0;
  m_seenIrap = false;
  m_cleanNoSlice = 0;
  m_cleanWaitIrap = 0;
  m_waitKeyframe = false;
  m_midStreamResets = 0;
}

void CDVDVideoCodecPS5::ResetDecoderState()
{
  ClearQueue();
  m_pts.clear();
  m_skipRasl = m_hevc;
  m_prependParameterSets = !m_parameterSets.empty();
  m_trace = 30;
  m_decoder->Reset();
  m_pendingAus.clear();
  m_vp9PendingShown.clear();
  CloseDeinterlacer(); // set up again with the next picture
  m_deintPts.clear();
  m_deintOutputs = 0;
  // The Annex-B filter injects SPS/PPS once, on its first packet. A plain
  // av_bsf_flush() does not re-arm that, so after a seek the first IDR would
  // reach the decoder without parameter sets (EVO Player's #57: pictures
  // decoded against nothing, or 0x811d0303 on every access unit). A fresh
  // filter injects them again.
  if (m_bsf)
  {
    av_bsf_free(&m_bsf);
    if (!SetupBitstreamFilter(m_hints))
      CLog::Log(LOGERROR, "CDVDVideoCodecPS5: cannot rebuild the bitstream filter after a seek");
  }
  m_errorsInRow = 0;
  m_codecControlFlags = 0;
}

CDVDVideoCodec::VCReturn CDVDVideoCodecPS5::GetPicture(VideoPicture* pVideoPicture)
{
  if (m_software)
    return m_software->GetPicture(pVideoPicture);
  if (m_fatal)
    return VC_ERROR;

  if (m_decoded.empty() && (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN))
  {
    VideoDec2Picture picture;
    if (m_decoder->Flush(&picture))
      Keep(picture);
    else
      return VC_EOF;
  }
  if (m_decoded.empty())
    return VC_BUFFER;

  Decoded d = m_decoded.front();
  m_decoded.pop_front();
  KODI::PLATFORM::PS5::ps5_video_frame_stats().decoded.fetch_add(1, std::memory_order_relaxed);
  if (m_picturesOut++ == 0)
  {
    m_replay.clear(); // the hardware takes this stream: nothing to replay
    m_replayBytes = 0;
    if (m_cleanStream)
      CLog::Log(LOGINFO, "CDVDVideoCodecPS5: {} decodes in hardware with the cleaned bitstream",
                m_streamName);
  }

  pVideoPicture->Reset(); // releases the previous picture's buffer
  pVideoPicture->videoBuffer = d.buffer;
  pVideoPicture->pts = d.pts;
  pVideoPicture->dts = DVD_NOPTS_VALUE;
  pVideoPicture->iDuration = m_deinterlace ? m_frameDuration / 2 : m_frameDuration;
  pVideoPicture->iWidth = m_width;
  pVideoPicture->iHeight = m_height;
  pVideoPicture->iDisplayWidth = m_displayWidth;
  pVideoPicture->iDisplayHeight = m_displayHeight;
  pVideoPicture->pixelFormat =
      m_deinterlace ? AV_PIX_FMT_YUV420P
                    : (m_zeroCopy ? (m_tenBit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12) : m_pixelFormat);
  pVideoPicture->colorBits = m_deinterlace ? 8 : m_colorBits;
  pVideoPicture->hasDisplayMetadata = m_hasDisplayMetadata;
  if (m_hasDisplayMetadata)
    pVideoPicture->displayMetadata = m_displayMetadata;
  pVideoPicture->hasLightMetadata = m_hasLightMetadata;
  if (m_hasLightMetadata)
    pVideoPicture->lightMetadata = m_lightMetadata;
  pVideoPicture->color_space = m_colorSpace;
  pVideoPicture->color_primaries = m_colorPrimaries;
  pVideoPicture->m_originalColorPrimaries = m_colorPrimaries;
  pVideoPicture->color_transfer = m_colorTransfer;
  pVideoPicture->color_range = m_fullRange ? 1 : 0;
  pVideoPicture->stereoMode = m_stereoMode;
  if (m_codecControlFlags & DVD_CODEC_CTRL_DROP)
    pVideoPicture->iFlags |= DVP_FLAG_DROPPED;
  return VC_PICTURE;
}

void KODI::PLATFORM::PS5::RegisterVideoCodecs()
{
  CDVDVideoCodecPS5::Register();
  // the zero-copy renderer takes only the decoder's own frames (kodi-zerocopy)
  CRendererPS5::Register();
}

bool CDVDVideoCodecPS5::SwitchToSoftware()
{
  auto software = std::make_unique<CDVDVideoCodecFFmpeg>(m_processInfo);
  CDVDStreamInfo hints = m_hints; // the stream's original hints and extradata
  hints.codecOptions |= CODEC_FORCE_SOFTWARE;
  CDVDCodecOptions options = m_options;
  if (!software->Open(hints, options))
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecPS5: software fallback could not open the stream");
    return false;
  }
  CLog::Log(LOGWARNING, "CDVDVideoCodecPS5: the hardware decoder refused {} before its first "
            "picture; continuing with software decoding ({})",
            m_streamName, software->GetName());
  // nothing of the hardware decoder was ever shown, so it can go now
  m_pendingAus.clear();
  m_pts.clear();
  m_decoder->Close();
  m_software = std::move(software);
  m_softwareQueue = std::move(m_replay); // from the start, not from the next keyframe
  m_replay.clear();
  m_replayBytes = 0;
  return true;
}

// ---------------------------------------------------------------------------
// Recovery when the hardware refuses a stream before its first picture
// ---------------------------------------------------------------------------
namespace
{
struct NalRef
{
  const uint8_t* p;
  size_t n;
};

// Annex-B access unit -> NAL unit payloads (without start codes)
std::vector<NalRef> SplitAnnexB(const uint8_t* d, size_t n)
{
  std::vector<NalRef> out;
  size_t i = 0;
  size_t start = SIZE_MAX;
  while (i < n)
  {
    size_t len = 0;
    if (i + 4 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1)
      len = 4;
    else if (i + 3 <= n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
      len = 3;
    if (len)
    {
      if (start != SIZE_MAX && i > start)
        out.push_back({d + start, i - start});
      i += len;
      start = i;
    }
    else
      ++i;
  }
  if (start != SIZE_MAX && start < n)
    out.push_back({d + start, n - start});
  return out;
}

// length-prefixed (4-byte big-endian sizes) -> NAL units; empty unless the
// sizes consume the buffer exactly, which Annex-B data practically never does
std::vector<NalRef> SplitLengthPrefixed(const uint8_t* d, size_t n)
{
  std::vector<NalRef> out;
  size_t pos = 0;
  while (pos + 4 <= n)
  {
    const size_t len = (size_t{d[pos]} << 24) | (size_t{d[pos + 1]} << 16) |
                       (size_t{d[pos + 2]} << 8) | d[pos + 3];
    pos += 4;
    if (len == 0 || len > n - pos || (d[pos] & 0x80)) // forbidden_zero_bit must be 0
      return {};
    out.push_back({d + pos, len});
    pos += len;
  }
  if (pos != n)
    return {};
  return out;
}

// the first bytes of a NAL unit with emulation prevention removed
std::vector<uint8_t> RbspHead(const NalRef& nal, size_t skip, size_t limit = 48)
{
  std::vector<uint8_t> out;
  int zeros = 0;
  for (size_t i = skip; i < nal.n && out.size() < limit; ++i)
  {
    if (zeros >= 2 && nal.p[i] == 3)
    {
      zeros = 0;
      continue;
    }
    zeros = nal.p[i] == 0 ? zeros + 1 : 0;
    out.push_back(nal.p[i]);
  }
  return out;
}

struct Bits
{
  const std::vector<uint8_t>& b;
  size_t pos = 0;
  bool ok = true;
  unsigned Read(unsigned count)
  {
    unsigned v = 0;
    for (unsigned i = 0; i < count; ++i, ++pos)
    {
      if (pos / 8 >= b.size())
      {
        ok = false;
        return 0;
      }
      v = (v << 1) | ((b[pos / 8] >> (7 - pos % 8)) & 1u);
    }
    return v;
  }
  unsigned Ue()
  {
    unsigned zeros = 0;
    while (ok && Read(1) == 0 && zeros < 31)
      ++zeros;
    return ok ? ((1u << zeros) - 1 + Read(zeros)) : 0;
  }
};

const char* HevcNalName(unsigned t)
{
  switch (t)
  {
    case 0: case 1: return "TRAIL";
    case 8: case 9: return "RASL";
    case 6: case 7: return "RADL";
    case 16: case 17: case 18: return "BLA";
    case 19: return "IDR_W_RADL";
    case 20: return "IDR_N_LP";
    case 21: return "CRA";
    case 32: return "VPS";
    case 33: return "SPS";
    case 34: return "PPS";
    case 35: return "AUD";
    case 36: return "EOS";
    case 37: return "EOB";
    case 38: return "FD";
    case 39: return "SEI";
    case 40: return "SEI_SUFFIX";
    case 62: return "DV_RPU";
    case 63: return "DV_EL";
    default: return t <= 31 ? "VCL" : "OTHER";
  }
}
} // namespace

void CDVDVideoCodecPS5::BufferForReplay(const DemuxPacket& packet)
{
  if (m_picturesOut > 0 || m_replayOverflow || !packet.pData || packet.iSize <= 0)
    return;
  constexpr size_t kMaxReplayPackets = 400;
  constexpr size_t kMaxReplayBytes = size_t{128} << 20;
  const size_t size = static_cast<size_t>(packet.iSize);
  if (m_replay.size() >= kMaxReplayPackets || m_replayBytes + size > kMaxReplayBytes)
  {
    // too long without a picture to keep it all: recovery then starts at the
    // next keyframe instead of at the beginning
    m_replayOverflow = true;
    m_replay.clear();
    m_replayBytes = 0;
    return;
  }
  m_replay.push_back(ReplayPacket{std::vector<uint8_t>(packet.pData, packet.pData + size),
                                  packet.pts, packet.dts, packet.recoveryPoint});
  m_replayBytes += size;
}

bool CDVDVideoCodecPS5::RunRecovery()
{
  m_recoveryPending = false;
  ++m_recoveryStage;
  if (m_recoveryStage == 1 && !m_vp9)
  {
    CLog::Log(LOGWARNING,
              "CDVDVideoCodecPS5: the hardware decoder refused {} ({} failed decodes, nothing "
              "shown); retrying in hardware from the start with a cleaned bitstream",
              m_streamName, m_failuresBeforeFirst);
    ResetDecoderState();
    m_cleanStream = true;
    m_seenIrap = false;
    m_failuresBeforeFirst = 0;
    m_cleanNoSlice = 0;
    m_cleanWaitIrap = 0;
    m_fingerprinted = false; // describe the cleaned stream too, if it is refused
    std::vector<uint8_t> cleanedSets;
    bool irap = false, hasSlice = false;
    CleanAccessUnit(m_parameterSets.data(), m_parameterSets.size(), cleanedSets, irap, hasSlice);
    m_parameterSets = std::move(cleanedSets);
    m_prependParameterSets = !m_parameterSets.empty();
    const std::deque<ReplayPacket> replay = m_replay; // stage 2 needs it again
    for (const ReplayPacket& r : replay)
    {
      DemuxPacket p;
      p.pData = const_cast<uint8_t*>(r.data.data());
      p.iSize = static_cast<int>(r.data.size());
      p.pts = r.pts;
      p.dts = r.dts;
      p.recoveryPoint = r.recoveryPoint;
      FeedHardware(p);
      if (m_recoveryPending)
        return RunRecovery(); // refused again: software
    }
    return true;
  }
  if (!SwitchToSoftware())
  {
    m_fatal = true;
    return false;
  }
  return true;
}

bool CDVDVideoCodecPS5::CleanAccessUnit(const uint8_t* data, size_t size,
                                        std::vector<uint8_t>& out, bool& irap,
                                        bool& hasSlice) const
{
  static const uint8_t startCode[4] = {0, 0, 0, 1};
  out.clear();
  irap = false;
  hasSlice = false;
  if (!data || size == 0)
    return false;
  for (const NalRef& nal : SplitAnnexB(data, size))
  {
    if (nal.n == 0)
      continue;
    bool keep = false;
    if (m_hevc)
    {
      const unsigned t = (nal.p[0] >> 1) & 0x3f;
      // VCL, VPS/SPS/PPS, end of sequence/bitstream; not AUD, filler, SEI,
      // reserved or unspecified (Dolby Vision RPU 62 / enhancement layer 63)
      keep = t <= 34 || t == 36 || t == 37;
      if (t <= 31)
      {
        hasSlice = true;
        if (t >= 16 && t <= 23)
          irap = true;
      }
    }
    else
    {
      const unsigned t = nal.p[0] & 0x1f;
      // slices, SPS, PPS, end of sequence/stream; not SEI, AUD, filler or
      // extensions (SVC/MVC: the base view only)
      keep = (t >= 1 && t <= 5) || t == 7 || t == 8 || t == 10 || t == 11;
      if (t >= 1 && t <= 5)
      {
        hasSlice = true;
        if (t == 5)
          irap = true;
        else if (t == 1)
        {
          const std::vector<uint8_t> head = RbspHead(nal, 1);
          Bits bits{head};
          bits.Ue(); // first_mb_in_slice
          const unsigned sliceType = bits.Ue();
          if (bits.ok && (sliceType % 5 == 2 || sliceType % 5 == 4))
            irap = true; // I/SI slice: a usable start for recovery-point streams
        }
      }
    }
    if (keep)
    {
      out.insert(out.end(), startCode, startCode + 4);
      out.insert(out.end(), nal.p, nal.p + nal.n);
    }
  }
  return true;
}

bool CDVDVideoCodecPS5::ToAnnexB(const uint8_t* data, size_t size,
                                 std::vector<uint8_t>& out) const
{
  if (!data || size < 5)
    return false;
  const std::vector<NalRef> nals = SplitLengthPrefixed(data, size);
  if (nals.empty())
    return false; // Annex-B already (or not parseable as length-prefixed)
  static const uint8_t startCode[4] = {0, 0, 0, 1};
  out.clear();
  out.reserve(size + nals.size() * 4);
  for (const NalRef& nal : nals)
  {
    out.insert(out.end(), startCode, startCode + 4);
    out.insert(out.end(), nal.p, nal.p + nal.n);
  }
  return true;
}

void CDVDVideoCodecPS5::LogFingerprint(const char* what, const uint8_t* data, size_t size) const
{
  std::string text;
  unsigned count = 0;
  std::vector<NalRef> nals = SplitAnnexB(data, size);
  if (nals.empty())
  {
    nals = SplitLengthPrefixed(data, size);
    if (!nals.empty())
      text = " [length-prefixed, not Annex-B]";
  }
  for (const NalRef& nal : nals)
  {
    if (nal.n == 0)
      continue;
    if (++count > 24)
    {
      text += " ...";
      break;
    }
    text += ' ';
    if (m_hevc && nal.n >= 2)
    {
      const unsigned t = (nal.p[0] >> 1) & 0x3f;
      text += std::to_string(t) + ":" + HevcNalName(t);
      if (t == 34)
      {
        const std::vector<uint8_t> head = RbspHead(nal, 2);
        Bits bits{head};
        const unsigned pps = bits.Ue();
        const unsigned sps = bits.Ue();
        if (bits.ok)
          text += "#" + std::to_string(pps) + "(sps " + std::to_string(sps) + ")";
      }
      else if (t <= 31)
      {
        const std::vector<uint8_t> head = RbspHead(nal, 2);
        Bits bits{head};
        bits.Read(1); // first_slice_segment_in_pic_flag
        if (t >= 16 && t <= 23)
          bits.Read(1); // no_output_of_prior_pics_flag
        const unsigned pps = bits.Ue();
        if (bits.ok)
          text += "->pps " + std::to_string(pps);
      }
    }
    else if (!m_hevc && nal.n >= 1)
    {
      const unsigned t = nal.p[0] & 0x1f;
      text += std::to_string(t);
      if (t == 7 && nal.n >= 4)
        text += ":SPS(profile " + std::to_string(nal.p[1]) + ", level " + std::to_string(nal.p[3]) +
                ")";
      else if (t == 8)
        text += ":PPS";
      else if (t == 6)
        text += ":SEI";
      else if (t == 5)
        text += ":IDR";
    }
    text += "[" + std::to_string(nal.n) + "]";
  }
  CLog::Log(LOGINFO,
            "CDVDVideoCodecPS5: {} ({} bytes; stream profile {}, level {}, {}x{}, {} bit):{}", what,
            size, m_hints.profile, m_hints.level, m_hints.width, m_hints.height,
            m_hints.bitsperpixel, text.empty() ? " (no NAL units)" : text);
}
