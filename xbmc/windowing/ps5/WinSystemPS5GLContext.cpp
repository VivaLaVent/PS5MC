/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemPS5GLContext.h"

#include "platform/ps5/video/VideoBufferPS5.h"

#include "platform/ps5/SandboxPS5.h"
#include "platform/ps5/VideoOutInfo.h"
#include "settings/DisplaySettings.h"

#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGL.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "utils/log.h"
#include "windowing/WindowSystemFactory.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <stdexcept>
#include <thread>

#if __has_include(<ps5_opengl_display.h>)
#include <ps5_opengl_display.h> // PS5_OPENGL_NATIVE_{WIDTH,HEIGHT,FPS}: SDK build profile
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>

using namespace KODI::WINDOWING::PS5;

// No EGL platform extension: the SDK's legacy eglGetDisplay() is the path.
CWinSystemPS5GLContext::CWinSystemPS5GLContext() : LINUX::CWinSystemEGL(EGL_NONE, "")
{
}

void CWinSystemPS5GLContext::Register()
{
  CWindowSystemFactory::RegisterWindowSystem(CreateWinSystem, "ps5");
}

std::unique_ptr<CWinSystemBase> CWinSystemPS5GLContext::CreateWinSystem()
{
  return std::make_unique<CWinSystemPS5GLContext>();
}

bool CWinSystemPS5GLContext::InitWindowSystem()
{
  // Software-decoded video rendered through the generic GL YUV renderer.
  // Hardware decoding (VideoDec2) plugs in here later as a HW accel + renderer.
  VIDEOPLAYER::CRendererFactory::ClearRenderer();
  CDVDFactoryCodec::ClearHWAccels();
  CLinuxRendererGL::Register();

  if (!CWinSystemPS5::InitWindowSystem())
    return false;

  if (!m_eglContext.CreateDisplay(EGL_DEFAULT_DISPLAY))
    return false;

  if (!m_eglContext.InitializeDisplay(EGL_OPENGL_API))
  {
    m_eglContext.Destroy();
    return false;
  }

  if (!m_eglContext.ChooseConfig(EGL_OPENGL_BIT))
  {
    m_eglContext.Destroy();
    return false;
  }

  if (!CreateContext())
  {
    m_eglContext.Destroy();
    return false;
  }

  return true;
}

bool CWinSystemPS5GLContext::DestroyWindowSystem()
{
  CDVDFactoryCodec::ClearHWAccels();
  VIDEOPLAYER::CRendererFactory::ClearRenderer();
  m_eglContext.Destroy();
  return CWinSystemPS5::DestroyWindowSystem();
}

bool CWinSystemPS5GLContext::CreateContext()
{
  // Kodi's GL renderer needs 3.2 Core; Mesa hands back the highest compatible
  // version (4.6 on ps5-opengl 0.3.0).
  const EGLint glMajor = 3;
  const EGLint glMinor = 2;

  CEGLAttributesVec contextAttribs;
  contextAttribs.Add({{EGL_CONTEXT_MAJOR_VERSION_KHR, glMajor},
                      {EGL_CONTEXT_MINOR_VERSION_KHR, glMinor},
                      {EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR}});

  if (!m_eglContext.CreateContext(contextAttribs))
  {
    CLog::Log(LOGERROR, "CWinSystemPS5GLContext: EGL OpenGL {}.{} core context creation failed",
              glMajor, glMinor);
    return false;
  }
  return true;
}

void CWinSystemPS5GLContext::QueryOutputGeometry()
{
  EGLint width = 0;
  EGLint height = 0;
  if (eglQuerySurface(m_eglContext.GetEGLDisplay(), m_eglContext.GetEGLSurface(), EGL_WIDTH, &width) &&
      eglQuerySurface(m_eglContext.GetEGLDisplay(), m_eglContext.GetEGLSurface(), EGL_HEIGHT, &height) &&
      width > 0 && height > 0)
  {
    m_outputWidth = width;
    m_outputHeight = height;
  }
  // Presentation rate of the GL SDK's build profile; the real output rate
  // (e.g. 59.94 Hz) is read from the system after the first frame.
#if defined(PS5_OPENGL_NATIVE_FPS)
  m_outputRefresh = static_cast<float>(PS5_OPENGL_NATIVE_FPS);
#else
  m_outputRefresh = 60.0f;
#endif
}

bool CWinSystemPS5GLContext::CreateNewWindow(const std::string& name,
                                             bool fullScreen,
                                             RESOLUTION_INFO& res)
{
  OnLostDevice();

  if (!DestroyWindow())
    return false;

  // A null native window selects the console's fullscreen output.
  if (!m_eglContext.CreateSurface(static_cast<EGLNativeWindowType>(0)))
  {
    CLog::Log(LOGERROR, "CWinSystemPS5GLContext: failed to create the EGL window surface");
    return false;
  }

  if (!m_eglContext.BindContext())
  {
    m_eglContext.DestroySurface();
    return false;
  }

  const int registeredWidth = CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP).iWidth;
  const int registeredHeight = CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP).iHeight;
  QueryOutputGeometry();
  if (m_outputWidth != registeredWidth || m_outputHeight != registeredHeight)
  {
    // Kodi registered another size than the surface has: correct the
    // desktop mode, otherwise the GUI covers only part of the screen.
    CLog::Log(LOGWARNING, "CWinSystemPS5: surface is {}x{}, Kodi had registered {}x{}; correcting",
              m_outputWidth, m_outputHeight, registeredWidth, registeredHeight);
    UpdateResolutions();
  }

  m_nWidth = m_outputWidth;
  m_nHeight = m_outputHeight;
  m_fRefreshRate = m_outputRefresh;
  m_bFullScreen = true;

  // Report what the output really is; the caller's RESOLUTION_INFO may carry
  // stale numbers from guisettings.xml.
  res.iWidth = m_nWidth;
  res.iHeight = m_nHeight;
  res.iScreenWidth = m_nWidth;
  res.iScreenHeight = m_nHeight;
  res.fRefreshRate = m_fRefreshRate;

  m_bWindowCreated = true;
  OnResetDevice();
  return true;
}

bool CWinSystemPS5GLContext::DestroyWindow()
{
  // the GL driver restores the output mode only in its 120 Hz builds
  RestoreOutputMode();
  m_eglContext.DestroySurface();
  m_bWindowCreated = false;
  return true;
}

bool CWinSystemPS5GLContext::SetFullScreen(bool fullScreen,
                                           RESOLUTION_INFO& res,
                                           bool blankOtherDisplays)
{
  // One size (the GL SDK profile). The output runs at the system rate, or in
  // VRR at a "(PS5 VRR)" mode's rate during playback. Recreating the surface is
  // only needed when the window does not exist yet.
  if (!m_bWindowCreated && !CreateNewWindow("Kodi", true, res))
    return false;

  SwitchOutputRate(res);

  res.iWidth = m_nWidth;
  res.iHeight = m_nHeight;
  res.iScreenWidth = m_nWidth;
  res.iScreenHeight = m_nHeight;
  res.fRefreshRate = m_fRefreshRate;

  CRenderSystemGL::ResetRenderSystem(m_nWidth, m_nHeight);
  return true;
}

void CWinSystemPS5GLContext::SetVSyncImpl(bool enable)
{
  m_eglContext.SetVSync(enable);
}

float CWinSystemPS5GLContext::GetDisplayLatency()
{
  const float pace = VrrTargetRate();
  if (pace <= 0.0f)
    return -1.0f; // Kodi's estimate for fixed-rate output
  return 1000.0f / pace; // the flip after the paced present
}

float CWinSystemPS5GLContext::GetFrameLatencyAdjustment()
{
  // this frame's wait for its pacing tick, so a frame due later is chosen
  // (negative: added to the latency)
  const float pace = VrrTargetRate();
  if (pace <= 0.0f || m_nextVrrPresent.time_since_epoch().count() == 0)
    return 0.0f;
  const double period = 1000.0 / static_cast<double>(pace);
  const double wait = std::chrono::duration<double, std::milli>(m_nextVrrPresent -
                                                                 std::chrono::steady_clock::now())
                          .count();
  if (wait <= 0.0 || wait > period)
    return 0.0f; // the pacer re-anchors: no extra wait
  return static_cast<float>(-wait);
}

bool CWinSystemPS5GLContext::BeginRender()
{
  if (m_stats)
  {
    // kodi-debug: the GUI thread was away from rendering for a while - the
    // log lines just before this one show what it was doing
    const auto now = std::chrono::steady_clock::now();
    if (m_frameStart.time_since_epoch().count() != 0)
    {
      const double sinceLast = std::chrono::duration<double, std::milli>(now - m_frameStart).count();
      if (sinceLast > 500.0)
        CLog::Log(LOGWARNING, "PS5 presentation (kodi-debug): GUI thread stalled for {:.0f} ms",
                  sinceLast);
    }
    m_frameStart = now;
  }
  const bool ok = CRenderSystemGL::BeginRender();
  m_hdr.BindTarget(m_nWidth, m_nHeight); // HDR: the frame renders into the 10-bit target
  return ok;
}

bool CWinSystemPS5GLContext::SetHDR(const VideoPicture* videoPicture)
{
  return m_hdr.SetHDR(videoPicture);
}

bool CWinSystemPS5GLContext::IsHDRDisplay()
{
  // HDR only when the link really runs in HDR; otherwise Kodi tone maps
  return KODI::PLATFORM::PS5::ScanoutFormatSwitchAvailable() &&
         KODI::PLATFORM::PS5::IsDisplayHdr();
}

CHDRCapabilities CWinSystemPS5GLContext::GetDisplayHDRCapabilities() const
{
  // what Kodi can output here: HDR10 natively, HLG converted to PQ
  CHDRCapabilities caps;
  if (KODI::PLATFORM::PS5::ScanoutFormatSwitchAvailable())
  {
    caps.SetHDR10();
    caps.SetHLG();
  }
  return caps;
}

bool CWinSystemPS5GLContext::SetGuiCompositing(int colorTransfer)
{
  return m_hdr.SetGuiCompositing(colorTransfer, UseLimitedColor());
}

bool CWinSystemPS5GLContext::BeginGuiComposite(bool guiWillRender)
{
#if PS5_KODI_MAJOR >= 22
  return m_hdr.BeginGuiComposite(guiWillRender, m_nWidth, m_nHeight,
                                 GetEnabledFrontToBackRendering());
#else // Kodi 21 has no front-to-back GUI rendering: no depth buffer
  return m_hdr.BeginGuiComposite(guiWillRender, m_nWidth, m_nHeight, false);
#endif
}

void CWinSystemPS5GLContext::EndGuiComposite()
{
  m_hdr.EndGuiComposite();
}

void CWinSystemPS5GLContext::CompositeGui()
{
  m_hdr.CompositeGui(GetGUIElementCount());
}

void CWinSystemPS5GLContext::PresentRender(bool rendered, bool videoLayer)
{
  if (!m_bRenderCreated)
    return;

  double renderMs = 0.0; // kodi-debug: the frame's cost, before the pacer's wait
  if (m_stats && m_frameStart.time_since_epoch().count() != 0)
    renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                        m_frameStart)
                   .count();

  if (rendered || videoLayer)
  {
    m_hdr.Pack(m_nWidth, m_nHeight); // HDR: the packed 10-bit words into the real framebuffer

    // the system may switch the output onto (or off) its VRR link at any time
    if (m_videoOutLogged)
    {
      const auto now = std::chrono::steady_clock::now();
      if (now - m_lastLinkCheck > std::chrono::seconds(2))
      {
        m_lastLinkCheck = now;
        RefreshLinkState();
      }
    }

    // Under VRR the display refreshes when we present: pace presentation at
    // the target rate, so e.g. 25 fps video is shown at an even 50 Hz.
    if (const float vrrHz = VrrTargetRate(); vrrHz > 0.0f)
    {
      const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(1.0 / static_cast<double>(vrrHz)));
      const auto now = std::chrono::steady_clock::now();
      if (m_nextVrrPresent < now - period || m_nextVrrPresent > now + 2 * period)
        m_nextVrrPresent = now; // (re)start the cadence
      std::this_thread::sleep_until(m_nextVrrPresent);
      m_nextVrrPresent += period;
    }

    // eglSwapBuffers is our only vertical-sync source.
    const auto beforeSwap = m_stats ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    if (!m_eglContext.TrySwapBuffers())
    {
      CEGLUtils::Log(LOGERROR, "eglSwapBuffers failed");
      throw std::runtime_error("eglSwapBuffers failed");
    }
    if (m_stats && beforeSwap.time_since_epoch().count() != 0)
    {
      const double swapMs =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - beforeSwap)
              .count();
      if (swapMs > 500.0)
        CLog::Log(LOGWARNING, "PS5 presentation (kodi-debug): eglSwapBuffers blocked for {:.0f} ms",
                  swapMs);
    }
    if (m_stats) // kodi-debug: what reaches the display, and what a frame costs
    {
      const auto now = std::chrono::steady_clock::now();
      ++m_statFrames;
      if (rendered)
        ++m_statGuiFrames;
      m_statRenderMs += renderMs;
      m_statRenderMaxMs = std::max(m_statRenderMaxMs, renderMs);
      if (m_statWindow.time_since_epoch().count() == 0)
        m_statWindow = now;
      const double seconds = std::chrono::duration<double>(now - m_statWindow).count();
      if (seconds >= 5.0)
      {
        CLog::Log(LOGINFO,
                  "PS5 presentation (kodi-debug): {:.1f} frames/s ({:.1f}/s with GUI content), "
                  "paced at {:.3f} Hz, {}, render {:.1f} ms average / {:.1f} ms longest, "
                  "video decoded {} presented {}",
                  m_statFrames / seconds, m_statGuiFrames / seconds, VrrTargetRate(),
                  VrrTargetRate() > 0.0f ? "VRR link" : "fixed-rate output",
                  m_statFrames ? m_statRenderMs / m_statFrames : 0.0, m_statRenderMaxMs,
                  KODI::PLATFORM::PS5::ps5_video_frame_stats().decoded.load(
                      std::memory_order_relaxed),
                  KODI::PLATFORM::PS5::ps5_video_frame_stats().presented.load(
                      std::memory_order_relaxed));
        m_statFrames = m_statGuiFrames = 0;
        m_statRenderMs = m_statRenderMaxMs = 0.0;
        m_statWindow = now;
      }
    }
    // The driver opens the video output with the first presented frame:
    // then report it and adopt the system's real refresh rate (e.g. 59.94).
    if (!m_videoOutLogged)
    {
      m_videoOutLogged = KODI::PLATFORM::PS5::LogVideoOutInfo();
      if (m_videoOutLogged)
      {
        EnsureSystemMode();
        ApplySystemRefreshRate(KODI::PLATFORM::PS5::QueryRefreshRate());
        DetectOutputModes();
        // /data and USB drives: the sandbox may be opened only now - graphics
        // and VideoOut cannot be brought up after that (EVO Player's findings)
        KODI::PLATFORM::PS5::RequestSandboxOpen();
        unsigned sysWidth = 0, sysHeight = 0;
        if (KODI::PLATFORM::PS5::QuerySystemResolution(sysWidth, sysHeight))
        {
          if (sysWidth == static_cast<unsigned>(m_outputWidth) &&
              sysHeight == static_cast<unsigned>(m_outputHeight))
            CLog::Log(LOGINFO, "CWinSystemPS5: rendering at the system resolution {}x{}",
                      sysWidth, sysHeight);
          else
            CLog::Log(LOGWARNING,
                      "CWinSystemPS5: rendering {}x{}, but the system outputs {}x{} (the PS5 "
                      "scales). Rebuild the GL SDK with PS5_SCANOUT_HEIGHT={} "
                      "(scripts/18-build-ps5-opengl.sh) to render natively.",
                      m_outputWidth, m_outputHeight, sysWidth, sysHeight, sysHeight);
        }
      }
    }
  }
  else
  {
    // Nothing changed this frame: yield instead of spinning a core.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
