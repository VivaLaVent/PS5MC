/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "CPUInfoPS5.h"

#include <sys/types.h>

#include <sys/sysctl.h>

std::shared_ptr<CCPUInfo> CCPUInfo::GetCPUInfo()
{
  return std::make_shared<CCPUInfoPS5>();
}

CCPUInfoPS5::CCPUInfoPS5()
{
  // Oberon SoC: 8 Zen 2 cores (one is normally reserved for the system).
  // The loader decides what this process may be pinned to; ask the kernel and
  // fall back to the physical count if the sysctl is not permitted.
  int ncpu = 0;
  size_t len = sizeof(ncpu);
  if (sysctlbyname("hw.ncpu", &ncpu, &len, nullptr, 0) != 0 || ncpu <= 0)
    ncpu = 8;

  m_cpuCount = ncpu;
  m_cpuModel = "AMD Zen 2 (PlayStation 5)";

  for (int core = 0; core < m_cpuCount; core++)
  {
    CoreInfo coreInfo;
    coreInfo.m_id = core;
    m_cores.emplace_back(coreInfo);
  }
}
