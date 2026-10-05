/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "NetworkPS5.h"

#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifdef AF_LINK
#include <net/if_dl.h>
#endif

namespace
{

bool IsUsable(const struct ifaddrs* ifa)
{
  return ifa->ifa_addr && (ifa->ifa_flags & IFF_UP) && !(ifa->ifa_flags & IFF_LOOPBACK);
}

std::string Ipv4ToString(const struct sockaddr* sa)
{
  if (!sa || sa->sa_family != AF_INET)
    return {};
  char buf[INET_ADDRSTRLEN] = {};
  const auto* sin = reinterpret_cast<const struct sockaddr_in*>(sa);
  if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
    return {};
  return buf;
}

// First usable IPv4 address (and netmask) of a non-loopback interface.
bool FindIpv4(std::string& address, std::string& netmask)
{
  struct ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0 || !list)
    return false;

  bool found = false;
  for (const struct ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next)
  {
    if (!IsUsable(ifa) || ifa->ifa_addr->sa_family != AF_INET)
      continue;
    address = Ipv4ToString(ifa->ifa_addr);
    netmask = Ipv4ToString(ifa->ifa_netmask);
    if (!address.empty())
    {
      found = true;
      break;
    }
  }
  freeifaddrs(list);
  return found;
}

} // namespace

std::unique_ptr<CNetworkBase> CNetworkBase::GetNetwork()
{
  return std::make_unique<CNetworkPS5>();
}

bool CNetworkInterfacePS5::IsConnected() const
{
  std::string address, netmask;
  return FindIpv4(address, netmask);
}

std::string CNetworkInterfacePS5::GetMacAddress() const
{
  char raw[6] = {};
  GetMacAddressRaw(raw);
  char buf[18];
  std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                static_cast<unsigned char>(raw[0]), static_cast<unsigned char>(raw[1]),
                static_cast<unsigned char>(raw[2]), static_cast<unsigned char>(raw[3]),
                static_cast<unsigned char>(raw[4]), static_cast<unsigned char>(raw[5]));
  return buf;
}

void CNetworkInterfacePS5::GetMacAddressRaw(char rawMac[6]) const
{
  std::memset(rawMac, 0, 6);
#ifdef AF_LINK
  struct ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0 || !list)
    return;
  for (const struct ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next)
  {
    if (!IsUsable(ifa) || ifa->ifa_addr->sa_family != AF_LINK)
      continue;
    const auto* sdl = reinterpret_cast<const struct sockaddr_dl*>(ifa->ifa_addr);
    if (sdl->sdl_alen == 6)
    {
      std::memcpy(rawMac, LLADDR(sdl), 6);
      break;
    }
  }
  freeifaddrs(list);
#endif
}

std::string CNetworkInterfacePS5::GetCurrentIPAddress() const
{
  std::string address, netmask;
  if (FindIpv4(address, netmask))
    return address;
  return "127.0.0.1";
}

std::string CNetworkInterfacePS5::GetCurrentNetmask() const
{
  std::string address, netmask;
  if (FindIpv4(address, netmask) && !netmask.empty())
    return netmask;
  return "255.0.0.0";
}

std::string CNetworkInterfacePS5::GetCurrentDefaultGateway() const
{
  // Would need a PF_ROUTE walk; not worth it for a settings-screen label.
  return "";
}

CNetworkPS5::CNetworkPS5() : CNetworkBase()
{
  m_interfaces.push_back(&m_iface);
}

CNetworkPS5::~CNetworkPS5() = default;

std::vector<CNetworkInterface*>& CNetworkPS5::GetInterfaceList()
{
  return m_interfaces;
}

bool CNetworkPS5::PingHost(unsigned long host, unsigned int timeout_ms)
{
  // No raw ICMP sockets for an unprivileged title.
  return false;
}

std::vector<std::string> CNetworkPS5::GetNameServers()
{
  return {};
}
