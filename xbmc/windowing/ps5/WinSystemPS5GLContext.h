/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <chrono>
#include <cstdlib>

#include "HdrOutputPS5.h"
#include "utils/HDRCapabilities.h"
#include "WinSystemPS5.h"
#include "guilib/DirtyRegion.h" // CDirtyRegionList (Kodi 21 does not reach it transitively)
#include "rendering/gl/RenderSystemGL.h"
#include "utils/EGLUtils.h"
#include "windowing/linux/WinSystemEGL.h"

#include <memory>
#include <string>

namespace KODI::WINDOWING::PS5
{

/*!
 * \brief OpenGL window system backed by the ps5-opengl SDK.
 *
 * The SDK exposes a Mesa/Gallium OpenGL 4.6 Core implementation behind a
 * plain EGL 1.4-style interface: eglGetDisplay(EGL_DEFAULT_DISPLAY) and a
 * window surface created with a null native window select the console's
 * fullscreen output. Its size is fixed by the SDK build profile, so we query
 * the surface for the real geometry instead of trusting Kodi's settings.
 */
/*
 * CWinSystemEGL (windowing/linux) is a small mixin that owns the
 * CEGLContextUtils and exposes the EGL handles; RetroPlayer's EGL hardware
 * rendering context casts the window system to it.
 */
class CWinSystemPS5GLContext : public CWinSystemPS5,
                               public CRenderSystemGL,
                               public LINUX::CWinSystemEGL
{
public:
  CWinSystemPS5GLContext();
  ~CWinSystemPS5GLContext() override = default;

  static void Register();
  static std::unique_ptr<CWinSystemBase> CreateWinSystem();

  // CWinSystemBase
  CRenderSystemBase* GetRenderSystem() override { return this; }
  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;
  bool CreateNewWindow(const std::string& name, bool fullScreen, RESOLUTION_INFO& res) override;
  bool DestroyWindow() override;
  bool SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays) override;
  // The PS5 GL driver reports buffer ages that do not match its swap chain
  // (stale GUI content showed through). Age 0 makes Kodi redraw the whole
  // screen every frame, which is cheap on this GPU.
#if PS5_KODI_MAJOR >= 22 // dirty-region hooks are Kodi 22 only
  void SetDirtyRegions(const CDirtyRegionList& dirtyRegions) override {}
  int GetBufferAge() override { return 0; }
#endif

  // CRenderSystemGL
  void PresentRender(bool rendered, bool videoLayer) override;
  bool BeginRender() override;

  // On the paced VRR link a presented frame reaches the screen at the next
  // pacing tick: Kodi schedules video against that (the render manager adds
  // GetDisplayLatency() and subtracts GetFrameLatencyAdjustment(), in ms).
  // Fixed-rate output keeps Kodi's own estimate.
  float GetDisplayLatency() override;
  float GetFrameLatencyAdjustment() override;

  // HDR output for PQ video (HdrOutputPS5): Kodi's HDR and GUI-compositing hooks
  // no plane-role model here (GBM's flip-flop): the surface is always right
#if PS5_KODI_MAJOR >= 22 // Kodi 22 only
  bool SetVideoOutput(const VideoPicture* videoPicture) override { return true; }
#endif
  bool SetHDR(const VideoPicture* videoPicture) override;
  bool IsHDRDisplay() override;
#if PS5_KODI_MAJOR >= 22 // HDR GUI compositing is Kodi 22 only
  bool SetGuiCompositing(int colorTransfer) override;
  bool IsHdrComposite() const override { return m_hdr.IsGuiCompositing(); }
#endif
  CHDRCapabilities GetDisplayHDRCapabilities() const override;
#if PS5_KODI_MAJOR >= 22 // HDR GUI compositing is Kodi 22 only
  bool BeginGuiComposite(bool guiWillRender) override;
  void EndGuiComposite() override;
  void CompositeGui() override;
#endif

protected:
  void SetVSyncImpl(bool enable) override;
  void PresentRenderImpl(bool rendered) override {}

private:
  bool m_videoOutLogged = false;
  std::chrono::steady_clock::time_point m_nextVrrPresent{}; // VRR presentation cadence
  std::chrono::steady_clock::time_point m_lastLinkCheck{};  // VRR link re-check (2 s)
  KODI::PLATFORM::PS5::CHdrOutputPS5 m_hdr;

  // kodi-debug: presented frames, pacing and render time, every 5 seconds
  const bool m_stats = std::getenv("KODI_PS5_DEBUG") != nullptr;
  std::chrono::steady_clock::time_point m_frameStart{};
  std::chrono::steady_clock::time_point m_statWindow{};
  unsigned m_statFrames = 0;
  unsigned m_statGuiFrames = 0;
  double m_statRenderMs = 0.0;
  double m_statRenderMaxMs = 0.0;

  bool CreateContext();
  void QueryOutputGeometry();

};

} // namespace KODI::WINDOWING::PS5
