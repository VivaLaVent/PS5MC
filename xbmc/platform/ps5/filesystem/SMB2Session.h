/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*
 * SMB2/SMB3 client plumbing on libsmb2, independent of Kodi's classes so it
 * can be tested on its own. Kodi's smb:// file and directory implementations
 * (SMB2File, SMB2Directory) are thin adapters on top.
 *
 * libsmb2 contexts are not thread-safe, so every user takes a connection out
 * of the pool exclusively and gives it back when done. Open files keep theirs
 * for their whole lifetime; directory and stat operations borrow one briefly.
 */

#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct smb2_context;
struct smb2fh;

namespace SMB2
{

struct Target
{
  std::string host;
  int port = 0; // 0: default (445)
  std::string share;
  std::string path; // inside the share, '/'-separated, no leading '/'
  std::string domain;
  std::string user;
  std::string password;

  std::string ConnectionKey() const; // host:port/share as user
};

struct Stat
{
  bool isDirectory = false;
  uint64_t size = 0;
  int64_t mtime = 0;
  int64_t atime = 0;
  int64_t ctime = 0;
};

struct DirEntry
{
  std::string name;
  Stat stat;
};

enum class Error
{
  None,
  NotFound,
  AccessDenied, // wrong or missing credentials: ask the user
  Network,      // host unreachable, connection lost, timeout
  Other,
};

struct Result
{
  Error error = Error::None;
  std::string message;
  explicit operator bool() const { return error == Error::None; }
};

class Connection
{
public:
  Connection(smb2_context* ctx, std::string key);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  smb2_context* Context() const { return m_ctx; }
  const std::string& Key() const { return m_key; }
  uint32_t MaxRead() const { return m_maxRead; }
  uint32_t MaxWrite() const { return m_maxWrite; }

  // classify the last libsmb2 failure (rc = negative errno)
  Result Fail(int rc, const char* what) const;
  // the connection itself is broken and must not go back to the pool
  bool IsBroken() const { return m_broken; }
  void MarkBroken() { m_broken = true; }

  time_t lastUsed = 0;

private:
  smb2_context* m_ctx;
  std::string m_key;
  uint32_t m_maxRead;
  uint32_t m_maxWrite;
  mutable bool m_broken = false;
};

using ConnectionPtr = std::unique_ptr<Connection>;

class Pool
{
public:
  static Pool& Get();

  // A connected session for the target's host/share/user (reused when idle).
  ConnectionPtr Acquire(const Target& target, Result& result);
  void Release(ConnectionPtr conn);
  // Drop idle connections to a target, e.g. after credentials changed.
  void Forget(const Target& target);

private:
  void ExpireLocked(time_t now);

  std::mutex m_mutex;
  std::map<std::string, std::vector<ConnectionPtr>> m_idle;
  static constexpr int kIdleSeconds = 120;
};

// Borrow-and-return helper for short operations.
class Borrowed
{
public:
  Borrowed(const Target& target, Result& result) : m_conn(Pool::Get().Acquire(target, result)) {}
  ~Borrowed()
  {
    if (m_conn)
      Pool::Get().Release(std::move(m_conn));
  }
  Connection* operator->() const { return m_conn.get(); }
  explicit operator bool() const { return m_conn != nullptr; }

private:
  ConnectionPtr m_conn;
};

// libsmb2 wants '\'-free, '/'-separated paths relative to the share.
std::string SharePath(const std::string& path);

Result StatPath(const Target& target, Stat& out);
Result ListDirectory(const Target& target, std::vector<DirEntry>& out);
Result ListShares(const Target& target, std::vector<std::string>& out);
Result MakeDirectory(const Target& target);
Result RemoveDirectory(const Target& target);
Result RemoveFile(const Target& target);
Result Rename(const Target& from, const std::string& toPath);

// An open remote file holding its own connection.
class OpenFile
{
public:
  ~OpenFile();
  static std::unique_ptr<OpenFile> Open(const Target& target, bool forWrite, bool overwrite,
                                        Result& result);

  int64_t Read(void* buf, size_t size, int64_t offset, Result& result);
  int64_t Write(const void* buf, size_t size, int64_t offset, Result& result);
  Result Fstat(Stat& out);
  uint32_t ChunkSize() const { return m_conn ? m_conn->MaxRead() : 0; }

private:
  OpenFile() = default;
  ConnectionPtr m_conn;
  smb2fh* m_fh = nullptr;
};

} // namespace SMB2
