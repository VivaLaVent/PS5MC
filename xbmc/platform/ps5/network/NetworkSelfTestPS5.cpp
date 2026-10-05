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
#include <pthread.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <sys/socket.h>
#include <unistd.h>

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
