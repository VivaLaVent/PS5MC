/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PS5StorageProvider.h"

#include "utils/log.h"

#include "MediaSource.h"

#include <sys/stat.h>

std::unique_ptr<IStorageProvider> IStorageProvider::CreateInstance()
{
  return std::make_unique<CPS5StorageProvider>();
}

namespace
{
bool Exists(const char* path)
{
  struct stat st;
  return stat(path, &st) == 0;
}

void Add(std::vector<CMediaSource>& drives, const char* path, const char* name)
{
  if (!Exists(path))
    return;
  CMediaSource share;
  share.strPath = path;
  share.strName = name;
#if PS5_KODI_MAJOR >= 22
  share.m_iDriveType = SourceType::LOCAL;
#else // Kodi 21: nested plain enum
  share.m_iDriveType = CMediaSource::SOURCE_TYPE_LOCAL;
#endif
  drives.push_back(share);
}
} // namespace

void CPS5StorageProvider::GetLocalDrives(std::vector<CMediaSource>& localDrives)
{
  // No local drive entries. The app's own areas (/app0, /download0, /data)
  // never hold media, and the loader's loopback FTP server is deliberately not
  // offered as a source: browsing or refreshing it drives synchronous FTP
  // Stat/connect round-trips that stall the GUI thread (folder-art probes,
  // directory-provider refreshes). Removed for UI responsiveness. To reach
  // console files, add ftp://127.0.0.1:2121/ (or :1337) as a source manually.
  (void)localDrives;
}
namespace
{
// USB drives and extended storage appear here once the sandbox is open
const char* const kRemovable[] = {"/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
                                  "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
                                  "/mnt/ext0", "/mnt/ext1"};
}

void CPS5StorageProvider::GetRemovableDrives(std::vector<CMediaSource>& removableDrives)
{
  for (const char* path : kRemovable)
    Add(removableDrives, path, path);
}

bool CPS5StorageProvider::PumpDriveChangeEvents(IStorageEventsCallback* callback)
{
  const auto now = std::chrono::steady_clock::now();
  if (m_checked && now - m_lastCheck < std::chrono::seconds(2))
    return false;
  m_lastCheck = now;

  std::set<std::string> present;
  for (const char* path : kRemovable)
    if (Exists(path))
      present.insert(path);
  if (!m_checked)
  {
    m_checked = true;
    m_removable = present; // the initial set: listed, not announced
    return false;
  }

  bool changed = false;
  for (const auto& path : present)
    if (!m_removable.count(path))
    {
      CLog::Log(LOGINFO, "CPS5StorageProvider: {} appeared", path);
      if (callback)
        callback->OnStorageAdded({path, path, MEDIA_DETECT::STORAGE::Type::UNKNOWN});
      changed = true;
    }
  for (const auto& path : m_removable)
    if (!present.count(path))
    {
      CLog::Log(LOGINFO, "CPS5StorageProvider: {} removed", path);
      if (callback)
        callback->OnStorageUnsafelyRemoved({path, path, MEDIA_DETECT::STORAGE::Type::UNKNOWN});
      changed = true;
    }
  m_removable = present;
  return changed;
}

std::vector<std::string> CPS5StorageProvider::GetDiskUsage()
{
  return {};
}
