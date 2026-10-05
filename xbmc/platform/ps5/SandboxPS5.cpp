/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SandboxPS5.h"

#include "utils/log.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
constexpr const char* kRequest = "/download0/etahen_jailbreak";

bool DropRequest()
{
  const int fd = open(kRequest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
  {
    CLog::Log(LOGWARNING, "PS5 sandbox: cannot write {} (errno {})", kRequest, errno);
    return false;
  }
  const std::string body = "{\"PID\":\"" + std::to_string(getpid()) + "\"}";
  const ssize_t written = write(fd, body.data(), body.size());
  close(fd);
  return written == static_cast<ssize_t>(body.size());
}

// one request: drop it, wait up to `tenths` x 100 ms for the sandbox to open
bool Attempt(int tenths, bool& consumed)
{
  consumed = false;
  if (!DropRequest())
    return false;
  for (int i = 0; i < tenths; ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    struct stat st;
    if (!consumed && stat(kRequest, &st) != 0)
      consumed = true; // the daemon unlinks the file when it acts
    if (KODI::PLATFORM::PS5::IsSandboxOpen())
      return true;
  }
  return false;
}

void Run()
{
  if (KODI::PLATFORM::PS5::IsSandboxOpen())
  {
    CLog::Log(LOGINFO, "PS5 sandbox: already open (/data is writable)");
    return;
  }
  bool consumed = false;
  bool opened = Attempt(25, consumed);
  if (!opened && consumed)
    opened = Attempt(25, consumed); // a first attempt can lose a timing race
  if (!consumed)
    unlink(kRequest); // no daemon: leave nothing behind for a later one
  if (opened)
    CLog::Log(LOGINFO, "PS5 sandbox: opened by the jailbreak daemon - /data and USB drives "
              "(/mnt/usbN) are reachable");
  else
    CLog::Log(LOGINFO, "PS5 sandbox: stays closed ({})",
              consumed ? "the daemon took the request, but /data did not open"
                       : "no jailbreak daemon answered");
}
} // namespace

bool KODI::PLATFORM::PS5::IsSandboxOpen()
{
  // What the open sandbox gives us is a usable /data: under some loaders it is
  // missing from a jailed title, under others present but not writable
  // (mkdir: EACCES). So the test is creating something there.
  if (mkdir("/data/.kodi-sandbox-probe", 0755) == 0)
  {
    rmdir("/data/.kodi-sandbox-probe");
    return true;
  }
  return errno == EEXIST;
}

void KODI::PLATFORM::PS5::RequestSandboxOpen()
{
  std::thread(Run).detach();
}
