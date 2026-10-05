/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "network/Network.h"

#include <string>
#include <vector>

class CNetworkInterfacePS5 : public CNetworkInterface
{
public:
  CNetworkInterfacePS5() = default;
  ~CNetworkInterfacePS5() override = default;

  bool IsEnabled() const override { return true; }
  bool IsConnected() const override;
  std::string GetMacAddress() const override;
  void GetMacAddressRaw(char rawMac[6]) const override;
  bool GetHostMacAddress(unsigned long host, std::string& mac) const override { return false; }
  std::string GetCurrentIPAddress() const override;
  std::string GetCurrentNetmask() const override;
  std::string GetCurrentDefaultGateway() const override;
};

/*!
 * \brief Network information for the console.
 *
 * Sockets, DNS and HTTP work through the FreeBSD-derived libc, so Kodi's
 * generic networking is untouched. This class only answers the "what is my
 * address" questions the GUI asks. It uses getifaddrs(); if that symbol turns
 * out to be missing from the console libc at link time, replace the body of
 * FindIpv4() in NetworkPS5.cpp with sceNetCtlGetInfo() (the -lSceNetCtl stub
 * is already on the link line).
 */
class CNetworkPS5 : public CNetworkBase
{
public:
  CNetworkPS5();
  ~CNetworkPS5() override;

  std::vector<CNetworkInterface*>& GetInterfaceList() override;
  bool PingHost(unsigned long host, unsigned int timeout_ms = 2000) override;
  std::vector<std::string> GetNameServers() override;

private:
  CNetworkInterfacePS5 m_iface;
  std::vector<CNetworkInterface*> m_interfaces;
};
