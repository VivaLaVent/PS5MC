/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "HdrOutputPS5.h"

#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "platform/ps5/VideoOutInfo.h"
#include "rendering/gl/GuiCompositeShaderGL.h"
#include "utils/log.h"

extern "C"
{
#include <libavutil/pixfmt.h>
}

using namespace KODI::PLATFORM::PS5;

namespace
{
CHdrOutputPS5* g_hdrOutput = nullptr; // for DefaultFramebuffer()

// Pack a pixel of the RGB10_A2 target into the 2:10:10:10 word the HDR
// scanout reads (R in bits 9-0, G 19-10, B 29-20, A 31-30), written into the
// B8G8R8A8 framebuffer as its four bytes: memory order B, G, R, A = word bytes
// 0, 1, 2, 3.
const char* const kPackVertex = R"(#version 150
in vec2 pos;
out vec2 uv;
void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); })";

const char* const kPackFragment = R"(#version 150
uniform sampler2D tex;
in vec2 uv;
out vec4 color;
void main() {
  vec3 c = clamp(texture(tex, uv).rgb, 0.0, 1.0);
  uvec3 q = uvec3(round(c * 1023.0));
  uint w = q.r | (q.g << 10u) | (q.b << 20u) | (3u << 30u);
  color = vec4(float((w >> 16u) & 255u), float((w >> 8u) & 255u), float(w & 255u),
               float(w >> 24u)) / 255.0;
})";

GLuint Compile(GLenum type, const char* source)
{
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok)
  {
    char log[1024] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    CLog::Log(LOGERROR, "CHdrOutputPS5: pack shader: {}", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}
} // namespace

unsigned int KODI::PLATFORM::PS5::DefaultFramebuffer()
{
  return g_hdrOutput ? g_hdrOutput->DefaultFramebuffer() : 0;
}

bool KODI::PLATFORM::PS5::HdrOutputConvertsHlg()
{
  return g_hdrOutput && g_hdrOutput->ConvertsHlg();
}

CHdrOutputPS5::~CHdrOutputPS5()
{
  SwitchScanout(false);
  DestroyTarget();
  if (m_packProgram)
    glDeleteProgram(m_packProgram);
  if (m_packVbo)
    glDeleteBuffers(1, &m_packVbo);
  if (m_packVao)
    glDeleteVertexArrays(1, &m_packVao);
  m_guiFbo.Cleanup();
  if (g_hdrOutput == this)
    g_hdrOutput = nullptr;
}

void CHdrOutputPS5::SwitchScanout(bool hdr)
{
  if (hdr == m_hdrScanout)
    return;
  int32_t results[4];
  const int rc = SetScanoutFormat(hdr ? kScanoutFormatHdr : kScanoutFormatSdr, results);
  CLog::Log(rc == 0 ? LOGINFO : LOGWARNING, "CHdrOutputPS5: scanout to {}: {} ({})",
            hdr ? "HDR (BT.2020 PQ, 10-bit)" : "SDR", rc == 0 ? "in effect" : "refused",
            DescribeScanoutResults(results));
  if (rc == 0)
    m_hdrScanout = hdr;
}

bool CHdrOutputPS5::SetHDR(const VideoPicture* picture)
{
  g_hdrOutput = this;
  // The platform's HDR format is BT.2020 PQ: PQ video passes through
  // unchanged, HLG is converted to PQ in Kodi's YUV shader (patch 0014)
  const bool pq = picture && picture->color_transfer == AVCOL_TRC_SMPTE2084;
  const bool hlg = picture && picture->color_transfer == AVCOL_TRC_ARIB_STD_B67;
  const bool wantHdr = (pq || hlg) && IsDisplayHdr();
  m_hlg = wantHdr && hlg;
  if (wantHdr && !CreatePackProgram())
    return false;
  SwitchScanout(wantHdr);
  m_active = wantHdr && m_hdrScanout;
  if (!m_active)
    DestroyTarget();
  CLog::Log(LOGINFO, "CHdrOutputPS5: HDR output {}{}", m_active ? "active" : "off",
            m_active && m_hlg ? " (HLG converted to PQ)" : "");
  return m_active;
}

bool CHdrOutputPS5::CreatePackProgram()
{
  if (m_packProgram)
    return true;
  const GLuint vs = Compile(GL_VERTEX_SHADER, kPackVertex);
  const GLuint fs = Compile(GL_FRAGMENT_SHADER, kPackFragment);
  if (!vs || !fs)
  {
    if (vs)
      glDeleteShader(vs);
    if (fs)
      glDeleteShader(fs);
    return false;
  }
  m_packProgram = glCreateProgram();
  glAttachShader(m_packProgram, vs);
  glAttachShader(m_packProgram, fs);
  glBindAttribLocation(m_packProgram, 0, "pos");
  glLinkProgram(m_packProgram);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = GL_FALSE;
  glGetProgramiv(m_packProgram, GL_LINK_STATUS, &ok);
  if (!ok)
  {
    CLog::Log(LOGERROR, "CHdrOutputPS5: pack program did not link");
    glDeleteProgram(m_packProgram);
    m_packProgram = 0;
    return false;
  }
  m_packTexLoc = glGetUniformLocation(m_packProgram, "tex");
  const GLfloat quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
  glGenVertexArrays(1, &m_packVao);
  glGenBuffers(1, &m_packVbo);
  glBindVertexArray(m_packVao);
  glBindBuffer(GL_ARRAY_BUFFER, m_packVbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  return true;
}

bool CHdrOutputPS5::CreateTarget(int width, int height)
{
  if (m_fbo && m_width == width && m_height == height)
    return true;
  DestroyTarget();
  glGenTextures(1, &m_texture);
  glBindTexture(GL_TEXTURE_2D, m_texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, width, height, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
               nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenRenderbuffers(1, &m_depth);
  glBindRenderbuffer(GL_RENDERBUFFER, m_depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  glGenFramebuffers(1, &m_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth);
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE)
  {
    CLog::Log(LOGERROR, "CHdrOutputPS5: 10-bit target {}x{} incomplete ({:#x})", width, height,
              status);
    DestroyTarget();
    return false;
  }
  m_width = width;
  m_height = height;
  CLog::Log(LOGINFO, "CHdrOutputPS5: 10-bit intermediate target {}x{}", width, height);
  return true;
}

void CHdrOutputPS5::DestroyTarget()
{
  if (m_fbo)
    glDeleteFramebuffers(1, &m_fbo);
  if (m_depth)
    glDeleteRenderbuffers(1, &m_depth);
  if (m_texture)
    glDeleteTextures(1, &m_texture);
  m_fbo = m_depth = m_texture = 0;
  m_width = m_height = 0;
}

void CHdrOutputPS5::BindTarget(int width, int height)
{
  if (!m_active)
    return;
  if (!CreateTarget(width, height))
  {
    // no target: fall back to SDR output rather than show nothing
    SwitchScanout(false);
    m_active = false;
    return;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
}

void CHdrOutputPS5::Pack(int width, int height)
{
  if (!m_active || !m_fbo || !m_packProgram)
    return;
  // Kodi's core-profile rendering keeps its own vertex array bound: restore it
  GLint previousVao = 0;
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DITHER); // the bytes must land exactly as computed
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glViewport(0, 0, width, height);
  glUseProgram(m_packProgram);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_texture);
  glUniform1i(m_packTexLoc, 0);
  glBindVertexArray(m_packVao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(static_cast<GLuint>(previousVao));
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
  glEnable(GL_DITHER);
}

// ---- GUI compositing, as CWinSystemGbmGLContext (Kodi's Linux GL) does it ----

bool CHdrOutputPS5::SetGuiCompositing(int colorTransfer, bool limitedColor)
{
  m_guiCompositing = colorTransfer != 0 && m_active;
  if (m_guiCompositing)
  {
    if (!m_compositeShader)
    {
      std::string defines;
      if (limitedColor)
        defines += "#define KODI_LIMITED_RANGE 1\n";
      m_compositeShader = std::make_unique<CGuiCompositeShaderGL>(defines);
      if (!m_compositeShader->CompileAndLink())
      {
        CLog::Log(LOGERROR, "CHdrOutputPS5: failed to compile the GUI composite shader");
        m_compositeShader.reset();
        m_guiCompositing = false;
      }
    }
  }
  else
  {
    m_guiFbo.Cleanup();
    m_guiFboWidth = 0;
    m_guiFboHeight = 0;
    m_compositeShader.reset();
  }
  return m_guiCompositing;
}

bool CHdrOutputPS5::BeginGuiComposite(bool guiWillRender, int width, int height, bool depth)
{
  if (!m_guiCompositing)
    return false;
  m_guiWillRender = guiWillRender;
  if (!m_guiFbo.IsValid() || m_guiFboWidth != width || m_guiFboHeight != height)
  {
    m_guiFbo.Cleanup();
    if (!m_guiFbo.Initialize() ||
        !m_guiFbo.CreateAndBindToTexture(GL_TEXTURE_2D, width, height, GL_RGBA) ||
#if PS5_KODI_MAJOR >= 22
        (depth && !m_guiFbo.AttachDepthBuffer(width, height)))
#else // Kodi 21: no front-to-back GUI rendering, so depth is never requested
        depth)
#endif
    {
      CLog::Log(LOGERROR, "CHdrOutputPS5: failed to create the GUI FBO {}x{}", width, height);
      m_guiFbo.Cleanup();
      return false;
    }
    m_guiFboWidth = width;
    m_guiFboHeight = height;
    m_guiFboClean = false;
  }
  if (!guiWillRender)
    return true;
  if (!m_guiFbo.BeginRender())
    return false;
  if (!m_guiFboClean)
  {
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    m_guiFboClean = true;
  }
  return true;
}

void CHdrOutputPS5::EndGuiComposite()
{
  if (m_guiWillRender)
    m_guiFbo.EndRender(); // binds the "default" framebuffer: our 10-bit target
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

void CHdrOutputPS5::CompositeGui(unsigned guiElementCount)
{
  if (!m_guiFbo.IsValid() || !m_guiFbo.IsBound() || !m_compositeShader)
    return;
  if (m_guiWillRender)
  {
    m_guiFboClean = guiElementCount == 0;
    if (m_guiFboClean)
      return;
  }
  else if (m_guiFboClean)
    return;

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_guiFbo.Texture());
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  const float w = static_cast<float>(m_guiFboWidth);
  const float h = static_cast<float>(m_guiFboHeight);
  const GLfloat proj[16] = {2.0f / w, 0, 0, 0, 0, -2.0f / h, 0, 0, 0, 0, -1, 0, -1.0f, 1.0f, 0, 1};
  m_compositeShader->SetProjection(proj);
  m_compositeShader->SetSdrPeak(203.0f / 10000.0f);
  m_compositeShader->Enable();
  const GLint posLoc = m_compositeShader->GetPosLoc();
  const GLint texLoc = m_compositeShader->GetTexLoc();
  const GLfloat vert[4][2] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
  const GLfloat tex[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  const GLubyte idx[4] = {0, 1, 3, 2};
  GLuint vbo[3];
  glGenBuffers(3, vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vert), vert, GL_STREAM_DRAW);
  glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(posLoc);
  glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(tex), tex, GL_STREAM_DRAW);
  glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(texLoc);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo[2]);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STREAM_DRAW);
  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, nullptr);
  glDisableVertexAttribArray(posLoc);
  glDisableVertexAttribArray(texLoc);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glDeleteBuffers(3, vbo);
  m_compositeShader->Disable();
}
