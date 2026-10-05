/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "platform/posix/CPUInfoPosix.h"

class CCPUInfoPS5 : public CCPUInfoPosix
{
public:
  CCPUInfoPS5();
  ~CCPUInfoPS5() = default;

  bool SupportsCPUUsage() const override { return false; }
  int GetUsedPercentage() override { return 0; }
  float GetCPUFrequency() override { return 3500.0f; }
};
