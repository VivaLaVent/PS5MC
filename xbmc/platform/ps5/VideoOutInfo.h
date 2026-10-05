/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <string>

namespace KODI::PLATFORM::PS5
{
// Video output access for the window system, through the GL driver's video
// out handle (exported by our ps5-opengl addition; -1 until the driver opens
// the output with the first presented frame).
int VideoOutHandle();

// One line to the log: system output size and refresh rate. Returns false
// before the output exists.
bool LogVideoOutInfo();

// The system's output size (false if unknown).
bool QuerySystemResolution(unsigned& width, unsigned& height);

// Current output refresh rate in Hz (0 if unknown).
float QueryRefreshRate();

// Vblank counter and the process time (microseconds) of the latest vblank.
bool QueryVblank(uint64_t& count, uint64_t& processTimeUs);

// Output presets a title may request (sceVideoOutConfigureOutput): the
// system's own mode, and the high-refresh preset that the PS5 turns into
// VRR (pegged at 120 Hz) when its VRR setting is on.
constexpr uint32_t kOutputModeDefault = 1;
constexpr uint32_t kOutputModeHighRefresh = 15;

// Whether the system offers the high-refresh preset to this title.
bool IsHighRefreshSupported();

// Switch the output preset; waits for the output to settle. 0 on success.
int SetOutputMode(uint32_t mode);

// sceVideoOutVrrUnpegFromFixedRate is linked through our extended video out
// stub (the SDK's lacks it); logs that once and returns true.
bool IsVrrUnpegAvailable();

// Release a VRR output from its fixed 120 Hz peg, so the display follows the
// title's presentation. 0 on success; negative if unavailable.
int VrrUnpegFromFixedRate();

// Scanout buffer formats (as ProsperoLight registers them): the driver's SDR
// 8:8:8:8 format and the HDR 10-bit BT.2020 PQ 2:10:10:10 one.
constexpr uint64_t kScanoutFormatSdr = UINT64_C(0x8000000000000000);
constexpr uint64_t kScanoutFormatHdr = UINT64_C(0x8100070422000000);
// Switch the scanout buffers to `format` (GL driver addition): in place
// first (SubmitChangeBufferAttribute2), else unregister/register, else restore.
// results[0..3] = change, unregister, register, restore (0x7fffffff: not
// attempted). 0 if the format is in effect; -1 if the driver lacks the addition.
int SetScanoutFormat(uint64_t format, int32_t results[4]);
bool ScanoutFormatSwitchAvailable(); // this build of the GL driver has the addition
// Whether the display link currently runs in HDR (the PS5's HDR setting and
// the TV): sceVideoOutGetOutputStatus, layout as verified by EVO Player /
// SharpProspero. Logs the first reading and every change.
bool IsDisplayHdr();
std::string DescribeScanoutResults(const int32_t results[4]);
} // namespace KODI::PLATFORM::PS5
