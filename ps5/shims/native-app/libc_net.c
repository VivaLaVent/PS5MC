/*
 *  Name resolution for a native title. The SDK binds getaddrinfo & co to
 *  libScePosixForWebKit, which titles do not get, so implement them on
 *  Sony's resolver in libSceNet (loaded in every title): numeric IPv4/IPv6
 *  literals directly, host names via sceNetResolverStartNtoa (IPv4 A records).
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

int sceNetPoolCreate(const char* name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceNetResolverCreate(const char* name, int pool, int flags);
int sceNetResolverDestroy(int resolver);
int sceNetResolverStartNtoa(int resolver, const char* host, uint32_t* address, int timeout_us,
                            int retries, int flags);

static int resolve_ipv4(const char* host, struct in_addr* out)
{
  int pool = sceNetPoolCreate("kodi-dns", 0x4000, 0);
  if (pool < 0)
    return EAI_MEMORY;
  int resolver = sceNetResolverCreate("kodi-dns", pool, 0);
  if (resolver < 0)
  {
    sceNetPoolDestroy(pool);
    return EAI_FAIL;
  }
  uint32_t addr = 0;
  int r = sceNetResolverStartNtoa(resolver, host, &addr, 5 * 1000 * 1000, 2, 0);
  sceNetResolverDestroy(resolver);
  sceNetPoolDestroy(pool);
  if (r < 0 || addr == 0)
    return EAI_NONAME;
  out->s_addr = addr; /* network byte order */
  return 0;
}

static int parse_service(const char* service, int socktype, uint16_t* port_be)
{
  *port_be = 0;
  if (!service || !*service)
    return 0;
  char* end = NULL;
  long v = strtol(service, &end, 10);
  if (end && *end == '\0' && v >= 0 && v <= 65535)
  {
    *port_be = htons((uint16_t)v);
    return 0;
  }
  static const struct { const char* name; uint16_t port; } known[] = {
      {"http", 80}, {"https", 443}, {"ftp", 21}, {"ssh", 22}, {"smtp", 25}, {"domain", 53},
      {"nfs", 2049}, {"microsoft-ds", 445}, {"netbios-ssn", 139}, {"rtsp", 554}, {"ntp", 123}};
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
    if (strcmp(service, known[i].name) == 0)
    {
      *port_be = htons(known[i].port);
      return 0;
    }
  (void)socktype;
  return EAI_SERVICE;
}

/* One allocation per entry: addrinfo + sockaddr_storage + canonical name. */
static struct addrinfo* make_entry(int family, const void* addr, uint16_t port_be, int socktype,
                                   int protocol, const char* canon)
{
  size_t clen = canon ? strlen(canon) + 1 : 0;
  struct addrinfo* ai = (struct addrinfo*)calloc(1, sizeof(*ai) + sizeof(struct sockaddr_storage) + clen);
  if (!ai)
    return NULL;
  struct sockaddr_storage* ss = (struct sockaddr_storage*)(ai + 1);
  if (family == AF_INET)
  {
    struct sockaddr_in* sin = (struct sockaddr_in*)ss;
#ifndef __linux__ /* BSD sockaddr length byte (host unit tests use Linux) */
    sin->sin_len = sizeof(*sin);
#endif
    sin->sin_family = AF_INET;
    sin->sin_port = port_be;
    memcpy(&sin->sin_addr, addr, sizeof(sin->sin_addr));
    ai->ai_addrlen = sizeof(*sin);
  }
  else
  {
    struct sockaddr_in6* sin6 = (struct sockaddr_in6*)ss;
#ifndef __linux__
    sin6->sin6_len = sizeof(*sin6);
#endif
    sin6->sin6_family = AF_INET6;
    sin6->sin6_port = port_be;
    memcpy(&sin6->sin6_addr, addr, sizeof(sin6->sin6_addr));
    ai->ai_addrlen = sizeof(*sin6);
  }
  ai->ai_family = family;
  ai->ai_socktype = socktype;
  ai->ai_protocol = protocol;
  ai->ai_addr = (struct sockaddr*)ss;
  if (canon)
  {
    ai->ai_canonname = (char*)(ss + 1);
    memcpy(ai->ai_canonname, canon, clen);
  }
  return ai;
}

void freeaddrinfo(struct addrinfo* ai)
{
  while (ai)
  {
    struct addrinfo* next = ai->ai_next;
    free(ai);
    ai = next;
  }
}

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                struct addrinfo** res)
{
  if (!res)
    return EAI_FAIL;
  *res = NULL;
  if (!node && !service)
    return EAI_NONAME;
  int family = hints ? hints->ai_family : AF_UNSPEC;
  int flags = hints ? hints->ai_flags : 0;
  int socktype = hints ? hints->ai_socktype : 0;
  int protocol = hints ? hints->ai_protocol : 0;
  if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6)
    return EAI_FAMILY;

  uint16_t port = 0;
  int err = parse_service(service, socktype, &port);
  if (err)
    return err;

  unsigned char addr[16];
  int addr_family;
  if (!node)
  {
    /* passive: wildcard; active: loopback */
    addr_family = family == AF_INET6 ? AF_INET6 : AF_INET;
    memset(addr, 0, sizeof(addr));
    if (!(flags & AI_PASSIVE))
    {
      if (addr_family == AF_INET)
      {
        struct in_addr lo = {htonl(INADDR_LOOPBACK)};
        memcpy(addr, &lo, sizeof(lo));
      }
      else
        addr[15] = 1;
    }
  }
  else if (family != AF_INET6 && inet_pton(AF_INET, node, addr) == 1)
    addr_family = AF_INET;
  else if (family != AF_INET && inet_pton(AF_INET6, node, addr) == 1)
    addr_family = AF_INET6;
  else
  {
    if (flags & AI_NUMERICHOST)
      return EAI_NONAME;
    if (family == AF_INET6)
      return EAI_NONAME; /* resolver answers A records only */
    struct in_addr in;
    err = resolve_ipv4(node, &in);
    if (err)
      return err;
    memcpy(addr, &in, sizeof(in));
    addr_family = AF_INET;
  }

  const char* canon = (flags & AI_CANONNAME) && node ? node : NULL;
  static const int types[2][2] = {{SOCK_STREAM, IPPROTO_TCP}, {SOCK_DGRAM, IPPROTO_UDP}};
  struct addrinfo* head = NULL;
  struct addrinfo** tail = &head;
  for (int i = 0; i < 2; ++i)
  {
    if (socktype && socktype != types[i][0])
      continue;
    int proto = protocol ? protocol : types[i][1];
    struct addrinfo* ai = make_entry(addr_family, addr, port, types[i][0], proto, canon);
    if (!ai)
    {
      freeaddrinfo(head);
      return EAI_MEMORY;
    }
    canon = NULL; /* canonical name on the first entry only */
    *tail = ai;
    tail = &ai->ai_next;
  }
  if (!head) /* unusual socktype (e.g. SOCK_RAW): one entry as asked */
  {
    head = make_entry(addr_family, addr, port, socktype, protocol, canon);
    if (!head)
      return EAI_MEMORY;
  }
  *res = head;
  return 0;
}

int getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, size_t hostlen, char* serv,
                size_t servlen, int flags)
{
  /* No reverse DNS: always numeric. */
  if (!sa)
    return EAI_FAIL;
  const void* addr;
  uint16_t port;
  if (sa->sa_family == AF_INET && salen >= sizeof(struct sockaddr_in))
  {
    addr = &((const struct sockaddr_in*)sa)->sin_addr;
    port = ((const struct sockaddr_in*)sa)->sin_port;
  }
  else if (sa->sa_family == AF_INET6 && salen >= sizeof(struct sockaddr_in6))
  {
    addr = &((const struct sockaddr_in6*)sa)->sin6_addr;
    port = ((const struct sockaddr_in6*)sa)->sin6_port;
  }
  else
    return EAI_FAMILY;
  if (host && hostlen)
  {
    if (flags & NI_NAMEREQD)
      return EAI_NONAME;
    if (!inet_ntop(sa->sa_family, addr, host, (socklen_t)hostlen))
      return EAI_OVERFLOW;
  }
  if (serv && servlen)
  {
    if (snprintf(serv, servlen, "%u", (unsigned)ntohs(port)) >= (int)servlen)
      return EAI_OVERFLOW;
  }
  return 0;
}

const char* gai_strerror(int ecode)
{
  switch (ecode)
  {
    case 0: return "Success";
    case EAI_AGAIN: return "Temporary failure in name resolution";
    case EAI_BADFLAGS: return "Invalid value for ai_flags";
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
    case EAI_FAMILY: return "ai_family not supported";
    case EAI_MEMORY: return "Memory allocation failure";
    case EAI_NONAME: return "Name does not resolve";
    case EAI_SERVICE: return "Service not supported";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_OVERFLOW: return "Argument buffer overflow";
    default: return "Unknown name resolution error";
  }
}

/* h_errno is (*__h_errno()) in FreeBSD's <netdb.h>; the function, too, was
   only in the WebKit module. Per-thread, like the real one. */
int* __h_errno(void)
{
  static _Thread_local int value;
  return &value;
}

struct hostent* gethostbyname(const char* name)
{
  static struct in_addr addr;
  static char* addr_list[2];
  static char* aliases[1];
  static char hname[256];
  static struct hostent he;
  if (!name)
    return NULL;
  if (inet_pton(AF_INET, name, &addr) != 1 && resolve_ipv4(name, &addr) != 0)
  {
    h_errno = HOST_NOT_FOUND;
    return NULL;
  }
  snprintf(hname, sizeof(hname), "%s", name);
  addr_list[0] = (char*)&addr;
  addr_list[1] = NULL;
  aliases[0] = NULL;
  he.h_name = hname;
  he.h_aliases = aliases;
  he.h_addrtype = AF_INET;
  he.h_length = sizeof(addr);
  he.h_addr_list = addr_list;
  return &he;
}
