/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "NetworkSelfTestPS5.h"

#include "filesystem/SpecialProtocol.h"
#include "utils/log.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
// Kodi has its own global CURL class (xbmc/URL.h): rename libcurl's type
#define CURL CURL_HANDLE
#include <curl/curl.h>
#undef CURL
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <pthread.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#if __has_include(<sys/sockio.h>)
#include <sys/sockio.h> // SIOCGIFCONF on FreeBSD
#endif
#include <sys/time.h>
#include <unistd.h>

// Sony's socket API: how Python's sockets are made non-blocking here
// (shims/native-app/libc_socket.c).
extern "C" int sceNetSetsockopt(int s, int level, int optname, const void* optval, int optlen);

namespace
{
constexpr const char* kTag = "PS5 network self-test";

std::string Errno()
{
  return "errno " + std::to_string(errno) + " (" + std::strerror(errno) + ")";
}

void Request(const char* url, std::vector<char>& ca)
{
  CURL_HANDLE* easy = curl_easy_init();
  if (!easy)
  {
    CLog::Log(LOGWARNING, "{}: {}: curl_easy_init returned nothing", kTag, url);
    return;
  }
  char error[CURL_ERROR_SIZE] = {};
  curl_easy_setopt(easy, CURLOPT_URL, url);
  curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error);
  if (!ca.empty())
  {
    curl_blob blob{ca.data(), ca.size(), CURL_BLOB_NOCOPY};
    curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &blob);
  }
  const auto start = std::chrono::steady_clock::now();
  const CURLcode rc = curl_easy_perform(easy);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  long status = 0;
  long verify = 0;
  char* ip = nullptr;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_getinfo(easy, CURLINFO_SSL_VERIFYRESULT, &verify);
  curl_easy_getinfo(easy, CURLINFO_PRIMARY_IP, &ip);
  CLog::Log(rc == CURLE_OK ? LOGINFO : LOGWARNING,
            "{}: {}: curl {} ({}){}{}, HTTP {}, peer {}, TLS verify {}, {:.0f} ms", kTag, url,
            static_cast<int>(rc), curl_easy_strerror(rc), error[0] ? ": " : "", error, status,
            ip && *ip ? ip : "-", verify, ms);
  curl_easy_cleanup(easy);
}

// ---- non-blocking sockets -------------------------------------------------
// Decides how 1.3 fixes curl's keep-alive stall (closing an idle connection
// once froze the GUI for ~35 s, see patch 0016). curl makes a socket
// non-blocking with fcntl(F_SETFL, O_NONBLOCK) and, when closing a
// connection, reads it once expecting that read to return at once. If
// O_NONBLOCK has no effect in the title sandbox, that read waits for the
// server. Measured three ways on a connected loopback pair that never
// carries data: a non-blocking recv() fails at once (EAGAIN), a blocking one
// waits until the safety timeout.
struct IdlePair
{
  int client = -1;
  int server = -1;
  int listener = -1;
};

void CloseIdlePair(IdlePair& p)
{
  for (const int fd : {p.client, p.server, p.listener})
    if (fd >= 0)
      close(fd);
  p = IdlePair{};
}

bool MakeIdlePair(IdlePair& p, std::string& error)
{
  p.listener = socket(AF_INET, SOCK_STREAM, 0);
  if (p.listener < 0)
  {
    error = "socket " + Errno();
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(addr);
  if (bind(p.listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      listen(p.listener, 1) != 0 ||
      getsockname(p.listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
  {
    error = "listen on loopback " + Errno();
    return false;
  }
  p.client = socket(AF_INET, SOCK_STREAM, 0);
  if (p.client < 0 || connect(p.client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
  {
    error = "connect on loopback " + Errno();
    return false;
  }
  p.server = accept(p.listener, nullptr, nullptr);
  if (p.server < 0)
  {
    error = "accept " + Errno();
    return false;
  }
  // first safety net: a blocking recv() gives up after 3 s
  timeval timeout{3, 0};
  setsockopt(p.client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  return true;
}

// One recv() on the idle client. "immediate" = failed with EAGAIN within
// 500 ms, i.e. it did not block. Second safety net: a watchdog shuts the pair
// down after 3.5 s, so even a socket ignoring SO_RCVTIMEO cannot hang here.
std::string TimedRecv(const IdlePair& p, int flags, bool& immediate)
{
  std::atomic<bool> finished{false};
  const int client = p.client;
  const int server = p.server;
  std::thread watchdog(
      [&finished, client, server]
      {
        for (int i = 0; i < 35 && !finished; ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!finished)
        {
          shutdown(server, SHUT_RDWR);
          shutdown(client, SHUT_RD);
        }
      });
  char byte = 0;
  const auto start = std::chrono::steady_clock::now();
  const ssize_t n = recv(p.client, &byte, 1, flags);
  const int err = errno;
  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  finished = true;
  watchdog.join();
  immediate = n < 0 && (err == EAGAIN || err == EWOULDBLOCK) && ms < 500;
  std::string text = "recv " + std::to_string(n);
  if (n < 0)
    text += " (errno " + std::to_string(err) + ")";
  return text + " after " + std::to_string(ms) + " ms";
}

void SocketChecks()
{
  bool fcntlWorks = false;
  bool dontwaitWorks = false;
  bool nbioWorks = false;
  std::string error;

  // a) fcntl(F_SETFL, O_NONBLOCK): how curl makes a socket non-blocking
  {
    IdlePair p;
    if (!MakeIdlePair(p, error))
    {
      CLog::Log(LOGWARNING, "{}: sockets: cannot build a loopback pair ({}); checks skipped", kTag,
                error);
      CloseIdlePair(p);
      return;
    }
    const int before = fcntl(p.client, F_GETFL);
    const int setRc = fcntl(p.client, F_SETFL, before | O_NONBLOCK);
    const std::string setText = setRc == 0 ? "ok" : Errno();
    const int after = fcntl(p.client, F_GETFL);
    const std::string recvText = TimedRecv(p, 0, fcntlWorks);
    CLog::Log(LOGINFO, "{}: sockets: fcntl(O_NONBLOCK) {}, flags read back {}, idle {} -> {}", kTag,
              setText, (after >= 0 && (after & O_NONBLOCK)) ? "with O_NONBLOCK" : "WITHOUT O_NONBLOCK",
              recvText, fcntlWorks ? "non-blocking" : "BLOCKED");
    CloseIdlePair(p);
  }

  // b) recv(MSG_DONTWAIT) on a blocking socket: the liujiny/kodi-ps5 curl fix
  {
    IdlePair p;
    if (MakeIdlePair(p, error))
    {
      const std::string recvText = TimedRecv(p, MSG_DONTWAIT, dontwaitWorks);
      CLog::Log(LOGINFO, "{}: sockets: MSG_DONTWAIT on a blocking socket, idle {} -> {}", kTag,
                recvText, dontwaitWorks ? "non-blocking" : "BLOCKED");
    }
    CloseIdlePair(p);
  }

  // c) sceNet SO_NBIO: how Python's sockets are made non-blocking here
  {
    IdlePair p;
    if (MakeIdlePair(p, error))
    {
      const int on = 1;
      const int rc = sceNetSetsockopt(p.client, 0xffff /* SOL_SOCKET */, 0x1200 /* SO_NBIO */, &on,
                                      static_cast<int>(sizeof(on)));
      const std::string recvText = TimedRecv(p, 0, nbioWorks);
      CLog::Log(LOGINFO, "{}: sockets: sceNet SO_NBIO rc {:#x}, idle {} -> {}", kTag,
                static_cast<unsigned>(rc), recvText, nbioWorks ? "non-blocking" : "BLOCKED");
    }
    CloseIdlePair(p);
  }

  // d) interface listing: ioctl(SIOCGIFCONF) is what UPnP (Neptune) used
  // before 1.3, getifaddrs() what it uses now (and what NetworkPS5 uses)
  {
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    char buffer[4096];
    ifconf config{};
    config.ifc_len = sizeof(buffer);
    config.ifc_buf = buffer;
    const int rc = s >= 0 ? ioctl(s, SIOCGIFCONF, &config) : -1;
    const std::string ifconfText =
        rc == 0 ? "ok, " + std::to_string(config.ifc_len) + " bytes" : "REFUSED, " + Errno();
    if (s >= 0)
      close(s);
    int entries = 0;
    int ipv4 = 0;
    ifaddrs* list = nullptr;
    const bool listed = getifaddrs(&list) == 0;
    for (const ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next)
    {
      ++entries;
      if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET)
        ++ipv4;
    }
    if (list)
      freeifaddrs(list);
    CLog::Log(LOGINFO, "{}: interfaces: ioctl(SIOCGIFCONF) {}; getifaddrs {} ({} entries, {} IPv4)",
              kTag, ifconfText, listed ? "ok" : "FAILED", entries, ipv4);
  }

  CLog::Log(LOGINFO, "{}: sockets verdict: {}", kTag,
            fcntlWorks ? "fcntl O_NONBLOCK works - curl's close-time read cannot block; the stall "
                         "is elsewhere (TLS shutdown)"
            : nbioWorks ? "fcntl O_NONBLOCK has NO effect, sceNet SO_NBIO works - fix in the fcntl "
                          "shim (set SO_NBIO too)"
            : dontwaitWorks ? "only MSG_DONTWAIT works - patch the reads that must not block"
                            : "no non-blocking method worked");
}
// ---- end non-blocking sockets ----------------------------------------------

void Run()
{
  // 1. curl's build
  const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
  CLog::Log(LOGINFO, "{}: curl {}, TLS {}, features:{}{}{}{}{}", kTag, v->version,
            v->ssl_version ? v->ssl_version : "none",
            (v->features & CURL_VERSION_SSL) ? " ssl" : " NO-SSL",
            (v->features & CURL_VERSION_ASYNCHDNS) ? " async-dns" : "",
            (v->features & CURL_VERSION_THREADSAFE) ? " threadsafe" : "",
            (v->features & CURL_VERSION_HTTP2) ? " http2" : "",
            (v->features & CURL_VERSION_IPV6) ? " ipv6" : "");

  // 2. OpenSSL: entropy is what its initialisation needs first
  unsigned char random[16];
  const int randomRc = RAND_bytes(random, sizeof(random));
  CLog::Log(randomRc == 1 ? LOGINFO : LOGWARNING, "{}: {}: RAND_status {}, RAND_bytes {}", kTag,
            OpenSSL_version(OPENSSL_VERSION), RAND_status(), randomRc);

  // 3. the socketpair curl's multi interface uses for wake-ups
  int pair[2] = {-1, -1};
  const int pairRc = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
  CLog::Log(pairRc == 0 ? LOGINFO : LOGWARNING, "{}: socketpair(AF_UNIX): {}", kTag,
            pairRc == 0 ? std::string("ok") : std::string("errno ") + std::to_string(errno) +
                                                  " (" + std::strerror(errno) + ")");
  if (pairRc == 0)
  {
    close(pair[0]);
    close(pair[1]);
  }

  // 3b. curl's resolver wake-up, step by step: pipe (through the socketpair
  // fallback), close-on-exec on both ends, pipe2 with O_CLOEXEC, and a thread
  // created the way curl does it (default attributes)
  int fds[2] = {-1, -1};
  const int pipeRc = pipe(fds);
  std::string pipeText = pipeRc == 0 ? "ok" : Errno();
  if (pipeRc == 0)
  {
    const int cloexec0 = fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    const int cloexec1 = fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    pipeText += cloexec0 == 0 && cloexec1 == 0 ? ", FD_CLOEXEC ok" : ", FD_CLOEXEC " + Errno();
    close(fds[0]);
    close(fds[1]);
  }
  const int pipe2Rc = pipe2(fds, O_CLOEXEC);
  pipeText += pipe2Rc == 0 ? ", pipe2 ok" : ", pipe2 " + Errno();
  if (pipe2Rc == 0)
  {
    close(fds[0]);
    close(fds[1]);
  }
  pthread_t thread;
  const int threadRc = pthread_create(&thread, nullptr, [](void*) -> void* { return nullptr; }, nullptr);
  if (threadRc == 0)
    pthread_join(thread, nullptr);
  pipeText += threadRc == 0 ? ", thread ok" : ", thread rc " + std::to_string(threadRc);
  CLog::Log(pipeRc == 0 && pipe2Rc == 0 && threadRc == 0 ? LOGINFO : LOGWARNING,
            "{}: wake-up steps: pipe {}", kTag, pipeText);

  // 4. name resolution
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const int gai = getaddrinfo("mirrors.kodi.tv", "443", &hints, &result);
  char address[INET6_ADDRSTRLEN] = "-";
  if (gai == 0 && result && result->ai_family == AF_INET)
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr, address,
              sizeof(address));
  CLog::Log(gai == 0 ? LOGINFO : LOGWARNING, "{}: getaddrinfo(mirrors.kodi.tv): {} ({}), {}",
            kTag, gai, gai == 0 ? "ok" : gai_strerror(gai), address);
  if (result)
    freeaddrinfo(result);

  // 5. curl's multi interface (the global initialisation is Kodi's: it must
  // not be repeated while other threads may be using curl)
  CURLM* multi = curl_multi_init();
  CLog::Log(multi ? LOGINFO : LOGWARNING, "{}: curl_multi_init {}", kTag,
            multi ? "ok" : "FAILED");
  if (multi)
    curl_multi_cleanup(multi);

  // 6. requests: plain, then TLS verified against Kodi's bundled CA list
  std::vector<char> ca;
  std::ifstream file(CSpecialProtocol::TranslatePath("special://xbmc/system/certs/cacert.pem"),
                     std::ios::binary);
  if (file)
    ca.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  CLog::Log(ca.empty() ? LOGWARNING : LOGINFO, "{}: CA bundle: {} bytes", kTag, ca.size());
  Request("http://mirrors.kodi.tv/", ca);
  Request("https://mirrors.kodi.tv/", ca);

  // 7. non-blocking sockets and interface listing
  SocketChecks();
  CLog::Log(LOGINFO, "{}: done", kTag);
}
} // namespace

void KODI::PLATFORM::PS5::StartNetworkSelfTest()
{
  if (!std::getenv("KODI_PS5_DEBUG"))
    return;
  std::thread(
      []
      {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        Run();
      })
      .detach();
}
