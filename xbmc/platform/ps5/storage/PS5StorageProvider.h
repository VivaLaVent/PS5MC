/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "storage/IStorageProvider.h"

#include <chrono>
#include <set>
#include <string>
#include <vector>

class CPS5StorageProvider : public IStorageProvider
{
public:
  CPS5StorageProvider() = default;
  ~CPS5StorageProvider() override = default;

  void Initialize() override {}
  void Stop() override {}
  void GetLocalDrives(std::vector<CMediaSource>& localDrives) override;
  void GetRemovableDrives(std::vector<CMediaSource>& removableDrives) override;
  bool Eject(const std::string& mountpath) override { return false; }
  std::vector<std::string> GetDiskUsage() override;
  // USB (and extended-storage) mounts appearing or disappearing while Kodi
  // runs: checked every 2 seconds, reported as Kodi's storage events
  bool PumpDriveChangeEvents(IStorageEventsCallback* callback) override;

private:
  std::set<std::string> m_removable; // paths present at the last check
  std::chrono::steady_clock::time_point m_lastCheck{};
  bool m_checked = false;
};
