/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SMB2Session.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

extern "C"
{
// order matters: smb2.h defines types libsmb2.h uses
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-dcerpc.h>
#include <smb2/libsmb2-dcerpc-srvsvc.h>
}

namespace SMB2
{

std::string Target::ConnectionKey() const
{
  return domain + ";" + user + "@" + host + ":" + std::to_string(port) + "/" + share;
}

std::string SharePath(const std::string& path)
{
  std::string p = path;
  std::replace(p.begin(), p.end(), '\\', '/');
  while (!p.empty() && p.front() == '/')
    p.erase(p.begin());
  while (!p.empty() && p.back() == '/')
    p.pop_back();
  return p;
}

namespace
{

Error ClassifyErrno(int err, const std::string& message)
{
  // The session itself failed (socket closed, unparsable reply): whatever the
  // errno, this connection cannot be used again.
  if (message.find("smb2_service failed") != std::string::npos ||
      message.find("socket") != std::string::npos ||
      message.find("Socket") != std::string::npos)
    return Error::Network;
  // libsmb2 reports a failed logon as ECONNREFUSED - look at the NT status.
  if (message.find("LOGON_FAILURE") != std::string::npos ||
      message.find("ACCESS_DENIED") != std::string::npos ||
      message.find("ACCOUNT_") != std::string::npos ||
      message.find("PASSWORD_") != std::string::npos || err == EACCES || err == EPERM)
    return Error::AccessDenied;
  if (err == ENOENT || err == ENOTDIR)
    return Error::NotFound;
  if (err == ECONNREFUSED || err == ECONNRESET || err == ENOTCONN || err == EPIPE ||
      err == ETIMEDOUT || err == EHOSTUNREACH || err == ENETUNREACH || err == EIO ||
      err == ECONNABORTED)
    return Error::Network;
  return Error::Other;
}

void FillStat(const smb2_stat_64& st, Stat& out)
{
  out.isDirectory = st.smb2_type == SMB2_TYPE_DIRECTORY;
  out.size = st.smb2_size;
  out.mtime = static_cast<int64_t>(st.smb2_mtime);
  out.atime = static_cast<int64_t>(st.smb2_atime);
  out.ctime = static_cast<int64_t>(st.smb2_ctime);
}

bool IsConnectionError(Error e) { return e == Error::Network; }

// Drive libsmb2's event loop until *done or the deadline passes. Used for the
// async calls so a misbehaving server can never block Kodi indefinitely.
// Returns 0, or -ETIMEDOUT / -EIO.
int ServiceUntil(smb2_context* ctx, const bool& done, int seconds)
{
  const time_t deadline = time(nullptr) + seconds;
  while (!done)
  {
    pollfd pfd{};
    pfd.fd = smb2_get_fd(ctx);
    pfd.events = static_cast<short>(smb2_which_events(ctx));
    if (pfd.fd < 0 && time(nullptr) > deadline)
      return -ETIMEDOUT;
    const int n = poll(&pfd, pfd.fd < 0 ? 0 : 1, 250);
    if (n < 0 && errno != EINTR)
      return -EIO;
    if (n > 0 && smb2_service(ctx, pfd.revents) < 0)
      return -EIO;
    if (!done && time(nullptr) > deadline)
      return -ETIMEDOUT;
  }
  return 0;
}

struct AsyncStatus
{
  bool done = false;
  int status = 0;
};

void StatusCallback(smb2_context*, int status, void*, void* private_data)
{
  auto* s = static_cast<AsyncStatus*>(private_data);
  s->status = status;
  s->done = true;
}

} // namespace

// ---- Connection -----------------------------------------------------------

Connection::Connection(smb2_context* ctx, std::string key)
  : m_ctx(ctx),
    m_key(std::move(key)),
    m_maxRead(smb2_get_max_read_size(ctx)),
    m_maxWrite(smb2_get_max_write_size(ctx))
{
  // stay well inside what one request can carry
  if (m_maxRead == 0 || m_maxRead > 1024 * 1024)
    m_maxRead = std::min<uint32_t>(m_maxRead ? m_maxRead : 65536, 1024 * 1024);
  if (m_maxWrite == 0 || m_maxWrite > 1024 * 1024)
    m_maxWrite = std::min<uint32_t>(m_maxWrite ? m_maxWrite : 65536, 1024 * 1024);
}

Connection::~Connection()
{
  if (!m_ctx)
    return;
  if (m_broken)
  {
    // A failed synchronous call can leave its request queued in the context;
    // smb2_destroy_context() would then invoke that request's callback with
    // the long-gone stack data of the sync wrapper and crash. Close the socket
    // and let the (small) context leak instead.
    const auto fd = smb2_get_fd(m_ctx);
    if (fd >= 0)
      close(fd);
    return;
  }
  smb2_disconnect_share(m_ctx);
  smb2_destroy_context(m_ctx);
}

Result Connection::Fail(int rc, const char* what) const
{
  Result r;
  const char* err = smb2_get_error(m_ctx);
  r.message = std::string(what) + ": " + (err && *err ? err : std::strerror(-rc));
  r.error = ClassifyErrno(-rc, r.message);
  if (IsConnectionError(r.error))
    m_broken = true;
  return r;
}

// ---- Pool -----------------------------------------------------------------

Pool& Pool::Get()
{
  static Pool pool;
  return pool;
}

void Pool::ExpireLocked(time_t now)
{
  for (auto it = m_idle.begin(); it != m_idle.end();)
  {
    auto& list = it->second;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [now](const ConnectionPtr& c)
                              { return now - c->lastUsed > kIdleSeconds; }),
               list.end());
    it = list.empty() ? m_idle.erase(it) : std::next(it);
  }
}

ConnectionPtr Pool::Acquire(const Target& target, Result& result)
{
  const std::string key = target.ConnectionKey();
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const time_t now = time(nullptr);
    ExpireLocked(now);
    auto it = m_idle.find(key);
    if (it != m_idle.end() && !it->second.empty())
    {
      ConnectionPtr conn = std::move(it->second.back());
      it->second.pop_back();
      result = Result{};
      return conn;
    }
  }

  smb2_context* ctx = smb2_init_context();
  if (!ctx)
  {
    result = Result{Error::Other, "smb2_init_context failed"};
    return nullptr;
  }
  smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
  smb2_set_version(ctx, SMB2_VERSION_ANY);
  smb2_set_timeout(ctx, 30);
  if (!target.user.empty())
    smb2_set_user(ctx, target.user.c_str());
  else
    smb2_set_user(ctx, "Guest"); // anonymous/guest access
  smb2_set_password(ctx, target.password.c_str());
  if (!target.domain.empty())
    smb2_set_domain(ctx, target.domain.c_str());
  smb2_set_workstation(ctx, "KODI-PS5");

  std::string server = target.host;
  if (target.port > 0 && target.port != 445)
    server += ":" + std::to_string(target.port);
  // async + our own deadline: a server that never answers (or answers
  // something libsmb2 cannot parse) must not hang Kodi
  AsyncStatus connect;
  int rc = smb2_connect_share_async(ctx, server.c_str(), target.share.c_str(),
                                    target.user.empty() ? "Guest" : target.user.c_str(),
                                    StatusCallback, &connect);
  if (rc == 0)
  {
    rc = ServiceUntil(ctx, connect.done, 20);
    if (rc == 0)
      rc = connect.status;
    else if (rc == -ETIMEDOUT)
      smb2_set_error(ctx, "no answer from the server within 20 seconds");
  }
  if (rc < 0)
  {
    const char* err = smb2_get_error(ctx);
    result.message = "connect to " + server + "/" + target.share + ": " +
                     (err && *err ? err : std::strerror(-rc));
    result.error = ClassifyErrno(-rc, result.message);
    // not smb2_destroy_context(): see ~Connection (a failed sync call can
    // leave callbacks behind that destroy would run against freed state)
    const auto fd = smb2_get_fd(ctx);
    if (fd >= 0)
      close(fd);
    return nullptr;
  }
  result = Result{};
  return std::make_unique<Connection>(ctx, key);
}

void Pool::Release(ConnectionPtr conn)
{
  if (!conn || conn->IsBroken())
    return; // destroyed
  conn->lastUsed = time(nullptr);
  std::lock_guard<std::mutex> lock(m_mutex);
  auto& list = m_idle[conn->Key()];
  if (list.size() < 4)
    list.push_back(std::move(conn));
}

void Pool::Forget(const Target& target)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_idle.erase(target.ConnectionKey());
}

// ---- operations -----------------------------------------------------------

// Run op on a pooled connection; on a broken connection retry once on a new one.
template<typename Op>
static Result WithConnection(const Target& target, Op op)
{
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    Result r;
    ConnectionPtr conn = Pool::Get().Acquire(target, r);
    if (!conn)
      return r;
    r = op(*conn);
    const bool retry = conn->IsBroken() && attempt == 0;
    Pool::Get().Release(std::move(conn));
    if (!retry)
      return r;
  }
  return Result{Error::Network, "connection lost"};
}

Result StatPath(const Target& target, Stat& out)
{
  const std::string path = SharePath(target.path);
  if (path.empty())
  {
    out = Stat{};
    out.isDirectory = true; // the share itself
    return WithConnection(target, [](Connection&) { return Result{}; });
  }
  return WithConnection(target,
                        [&](Connection& c)
                        {
                          smb2_stat_64 st{};
                          const int rc = smb2_stat(c.Context(), path.c_str(), &st);
                          if (rc < 0)
                            return c.Fail(rc, "stat");
                          FillStat(st, out);
                          return Result{};
                        });
}

Result ListDirectory(const Target& target, std::vector<DirEntry>& out)
{
  const std::string path = SharePath(target.path);
  return WithConnection(target,
                        [&](Connection& c)
                        {
                          out.clear();
                          smb2dir* dir = smb2_opendir(c.Context(), path.c_str());
                          if (!dir)
                            return c.Fail(-errno ? -errno : -EIO, "opendir");
                          while (smb2dirent* e = smb2_readdir(c.Context(), dir))
                          {
                            if (!e->name || !std::strcmp(e->name, ".") ||
                                !std::strcmp(e->name, ".."))
                              continue;
                            DirEntry d;
                            d.name = e->name;
                            FillStat(e->st, d.stat);
                            out.push_back(std::move(d));
                          }
                          smb2_closedir(c.Context(), dir);
                          return Result{};
                        });
}

namespace
{
struct ShareEnumState
{
  bool done = false;
  int status = 0;
  std::vector<std::string>* out = nullptr;
};

void ShareEnumCallback(smb2_context* smb2, int status, void* command_data, void* private_data)
{
  auto* state = static_cast<ShareEnumState*>(private_data);
  state->status = status;
  if (status == 0 && command_data)
  {
    auto* rep = static_cast<srvsvc_NetrShareEnum_rep*>(command_data);
    const auto& level1 = rep->ses.ShareInfo.Level1;
    for (uint32_t i = 0; i < level1.EntriesRead; ++i)
    {
      const auto& info = level1.Buffer->share_info_1[i];
      // disk shares only; hidden (admin) shares end in '$'
      if ((info.type & 3) != SHARE_TYPE_DISKTREE || !info.netname.utf8)
        continue;
      std::string name = info.netname.utf8;
      if (!name.empty() && name.back() == '$')
        continue;
      state->out->push_back(name);
    }
    smb2_free_data(smb2, rep);
  }
  state->done = true;
}
} // namespace

Result ListShares(const Target& target, std::vector<std::string>& out)
{
  Target ipc = target;
  ipc.share = "IPC$";
  ipc.path.clear();
  return WithConnection(
      ipc,
      [&](Connection& c)
      {
        out.clear();
        ShareEnumState state;
        state.out = &out;
        if (smb2_share_enum_async(c.Context(), SHARE_INFO_1, ShareEnumCallback, &state) != 0)
          return c.Fail(-EIO, "share enum");
        const int rc = ServiceUntil(c.Context(), state.done, 30);
        if (rc < 0)
        {
          c.MarkBroken(); // a late answer may still arrive; do not reuse
          return Result{Error::Network, rc == -ETIMEDOUT ? "share enum: timeout"
                                                         : "share enum: connection failed"};
        }
        if (state.status < 0)
          return c.Fail(state.status, "share enum");
        std::sort(out.begin(), out.end());
        return Result{};
      });
}

Result MakeDirectory(const Target& target)
{
  const std::string path = SharePath(target.path);
  return WithConnection(target,
                        [&](Connection& c)
                        {
                          const int rc = smb2_mkdir(c.Context(), path.c_str());
                          return rc < 0 ? c.Fail(rc, "mkdir") : Result{};
                        });
}

Result RemoveDirectory(const Target& target)
{
  const std::string path = SharePath(target.path);
  return WithConnection(target,
                        [&](Connection& c)
                        {
                          const int rc = smb2_rmdir(c.Context(), path.c_str());
                          return rc < 0 ? c.Fail(rc, "rmdir") : Result{};
                        });
}

Result RemoveFile(const Target& target)
{
  const std::string path = SharePath(target.path);
  return WithConnection(target,
                        [&](Connection& c)
                        {
                          const int rc = smb2_unlink(c.Context(), path.c_str());
                          return rc < 0 ? c.Fail(rc, "unlink") : Result{};
                        });
}

Result Rename(const Target& from, const std::string& toPath)
{
  const std::string a = SharePath(from.path);
  const std::string b = SharePath(toPath);
  return WithConnection(from,
                        [&](Connection& c)
                        {
                          const int rc = smb2_rename(c.Context(), a.c_str(), b.c_str());
                          return rc < 0 ? c.Fail(rc, "rename") : Result{};
                        });
}

// ---- OpenFile -------------------------------------------------------------

OpenFile::~OpenFile()
{
  if (m_conn && m_fh)
    smb2_close(m_conn->Context(), m_fh);
  if (m_conn)
    Pool::Get().Release(std::move(m_conn));
}

std::unique_ptr<OpenFile> OpenFile::Open(const Target& target, bool forWrite, bool overwrite,
                                         Result& result)
{
  const std::string path = SharePath(target.path);
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    ConnectionPtr conn = Pool::Get().Acquire(target, result);
    if (!conn)
      return nullptr;
    int flags = O_RDONLY;
    if (forWrite)
      flags = O_WRONLY | O_CREAT | (overwrite ? O_TRUNC : 0);
    smb2fh* fh = smb2_open(conn->Context(), path.c_str(), flags);
    if (fh)
    {
      std::unique_ptr<OpenFile> file(new OpenFile());
      file->m_conn = std::move(conn);
      file->m_fh = fh;
      result = Result{};
      return file;
    }
    result = conn->Fail(-(errno ? errno : EIO), "open");
    const bool retry = conn->IsBroken() && attempt == 0;
    Pool::Get().Release(std::move(conn));
    if (!retry)
      return nullptr;
  }
  return nullptr;
}

int64_t OpenFile::Read(void* buf, size_t size, int64_t offset, Result& result)
{
  if (!m_conn || !m_fh)
  {
    result = Result{Error::Other, "file not open"};
    return -1;
  }
  const uint32_t count = static_cast<uint32_t>(std::min<size_t>(size, m_conn->MaxRead()));
  const int rc = smb2_pread(m_conn->Context(), m_fh, static_cast<uint8_t*>(buf), count,
                            static_cast<uint64_t>(offset));
  if (rc < 0)
  {
    result = m_conn->Fail(rc, "read");
    return -1;
  }
  result = Result{};
  return rc;
}

int64_t OpenFile::Write(const void* buf, size_t size, int64_t offset, Result& result)
{
  if (!m_conn || !m_fh)
  {
    result = Result{Error::Other, "file not open"};
    return -1;
  }
  const uint32_t count = static_cast<uint32_t>(std::min<size_t>(size, m_conn->MaxWrite()));
  const int rc = smb2_pwrite(m_conn->Context(), m_fh, static_cast<const uint8_t*>(buf), count,
                             static_cast<uint64_t>(offset));
  if (rc < 0)
  {
    result = m_conn->Fail(rc, "write");
    return -1;
  }
  result = Result{};
  return rc;
}

Result OpenFile::Fstat(Stat& out)
{
  if (!m_conn || !m_fh)
    return Result{Error::Other, "file not open"};
  smb2_stat_64 st{};
  const int rc = smb2_fstat(m_conn->Context(), m_fh, &st);
  if (rc < 0)
    return m_conn->Fail(rc, "fstat");
  FillStat(st, out);
  return Result{};
}

} // namespace SMB2
