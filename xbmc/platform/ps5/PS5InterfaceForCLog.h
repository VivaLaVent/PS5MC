/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/IPlatformLog.h"

/*
 * Log sinks for the PS5: besides the usual kodi.log file (added by CLog),
 * every log line also goes to klog (the system log, readable over TCP 3232
 * with klogsrv), since a title's stdout/stderr go nowhere.
 */
class CPS5InterfaceForCLog : public IPlatformLog
{
public:
  CPS5InterfaceForCLog() = default;
  ~CPS5InterfaceForCLog() override = default;

  spdlog_filename_t GetLogFilename(const std::string& filename) const override { return filename; }
  void AddSinks(
      std::shared_ptr<spdlog::sinks::dist_sink<std::mutex>> distributionSink) const override;
};
