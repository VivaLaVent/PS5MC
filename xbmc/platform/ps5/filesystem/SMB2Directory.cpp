/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SMB2Directory.h"

#include "FileItem.h"
#include "FileItemList.h"
#include "PasswordManager.h"
#include "SMB2File.h"
#include "SMB2Session.h"
#include "URL.h"
#include "utils/URIUtils.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"

#include <memory>
#include <vector>

using namespace XFILE;

namespace
{
// Kodi 22 added CGUIListItem::SetFolder(); on Kodi 21 the flag is the public m_bIsFolder.
inline void SetIsFolder(CFileItem& item, bool folder)
{
#if PS5_KODI_MAJOR >= 22
  item.SetFolder(folder);
#else
  item.m_bIsFolder = folder;
#endif
}

// Kodi 22 added CFileItem::SetSize(); on Kodi 21 the size is the public m_dwSize.
inline void SetItemSize(CFileItem& item, int64_t size)
{
#if PS5_KODI_MAJOR >= 22
  item.SetSize(size);
#else
  item.m_dwSize = size;
#endif
}

// Kodi 22 added CFileItem::SetDateTime(); on Kodi 21 it is the public m_dateTime
// (CDateTime assigns from a FileTime on both versions).
inline void SetItemDateTime(CFileItem& item, const KODI::TIME::FileTime& time)
{
#if PS5_KODI_MAJOR >= 22
  item.SetDateTime(time);
#else
  item.m_dateTime = time;
#endif
}

// Kodi 22 added CFileItemList::AddItems(); Kodi 21 adds one item at a time.
inline void AddAllItems(CFileItemList& list, std::vector<std::shared_ptr<CFileItem>>&& items)
{
#if PS5_KODI_MAJOR >= 22
  list.AddItems(std::move(items));
#else
  for (auto& item : items)
    list.Add(std::move(item));
#endif
}
} // namespace

namespace
{
KODI::TIME::FileTime ToLocalFileTime(int64_t mtime, int64_t ctime)
{
  const int64_t t = mtime ? mtime : ctime;
  long long ll = t & 0xffffffff;
  ll *= 10000000ll;
  ll += 116444736000000000ll;
  KODI::TIME::FileTime fileTime{};
  fileTime.lowDateTime = static_cast<DWORD>(ll & 0xffffffff);
  fileTime.highDateTime = static_cast<DWORD>(ll >> 32);
  KODI::TIME::FileTime localTime{};
  KODI::TIME::FileTimeToLocalFileTime(&fileTime, &localTime);
  return localTime;
}
} // namespace

bool CSMB2Directory::GetDirectory(const CURL& urlIn, CFileItemList& items)
{
  CURL url(urlIn);
  CPasswordManager::GetInstance().AuthenticateURL(url);
  const SMB2::Target target = SMB2::TargetFromURL(url);

  if (target.host.empty())
    return true; // smb:// - no network browsing without NetBIOS/WS-Discovery

  std::string base = urlIn.Get();
  URIUtils::AddSlashAtEnd(base);
  std::vector<std::shared_ptr<CFileItem>> fileItems;
  SMB2::Result r;

  if (target.share.empty())
  {
    std::vector<std::string> shares;
    r = SMB2::ListShares(target, shares);
    for (const auto& share : shares)
    {
      auto item = std::make_shared<CFileItem>(share);
      item->SetPath(base + share + "/");
      SetIsFolder(*item, true);
      fileItems.push_back(std::move(item));
    }
  }
  else
  {
    std::vector<SMB2::DirEntry> entries;
    r = SMB2::ListDirectory(target, entries);
    for (const auto& e : entries)
    {
      auto item = std::make_shared<CFileItem>(e.name);
      std::string path = base + e.name;
      if (e.stat.isDirectory)
        URIUtils::AddSlashAtEnd(path);
      item->SetPath(path);
      SetIsFolder(*item, e.stat.isDirectory);
      SetItemSize(*item, static_cast<int64_t>(e.stat.size));
      SetItemDateTime(*item, ToLocalFileTime(e.stat.mtime, e.stat.ctime));
#if PS5_KODI_MAJOR >= 22 // raw stat-time properties are new in Kodi 22 (21 neither has nor reads them)
      item->SetProperty(DIR_PROPERTY_STAT_MTIME, e.stat.mtime);
      item->SetProperty(DIR_PROPERTY_STAT_CTIME, e.stat.ctime);
#endif
      if (!e.name.empty() && e.name[0] == '.')
        item->SetProperty("file:hidden", true);
      fileItems.push_back(std::move(item));
    }
  }

  if (!r)
  {
    CLog::Log(LOGERROR, "CSMB2Directory: {}: {}", urlIn.GetRedacted(), r.message);
    if (r.error == SMB2::Error::LockedOut)
    {
      CLog::Log(LOGERROR,
                "CSMB2Directory: {} is locked out on the server (too many failed logins). "
                "The password is not re-requested; unlock the account on the server or wait for "
                "the lockout to expire.",
                CURL(urlIn).GetHostName());
      return false; // do not RequireAuthentication - that would retry and relock
    }
    if (r.error == SMB2::Error::AccessDenied)
    {
      SMB2::Pool::Get().Forget(target);
      if (m_flags & DIR_FLAG_ALLOW_PROMPT)
        RequireAuthentication(urlIn); // Kodi asks for user name and password
    }
    else if (m_flags & DIR_FLAG_ALLOW_PROMPT)
      SetErrorDialog(257, r.message); // "Error"
    return false;
  }

  AddAllItems(items, std::move(fileItems));
  return true;
}

bool CSMB2Directory::Create(const CURL& urlIn)
{
  CURL url(urlIn);
  CPasswordManager::GetInstance().AuthenticateURL(url);
  const SMB2::Result r = SMB2::MakeDirectory(SMB2::TargetFromURL(url));
  if (!r)
    CLog::Log(LOGERROR, "CSMB2Directory: create {} failed: {}", urlIn.GetRedacted(), r.message);
  return static_cast<bool>(r);
}

bool CSMB2Directory::Exists(const CURL& urlIn)
{
  CURL url(urlIn);
  CPasswordManager::GetInstance().AuthenticateURL(url);
  const SMB2::Target target = SMB2::TargetFromURL(url);
  if (target.host.empty())
    return true;
  if (target.share.empty())
  {
    std::vector<std::string> shares;
    return static_cast<bool>(SMB2::ListShares(target, shares));
  }
  SMB2::Stat st;
  return SMB2::StatPath(target, st) && st.isDirectory;
}

bool CSMB2Directory::Remove(const CURL& urlIn)
{
  CURL url(urlIn);
  CPasswordManager::GetInstance().AuthenticateURL(url);
  const SMB2::Result r = SMB2::RemoveDirectory(SMB2::TargetFromURL(url));
  if (!r)
    CLog::Log(LOGERROR, "CSMB2Directory: remove {} failed: {}", urlIn.GetRedacted(), r.message);
  return static_cast<bool>(r);
}
