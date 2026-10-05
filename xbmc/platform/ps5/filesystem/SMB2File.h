/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "URL.h"
#include "filesystem/IFile.h"

#include <memory>

namespace SMB2
{
class OpenFile;
struct Target;
SMB2::Target TargetFromURL(const CURL& url);
} // namespace SMB2

namespace XFILE
{

/*!
 * smb:// files over libsmb2 (SMB2/SMB3), for platforms without libsmbclient.
 */
class CSMB2File : public IFile
{
public:
  CSMB2File();
  ~CSMB2File() override;

  bool Open(const CURL& url) override;
  bool OpenForWrite(const CURL& url, bool bOverWrite = false) override;
  void Close() override;

  ssize_t Read(void* lpBuf, size_t uiBufSize) override;
  ssize_t Write(const void* lpBuf, size_t uiBufSize) override;
  int64_t Seek(int64_t iFilePosition, int iWhence = SEEK_SET) override;
  int64_t GetPosition() override { return m_position; }
  int64_t GetLength() override { return m_length; }
  int GetChunkSize() override;

  bool Exists(const CURL& url) override;
  int Stat(const CURL& url, struct __stat64* buffer) override;
  int Stat(struct __stat64* buffer) override;
  bool Delete(const CURL& url) override;
  bool Rename(const CURL& url, const CURL& urlnew) override;

  int IoControl(IOControl request, void* param) override
  {
    return request == IOControl::SEEK_POSSIBLE ? 1 : -1;
  }

private:
  bool OpenInternal(const CURL& url, bool forWrite, bool overwrite);

  std::unique_ptr<SMB2::OpenFile> m_file;
  int64_t m_position = 0;
  int64_t m_length = 0;
};

} // namespace XFILE
