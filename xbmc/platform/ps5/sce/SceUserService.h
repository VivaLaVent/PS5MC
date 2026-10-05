/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*
 * Clean-room declarations for libSceUserService. A controller is bound to a
 * logged-in user, so pads are opened per user id (see input/PS5PadInput.cpp).
 */

#include <cstddef>
#include <cstdint>

extern "C"
{
  int sceUserServiceGetForegroundUser(int32_t* userId);
  int sceUserServiceInitialize(const void* params);
  int sceUserServiceTerminate(void);
  // Fills 4 slots; unused slots are -1.
  int sceUserServiceGetLoginUserIdList(int32_t userIds[4]);
  int sceUserServiceGetUserName(int32_t userId, char* name, size_t size);
}

namespace KODI::PLATFORM::PS5
{
constexpr int USER_SERVICE_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x80960003);
constexpr int USER_SERVICE_MAX_USERS = 4;
} // namespace KODI::PLATFORM::PS5
