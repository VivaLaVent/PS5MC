/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

namespace KODI::PLATFORM::PS5
{
// Ask a resident jailbreak daemon (PS5-Lapy-JB-Daemon, etaHEN) to open Kodi's
// sandbox, so /data and USB drives (/mnt/usbN, /mnt/extN) become reachable:
// the file-drop protocol - {"PID":"<pid>"} written to
// /download0/etahen_jailbreak, which the daemon consumes. In the background;
// logs the outcome. Does nothing without a daemon.
// Call after the system modules Kodi needs later are loaded: once the sandbox
// is open, loading a system module fails.
void RequestSandboxOpen();
bool IsSandboxOpen();
} // namespace KODI::PLATFORM::PS5
