/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>

namespace KODI::PLATFORM::PS5
{
// Frame counters for the 5-second "PS5 presentation" stats line: the codec
// bumps decoded as pictures leave the decoder, the renderer bumps presented as
// frames reach the screen. A black screen with audio is diagnosed by which of
// the two stops moving.
struct VideoFrameStats
{
  std::atomic<unsigned> decoded{0};
  std::atomic<unsigned> presented{0};
};
VideoFrameStats& ps5_video_frame_stats();
} // namespace KODI::PLATFORM::PS5

/*
 * Zero-copy video on the PS5: a picture is the hardware decoder's own frame,
 * shown by CRendererPS5 through textures over that memory (driver additions
 * in patches/ps5-opengl). The frame goes back to the decoder when Kodi
 * releases the picture; the buffer keeps the decoder (and so the memory)
 * alive until then.
 */

#include "VideoDec2.h"
#include "cores/VideoPlayer/Buffers/VideoBuffer.h"

#include <memory>
#include <mutex>
#include <vector>

namespace KODI::PLATFORM::PS5
{

// The driver additions, if this build of the GL driver has them.
bool IsZeroCopyAvailable();
void* CreateMemoryImage(const void* data, unsigned width, unsigned height, unsigned stride,
                        unsigned components, unsigned bytesPerComponent);
void DestroyMemoryImage(void* image);

class CVideoBufferPS5 : public CVideoBuffer
{
public:
  explicit CVideoBufferPS5(int id) : CVideoBuffer(id) {}

  void Set(const std::shared_ptr<CVideoDec2>& decoder, const VideoDec2Picture& picture,
           AVPixelFormat format);
  void ReturnFrame(); // to the decoder; the buffer is then empty

  // for software paths (screenshots): NV12-style planes in the decoder's frame
  void GetPlanes(uint8_t* (&planes)[YuvImage::MAX_PLANES]) override;
  void GetStrides(int (&strides)[YuvImage::MAX_PLANES]) override;

  const uint8_t* Luma() const { return m_data; }
  const uint8_t* Chroma() const { return m_data + static_cast<size_t>(m_pitch) * m_codedHeight; }
  unsigned Pitch() const { return m_pitch; }            // bytes per row, both planes
  unsigned CodedHeight() const { return m_codedHeight; } // rows before the chroma plane
  unsigned BitDepth() const { return m_bitDepth; }       // 8 (NV12) or 10 (16-bit samples)

  // The decoder whose memory this picture shows (0: none).
  uint64_t DecoderInstance() const;
  // Releasing this picture would destroy its decoder and free that memory.
  bool HoldsLastDecoderReference() const;

private:
  std::shared_ptr<CVideoDec2> m_decoder;
  int m_frameIndex = -1;
  const uint8_t* m_data = nullptr;
  unsigned m_pitch = 0;
  unsigned m_codedHeight = 0;
  unsigned m_bitDepth = 8;
};

class CVideoBufferPoolPS5 : public IVideoBufferPool
{
public:
  ~CVideoBufferPoolPS5() override;
  CVideoBuffer* Get() override;
  void Return(int id) override;

private:
  std::mutex m_mutex;
  std::vector<CVideoBufferPS5*> m_all;
  std::vector<int> m_free;
};

} // namespace KODI::PLATFORM::PS5
