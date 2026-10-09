/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*
 * The PS5's hardware video decoder (system library libSceVideodec2).
 *
 * The structure layouts and call sequence follow what is known to work on
 * hardware (interface facts, as used by ProsperoLight). Decoded pictures come
 * back as linear NV12 in CPU-visible direct memory: luma rows of `pitch`
 * bytes, `height` rows (the coded height, e.g. 1088), then the interleaved
 * chroma plane with the same pitch.
 */

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace KODI::PLATFORM::PS5
{

enum class VideoDec2Codec
{
  H264,
  HEVC,
  HEVCMain10,
  VP9,          // Profile 0 (8-bit)
  VP9Profile2,  // 10-bit
  H264High10,   // tried on the hardware; refused -> FFmpeg
};

struct VideoDec2Picture
{
  const uint8_t* data = nullptr; // luma; chroma at data + pitch * height
  uint32_t width = 0;  // decoded width
  uint32_t height = 0; // coded height (chroma offset in rows)
  uint32_t pitch = 0;  // bytes per row, both planes
  uint32_t bitDepth = 8; // 8: NV12; 10: 16 bits per sample, semi-planar
  int frameIndex = -1;   // pooled mode: the frame to hand back with ReleaseFrame
  bool immediate = false; // came out of the frame offered with this access unit
  // trace (kodi-debug): the frame offered with this call, whether the decoder
  // took it, and the decoder's own picture count for the returned output
  int offeredIndex = -1;
  bool offeredAccepted = false;
  unsigned pictureCount = 0;
};

class CVideoDec2
{
public:
  CVideoDec2() = default;
  ~CVideoDec2();
  CVideoDec2(const CVideoDec2&) = delete;
  CVideoDec2& operator=(const CVideoDec2&) = delete;

  // The video decoder system module, loaded once (call at start-up, before
  // the sandbox is opened); negative on failure.
  static int32_t LoadModule();

  // Pooled mode (zero-copy video): a frame returned as a picture stays out of
  // the decoder's reach until ReleaseFrame. Set before Open.
  void SetPooled(bool pooled) { m_pooled = pooled; }
  bool IsPooled() const { return m_pooled; }
  // pooled mode: Decode found no free frame; the access unit was not consumed
  bool Stalled() const { return m_stalled; }
  // pooled mode: Kodi no longer shows the picture in this frame (any thread)
  void ReleaseFrame(int index);
  // pooled mode: whether a frame is free for the next decode (always in ring mode)
  bool HasFreeFrame() const;

  // Sets up memory, compute queue and decoder for streams up to width x height.
  // streamLevel: the stream's level_idc (FFmpeg's codecpar->level), 0 if
  // unknown; raises the decoder's maximum level for streams above the default.
  bool Open(VideoDec2Codec codec, int width, int height, std::string& error,
            bool interlaced = false, int streamLevel = 0);
  void Close();

  // Decode one access unit (Annex-B). Returns false on a decoder error.
  // gotPicture tells whether *picture holds a decoded frame; its data stays
  // valid until the frame buffer is reused (kFrameBuffers decodes later).
  bool Decode(const uint8_t* au, size_t size, bool& gotPicture, VideoDec2Picture* picture,
              std::string& error);
  // Squeeze out a held-back picture at end of stream; false when none is left.
  bool Flush(VideoDec2Picture* picture);
  // After a seek: drop all state (next input must start with a keyframe).
  void Reset();

  size_t MaxAccessUnit() const { return m_inputSize; }

  // Unique per decoder object, never reused (unlike its memory addresses):
  // lets the renderer tell one decoder's frames from the next one's.
  uint64_t Instance() const { return m_instance; }

private:
  static uint64_t NextInstance();
  const uint64_t m_instance = NextInstance();
  struct DirectMemory
  {
    void* address = nullptr;
    size_t size = 0;
    int64_t start = -1;
  };
  static bool AllocateDirect(size_t size, int protection, DirectMemory& out, std::string& error);
  static void FreeDirect(DirectMemory& mem);
  bool ToPicture(const void* output, VideoDec2Picture* picture);
  uint8_t* NextFrameBuffer(int& index);
  int FrameIndexOf(const void* buffer) const;

  static constexpr unsigned kFrameBuffers = 20; // DPB (up to 16) + in flight + margin

  bool m_moduleLoaded = false;
  void* m_computeQueue = nullptr;
  void* m_decoder = nullptr;
  DirectMemory m_computeMemory;
  DirectMemory m_gpuMemory;
  DirectMemory m_cpuGpuMemory;
  DirectMemory m_inputMemory;
  DirectMemory m_frameMemory;
  DirectMemory m_cpuWorkspaceDirect; // fallback when flexible memory is exhausted
  void* m_cpuWorkspace = nullptr;
  size_t m_cpuWorkspaceSize = 0;
  size_t m_inputSize = 0;
  size_t m_frameSize = 0;
  unsigned m_nextFrame = 0;

  // pooled mode: who holds each frame
  enum class FrameState
  {
    Free,
    Decoder, // offered to and accepted by the decoder (reference or reordering)
    Kodi,    // returned as a picture, until ReleaseFrame
  };
  bool m_pooled = false;
  bool m_stalled = false;
  mutable std::mutex m_frameMutex;
  FrameState m_frameState[kFrameBuffers] = {};
  uint32_t m_codecType = 0;
  bool m_tenBit = false;
  mutable bool m_formatLogged = false;
};

} // namespace KODI::PLATFORM::PS5
