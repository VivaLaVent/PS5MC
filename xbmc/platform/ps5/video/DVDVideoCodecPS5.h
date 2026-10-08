/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "VideoBufferPS5.h"
#include "VideoDec2.h"
#include "cores/VideoPlayer/DVDCodecs/DVDCodecs.h" // CDVDCodecOptions (stored for the software fallback)
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"

#include <deque>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct AVBSFContext;
struct AVPacket;
class CVideoBuffer;

extern "C"
{
struct AVFilterGraph;
struct AVFilterContext;
struct AVFrame;
}

namespace KODI::PLATFORM::PS5
{

/*!
 * Hardware H.264 / HEVC Main decoding on the PS5 (libSceVideodec2).
 *
 * H.264 (and High 10 where the decoder accepts it), HEVC Main/Main10 and VP9
 * profiles 0/2 up to 3840x2176 go to the hardware; other codecs, 4:2:2/4:4:4
 * and 12-bit go to FFmpeg. A stream the hardware refuses before its first
 * picture is replayed from its start, first in hardware with a cleaned
 * bitstream, then in software (see RunRecovery).
 * An empty file "kodi-swdecode" in the title folder turns this decoder off.
 */
class CDVDVideoCodecPS5 : public CDVDVideoCodec
{
public:
  explicit CDVDVideoCodecPS5(CProcessInfo& processInfo);
  ~CDVDVideoCodecPS5() override;

  static std::unique_ptr<CDVDVideoCodec> Create(CProcessInfo& processInfo);
  static void Register();

  bool Open(CDVDStreamInfo& hints, CDVDCodecOptions& options) override;
  bool AddData(const DemuxPacket& packet) override;
  void Reset() override;
  VCReturn GetPicture(VideoPicture* pVideoPicture) override;
  const char* GetName() override { return m_software ? m_software->GetName() : "ps5-videodec2"; }
  unsigned GetAllowedReferences() override
  {
    return m_software ? m_software->GetAllowedReferences() : 4;
  }
  void SetCodecControl(int flags) override
  {
    m_codecControlFlags = flags;
    if (m_software)
      m_software->SetCodecControl(flags);
  }
  void SetSpeed(int speed) override
  {
    if (m_software)
      m_software->SetSpeed(speed);
  }
  bool GetCodecStats(double& pts, int& droppedFrames, int& skippedPics) override
  {
    return m_software ? m_software->GetCodecStats(pts, droppedFrames, skippedPics)
                      : CDVDVideoCodec::GetCodecStats(pts, droppedFrames, skippedPics);
  }

private:
  struct Decoded
  {
    CVideoBuffer* buffer = nullptr;
    double pts = 0;
  };

  bool SetupBitstreamFilter(const CDVDStreamInfo& hints);
  // The stream's parameter sets (SPS/PPS, and VPS for HEVC) as Annex-B NAL
  // units, prepended to the first access unit after an open or a reset. The
  // Annex-B filter injects them only in front of an IDR; a seek that lands on
  // a non-IDR I-frame would otherwise leave the decoder without them (black
  // pictures that P-frames slowly paint in).
  void BuildParameterSets(const CDVDStreamInfo& hints);
  std::vector<uint8_t> m_parameterSets;
  bool m_prependParameterSets = false;
  CDVDStreamInfo m_hints; // for rebuilding the bitstream filter on a seek
  bool DecodeOne(const uint8_t* data, size_t size);
  bool Keep(const VideoDec2Picture& picture);
  // kodi-debug trace of the first pictures after an open or a reset: frames
  // offered/accepted/returned, and luma samples of the returned picture at
  // return and at hand-off (black regions read 0 or 16)
  unsigned m_trace = 0;
  std::string LumaSamples(const VideoDec2Picture& picture) const;
  void Trace(const char* stage, const VideoDec2Picture& picture, bool gotPicture);
  double NextPts();
  void ClearQueue();

  // shared with zero-copy pictures: the decoder's memory lives until the last
  // picture showing one of its frames is released
  // Recovery when the hardware decoder refuses a stream before its first
  // picture. Every packet is kept until the first picture appears; on refusal
  // the stream is replayed from its start, first in hardware with a cleaned
  // bitstream (stage 1: SEI, AUD, filler and unspecified NAL units such as
  // Dolby Vision RPU/EL removed, decoding from the first IRAP with every
  // parameter set), then, if that is refused too, in software (stage 2).
  // Streams the hardware takes as they are never enter this path.
  struct ReplayPacket
  {
    std::vector<uint8_t> data;
    double pts;
    double dts;
    bool recoveryPoint;
  };
  bool FeedHardware(const DemuxPacket& packet);
  void BufferForReplay(const DemuxPacket& packet);
  void ResetDecoderState();
  bool RunRecovery();
  bool SwitchToSoftware();
  bool CleanAccessUnit(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                       bool& irap, bool& hasSlice) const;
  void LogFingerprint(const char* what, const uint8_t* data, size_t size) const;
  // Length-prefixed NAL units (MP4/MKV layout) without codec extradata: the
  // Annex-B filter needs extradata to run, so convert here; the parameter sets
  // are then in-band. Returns false (out untouched) for Annex-B input.
  bool ToAnnexB(const uint8_t* data, size_t size, std::vector<uint8_t>& out) const;
  // A stream that carries its OWN parameter sets in-band, differing from the
  // container's (extradata) copy, gets the container copies removed per type,
  // so the stream's own are what the decoder uses - as FFmpeg does. Returns
  // false (out untouched) when the access unit has no such in-band set.
  bool DropInjectedParameterSets(const uint8_t* data, size_t size, std::vector<uint8_t>& out);
  // HEVC coded in tiles (tiles_enabled_flag in a PPS): the PS5's decoder does
  // not support them (a hardware finding reported by the unofficial Stremio
  // PS5 port and by EVO/Nuvio). Checked on the container's parameter sets so
  // Open() can hand the stream to FFmpeg up front.
  bool ParameterSetsUseTiles() const;
  // The container's parameter sets as (offset, length) into m_parameterSets,
  // split once whenever m_parameterSets changes (not once per access unit).
  void RefreshContainerSets();
  std::vector<std::pair<size_t, size_t>> m_containerSetSpans;
  bool m_loggedInbandSets = false;
  std::vector<uint8_t> m_annexB;
  bool m_loggedLengthPrefixed = false;
  unsigned m_cleanNoSlice = 0;  // stage 1: access units with nothing decodable in them
  unsigned m_cleanWaitIrap = 0; // stage 1: access units dropped waiting for a keyframe
  std::unique_ptr<CDVDVideoCodec> m_software;
  CDVDCodecOptions m_options;
  unsigned m_picturesOut = 0;
  unsigned m_failuresBeforeFirst = 0; // decode failures while nothing has been shown yet
  unsigned m_midStreamResets = 0;     // decoder resyncs after a mid-stream failure run
  bool m_waitKeyframe = false;        // after a resync: drop AUs until a keyframe
  std::deque<ReplayPacket> m_replay;
  size_t m_replayBytes = 0;
  bool m_replayOverflow = false;
  std::deque<ReplayPacket> m_softwareQueue; // replayed into FFmpeg before new packets
  int m_recoveryStage = 0;                  // 0: as is, 1: cleaned bitstream, 2: software
  bool m_recoveryPending = false;
  bool m_cleanStream = false;
  bool m_seenIrap = false;
  bool m_fingerprinted = false;

  std::shared_ptr<KODI::PLATFORM::PS5::CVideoDec2> m_decoder =
      std::make_shared<KODI::PLATFORM::PS5::CVideoDec2>();
  bool m_zeroCopy = false; // kodi-zerocopy, with the GL driver additions present
  std::shared_ptr<KODI::PLATFORM::PS5::CVideoBufferPoolPS5> m_zeroCopyPool;
  // zero-copy: access units waiting for a free frame, in stream order
  std::deque<std::vector<uint8_t>> m_pendingAus;
  bool RetryPending();
  AVBSFContext* m_bsf = nullptr;
  AVPacket* m_packet = nullptr;

  std::deque<Decoded> m_decoded;
  std::multiset<double> m_pts; // decoded in display order: the smallest pending pts is next
  bool m_ptsTrimmed = false;   // logged once: stale timestamps discarded
  double m_lastPts = 0;
  double m_frameDuration = 0;

  unsigned m_width = 0;
  unsigned m_height = 0;
  unsigned m_displayWidth = 0;
  unsigned m_displayHeight = 0;
  AVColorSpace m_colorSpace = AVCOL_SPC_UNSPECIFIED;
  AVColorPrimaries m_colorPrimaries = AVCOL_PRI_UNSPECIFIED;
  AVColorTransferCharacteristic m_colorTransfer = AVCOL_TRC_UNSPECIFIED;

  // 10-bit HEVC: the decoder's samples are 16 bits wide, the value either in
  // the upper 10 bits (P010) or the lower 10; decided on the first picture.
  bool m_tenBit = false;
  bool m_vp9 = false; // superframes split; hidden frames' outputs not shown
  bool m_hevc = false;
  // HEVC after a seek (or at the start): the demuxer resumes at a CRA picture
  // whose RASL leading pictures reference frames from before it. FFmpeg drops
  // them silently; the hardware decoder errors on each. Dropped until the
  // first non-RASL picture.
  bool m_skipRasl = false;
  static bool HevcAccessUnitIsRasl(const uint8_t* data, size_t size);
  // VP9: show flags of access units whose picture has not come out yet (with
  // frames in flight, a picture belongs to an earlier access unit)
  std::deque<bool> m_vp9PendingShown;

  // decode time statistics: every 5 seconds with kodi-debug, and one summary
  // line per stream when the codec closes (always)
  const bool m_timeDecodes = getenv("KODI_PS5_DEBUG") != nullptr;
  std::string m_streamName;      // for the summary: codec and size
  float m_streamFps = 0.0f;
  std::chrono::steady_clock::time_point m_streamStart{};
  unsigned m_streamDecodes = 0;  // pictures decoded in this stream
  double m_streamDecodeMs = 0.0; // total decode time
  double m_streamMaxMs = 0.0;
  unsigned m_streamOverBudget = 0; // decodes longer than one frame period
  void LogStreamSummary();
  std::chrono::steady_clock::time_point m_decodeWindow{};
  double m_decodeTotalMs = 0.0;
  double m_decodeMaxMs = 0.0;
  unsigned m_decodeCount = 0;
  bool m_alignmentKnown = false;
  unsigned m_alignmentSamples = 0; // pictures sampled without a decision
  AVPixelFormat m_pixelFormat = AV_PIX_FMT_NV12;
  unsigned m_colorBits = 8;
  bool m_hasDisplayMetadata = false;
  AVMasteringDisplayMetadata m_displayMetadata{};
  bool m_hasLightMetadata = false;
  AVContentLightMetadata m_lightMetadata{};

  void DetectAlignment(const VideoDec2Picture& picture, unsigned width, unsigned height);

  // kodi-hw-interlaced: FFmpeg's bwdif over the hardware decoder's (woven)
  // frames, one progressive picture per field
  bool m_deinterlace = false;
  AVFilterGraph* m_deintGraph = nullptr;
  AVFilterContext* m_deintSource = nullptr;
  AVFilterContext* m_deintSink = nullptr;
  AVFrame* m_deintIn = nullptr;
  AVFrame* m_deintOut = nullptr;
  int64_t m_deintFrames = 0;
  std::deque<double> m_deintPts; // input pictures' pts, for the two fields each
  unsigned m_deintOutputs = 0;   // outputs of the front input so far
  bool SetupDeinterlacer();
  void CloseDeinterlacer();
  bool KeepDeinterlaced(const VideoDec2Picture& picture, unsigned width, unsigned height);
  bool m_fullRange = false;
  std::string m_stereoMode;

  int m_codecControlFlags = 0;
  unsigned m_errorsInRow = 0;
  bool m_fatal = false;
};

} // namespace KODI::PLATFORM::PS5
