/*
 *  Copyright (C) 2016-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#include "FilesystemInstaller.h"

#include "Util.h"

#include "FileItem.h"
#include "FileItemList.h"
#include "filesystem/Directory.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "utils/FileOperationJob.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <chrono>

using namespace XFILE;

namespace
{
bool UnpackArchive(std::string path, const std::string& dest)
{
  if (!URIUtils::IsProtocol(path, "zip"))
    path = URIUtils::CreateArchivePath("zip", CURL(path), "").Get();

  CFileItemList files;
  if (!CDirectory::GetDirectory(path, files, "", DIR_FLAG_DEFAULTS))
    return false;

  if (files.Size() == 1 && files[0]->IsFolder())
  {
    path = files[0]->GetPath();
    files.Clear();
    if (!CDirectory::GetDirectory(path, files, "", DIR_FLAG_DEFAULTS))
      return false;
  }
  CLog::Log(LOGDEBUG, "Unpacking {} to {}", path, dest);

  for (auto i = 0; i < files.Size(); ++i)
    files[i]->Select(true);

  CFileOperationJob job(CFileOperationJob::ActionCopy, files, dest);
  return job.DoWork();
}
} // unnamed namespace

CFilesystemInstaller::CFilesystemInstaller()
  : m_addonFolder(CSpecialProtocol::TranslatePath("special://home/addons/")),
    m_tempFolder(CSpecialProtocol::TranslatePath("special://home/addons/temp/"))
{
}

bool CFilesystemInstaller::InstallToFilesystem(const std::string& archive,
                                               const std::string& addonId) const
{
  const std::string addonFolder = URIUtils::AddFileToFolder(m_addonFolder, addonId);
  const std::string newAddonData =
      URIUtils::AddFileToFolder(m_tempFolder, StringUtils::CreateUUID());
  const std::string oldAddonData =
      URIUtils::AddFileToFolder(m_tempFolder, StringUtils::CreateUUID());

  // PS5: add-on installs take tens of seconds while downloads are fast; time the
  // file steps (logged below) to find out whether it is per file or per byte
  [[maybe_unused]] const auto started = std::chrono::steady_clock::now();
  if (!CDirectory::Create(newAddonData))
    return false;

  if (!UnpackArchive(archive, newAddonData))
  {
    CLog::Log(LOGERROR, "Failed to unpack archive '{}' to '{}'", archive, newAddonData);
    return false;
  }

  [[maybe_unused]] const auto unpacked = std::chrono::steady_clock::now();
  const bool hasOldData = CDirectory::Exists(addonFolder);
  if (hasOldData && !CFile::Rename(addonFolder, oldAddonData))
  {
    CLog::Log(LOGERROR, "Failed to move old addon files from '{}' to '{}'", addonFolder,
              oldAddonData);
    return false;
  }

  if (!CFile::Rename(newAddonData, addonFolder))
  {
    CLog::Log(LOGERROR, "Failed to move new addon files from '{}' to '{}'", newAddonData,
              addonFolder);
    return false;
  }
  [[maybe_unused]] const auto moved = std::chrono::steady_clock::now();

  if (hasOldData && !CDirectory::RemoveRecursive(oldAddonData))
  {
    CLog::Log(LOGWARNING, "Failed to delete old addon files in '{}'", oldAddonData);
  }
#if defined(TARGET_PS5)
  {
    const auto done = std::chrono::steady_clock::now();
    auto ms = [](auto from, auto to)
    { return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count(); };
    CFileItemList files;
    CUtil::GetRecursiveListing(addonFolder, files, "", DIR_FLAG_NO_FILE_DIRS);
    int64_t bytes = 0;
    for (int i = 0; i < files.Size(); ++i)
      bytes += files[i]->GetSize();
    CLog::Log(LOGINFO,
              "CFilesystemInstaller[{}]: {} files, {} KiB: unpacked in {} ms, moved in {} ms, "
              "old version removed in {} ms",
              addonId, files.Size(), bytes / 1024, ms(started, unpacked), ms(unpacked, moved),
              ms(moved, done));
  }
#endif
  return true;
}

bool CFilesystemInstaller::UnInstallFromFilesystem(const std::string& addonFolder) const
{
  const std::string tempFolder = URIUtils::AddFileToFolder(m_tempFolder, StringUtils::CreateUUID());
  if (!CFile::Rename(addonFolder, tempFolder))
  {
    CLog::Log(LOGERROR, "Failed to move old addon files from '{}' to '{}'", addonFolder,
              tempFolder);
    return false;
  }

  if (!CDirectory::RemoveRecursive(tempFolder))
  {
    CLog::Log(LOGWARNING, "Failed to delete old addon files in '{}'", tempFolder);
  }
  return true;
}
