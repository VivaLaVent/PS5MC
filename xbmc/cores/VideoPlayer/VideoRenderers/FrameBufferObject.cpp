/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "FrameBufferObject.h"

#include "ServiceBroker.h"
#include "rendering/RenderSystem.h"
#include "utils/GLUtils.h"
#include "utils/log.h"

// The "default" framebuffer: 0, except on the PS5 while HDR output is active,
// where Kodi's frame is rendered into a 10-bit intermediate target that a final
// pass packs for the HDR scanout (xbmc/windowing/ps5/HdrOutputPS5.cpp).
#if defined(TARGET_PS5)
#include "windowing/ps5/DefaultFramebufferPS5.h"
#define KODI_DEFAULT_FRAMEBUFFER KODI::PLATFORM::PS5::DefaultFramebuffer()
#else
#define KODI_DEFAULT_FRAMEBUFFER 0
#endif

//////////////////////////////////////////////////////////////////////
// CFrameBufferObject
//////////////////////////////////////////////////////////////////////

CFrameBufferObject::CFrameBufferObject()
{
  m_valid = false;
  m_supported = false;
  m_bound = false;
}

bool CFrameBufferObject::IsSupported()
{
  if(CServiceBroker::GetRenderSystem()->IsExtSupported("GL_EXT_framebuffer_object"))
    m_supported = true;
  else
    m_supported = false;
  return m_supported;
}

bool CFrameBufferObject::Initialize()
{
  if (!IsSupported())
    return false;

  Cleanup();

  glGenFramebuffers(1, &m_fbo);
  VerifyGLState();

  if (!m_fbo)
    return false;

  m_valid = true;
  return true;
}

void CFrameBufferObject::Cleanup()
{
  if (!IsValid())
    return;

  if (m_fbo)
    glDeleteFramebuffers(1, &m_fbo);

  if (m_texid)
    glDeleteTextures(1, &m_texid);

  m_texid = 0;
  m_fbo = 0;
  m_valid = false;
  m_bound = false;
}

bool CFrameBufferObject::CreateAndBindToTexture(GLenum target, int width, int height, GLenum format, GLenum type,
                                                GLenum filter, GLenum clampmode)
{
  if (!IsValid())
    return false;

  if (m_texid)
    glDeleteTextures(1, &m_texid);

  glGenTextures(1, &m_texid);
  glBindTexture(target, m_texid);
  glTexImage2D(target, 0, format,  width, height, 0, GL_RGBA, type, NULL);
  glTexParameteri(target, GL_TEXTURE_WRAP_S, clampmode);
  glTexParameteri(target, GL_TEXTURE_WRAP_T, clampmode);
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
  VerifyGLState();

  m_bound = false;
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glBindTexture(target, m_texid);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, m_texid, 0);
  VerifyGLState();
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, KODI_DEFAULT_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE)
  {
    VerifyGLState();
    return false;
  }
  m_bound = true;
  return true;
}

void CFrameBufferObject::SetFiltering(GLenum target, GLenum mode)
{
  glBindTexture(target, m_texid);
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, mode);
  glTexParameteri(target, GL_TEXTURE_MIN_FILTER, mode);
}

// Begin rendering to FBO
bool CFrameBufferObject::BeginRender()
{
  if (IsValid() && IsBound())
  {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    return true;
  }
  return false;
}

// Finish rendering to FBO
void CFrameBufferObject::EndRender() const
{
  if (IsValid())
    glBindFramebuffer(GL_FRAMEBUFFER, KODI_DEFAULT_FRAMEBUFFER);
}
