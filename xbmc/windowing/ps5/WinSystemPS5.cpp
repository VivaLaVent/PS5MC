/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemPS5.h"

#include "VideoSyncPS5.h"
#include "platform/ps5/VideoOutInfo.h"

#include "ServiceBroker.h"
#include "guilib/DispResource.h"
#include "settings/DisplaySettings.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"

#include "platform/ps5/input/PS5PadInput.h"
#include "platform/ps5/video/VideoCodecRegistration.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unistd.h>
#include <mutex>

using namespace KODI::WINDOWING::PS5;

CWinSystemPS5::CWinSystemPS5() = default;

CWinSystemPS5::~CWinSystemPS5() = default;

bool CWinSystemPS5::InitWindowSystem()
{
  if (!CWinSystemBase::InitWindowSystem())
    return false;

  m_padInput = std::make_unique<KODI::PLATFORM::PS5::CPadInput>();
  m_padInput->Start();

  // hardware H.264/HEVC decoding (falls back to FFmpeg per stream)
  KODI::PLATFORM::PS5::RegisterVideoCodecs();
  return true;
}

bool CWinSystemPS5::DestroyWindowSystem()
{
  if (m_padInput)
  {
    m_padInput->Stop();
    m_padInput.reset();
  }
  return CWinSystemBase::DestroyWindowSystem();
}

bool CWinSystemPS5::ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop)
{
  // Fixed output; nothing to resize.
  return true;
}

void CWinSystemPS5::UpdateResolutions()
{
  CWinSystemBase::UpdateResolutions();

  // The desktop mode is the system's own rate: the GUI and every stopped
  // state use it. VRR modes (registered after the first frame, when the
  // system has shown it can do VRR) serve "Adjust display refresh rate".
  const float desktopHz = m_systemRefresh > 0.0f ? m_systemRefresh : m_outputRefresh;
  RESOLUTION_INFO& desktop = CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP);
  UpdateDesktopResolution(desktop, "PS5", m_outputWidth, m_outputHeight, desktopHz, 0);
  CDisplaySettings::GetInstance().ClearCustomResolutions();

  if (m_vrrAvailable)
  {
    // the lowest multiple of common frame rates within the PS5's VRR range
    // (48-120 Hz): 23.976 -> 71.928, 24 -> 48, 25 -> 50, 29.97/30 -> 59.94/60
    for (const float hz : kVrrRates)
    {
      RESOLUTION_INFO vrr;
      UpdateDesktopResolution(vrr, "PS5", m_outputWidth, m_outputHeight, hz, 0);
      vrr.strMode = StringUtils::Format("{}x{} @ {:.3f}Hz {}", m_outputWidth, m_outputHeight, hz,
                                        kVrrModeTag);
      GetGfxContext().ResetOverscan(vrr);
      CDisplaySettings::GetInstance().AddResolutionInfo(vrr);
    }
  }
  CDisplaySettings::GetInstance().ApplyCalibrations();

  CLog::Log(LOGINFO, "CWinSystemPS5: output {}x{} @ {:.2f} Hz{}", m_outputWidth, m_outputHeight,
            desktopHz, m_vrrAvailable ? ", VRR modes for playback" : "");
}

void CWinSystemPS5::Register(IDispResource* resource)
{
  std::unique_lock lock(m_resourceSection);
  m_resources.push_back(resource);
}

void CWinSystemPS5::Unregister(IDispResource* resource)
{
  std::unique_lock lock(m_resourceSection);
  auto it = std::find(m_resources.begin(), m_resources.end(), resource);
  if (it != m_resources.end())
    m_resources.erase(it);
}

void CWinSystemPS5::OnLostDevice()
{
  CLog::Log(LOGDEBUG, "CWinSystemPS5::{} - notify display lost", __FUNCTION__);
  std::unique_lock lock(m_resourceSection);
  for (IDispResource* resource : m_resources)
    resource->OnLostDisplay();
}

void CWinSystemPS5::OnResetDevice()
{
  CLog::Log(LOGDEBUG, "CWinSystemPS5::{} - notify display reset", __FUNCTION__);
  std::unique_lock lock(m_resourceSection);
  for (IDispResource* resource : m_resources)
    resource->OnResetDisplay();
}

std::unique_ptr<CVideoSync> CWinSystemPS5::GetVideoSync(CVideoReferenceClock* clock)
{
  return std::make_unique<CVideoSyncPS5>(clock);
}

void CWinSystemPS5::ApplySystemRefreshRate(float hz)
{
  if (m_linkIsVrr)
    hz = 59.94f; // the link's ~120 Hz is not the rate Kodi should run at
  if (hz > 0.0f && !m_vrrActive)
    m_systemRefresh = hz;
  if (hz <= 0.0f || std::abs(hz - m_outputRefresh) < 0.005f)
    return;
  CLog::Log(LOGINFO, "CWinSystemPS5: system output runs at {:.3f} Hz (was assuming {:.3f})", hz,
            m_outputRefresh);
  m_outputRefresh = hz;
  m_fRefreshRate = hz;
  RESOLUTION_INFO& desktop = CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP);
  desktop.fRefreshRate = hz;
  desktop.strMode = StringUtils::Format("{}x{} @ {:.2f}Hz (PS5)", desktop.iScreenWidth,
                                        desktop.iScreenHeight, hz);
}

void CWinSystemPS5::EnsureSystemMode()
{
  using namespace KODI::PLATFORM::PS5;
  const float before = QueryRefreshRate();
  const int rc = SetOutputMode(kOutputModeDefault);
  const float after = QueryRefreshRate();
  CLog::Log(rc == 0 ? LOGINFO : LOGWARNING,
            "CWinSystemPS5: system mode requested ({:#x}): output {:.3f} Hz before, {:.3f} Hz after",
            static_cast<uint32_t>(rc), before, after);
  // With the PS5's VRR setting on, the system keeps the title on a VRR link
  // (~120 Hz) whatever it requests; the display then follows Kodi's
  // presentation, so Kodi paces the menus at the system rate, 59.94 Hz.
  m_linkIsVrr = after > 100.0f;
  if (m_linkIsVrr)
    CLog::Log(LOGINFO, "CWinSystemPS5: the system runs Kodi on a VRR link ({:.3f} Hz): menus paced "
              "at 59.94 Hz", after);
}

void CWinSystemPS5::DetectOutputModes()
{
  // VRR for playback exists only on the system's own VRR link (the PS5's VRR
  // setting on, with a VRR-capable TV): that link follows Kodi's presentation.
  // Without it no VRR modes are offered - requesting the high-refresh preset
  // and releasing its peg is refused (unpeg 0x8029001c),
  // it only blanked the TV and left Kodi believing in a rate never output.
  m_vrrAvailable = m_linkIsVrr;
  CLog::Log(LOGINFO, "CWinSystemPS5: VRR for playback {}",
            m_vrrAvailable ? "available (system VRR link)" : "not available (no VRR link)");
  if (m_vrrAvailable)
    UpdateResolutions();
}

float CWinSystemPS5::SwitchOutputRate(const RESOLUTION_INFO& res)
{
  // Kodi requests a "(PS5 VRR)" mode only through "Adjust display refresh
  // rate" (patch 0012: for a video, with any setting but Off; when it switches
  // back to the desktop mode is Kodi's), so the requested mode alone decides. "Sync playback to
  // display" plays no part in it.
  if (!m_vrrActive)
    RefreshLinkState();
  const bool vrrMode = res.strMode.find(kVrrModeTag) != std::string::npos;
  const bool wantVrr = vrrMode && m_vrrAvailable && res.fRefreshRate > 0.0f;

  if (!wantVrr)
  {
    if (m_vrrActive)
    {
      m_vrrActive = false;
      m_vrrTargetHz = 0.0f;
      m_vblankClockUnreliable = false;
      m_fRefreshRate = m_outputRefresh = m_systemRefresh > 0.0f ? m_systemRefresh : 59.94f;
      CLog::Log(LOGINFO, "CWinSystemPS5: display mode {}: VRR off; {}", res.strMode,
                PacingDescription());
    }
    else if (vrrMode)
      CLog::Log(LOGWARNING, "CWinSystemPS5: display mode {} requested without a VRR link: "
                "system rate; {}", res.strMode, PacingDescription());
    return m_fRefreshRate;
  }

  // On the VRR link the display follows our presentation: engaging VRR is
  // pacing at the mode's rate (see PresentRender). No output mode changes.
  if (!m_vrrActive)
  {
    m_vrrActive = true;
    m_vblankClockUnreliable = false;
  }
  m_vrrTargetHz = res.fRefreshRate;
  m_fRefreshRate = m_outputRefresh = res.fRefreshRate;
  CLog::Log(LOGINFO, "CWinSystemPS5: display mode {}: VRR on; {}", res.strMode,
            PacingDescription());
  return m_fRefreshRate;
}

void CWinSystemPS5::RefreshLinkState()
{
  // A VRR link reports ~120 Hz. The system switches a title onto it (or off
  // it) at a moment of its own choosing, so this is re-read before every
  // mode decision and every 2 seconds while presenting; the VRR modes are
  // offered exactly while the link exists.
  const float hz = KODI::PLATFORM::PS5::QueryRefreshRate();
  if (hz <= 0.0f)
    return;
  const bool link = hz > 100.0f;
  if (link == m_linkIsVrr)
    return;
  m_linkIsVrr = link;
  m_vrrAvailable = link;
  if (link)
  {
    if (m_systemRefresh <= 0.0f || m_systemRefresh > 100.0f)
      m_systemRefresh = 59.94f;
  }
  else
  {
    m_vrrActive = false;
    m_vrrTargetHz = 0.0f;
    m_systemRefresh = hz;
  }
  if (!m_vrrActive)
    m_fRefreshRate = m_outputRefresh = m_systemRefresh;
  CLog::Log(LOGINFO, "CWinSystemPS5: the output is {} ({:.3f} Hz); VRR for playback {}; {}",
            link ? "now a VRR link" : "no longer a VRR link", hz,
            link ? "available" : "not available", PacingDescription());
  UpdateResolutions(); // add or remove the VRR modes
}

std::string CWinSystemPS5::PacingDescription() const
{
  const float pace = VrrTargetRate();
  return pace > 0.0f ? StringUtils::Format("presenting at {:.3f} Hz on the VRR link", pace)
                     : StringUtils::Format("fixed-rate output at {:.3f} Hz", m_fRefreshRate);
}

void CWinSystemPS5::RestoreOutputMode()
{
  if (!m_vrrActive)
    return;
  const int rc = KODI::PLATFORM::PS5::SetOutputMode(KODI::PLATFORM::PS5::kOutputModeDefault);
  CLog::Log(rc == 0 ? LOGINFO : LOGWARNING, "CWinSystemPS5: restored the system output mode ({:#x})",
            static_cast<uint32_t>(rc));
  m_vrrActive = false;
  m_vrrTargetHz = 0.0f;
}
