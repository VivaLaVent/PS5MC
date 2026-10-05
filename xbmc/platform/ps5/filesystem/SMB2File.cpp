/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SMB2File.h"

#include "PasswordManager.h"
#include "SMB2Session.h"
#include "utils/log.h"

#include <cstring>

#include <sys/stat.h>

namespace SMB2
{
Target TargetFromURL(const CURL& input)
{
  Target t;
  t.host = input.GetHostName();
  t.port = input.HasPort() ? input.GetPort() : 0;
  t.share = input.GetShareName();
  // GetFileName() is "share/path/inside"; keep the part after the share
  std::string file = input.GetFileName();
  if (!t.share.empty() && file.compare(0, t.share.size(), t.share) == 0)
    file.erase(0, t.share.size());
  t.path = SharePath(file);
  t.domain = input.GetDomain();
  t.user = input.GetUserName();
  t.password = input.GetPassWord();
  // also accept DOMAIN\user
  const auto backslash = t.user.find('\\');
  if (t.domain.empty() && backslash != std::string::npos)
  {
    t.domain = t.user.substr(0, backslash);
    t.user.erase(0, backslash + 1);
  }
  return t;
}
} // namespace SMB2

using namespace XFILE;

namespace
{
CURL Authenticated(const CURL& url)
{
  CURL authed(url);
  CPasswordManager::GetInstance().AuthenticateURL(authed);
  return authed;
}

void FillStat(const SMB2::Stat& st, struct __stat64* buffer)
{
  std::memset(buffer, 0, sizeof(*buffer));
  buffer->st_size = static_cast<decltype(buffer->st_size)>(st.size);
  buffer->st_mode = st.isDirectory ? (S_IFDIR | 0755) : (S_IFREG | 0644);
  buffer->st_nlink = 1;
  buffer->st_mtime = static_cast<time_t>(st.mtime);
  buffer->st_atime = static_cast<time_t>(st.atime);
  buffer->st_ctime = static_cast<time_t>(st.ctime);
}
} // namespace

CSMB2File::CSMB2File() = default;

CSMB2File::~CSMB2File()
{
  Close();
}

bool CSMB2File::OpenInternal(const CURL& url, bool forWrite, bool overwrite)
{
  Close();
  const CURL authed = Authenticated(url);
  const SMB2::Target target = SMB2::TargetFromURL(authed);
  if (target.host.empty() || target.share.empty() || target.path.empty())
    return false;

  SMB2::Result r;
  m_file = SMB2::OpenFile::Open(target, forWrite, overwrite, r);
  if (!m_file)
  {
    CLog::Log(LOGERROR, "CSMB2File: cannot open {}: {}", url.GetRedacted(), r.message);
    return false;
  }
  SMB2::Stat st;
  if (m_file->Fstat(st))
    m_length = static_cast<int64_t>(st.size);
  m_position = 0;
  return true;
}

bool CSMB2File::Open(const CURL& url)
{
  return OpenInternal(url, false, false);
}

bool CSMB2File::OpenForWrite(const CURL& url, bool bOverWrite)
{
  return OpenInternal(url, true, bOverWrite);
}

void CSMB2File::Close()
{
  m_file.reset();
  m_position = 0;
  m_length = 0;
}

int CSMB2File::GetChunkSize()
{
  return m_file ? static_cast<int>(m_file->ChunkSize()) : 0;
}

ssize_t CSMB2File::Read(void* lpBuf, size_t uiBufSize)
{
  if (!m_file)
    return -1;
  if (m_length > 0 && m_position >= m_length)
    return 0; // EOF: never ask the server for data past the end
  size_t want = uiBufSize;
  if (m_length > 0)
    want = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(want), m_length - m_position));

  size_t done = 0;
  while (done < want)
  {
    SMB2::Result r;
    const int64_t n = m_file->Read(static_cast<char*>(lpBuf) + done, want - done,
                                   m_position + static_cast<int64_t>(done), r);
    if (n < 0)
    {
      CLog::Log(LOGERROR, "CSMB2File: read failed: {}", r.message);
      if (done == 0)
        return -1;
      break;
    }
    if (n == 0)
      break;
    done += static_cast<size_t>(n);
  }
  m_position += static_cast<int64_t>(done);
  return static_cast<ssize_t>(done);
}

ssize_t CSMB2File::Write(const void* lpBuf, size_t uiBufSize)
{
  if (!m_file)
    return -1;
  size_t done = 0;
  while (done < uiBufSize)
  {
    SMB2::Result r;
    const int64_t n = m_file->Write(static_cast<const char*>(lpBuf) + done, uiBufSize - done,
                                    m_position + static_cast<int64_t>(done), r);
    if (n <= 0)
    {
      CLog::Log(LOGERROR, "CSMB2File: write failed: {}", r.message);
      if (done == 0)
        return -1;
      break;
    }
    done += static_cast<size_t>(n);
  }
  m_position += static_cast<int64_t>(done);
  m_length = std::max(m_length, m_position);
  return static_cast<ssize_t>(done);
}

int64_t CSMB2File::Seek(int64_t iFilePosition, int iWhence)
{
  if (!m_file)
    return -1;
  int64_t target = 0;
  switch (iWhence)
  {
    case SEEK_SET:
      target = iFilePosition;
      break;
    case SEEK_CUR:
      target = m_position + iFilePosition;
      break;
    case SEEK_END:
      target = m_length + iFilePosition;
      break;
    default:
      return -1;
  }
  if (target < 0)
    return -1;
  m_position = target; // reads are positional (pread), so seeking is free
  return m_position;
}

bool CSMB2File::Exists(const CURL& url)
{
  struct __stat64 st;
  return Stat(url, &st) == 0 && !S_ISDIR(st.st_mode);
}

int CSMB2File::Stat(const CURL& url, struct __stat64* buffer)
{
  const SMB2::Target target = SMB2::TargetFromURL(Authenticated(url));
  if (target.host.empty() || target.share.empty())
    return -1;
  SMB2::Stat st;
  const SMB2::Result r = SMB2::StatPath(target, st);
  if (!r)
  {
    errno = r.error == SMB2::Error::NotFound ? ENOENT : EIO;
    return -1;
  }
  if (buffer)
    FillStat(st, buffer);
  return 0;
}

int CSMB2File::Stat(struct __stat64* buffer)
{
  if (!m_file || !buffer)
    return -1;
  SMB2::Stat st;
  if (!m_file->Fstat(st))
    return -1;
  FillStat(st, buffer);
  return 0;
}

bool CSMB2File::Delete(const CURL& url)
{
  const SMB2::Result r = SMB2::RemoveFile(SMB2::TargetFromURL(Authenticated(url)));
  if (!r)
    CLog::Log(LOGERROR, "CSMB2File: delete {} failed: {}", url.GetRedacted(), r.message);
  return static_cast<bool>(r);
}

bool CSMB2File::Rename(const CURL& url, const CURL& urlnew)
{
  const SMB2::Target from = SMB2::TargetFromURL(Authenticated(url));
  const SMB2::Target to = SMB2::TargetFromURL(urlnew);
  if (from.host != to.host || from.share != to.share)
    return false; // SMB renames within one share only
  const SMB2::Result r = SMB2::Rename(from, to.path);
  if (!r)
    CLog::Log(LOGERROR, "CSMB2File: rename {} failed: {}", url.GetRedacted(), r.message);
  return static_cast<bool>(r);
}
