/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "threads/CriticalSection.h"
#include "windowing/WinSystem.h"

#include <memory>
#include <string>
#include <vector>

#if __has_include(<ps5_opengl_display.h>)
#include <ps5_opengl_display.h> // PS5_OPENGL_NATIVE_{WIDTH,HEIGHT,FPS}: GL SDK build profile
#endif

class IDispResource;

namespace KODI::PLATFORM::PS5
{
class CPadInput;
}

namespace KODI::WINDOWING::PS5
{

/*!
 * \brief Window system for PlayStation 5 homebrew.
 *
 * The console has one output whose geometry is fixed by the ps5-opengl SDK
 * build profile (1080p60, 1440p120 or 4K120): no windowed mode, no cursor,
 * no screensaver, no runtime mode switching. Input is pushed straight into
 * the application port by the pad thread, so MessagePump() has nothing to do.
 */
class CWinSystemPS5 : public CWinSystemBase
{
public:
  CWinSystemPS5();
  ~CWinSystemPS5() override;

  const std::string GetName() override { return "ps5"; }

  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;

  bool ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop) override;
  void UpdateResolutions() override;

  // Vertical sync for Kodi's reference clock (VideoSyncPS5).
  std::unique_ptr<CVideoSync> GetVideoSync(CVideoReferenceClock* clock) override;

  // The system reported the real output refresh rate (after the first
  // presented frame): adopt it for the desktop resolution.
  void ApplySystemRefreshRate(float hz);

  // After the first frame: request the system's own mode, so Kodi starts at
  // the plain system rate (the PS5 may have started the title on a VRR link),
  // and log what the system then reports.
  void EnsureSystemMode();

  // After the first frame: VRR modes are offered to Kodi's "Adjust display
  // refresh rate" when the system can do VRR for this title.
  void DetectOutputModes();

  // Kodi selected a display mode: the desktop mode (the system rate) or one of
  // the "(PS5 VRR)" modes, which Kodi requests for a video through "Adjust
  // display refresh rate" (any setting but Off). Returns the rate in effect.
  float SwitchOutputRate(const RESOLUTION_INFO& res);

  // Back to the system's own mode (on exit).
  void RestoreOutputMode();

  // Re-read whether the output is a VRR link (after any mode change).
  void RefreshLinkState();

  std::string PacingDescription() const;

  // The output's refresh rate as set up (the VRR target during VRR).
  float OutputRefreshRate() const { return m_fRefreshRate; }

  // On a VRR link the display refreshes when Kodi presents, so the window
  // system paces presentation at this rate: the VRR mode's rate during
  // playback, the system rate (59.94 Hz) otherwise. 0: fixed-rate output.
  float VrrTargetRate() const
  {
    return m_vrrActive ? m_vrrTargetHz : (m_linkIsVrr ? m_systemRefresh : 0.0f);
  }

  // The vblank clock found vblanks arriving at a rate other than the output's:
  // don't use it again until the next output mode change.
  void SetVblankClockUnreliable() { m_vblankClockUnreliable = true; }
  bool IsVblankClockUnreliable() const { return m_vblankClockUnreliable; }

  bool CanDoWindowed() override { return false; }
  bool SupportsScreenMove() override { return false; }
  bool HasCursor() override { return false; }
  bool MessagePump() override { return false; }

  void Register(IDispResource* resource) override;
  void Unregister(IDispResource* resource) override;

protected:
  void OnLostDevice();
  void OnResetDevice();

  // Output geometry. Kodi registers its resolutions before the window (and
  // the EGL surface) exists, so start from the GL SDK's build profile - the
  // size its surface will have - and refine from the surface afterwards.
#if defined(PS5_OPENGL_NATIVE_WIDTH) && defined(PS5_OPENGL_NATIVE_HEIGHT)
  int m_outputWidth{PS5_OPENGL_NATIVE_WIDTH};
  int m_outputHeight{PS5_OPENGL_NATIVE_HEIGHT};
#else
  int m_outputWidth{1920};
  int m_outputHeight{1080};
#endif
#if defined(PS5_OPENGL_NATIVE_FPS)
  float m_outputRefresh{static_cast<float>(PS5_OPENGL_NATIVE_FPS)};
#else
  float m_outputRefresh{60.0f};
#endif

  float m_systemRefresh{0.0f}; // the system's own rate (0: not known yet)
  bool m_vrrAvailable{false};  // VRR modes offered: exactly while the output is a VRR link
  bool m_vrrActive{false};
  bool m_linkIsVrr{false};     // the system runs the title on a VRR link (PS5 VRR setting)
  float m_vrrTargetHz{0.0f};
  bool m_vblankClockUnreliable{false};

  // VRR modes offered to Kodi: the lowest multiple of common frame rates
  // within the PS5's VRR range (48-120 Hz)
  static constexpr float kVrrRates[] = {48.0f, 50.0f, 59.94f, 60.0f, 71.928f};
  static constexpr const char* kVrrModeTag = "(PS5 VRR)";

  CCriticalSection m_resourceSection;
  std::vector<IDispResource*> m_resources;

  std::unique_ptr<KODI::PLATFORM::PS5::CPadInput> m_padInput;
};

} // namespace KODI::WINDOWING::PS5
