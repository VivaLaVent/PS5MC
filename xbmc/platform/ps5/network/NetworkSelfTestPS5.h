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
// kodi-debug: once, 10 seconds after start-up and in the background, check
// each piece Kodi's HTTP(S) access depends on - curl's build, OpenSSL's
// entropy, the socketpair curl uses internally, name resolution - and then
// make one plain and one TLS request, logging every result.
void StartNetworkSelfTest();
} // namespace KODI::PLATFORM::PS5
