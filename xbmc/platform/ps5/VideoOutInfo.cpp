/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoOutInfo.h"

#include "utils/StringUtils.h"
#include "utils/log.h"

#include <cstddef>
#include <cstdint>

namespace
{
// libSceVideoOut resolution status (48 bytes; layout as used on hardware)
struct ResolutionStatus
{
  uint32_t fullWidth, fullHeight, paneWidth, paneHeight;
  uint64_t refreshRate;
  float screenInches;
  uint32_t reserved[4];
};
static_assert(sizeof(ResolutionStatus) == 48 && offsetof(ResolutionStatus, refreshRate) == 16);

// SceVideoOutRefreshRate codes (0x3 is what the system reports at 59.94 Hz)
float RefreshFromId(uint64_t id)
{
  switch (id)
  {
    case 0x1:
      return 23.976f;
    case 0x2:
      return 50.0f;
    case 0x3:
      return 59.94f;
    case 0x4:
      return 29.97f;
    case 0xd:
      return 119.88f;
    case 0x23:
      return 89.91f;
    default:
      return 0.0f;
  }
}
} // namespace

extern "C"
{
int sceVideoOutGetResolutionStatus(int32_t handle, ResolutionStatus* status);
int sceVideoOutIsOutputSupported(int32_t handle, uint32_t mode, const void*, const void*,
                                 const void*);
int sceVideoOutConfigureOutput(int32_t handle, uint32_t mode, const void*, const void*,
                               const void*);
int sceVideoOutWaitVblank(int32_t handle);
int sceVideoOutGetVblankStatus(int32_t handle, void* status);
int ps5_opengl_video_out_handle(void);
// output status: resolution, dynamic range (0 unknown, 1 SDR, 2 HDR), refresh
// rate, flags (bit 0: HDR output active), reserved
struct VideoOutOutputStatus
{
  uint32_t resolution;
  uint32_t dynamicRange;
  uint64_t refreshRate;
  uint64_t flags;
  uint64_t reserved[3];
};
int32_t sceVideoOutGetOutputStatus(int32_t handle, VideoOutOutputStatus* status);
// patches/ps5-opengl/kodi-additions.py (HDR part); weak: absent in older drivers
int ps5_opengl_set_scanout_format(uint64_t pixel_format, int32_t results[4]) __attribute__((weak));
}


int KODI::PLATFORM::PS5::VideoOutHandle()
{
  return ps5_opengl_video_out_handle();
}

bool KODI::PLATFORM::PS5::LogVideoOutInfo()
{
  const int32_t handle = ps5_opengl_video_out_handle();
  if (handle < 0)
    return false;
  ResolutionStatus status{};
  const int rc = sceVideoOutGetResolutionStatus(handle, &status);
  if (rc != 0)
    CLog::Log(LOGWARNING, "PS5 video out: resolution status failed ({:#x})",
              static_cast<uint32_t>(rc));
  else
    CLog::Log(LOGINFO, "PS5 video out: system output {}x{} at {:.3f} Hz (refresh code {:#x})",
              status.fullWidth, status.fullHeight, RefreshFromId(status.refreshRate),
              status.refreshRate);
  return true;
}

bool KODI::PLATFORM::PS5::QuerySystemResolution(unsigned& width, unsigned& height)
{
  const int32_t handle = ps5_opengl_video_out_handle();
  ResolutionStatus status{};
  if (handle < 0 || sceVideoOutGetResolutionStatus(handle, &status) != 0 || !status.fullWidth)
    return false;
  width = status.fullWidth;
  height = status.fullHeight;
  return true;
}

float KODI::PLATFORM::PS5::QueryRefreshRate()
{
  const int32_t handle = ps5_opengl_video_out_handle();
  ResolutionStatus status{};
  if (handle < 0 || sceVideoOutGetResolutionStatus(handle, &status) != 0)
    return 0.0f;
  return RefreshFromId(status.refreshRate);
}

bool KODI::PLATFORM::PS5::QueryVblank(uint64_t& count, uint64_t& processTimeUs)
{
  const int32_t handle = ps5_opengl_video_out_handle();
  if (handle < 0)
    return false;
  // SceVideoOutVblankStatus: count, processTime, tsc, reserved, flags (40
  // bytes); a larger zeroed buffer keeps us safe if the layout grew.
  uint64_t status[8] = {};
  if (sceVideoOutGetVblankStatus(handle, status) != 0)
    return false;
  count = status[0];
  processTimeUs = status[1];
  return true;
}

bool KODI::PLATFORM::PS5::IsHighRefreshSupported()
{
  const int32_t handle = ps5_opengl_video_out_handle();
  return handle >= 0 &&
         sceVideoOutIsOutputSupported(handle, kOutputModeHighRefresh, nullptr, nullptr, nullptr) >
             0;
}

int KODI::PLATFORM::PS5::SetOutputMode(uint32_t mode)
{
  const int32_t handle = ps5_opengl_video_out_handle();
  if (handle < 0)
    return -1;
  const int rc = sceVideoOutConfigureOutput(handle, mode, nullptr, nullptr, nullptr);
  if (rc != 0)
    return rc;
  // as the GL driver does after a mode change: let two vblanks pass
  for (int i = 0; i < 2; ++i)
    if (const int wait = sceVideoOutWaitVblank(handle); wait != 0)
      return wait;
  return 0;
}

extern "C"
{
// VRR: in our libSceVideoOut link stub (scripts/17), not in the SDK's. A
// normal import, as ProsperoLight links it: the PS5 loader left a weak import
// of it empty, although the function exists (ProsperoLight's VRR works on
// the same firmware).
int sceVideoOutVrrUnpegFromFixedRate(int32_t handle);
}

bool KODI::PLATFORM::PS5::IsVrrUnpegAvailable()
{
  static const bool logged = []
  {
    CLog::Log(LOGINFO, "PS5 VRR: sceVideoOutVrrUnpegFromFixedRate linked");
    return true;
  }();
  return logged;
}

int KODI::PLATFORM::PS5::VrrUnpegFromFixedRate()
{
  const int32_t handle = ps5_opengl_video_out_handle();
  if (handle < 0)
    return -1;
  return sceVideoOutVrrUnpegFromFixedRate(handle);
}

int KODI::PLATFORM::PS5::SetScanoutFormat(uint64_t format, int32_t results[4])
{
  for (int i = 0; i < 4; ++i)
    results[i] = 0x7fffffff;
  if (!ps5_opengl_set_scanout_format)
    return -1;
  return ps5_opengl_set_scanout_format(format, results);
}

bool KODI::PLATFORM::PS5::ScanoutFormatSwitchAvailable()
{
  return ps5_opengl_set_scanout_format != nullptr;
}

bool KODI::PLATFORM::PS5::IsDisplayHdr()
{
  static int last = -1;
  const int32_t handle = ps5_opengl_video_out_handle();
  if (handle < 0)
    return false;
  VideoOutOutputStatus status{};
  const int32_t rc = sceVideoOutGetOutputStatus(handle, &status);
  const bool hdr = rc == 0 && (status.dynamicRange == 2 || (status.flags & 1));
  if (static_cast<int>(hdr) != last)
  {
    last = hdr;
    CLog::Log(LOGINFO, "PS5 display: {} (output status {:#x}, dynamic range {}, flags {:#x})",
              hdr ? "HDR" : "SDR", static_cast<uint32_t>(rc), status.dynamicRange, status.flags);
  }
  return hdr;
}

std::string KODI::PLATFORM::PS5::DescribeScanoutResults(const int32_t results[4])
{
  static const char* const names[4] = {"change", "unregister", "register", "restore"};
  std::string text;
  for (int i = 0; i < 4; ++i)
  {
    if (results[i] == 0x7fffffff)
      continue;
    if (!text.empty())
      text += ", ";
    text += names[i];
    text += results[i] == 0 ? " ok" : StringUtils::Format(" {:#x}", static_cast<uint32_t>(results[i]));
  }
  return text.empty() ? "nothing attempted" : text;
}
