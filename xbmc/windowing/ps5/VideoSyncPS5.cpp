/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoSyncPS5.h"

#include "WinSystemPS5.h"

#include "ServiceBroker.h"
#include "cores/VideoPlayer/VideoReferenceClock.h"
#include "platform/ps5/VideoOutInfo.h"
#include "threads/Event.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <chrono>
#include <cmath>
#include <thread>

extern "C" uint64_t sceKernelGetProcessTime(void);

using namespace KODI::PLATFORM::PS5;

CVideoSyncPS5::CVideoSyncPS5(CVideoReferenceClock* clock)
  : CVideoSync(clock), m_winSystem(CServiceBroker::GetWinSystem())
{
}

bool CVideoSyncPS5::Setup()
{
  m_abort = false;
  if (auto* ps5 = dynamic_cast<KODI::WINDOWING::PS5::CWinSystemPS5*>(m_winSystem); ps5)
  {
    // Under VRR the display follows our presentation: there is no fixed
    // refresh to lock to, so Kodi uses its system clock.
    if (ps5->VrrTargetRate() > 0.0f)
    {
      CLog::Log(LOGINFO, "CVideoSyncPS5: VRR active, using the system clock");
      return false;
    }
    if (ps5->IsVblankClockUnreliable())
    {
      CLog::Log(LOGINFO, "CVideoSyncPS5: vblanks unreliable in this output mode, using the "
                         "system clock");
      return false;
    }
  }
  uint64_t count = 0, when = 0;
  if (!QueryVblank(count, when))
  {
    CLog::Log(LOGWARNING, "CVideoSyncPS5: vblank status unavailable, no display sync");
    return false;
  }
  m_lastCount = count;
  if (m_winSystem)
    m_winSystem->Register(this);
  CLog::Log(LOGINFO, "CVideoSyncPS5: vblank counter at {}, display {:.3f} Hz", count, GetFps());
  return true;
}

void CVideoSyncPS5::Run(CEvent& stopEvent)
{
  unsigned polls = 0;
  // self-check: do vblanks arrive at the rate the output claims?
  uint64_t checkCount = m_lastCount;
  auto checkStart = std::chrono::steady_clock::now();
  int badWindows = 0;
  while (!stopEvent.Signaled() && !m_abort)
  {
    // safety net: a changed output rate ends this run, so the reference
    // clock restarts with the new rate (Kodi reads it only at Setup)
    if (++polls % 100 == 0 && std::abs(OutputRate() - m_fps) > 0.01f)
    {
      CLog::Log(LOGINFO, "CVideoSyncPS5: display rate changed ({:.3f} -> {:.3f} Hz), restarting",
                m_fps, OutputRate());
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    uint64_t count = 0, when = 0;
    if (!QueryVblank(count, when))
    {
      CLog::Log(LOGWARNING, "CVideoSyncPS5: vblank status failed, stopping display sync");
      break;
    }
    if (count == m_lastCount)
      continue;

    // Timestamp of the vblank itself (process time, microseconds), so the
    // 1 ms polling interval does not add jitter; fall back to "now" if it
    // does not look like a recent process-time value.
    int64_t hostTime = CurrentHostCounter();
    const uint64_t now = sceKernelGetProcessTime();
    if (when != 0 && when <= now && now - when < 100000)
      hostTime -= static_cast<int64_t>((now - when) * CurrentHostFrequency() / 1000000);

    m_refClock->UpdateClock(static_cast<int>(count - m_lastCount), static_cast<uint64_t>(hostTime));
    m_lastCount = count;

    const auto nowSteady = std::chrono::steady_clock::now();
    const double window = std::chrono::duration<double>(nowSteady - checkStart).count();
    if (window >= 2.0)
    {
      const double measured = static_cast<double>(count - checkCount) / window;
      const double expected = static_cast<double>(m_fps);
      badWindows = std::abs(measured / expected - 1.0) > 0.02 ? badWindows + 1 : 0;
      checkCount = count;
      checkStart = nowSteady;
      if (badWindows >= 2)
      {
        CLog::Log(LOGWARNING,
                  "CVideoSyncPS5: vblanks arrive at {:.2f}/s, not {:.3f} Hz: using the system "
                  "clock for this output mode",
                  measured, m_fps);
        if (auto* ps5 = dynamic_cast<KODI::WINDOWING::PS5::CWinSystemPS5*>(m_winSystem))
          ps5->SetVblankClockUnreliable();
        break;
      }
    }
  }
}

void CVideoSyncPS5::Cleanup()
{
  if (m_winSystem)
    m_winSystem->Unregister(this);
}

void CVideoSyncPS5::OnResetDisplay()
{
  m_abort = true;
}

float CVideoSyncPS5::OutputRate() const
{
  if (auto* ps5 = dynamic_cast<KODI::WINDOWING::PS5::CWinSystemPS5*>(m_winSystem))
    return ps5->OutputRefreshRate();
  return m_winSystem ? m_winSystem->GetGfxContext().GetFPS() : 60.0f;
}

float CVideoSyncPS5::GetFps()
{
  // the real output rate, as the window system set it up
  m_fps = OutputRate();
  return m_fps;
}

void CVideoSyncPS5::RefreshChanged()
{
  // the output mode changed (system rate <-> VRR): restart the clock
  if (std::abs(OutputRate() - m_fps) > 0.01f)
    m_abort = true;
}
