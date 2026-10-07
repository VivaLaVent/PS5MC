/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "application/AppEnvironment.h"
#include "platform/ps5/BuildStamp.h"
#include "platform/ps5/JitProbe.h"
#include "application/AppParamParser.h"
#include "application/AppParams.h"
#include "platform/xbmc.h"

#include "platform/posix/PlatformPosix.h"

#include <cerrno>
#include <clocale>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int sceKernelDebugOutText(int channel, const char* text);
// FreeBSD <dirent.h> declares these only with __BSD_VISIBLE; used by the
// directory diagnostics below.
extern "C" int getdents(int fd, char* buf, int nbytes);
extern "C" int getdirentries(int fd, char* buf, int nbytes, long* basep);

// The few SQLite calls the startup probe needs (libsqlite3 is linked into
// Kodi; its header lives outside the kodi target's include path).
extern "C"
{
typedef struct sqlite3 sqlite3;
int sqlite3_open_v2(const char* filename, sqlite3** db, int flags, const char* vfs);
int sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg,
                 char** errmsg);
const char* sqlite3_errmsg(sqlite3* db);
int sqlite3_close(sqlite3* db);
int sqlite3_extended_errcode(sqlite3* db);
int sqlite3_config(int op, ...);
typedef struct sqlite3_vfs sqlite3_vfs;
sqlite3_vfs* sqlite3_vfs_find(const char* name);
int sqlite3_vfs_register(sqlite3_vfs* vfs, int makeDefault);
}
// Public, stable layout of sqlite3_vfs (sqlite3.h, iVersion 3); only the
// system-call override hook is used.
typedef void (*sqlite3_syscall_ptr)(void);
struct Sqlite3VfsV3
{
  int iVersion;
  int szOsFile;
  int mxPathname;
  void* pNext;
  const char* zName;
  void* pAppData;
  void* xOpen;
  void* xDelete;
  void* xAccess;
  void* xFullPathname;
  void* xDlOpen;
  void* xDlError;
  void* xDlSym;
  void* xDlClose;
  void* xRandomness;
  void* xSleep;
  void* xCurrentTime;
  void* xGetLastError;
  void* xCurrentTimeInt64;
  int (*xSetSystemCall)(sqlite3_vfs*, const char* name, sqlite3_syscall_ptr call);
  sqlite3_syscall_ptr (*xGetSystemCall)(sqlite3_vfs*, const char* name);
  const char* (*xNextSystemCall)(sqlite3_vfs*, const char* name);
};
constexpr int SQLITE_OK = 0;
constexpr int SQLITE_CONFIG_LOG = 16;
constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
constexpr int SQLITE_OPEN_CREATE = 0x00000004;

namespace
{
extern "C" void XBMC_PS5_HandleSignal(int sig)
{
  CPlatformPosix::RequestQuit();
}

/*
 * A title's stdout/stderr go nowhere; Kodi's log reaches klog through the
 * platform log sink (PS5InterfaceForCLog). These markers bracket startup and
 * are written straight to klog, so they appear even if logging never starts.
 */
void Klog(const char* text)
{
  sceKernelDebugOutText(0, text);
}

void Klogf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Klogf(const char* fmt, ...)
{
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Klog(buf);
}

void SqliteLog(void*, int code, const char* msg)
{
  Klogf("[kodi-ps5]   sqlite log (%d): %s\n", code, msg);
}

/*
 * SQLite resolves symlinks in every database path by lstat()-ing each
 * component, and the title sandbox refuses lstat() on its own mount points
 * (/app0, /download0) with EPERM, so every open fails. For exactly that case,
 * answer "a plain directory" - which is what those mount points are.
 */
int SandboxLstat(const char* path, struct stat* st)
{
  if (lstat(path, st) == 0)
    return 0;
  if (errno != EPERM)
    return -1;
  if (stat(path, st) == 0)
    return 0;
  std::memset(st, 0, sizeof(*st));
  st->st_mode = S_IFDIR | 0755;
  errno = 0;
  return 0;
}

// The console filesystems report st_nlink == 0 for every file, which SQLite
// reads as "database file was deleted" and warns about on every open.
int NlinkFstat(int fd, struct stat* st)
{
  const int r = fstat(fd, st);
  if (r == 0 && st->st_nlink == 0)
    st->st_nlink = 1;
  return r;
}

void InstallSqliteSandboxFix()
{
  // The syscall table is shared by all unix* VFSes, so patch it once.
  auto* vfs = reinterpret_cast<Sqlite3VfsV3*>(sqlite3_vfs_find("unix"));
  if (!vfs || vfs->iVersion < 3 || !vfs->xSetSystemCall)
  {
    Klog("[kodi-ps5] sqlite: unix VFS without syscall hooks - sandbox fix not installed\n");
    return;
  }
  const int rc = vfs->xSetSystemCall(reinterpret_cast<sqlite3_vfs*>(vfs), "lstat",
                                     reinterpret_cast<sqlite3_syscall_ptr>(&SandboxLstat));
  const int rc2 = vfs->xSetSystemCall(reinterpret_cast<sqlite3_vfs*>(vfs), "fstat",
                                      reinterpret_cast<sqlite3_syscall_ptr>(&NlinkFstat));
  Klogf("[kodi-ps5] sqlite: sandbox lstat fix %s, nlink fix %s\n",
        rc == SQLITE_OK ? "installed" : "FAILED", rc2 == SQLITE_OK ? "installed" : "FAILED");
}

// BEGIN-FS-HELPERS
/*
 * Files the title creates belong to the title's own account, and with the
 * usual 0755/0644 modes other accounts - e.g. the FTP server - can read but
 * not delete them. Kodi's data is therefore kept world-writable.
 */
// Returns how many chmod() calls failed.
int OpenUpTree(const std::string& path)
{
  struct stat st;
  if (lstat(path.c_str(), &st) != 0)
    return 0;
  int failed = 0;
  if (S_ISDIR(st.st_mode))
  {
    if (chmod(path.c_str(), 0777) != 0)
      ++failed;
    DIR* dir = opendir(path.c_str());
    if (!dir)
      return failed;
    while (struct dirent* e = readdir(dir))
    {
      if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
        continue;
      failed += OpenUpTree(path + "/" + e->d_name);
    }
    closedir(dir);
  }
  else if (S_ISREG(st.st_mode) && chmod(path.c_str(), 0666) != 0)
    ++failed;
  return failed;
}

// Delete a directory tree (the reset switch).
int RemoveTree(const std::string& path)
{
  struct stat st;
  if (lstat(path.c_str(), &st) != 0)
    return 0;
  int removed = 0;
  if (S_ISDIR(st.st_mode))
  {
    if (DIR* dir = opendir(path.c_str()))
    {
      while (struct dirent* e = readdir(dir))
      {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
          continue;
        removed += RemoveTree(path + "/" + e->d_name);
      }
      closedir(dir);
    }
    if (rmdir(path.c_str()) == 0)
      ++removed;
  }
  else if (unlink(path.c_str()) == 0)
    ++removed;
  return removed;
}
// Recursively copy src -> dst (files, dirs, symlinks). Returns false on the
// first failure, leaving a partial dst (the caller treats that as "retry next
// run" via the migration marker, never as a finished home).
bool CopyTree(const std::string& src, const std::string& dst)
{
  struct stat st;
  if (lstat(src.c_str(), &st) != 0)
    return false;
  if (S_ISLNK(st.st_mode))
  {
    char target[1024];
    const ssize_t n = readlink(src.c_str(), target, sizeof target - 1);
    if (n < 0)
      return false;
    target[n] = '\0';
    unlink(dst.c_str());
    return symlink(target, dst.c_str()) == 0;
  }
  if (S_ISDIR(st.st_mode))
  {
    if (mkdir(dst.c_str(), 0777) != 0 && errno != EEXIST)
      return false;
    DIR* dir = opendir(src.c_str());
    if (!dir)
      return false;
    bool ok = true;
    while (struct dirent* e = readdir(dir))
    {
      if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
        continue;
      if (!CopyTree(src + "/" + e->d_name, dst + "/" + e->d_name))
      {
        ok = false;
        break;
      }
    }
    closedir(dir);
    return ok;
  }
  if (!S_ISREG(st.st_mode))
    return true; // skip sockets/devices; nothing Kodi writes
  const int in = open(src.c_str(), O_RDONLY);
  if (in < 0)
    return false;
  const int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (out < 0)
  {
    close(in);
    return false;
  }
  char buf[65536];
  ssize_t r;
  bool ok = true;
  while ((r = read(in, buf, sizeof buf)) > 0)
  {
    ssize_t off = 0;
    while (off < r)
    {
      const ssize_t w = write(out, buf + off, static_cast<size_t>(r - off));
      if (w <= 0)
      {
        ok = false;
        break;
      }
      off += w;
    }
    if (!ok)
      break;
  }
  if (r < 0)
    ok = false;
  close(in);
  close(out);
  return ok;
}
// END-FS-HELPERS

bool MakeDirs(const std::string& path)
{
  for (size_t pos = 1; pos <= path.size(); ++pos)
  {
    if (pos != path.size() && path[pos] != '/')
      continue;
    const std::string part = path.substr(0, pos);
    if (mkdir(part.c_str(), 0777) != 0 && errno != EEXIST)
    {
      Klogf("[kodi-ps5]   mkdir %s: errno %d (%s)\n", part.c_str(), errno, std::strerror(errno));
      return false;
    }
  }
  return true;
}

/*
 * Can Kodi live here? Create $home/.kodi/userdata/Database, write a file and
 * create a real SQLite database (Kodi's first action that needs the disk).
 */
bool ProbeHome(const std::string& home)
{
  Klogf("[kodi-ps5] probing %s\n", home.c_str());
  const std::string dir = home + "/.kodi/userdata/Database";
  if (!MakeDirs(dir))
    return false;

  const std::string file = home + "/.kodi/temp-probe.txt";
  const int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
  {
    Klogf("[kodi-ps5]   open %s: errno %d (%s)\n", file.c_str(), errno, std::strerror(errno));
    return false;
  }
  const ssize_t written = write(fd, "ok\n", 3);
  close(fd);
  unlink(file.c_str());
  if (written != 3)
  {
    Klogf("[kodi-ps5]   write: errno %d (%s)\n", errno, std::strerror(errno));
    return false;
  }

  // SQLite: try its default file access first, then the alternatives that
  // avoid POSIX advisory locks. The first that works becomes the default for
  // all of Kodi (single process, so OS-level locking is not needed).
  const std::string db = dir + "/probe.db";
  static const char* const vfsNames[] = {"unix", "unix-none", "unix-dotfile", "unix-excl"};
  bool sqliteOk = false;
  for (const char* vfs : vfsNames)
  {
    unlink(db.c_str());
    unlink((db + "-journal").c_str());
    rmdir((db + ".lock").c_str());
    sqlite3* handle = nullptr;
    int rc = sqlite3_open_v2(db.c_str(), &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs);
    if (rc == SQLITE_OK)
      rc = sqlite3_exec(handle, "CREATE TABLE IF NOT EXISTS t(x); INSERT INTO t VALUES(1);", nullptr,
                        nullptr, nullptr);
    if (rc == SQLITE_OK)
    {
      Klogf("[kodi-ps5]   sqlite vfs '%s' works\n", vfs);
      if (std::strcmp(vfs, "unix") != 0)
      {
        sqlite3_vfs_register(sqlite3_vfs_find(vfs), 1);
        Klogf("[kodi-ps5]   made sqlite vfs '%s' the default\n", vfs);
      }
      sqliteOk = true;
    }
    else
      Klogf("[kodi-ps5]   sqlite vfs '%s': rc %d/%d (%s), errno %d (%s)\n", vfs, rc,
            handle ? sqlite3_extended_errcode(handle) : -1,
            handle ? sqlite3_errmsg(handle) : "no handle", errno, std::strerror(errno));
    sqlite3_close(handle);
    unlink(db.c_str());
    unlink((db + "-journal").c_str());
    if (sqliteOk)
      break;
  }
  if (!sqliteOk)
    return false;

  Klogf("[kodi-ps5]   %s is usable\n", home.c_str());
  return true;
}
/*
 * One-time move of an earlier /download0/.kodi install into the /data home.
 * Safe by construction:
 *   - runs only when dst has no settings yet AND src has a real install;
 *   - copies, verifies, writes a .migrated marker, THEN deletes the source. A
 *     failure leaves the marker absent, so a partial dst is redone next run
 *     rather than trusted;
 *   - with the marker present it never runs again, so it cannot overwrite the
 *     live /data home with a stale /download0.
 */
void MigrateHome(const std::string& srcHome, const std::string& dstHome)
{
  const std::string src = srcHome + "/.kodi";
  const std::string dst = dstHome + "/.kodi";
  const std::string marker = dst + "/.migrated-from-download0";
  struct stat st;
  if (lstat(marker.c_str(), &st) == 0)
    return; // already migrated once
  // dst already a real home: leave it, just mark done so we never reconsider.
  if (lstat((dst + "/userdata/guisettings.xml").c_str(), &st) == 0)
  {
    const int fd = open(marker.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
      close(fd);
    return;
  }
  // Nothing to migrate unless the old location holds a real install.
  if (lstat((src + "/userdata/guisettings.xml").c_str(), &st) != 0)
    return;
  Klogf("[kodi-ps5] migrating %s -> %s\n", src.c_str(), dst.c_str());
  MakeDirs(dst); // ensure the destination tree exists before copying into it
  if (!CopyTree(src, dst))
  {
    Klogf("[kodi-ps5]   migration copy failed; keeping %s, will retry next run\n", src.c_str());
    return;
  }
  if (lstat((dst + "/userdata/guisettings.xml").c_str(), &st) != 0)
  {
    Klogf("[kodi-ps5]   migration incomplete (dst guisettings.xml missing); will retry\n");
    return;
  }
  const int fd = open(marker.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0)
    close(fd);
  const int removed = RemoveTree(src);
  Klogf("[kodi-ps5]   migration complete; removed %d files/folders of %s\n", removed, src.c_str());
}
} // namespace

// Switch files in the title folder. access() is refused inside the title
// sandbox (the switches were never seen with it), while stat() works on the
// same paths, so presence is tested with stat().
static bool SwitchPresent(const char* path)
{
  struct stat st;
  if (stat(path, &st) == 0)
    return true;
  if (errno != ENOENT)
    Klogf("[kodi-ps5] switch check %s: errno %d (%s)\n", path, errno, strerror(errno));
  return false;
}

int main(int argc, char* argv[])
{
  // Written straight to klog: visible even if everything after this fails.
  Klog("[kodi-ps5] main() reached\n");
  Klogf("[kodi-ps5] build %s\n", KODI_PS5_BUILD_STAMP);

  struct sigaction signalHandler;
  std::memset(&signalHandler, 0, sizeof(signalHandler));
  signalHandler.sa_handler = &XBMC_PS5_HandleSignal;
  signalHandler.sa_flags = SA_RESTART;
  sigaction(SIGINT, &signalHandler, nullptr);
  sigaction(SIGTERM, &signalHandler, nullptr);

  setlocale(LC_NUMERIC, "C");

  // Title layout: the application image is mounted read-only at /app0 and the
  // title's writable area is /download0 (requires downloadDataSize > 0 in
  // sce_sys/param.json). Kodi keeps everything mutable under $HOME/.kodi.
  // Both default with overwrite=0 so a loader-provided environment wins, which
  // lets you run a development copy from e.g. /data/kodi.
  sqlite3_config(SQLITE_CONFIG_LOG, &SqliteLog, nullptr);
  InstallSqliteSandboxFix();

  // Everything Kodi creates is made deletable over FTP by OpenUpTree()
  // below (at start) and before exit; umask() itself is a no-op on a title.

  // Switches, created as empty files in the title folder over FTP
  // (/data/homebrew/<TITLE_ID>/...). Files the title created cannot be
  // deleted from outside its sandbox (FTP gets "permission denied" whatever
  // their mode), so the title removes its own data:
  //   kodi-reset      wipe Kodi's data, then start Kodi fresh
  //   kodi-uninstall  wipe Kodi's data and quit; afterwards the whole title
  //                   folder can be deleted over FTP
  // Kodi's data lives in its save data, /download0/.kodi (see the HOME choice
  // below); these wipe that, not the read-only /app0 image.
  auto wipeAllHomes = []() {
    // Clear .kodi from every place a home can be: the title's save data and
    // both /data per-version homes (whichever this install actually used).
    return RemoveTree("/download0/.kodi") + RemoveTree("/data/ps5mc/kodi22/.kodi") +
           RemoveTree("/data/ps5mc/kodi21/.kodi");
  };
  if (SwitchPresent("/app0/kodi-uninstall"))
  {
    const int n = wipeAllHomes();
    unlink("/app0/kodi-uninstall");
    Klogf("[kodi-ps5] kodi-uninstall found: removed %d files and folders of Kodi's data, "
          "quitting\n", n);
    _exit(0);
  }
  if (SwitchPresent("/app0/kodi-reset"))
  {
    const int n = wipeAllHomes();
    unlink("/app0/kodi-reset");
    Klogf("[kodi-ps5] kodi-reset found: removed %d files and folders of Kodi's data\n", n);
  }

  // kodi-jitprobe: one-shot diagnostic. Tests whether this title can obtain
  // executable memory (the prerequisite for an in-process binary-add-on
  // loader) and logs the verdict over klog, then removes its own switch and
  // continues starting Kodi normally. See platform/ps5/JitProbe.cpp.
  if (SwitchPresent("/app0/kodi-jitprobe"))
  {
    XBMC_PS5_RunJitProbe();
    unlink("/app0/kodi-jitprobe");
  }

  // Kodi writes everything under $HOME/.kodi, and the only place a title may
  // write is its own save-data area, /download0 (a private read-write image
  // sized by downloadDataSize in sce_sys/param.json). /app0 is the read-only
  // application image and is never a write target: some loaders leave it
  // loosely mounted so small writes seem to work, but real writes (an add-on
  // download) then hang, so it is not even a fallback. /data is the shared
  // homebrew filesystem, reachable only after the jailbreak daemon opens the
  // sandbox and useful for a development copy; it is opt-in through
  // kodi-home-data rather than chosen silently, so the shipped title always
  // keeps its state in its own save data.
  //
  // HOME selection only happens if HOME is unset; the environment below
  // (PYTHONHOME, TMPDIR, SSL_CERT_FILE) is derived from the home dir and must
  // be applied on every launch, whether or not we set HOME this time - some
  // loaders start the title with HOME already set, and Python add-ons, temp
  // files and Python https all depend on this env regardless.
  std::string chosen;
  if (const char* existing = std::getenv("HOME"))
  {
    chosen = existing;
    Klogf("[kodi-ps5] HOME already set to %s\n", chosen.c_str());
  }
  else
  {
    // Prefer /data, which ShadowMountPlus 1.7beta4+ mounts into the sandbox
    // (no Lapy/etaHEN daemon needed): a per-version home so the Kodi 22 and 21
    // builds keep separate libraries. Fall back silently to the title's own
    // save data (/download0) when /data is not writable - an older loader, or
    // SM+ not yet up. kodi-home-download0 forces the fallback for testing.
#if PS5_KODI_MAJOR >= 22
    const char* kDataHome = "/data/ps5mc/kodi22";
#else
    const char* kDataHome = "/data/ps5mc/kodi21";
#endif
    const bool forceDownload0 = SwitchPresent("/app0/kodi-home-download0");
    if (!forceDownload0 && MakeDirs(kDataHome) && ProbeHome(kDataHome))
    {
      chosen = kDataHome;
      MigrateHome("/download0", chosen); // one-time, from an earlier /download0 install
    }
    else
    {
      chosen = "/download0";
      if (!ProbeHome(chosen.c_str()))
        Klogf("[kodi-ps5] %s is not writable; Kodi will fail to start (is downloadDataSize set in "
              "param.json?)\n", chosen.c_str());
    }
    Klogf("[kodi-ps5] HOME=%s\n", chosen.c_str());
    setenv("HOME", chosen.c_str(), 1);
  }
  {
    // Python add-ons: the standard library ships in the title (Install.cmake)
    struct stat pythonLib;
    if (stat("/app0/share/kodi/python/lib/python3.14", &pythonLib) == 0)
    {
      setenv("PYTHONHOME", "/app0/share/kodi/python", 1);
      setenv("PYTHONNOUSERSITE", "1", 1);
      Klog("[kodi-ps5] PYTHONHOME=/app0/share/kodi/python\n");
    }
    // Python's tempfile (and anything else honoring TMPDIR) needs a writable
    // temp directory; a title has no /tmp. Kodi's own special://temp lives in
    // the same place, created here so it exists before its first use.
    const std::string tmpDir = chosen + "/.kodi/temp";
    MakeDirs(tmpDir);
    setenv("TMPDIR", tmpDir.c_str(), 1);
    // OpenSSL's compiled-in default verify paths point nowhere in a title, so
    // Python's ssl module (urllib & co in add-ons) would fail every https
    // certificate check. Point the defaults at the CA bundle Kodi ships;
    // Kodi's own curl passes its CAINFO explicitly and is unaffected.
    struct stat caBundle;
    if (stat("/app0/share/kodi/system/certs/cacert.pem", &caBundle) == 0)
      setenv("SSL_CERT_FILE", "/app0/share/kodi/system/certs/cacert.pem", 1);
    else
      Klog("[kodi-ps5] no cacert.pem in the title: Python https will fail verification\n");
  }
  // A marker in the save data, so the folder is identifiable as Kodi's on disk
  // (over FTP) as well as in the console's data manager.
  {
    const std::string marker = std::string(std::getenv("HOME")) + "/sce_sys/keystone-note.txt";
    MakeDirs(std::string(std::getenv("HOME")) + "/sce_sys");
    const int fd = open(marker.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
    {
      const char* note = "Kodi for PlayStation 5 (PPSA99420) save data.\n";
      const ssize_t n = write(fd, note, std::strlen(note));
      (void)n;
      close(fd);
    }
  }

  const std::string kodiData = std::string(std::getenv("HOME")) + "/.kodi";
  if (const int failed = OpenUpTree(kodiData)) // files left by earlier runs
    Klogf("[kodi-ps5] could not open up permissions of %d items in %s\n", failed,
          kodiData.c_str());
  setenv("KODI_HOME", "/app0/share/kodi", 0);

  // Directory listing check (Kodi's add-on scan starts with this folder).
  {
    const char* path = "/app0/share/kodi/addons";
    struct stat st;
    std::memset(&st, 0, sizeof(st));
    const int sr = stat(path, &st);
    Klogf("[kodi-ps5] stat %s: %d (errno %d) mode %o nlink %u size %lld\n", path, sr,
          sr ? errno : 0, static_cast<unsigned>(st.st_mode), static_cast<unsigned>(st.st_nlink),
          static_cast<long long>(st.st_size));
    const char* known = "/app0/share/kodi/addons/skin.estuary/addon.xml";
    const int kr = stat(known, &st);
    Klogf("[kodi-ps5] stat %s: %d (errno %d) size %lld\n", known, kr, kr ? errno : 0,
          static_cast<long long>(st.st_size));

    const int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
      Klogf("[kodi-ps5] open dir: errno %d (%s)\n", errno, std::strerror(errno));
    else
    {
      static char buf[64 * 1024];
      errno = 0;
      const int n1 = getdents(fd, buf, sizeof(buf));
      Klogf("[kodi-ps5] getdents -> %d (errno %d %s)\n", n1, n1 < 0 ? errno : 0,
            n1 < 0 ? std::strerror(errno) : "");
      if (n1 > 0)
      {
        const struct dirent* e = reinterpret_cast<const struct dirent*>(buf);
        Klogf("[kodi-ps5]   first record: fileno %u reclen %u type %u namlen %u name '%.*s'\n",
              e->d_fileno, e->d_reclen, e->d_type, e->d_namlen, e->d_namlen, e->d_name);
      }
      lseek(fd, 0, SEEK_SET);
      long base = 0;
      errno = 0;
      const int n2 = getdirentries(fd, buf, sizeof(buf), &base);
      Klogf("[kodi-ps5] getdirentries -> %d (errno %d %s)\n", n2, n2 < 0 ? errno : 0,
            n2 < 0 ? std::strerror(errno) : "");
      close(fd);
    }

    errno = 0;
    DIR* dir = opendir(path);
    if (!dir)
      Klogf("[kodi-ps5] opendir %s: errno %d (%s)\n", path, errno, std::strerror(errno));
    else
    {
      int entries = 0;
      errno = 0;
      while (readdir(dir))
        ++entries;
      const int err = errno;
      closedir(dir);
      Klogf("[kodi-ps5] %s lists %d entries (errno %d)\n", path, entries, err);
    }
  }

  // Logging goes to kodi.log in $HOME/.kodi/temp and, via the platform sink,
  // to klog. Debug-level logging (slow: every line goes through the kernel)
  // only with an empty file "kodi-debug" in the title folder.
  std::vector<char*> args(argv, argv + argc);
  static char debugFlag[] = "--debug";

  // Switches that stay in place (unlike reset/uninstall) are read here, once,
  // and each found switch sets an environment variable that the rest of the
  // port (the hardware decoder) reads.
  static const struct
  {
    const char* file;
    const char* env;
  } kSwitches[] = {
      {"/app0/kodi-debug", "KODI_PS5_DEBUG"},
  };
  for (const auto& sw : kSwitches)
  {
    if (SwitchPresent(sw.file))
    {
      setenv(sw.env, "1", 1);
      Klogf("[kodi-ps5] switch %s found (%s=1)\n", sw.file + 6, sw.env);
    }
  }
  if (getenv("KODI_PS5_DEBUG"))
  {
    args.push_back(debugFlag);
    Klog("[kodi-ps5] kodi-debug: debug logging on\n");
  }

  CAppParamParser appParamParser;
  appParamParser.Parse(args.data(), static_cast<int>(args.size()));

  Klog("[kodi-ps5] setting up the app environment\n");
  CAppEnvironment::SetUp(appParamParser.GetAppParams());
  Klog("[kodi-ps5] starting XBMC_Run\n");
  const int status = XBMC_Run(true);
  char msg[96];
  std::snprintf(msg, sizeof(msg), "[kodi-ps5] XBMC_Run returned %d\n", status);
  Klog(msg);
  CAppEnvironment::TearDown();
  OpenUpTree(kodiData); // files created during this run (best effort)
  Klog("[kodi-ps5] exiting\n");
  // Leave immediately: returning from main in a title does not always end the
  // process, which leaves the launch screen up with nothing behind it.
  _exit(status);
}
