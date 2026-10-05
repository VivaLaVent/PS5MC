/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*
 * Zero-copy renderer for the PS5 hardware decoder (kodi-zerocopy): the GL
 * textures lie over the decoder's frames (driver additions in
 * patches/ps5-opengl), so nothing is copied or uploaded per frame. Modelled
 * on Kodi's VAAPI GL renderer: NV12 shader, textures per frame, fences so a
 * frame returns to the decoder only after the GPU is done with it.
 */

#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGL.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <tuple>

class CRendererPS5 : public CLinuxRendererGL
{
public:
  CRendererPS5() = default;
  ~CRendererPS5() override;

  static CBaseRenderer* Create(CVideoBuffer* buffer);
  static void Register();

  bool Configure(const VideoPicture& picture, float fps, unsigned int orientation) override;
  void ReleaseBuffer(int idx) override;
  bool NeedBuffer(int idx) override;

protected:
  bool LoadShadersHook() override { return false; }
  bool RenderHook(int idx) override { return false; }
  void AfterRenderHook(int idx) override;
  bool UploadTexture(int index) override;
  void DeleteTexture(int index) override;
  bool CreateTexture(int index) override;
  EShaderFormat GetShaderFormat() override { return SHADER_NV12; }

private:
  struct FrameTextures
  {
    GLuint luma = 0;
    GLuint chroma = 0;
    void* lumaImage = nullptr;
    void* chromaImage = nullptr;
  };
  // a frame's memory never moves: its textures are made once and reused
  using FrameKey = std::tuple<const void*, unsigned, unsigned, unsigned>; // data, pitch, rows, bits
  std::map<FrameKey, FrameTextures> m_frames;
  GLsync m_fences[NUM_BUFFERS] = {};
  using ImageTargetTexture2D = void (*)(GLenum target, void* image);
  ImageTargetTexture2D m_imageTargetTexture2D = nullptr;
  bool m_failureLogged = false;
  const bool m_debug = std::getenv("KODI_PS5_DEBUG") != nullptr; // per-buffer texture log

  void DeleteFrames();
};
