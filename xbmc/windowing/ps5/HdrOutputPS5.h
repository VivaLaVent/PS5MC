/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*
 * HDR output on the PS5, for HDR10 (PQ) and HLG video (BT.2020).
 *
 * The scanout buffers are switched to the platform's HDR 10-bit format
 * (2:10:10:10 PQ; GL driver addition, in place via
 * sceVideoOutSubmitChangeBufferAttribute2) while such a video plays, and back
 * to SDR afterwards. Kodi's GL renderer then delivers the video's PQ-coded
 * BT.2020 RGB unchanged (HDR passthrough; HLG is converted to PQ in the
 * shader, patch 0014) and composites the GUI in PQ (its GUI-compositing path,
 * as Kodi's Linux GL window system implements it).
 *
 * The GL framebuffer stays 8-bit BGRA. While HDR is active, Kodi's frame is
 * rendered into a 10-bit intermediate framebuffer (the "default framebuffer"
 * Kodi binds; see patch 0013), and a final pass packs each pixel into the
 * 2:10:10:10 word the scanout reads, written as four bytes.
 */

#include "DefaultFramebufferPS5.h"
#include "cores/VideoPlayer/VideoRenderers/FrameBufferObject.h"
#include "rendering/gl/GuiCompositeShaderGL.h"
#include "system_gl.h"

#include <memory>

struct VideoPicture;

namespace KODI::PLATFORM::PS5
{

class CHdrOutputPS5
{
public:
  ~CHdrOutputPS5();

  // Kodi's window-system hooks
  bool SetHDR(const VideoPicture* picture); // true: PQ passthrough active
  bool IsActive() const { return m_active; }
  bool ConvertsHlg() const { return m_active && m_hlg; }
  bool IsGuiCompositing() const { return m_guiCompositing; }
  bool SetGuiCompositing(int colorTransfer, bool limitedColor);
  bool BeginGuiComposite(bool guiWillRender, int width, int height, bool depth);
  void EndGuiComposite();
  void CompositeGui(unsigned guiElementCount);

  // frame plumbing: bind the 10-bit target at frame start; pack into the real
  // framebuffer before presenting. Both no-ops when HDR is not active.
  void BindTarget(int width, int height);
  void Pack(int width, int height);
  GLuint DefaultFramebuffer() const { return m_active ? m_fbo : 0; }

private:
  bool CreateTarget(int width, int height);
  void DestroyTarget();
  bool CreatePackProgram();
  void SwitchScanout(bool hdr);

  bool m_active = false;   // scanout in the HDR format, frame packed
  bool m_hlg = false;      // the video is HLG: converted to PQ in the shader
  bool m_hdrScanout = false;
  GLuint m_fbo = 0;        // RGB10_A2 + depth intermediate target
  GLuint m_texture = 0;
  GLuint m_depth = 0;
  int m_width = 0;
  int m_height = 0;
  GLuint m_packProgram = 0;
  GLint m_packTexLoc = -1;
  GLuint m_packVao = 0;
  GLuint m_packVbo = 0;

  // GUI compositing (as CWinSystemGbmGLContext)
  bool m_guiCompositing = false;
  bool m_guiWillRender = false;
  bool m_guiFboClean = false;
  CFrameBufferObject m_guiFbo;
  int m_guiFboWidth = 0;
  int m_guiFboHeight = 0;
  std::unique_ptr<CGuiCompositeShaderGL> m_compositeShader;
  // failure-only diagnostics (see BindTarget / Pack)
  GLenum m_lastPackError{GL_NO_ERROR};
  unsigned m_packFrames{0};
};


} // namespace KODI::PLATFORM::PS5
