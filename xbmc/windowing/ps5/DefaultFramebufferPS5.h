/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// The framebuffer Kodi's GL code binds as the "default" one: 0 normally; the
// 10-bit intermediate target while HDR output is active (HdrOutputPS5.cpp).
// Used by patch 0013 in cores/VideoPlayer/VideoRenderers/FrameBufferObject.cpp.
namespace KODI::PLATFORM::PS5
{
unsigned int DefaultFramebuffer();
// HDR output active for HLG video: the GL YUV shader converts HLG to PQ
// (patch 0014), since the PS5's HDR scanout is PQ only.
bool HdrOutputConvertsHlg();
}
