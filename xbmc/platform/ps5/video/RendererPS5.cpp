/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RendererPS5.h"

#include "VideoBufferPS5.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "utils/GLUtils.h"
#include "utils/log.h"

#include <EGL/egl.h>

using namespace KODI::PLATFORM::PS5;

namespace KODI::PLATFORM::PS5
{
VideoFrameStats& ps5_video_frame_stats()
{
  static VideoFrameStats s;
  return s;
}
} // namespace KODI::PLATFORM::PS5

CBaseRenderer* CRendererPS5::Create(CVideoBuffer* buffer)
{
  if (dynamic_cast<CVideoBufferPS5*>(buffer))
    return new CRendererPS5();
  return nullptr;
}

void CRendererPS5::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("ps5", CRendererPS5::Create);
}

// Teardown order (any path where a decoder can go away): the pictures may
// hold the last reference to their decoder, and releasing them frees the
// frame memory our textures alias. So the GPU first finishes every draw that
// samples them, then the textures and images over that memory are destroyed,
// and only then are the pictures released. Kodi deletes the renderer without
// calling UnInit first, and this destructor runs before the base class's
// (which has the glFinish) - so it must do this itself.
CRendererPS5::~CRendererPS5()
{
  glFinish();
  DeleteFrames();
  for (int i = 0; i < NUM_BUFFERS; ++i)
    DeleteTexture(i);
}

void CRendererPS5::UnInit()
{
  glFinish();
  DeleteFrames();
  CLinuxRendererGL::UnInit();
}

bool CRendererPS5::HasFrames(uint64_t decoder) const
{
  for (const auto& [key, frame] : m_frames)
    if (std::get<0>(key) == decoder)
      return true;
  return false;
}

void CRendererPS5::DeleteFrames(uint64_t decoder)
{
  for (auto it = m_frames.begin(); it != m_frames.end();)
  {
    if (decoder != 0 && std::get<0>(it->first) != decoder)
    {
      ++it;
      continue;
    }
    FrameTextures& frame = it->second;
    if (frame.luma)
      glDeleteTextures(1, &frame.luma);
    if (frame.chroma)
      glDeleteTextures(1, &frame.chroma);
    DestroyMemoryImage(frame.lumaImage); // the textures keep their own reference
    DestroyMemoryImage(frame.chromaImage);
    it = m_frames.erase(it);
  }
}

bool CRendererPS5::Configure(const VideoPicture& picture, float fps, unsigned int orientation)
{
  m_imageTargetTexture2D =
      reinterpret_cast<ImageTargetTexture2D>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  for (auto& fence : m_fences)
    fence = {};
  if (m_debug)
    CLog::Log(LOGINFO, "CRendererPS5 (kodi-debug): Configure {}x{} @ {:.3f} fps, {} cached frames dropped",
              picture.iWidth, picture.iHeight, fps, m_frames.size());
  DeleteFrames(); // a new stream: frames may sit elsewhere now
  const bool ok = CLinuxRendererGL::Configure(picture, fps, orientation);
  // EGL images bind to 2D textures (the copying path uses rectangle textures)
  m_textureTarget = GL_TEXTURE_2D;
  CLog::Log(LOGINFO, "CRendererPS5: zero-copy video {}",
            m_imageTargetTexture2D ? "enabled" : "unavailable (no glEGLImageTargetTexture2DOES)");
  return ok;
}

bool CRendererPS5::CreateTexture(int index)
{
  CPictureBuffer& buf = m_buffers[index];
  YuvImage& im = buf.image;
  CYuvPlane(&planes)[YuvImage::MAX_PLANES] = buf.fields[0];

  DeleteTexture(index);
  im = {};
  for (auto& plane : planes)
    plane = {};
  im.height = m_sourceHeight;
  im.width = m_sourceWidth;
  im.cshift_x = 1;
  im.cshift_y = 1;
  planes[0].id = 1; // textures are attached per frame in UploadTexture
  // as the base renderer does: a picture still held in this buffer must be
  // attached again before it is rendered (after a reconfigure, e.g. the
  // refresh-rate switch for VRR, the ids above are placeholders)
  buf.loaded = false;
  if (m_debug)
    CLog::Log(LOGINFO, "CRendererPS5 (kodi-debug): CreateTexture {} ({}x{})", index, im.width,
              im.height);
  return true;
}

void CRendererPS5::DeleteTexture(int index)
{
  ReleaseBuffer(index);
  CPictureBuffer& buf = m_buffers[index];
  buf.fields[FIELD_FULL][0].id = 0;
  buf.fields[FIELD_FULL][1].id = 0;
  buf.fields[FIELD_FULL][2].id = 0;
}

bool CRendererPS5::UploadTexture(int index)
{
  CPictureBuffer& buf = m_buffers[index];
  auto* frame = dynamic_cast<CVideoBufferPS5*>(buf.videoBuffer);
  if (!frame || !frame->Luma() || !m_imageTargetTexture2D)
  {
    if (m_debug)
      CLog::Log(LOGWARNING, "CRendererPS5 (kodi-debug): buffer {} has {}", index,
                !frame ? "no zero-copy picture" : (!frame->Luma() ? "an empty picture" : "no EGL image function"));
    return false;
  }

  const unsigned bytes = frame->BitDepth() > 8 ? 2 : 1;
  const unsigned pitch = frame->Pitch();
  const unsigned rows = frame->CodedHeight();
  // texture widths from the pitch: the driver's linear rows are exactly
  // width x bytes rounded up to 256; only the visible part is sampled
  const unsigned lumaWidth = pitch / bytes;
  const unsigned chromaWidth = pitch / (2 * bytes);
  const unsigned chromaRows = rows / 2;

  const FrameKey key{frame->DecoderInstance(), frame->Luma(), pitch, rows, bytes};
  auto it = m_frames.find(key);
  if (it == m_frames.end())
  {
    FrameTextures textures;
    textures.lumaImage = CreateMemoryImage(frame->Luma(), lumaWidth, rows, pitch, 1, bytes);
    textures.chromaImage =
        CreateMemoryImage(frame->Chroma(), chromaWidth, chromaRows, pitch, 2, bytes);
    if (!textures.lumaImage || !textures.chromaImage)
    {
      DestroyMemoryImage(textures.lumaImage);
      DestroyMemoryImage(textures.chromaImage);
      if (!m_failureLogged)
        CLog::Log(LOGERROR,
                  "CRendererPS5: the GL driver refused a texture over the decoder's frame "
                  "({} bytes per row, {} rows, {}-bit)",
                  pitch, rows, bytes * 8);
      m_failureLogged = true;
      return false;
    }
    glGenTextures(1, &textures.luma);
    glGenTextures(1, &textures.chroma);
    const std::pair<GLuint, void*> bind[] = {{textures.luma, textures.lumaImage},
                                             {textures.chroma, textures.chromaImage}};
    for (const auto& [texture, image] : bind)
    {
      glBindTexture(GL_TEXTURE_2D, texture);
      m_imageTargetTexture2D(GL_TEXTURE_2D, image);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    VerifyGLState();
    it = m_frames.emplace(key, textures).first;
    if (m_debug)
      CLog::Log(LOGINFO,
                "CRendererPS5 (kodi-debug): buffer {}: textures {} (luma) / {} (chroma) over frame "
                "{} ({}x{} texels, {}-bit)",
                index, textures.luma, textures.chroma, static_cast<const void*>(frame->Luma()),
                lumaWidth, rows, bytes * 8);
    if (m_frames.size() == 1)
      CLog::Log(LOGINFO,
                "CRendererPS5: first frame shown without copying ({} bytes per row, {} rows, "
                "{}-bit textures, {}-bit values)",
                pitch, rows, bytes * 8, bytes == 2 ? buf.m_srcBits : 8);
  }

  CYuvPlane(&planes)[3] = buf.fields[0];
  planes[0].texwidth = lumaWidth;
  planes[0].texheight = rows;
  planes[1].texwidth = chromaWidth;
  planes[1].texheight = chromaRows;
  planes[2].texwidth = chromaWidth;
  planes[2].texheight = chromaRows;
  for (auto& plane : planes)
  {
    plane.pixpertex_x = 1;
    plane.pixpertex_y = 1;
  }
  planes[0].id = it->second.luma;
  planes[1].id = it->second.chroma;
  planes[2].id = it->second.chroma;
  // Kodi's convention: the depth of the values inside the texture, not the
  // texture's width - 10 for samples in the lower 10 bits of 16-bit words
  // (the shader then scales by 65535/1023), 16 for upper-aligned ones. The
  // codec reports that as the picture's colorBits (buf.m_srcBits).
  buf.m_srcTextureBits = bytes == 2 ? buf.m_srcBits : 8;

  CalculateTextureSourceRects(index, 3);
  return true;
}

void CRendererPS5::AfterRenderHook(int idx)
{
  // a frame has been drawn for this buffer: counts as presented
  ps5_video_frame_stats().presented.fetch_add(1, std::memory_order_relaxed);

  if (glIsSync(m_fences[idx]))
    glDeleteSync(m_fences[idx]);
  m_fences[idx] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

bool CRendererPS5::NeedBuffer(int idx)
{
  // keep the frame from the decoder until the GPU has finished sampling it
  if (glIsSync(m_fences[idx]))
  {
    GLint state = 0;
    GLsizei length = 0;
    glGetSynciv(m_fences[idx], GL_SYNC_STATUS, 1, &length, &state);
    if (state != GL_SIGNALED)
      return true;
    glDeleteSync(m_fences[idx]);
    m_fences[idx] = {};
  }
  return false;
}

void CRendererPS5::ReleaseBuffer(int idx)
{
  // Normally Kodi asks NeedBuffer first and the GPU is long done; teardown and
  // flush paths release directly, so wait (bounded) for the GPU to finish
  // sampling this picture before it goes back to the decoder.
  if (glIsSync(m_fences[idx]))
  {
    const GLenum result =
        glClientWaitSync(m_fences[idx], GL_SYNC_FLUSH_COMMANDS_BIT, 500 * 1000 * 1000);
    if (result == GL_TIMEOUT_EXPIRED && !m_fenceTimeoutLogged)
    {
      m_fenceTimeoutLogged = true;
      CLog::Log(LOGWARNING, "CRendererPS5: the GPU had not finished with picture {} after "
                            "500 ms; releasing it anyway", idx);
    }
    glDeleteSync(m_fences[idx]);
    m_fences[idx] = {};
  }
  // This picture may be the last owner of its decoder: then releasing it frees
  // the memory our textures for that decoder alias - destroy them first, once
  // the GPU has finished with every frame.
  const auto* frame = dynamic_cast<CVideoBufferPS5*>(m_buffers[idx].videoBuffer);
  if (frame && frame->HoldsLastDecoderReference() && HasFrames(frame->DecoderInstance()))
  {
    glFinish();
    DeleteFrames(frame->DecoderInstance());
  }
  CLinuxRendererGL::ReleaseBuffer(idx);
}
