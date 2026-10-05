/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "platform/posix/GPUInfoPosix.h"

class CGPUInfoPS5 : public CGPUInfoPosix
{
public:
  CGPUInfoPS5() = default;
  ~CGPUInfoPS5() = default;

private:
  bool SupportsPlatformTemperature() const override { return false; }
  bool GetGPUPlatformTemperature(CTemperature& temperature) const override { return false; }
};
