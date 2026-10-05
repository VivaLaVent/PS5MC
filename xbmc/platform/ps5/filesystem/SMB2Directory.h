/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "filesystem/IDirectory.h"

namespace XFILE
{

/*!
 * smb:// directories over libsmb2: smb://host/ lists the host's shares,
 * smb://host/share/... lists folders. There is no network-neighbourhood
 * browsing (smb:// alone), so sources are added by host name or IP address.
 */
class CSMB2Directory : public IDirectory
{
public:
  CSMB2Directory() = default;
  ~CSMB2Directory() override = default;

  bool GetDirectory(const CURL& url, CFileItemList& items) override;
  CacheType GetCacheType(const CURL& url) const override { return CacheType::ONCE; }
  bool Create(const CURL& url) override;
  bool Exists(const CURL& url) override;
  bool Remove(const CURL& url) override;
};

} // namespace XFILE
