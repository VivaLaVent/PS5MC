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
      item->SetFolder(true);
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
      item->SetFolder(e.stat.isDirectory);
      item->SetSize(static_cast<int64_t>(e.stat.size));
      item->SetDateTime(ToLocalFileTime(e.stat.mtime, e.stat.ctime));
      item->SetProperty(DIR_PROPERTY_STAT_MTIME, e.stat.mtime);
      item->SetProperty(DIR_PROPERTY_STAT_CTIME, e.stat.ctime);
      if (!e.name.empty() && e.name[0] == '.')
        item->SetProperty("file:hidden", true);
      fileItems.push_back(std::move(item));
    }
  }

  if (!r)
  {
    CLog::Log(LOGERROR, "CSMB2Directory: {}: {}", urlIn.GetRedacted(), r.message);
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

  items.AddItems(std::move(fileItems));
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
