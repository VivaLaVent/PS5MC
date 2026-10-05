/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PlatformPS5.h"

#include "filesystem/CurlFile.h"
#include "platform/ps5/input/PS5ImeDialog.h"
#include "platform/ps5/network/NetworkSelfTestPS5.h"
#include "platform/ps5/video/VideoDec2.h"
#include "utils/log.h"

#include "platform/ps5/audio/AESinkPS5.h"
#include "windowing/ps5/WinSystemPS5GLContext.h"

CPlatform* CPlatform::CreateInstance()
{
  return new CPlatformPS5();
}

bool CPlatformPS5::InitStageOne()
{
  if (!CPlatformPosix::InitStageOne())
    return false;

  // Exactly one window system and one audio sink exist on the console.
  KODI::WINDOWING::PS5::CWinSystemPS5GLContext::Register();
  CAESinkPS5::Register();

  // HTTPS: curl's compiled-in CA path is a path on the build machine, so the
  // bundled list goes to curl as a memory blob, as on Kodi's UWP platform
  XFILE::CCurlFile::PreloadCaCertsBlob();
  KODI::PLATFORM::PS5::StartNetworkSelfTest(); // kodi-debug only

  // System modules needed later are loaded now: once the sandbox is opened
  // (after the first frame, see CWinSystemPS5GLContext), loading them fails.
  const int32_t videodec = KODI::PLATFORM::PS5::CVideoDec2::LoadModule();
  if (videodec < 0)
    CLog::Log(LOGWARNING, "CPlatformPS5: video decoder module not loaded ({:#x})",
              static_cast<uint32_t>(videodec));
  KODI::PLATFORM::PS5::CPS5ImeDialog::Preload(); // the native keyboard (logs itself)

  return true;
}
