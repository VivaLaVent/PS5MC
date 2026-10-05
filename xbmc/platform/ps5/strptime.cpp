/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

/*
 * strptime() for the PS5: the console libc declares it (FreeBSD headers) but
 * does not export it. Covers the conversions Kodi uses (RFC 1123 / ISO 8601
 * dates in DAV, scrapers, EPG) plus the common POSIX set.
 */

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace
{

const char* const kMonths[12] = {"january", "february", "march",     "april",   "may",      "june",
                                 "july",    "august",   "september", "october", "november", "december"};
const char* const kDays[7] = {"sunday",   "monday", "tuesday", "wednesday",
                              "thursday", "friday", "saturday"};

const char* MatchName(const char* s, const char* const* names, int count, int* out)
{
  for (int i = 0; i < count; ++i)
  {
    const char* n = names[i];
    size_t len = std::strlen(n);
    // full name
    size_t k = 0;
    while (k < len && s[k] && std::tolower(static_cast<unsigned char>(s[k])) == n[k])
      ++k;
    if (k == len)
    {
      *out = i;
      return s + len;
    }
    // three-letter abbreviation
    if (k >= 3)
    {
      *out = i;
      return s + 3;
    }
  }
  return nullptr;
}

const char* Number(const char* s, int minDigits, int maxDigits, int lo, int hi, int* out)
{
  int v = 0, n = 0;
  while (n < maxDigits && std::isdigit(static_cast<unsigned char>(s[n])))
  {
    v = v * 10 + (s[n] - '0');
    ++n;
  }
  if (n < minDigits || v < lo || v > hi)
    return nullptr;
  *out = v;
  return s + n;
}

const char* Space(const char* s)
{
  while (std::isspace(static_cast<unsigned char>(*s)))
    ++s;
  return s;
}

} // namespace

extern "C" char* strptime(const char* s, const char* fmt, struct tm* tm)
{
  int v = 0;
  int pm = -1; // -1: no %p seen, 0: AM, 1: PM
  int century = -1;
  bool haveYear = false;

  while (*fmt)
  {
    if (std::isspace(static_cast<unsigned char>(*fmt)))
    {
      s = Space(s);
      ++fmt;
      continue;
    }
    if (*fmt != '%')
    {
      if (*s != *fmt)
        return nullptr;
      ++s;
      ++fmt;
      continue;
    }
    ++fmt;
    if (*fmt == 'E' || *fmt == 'O') // POSIX modifiers, no effect here
      ++fmt;

    switch (*fmt++)
    {
      case '%':
        if (*s != '%')
          return nullptr;
        ++s;
        break;
      case 'n':
      case 't':
        s = Space(s);
        break;
      case 'a':
      case 'A':
        s = Space(s);
        if (!(s = MatchName(s, kDays, 7, &v)))
          return nullptr;
        tm->tm_wday = v;
        break;
      case 'b':
      case 'B':
      case 'h':
        s = Space(s);
        if (!(s = MatchName(s, kMonths, 12, &v)))
          return nullptr;
        tm->tm_mon = v;
        break;
      case 'C':
        s = Space(s);
        if (!(s = Number(s, 1, 2, 0, 99, &v)))
          return nullptr;
        century = v;
        break;
      case 'd':
      case 'e':
        s = Space(s);
        if (!(s = Number(s, 1, 2, 1, 31, &v)))
          return nullptr;
        tm->tm_mday = v;
        break;
      case 'D':
        if (!(s = strptime(s, "%m/%d/%y", tm)))
          return nullptr;
        break;
      case 'F':
        if (!(s = strptime(s, "%Y-%m-%d", tm)))
          return nullptr;
        break;
      case 'H':
      case 'k':
        s = Space(s);
        if (!(s = Number(s, 1, 2, 0, 23, &v)))
          return nullptr;
        tm->tm_hour = v;
        break;
      case 'I':
      case 'l':
        s = Space(s);
        if (!(s = Number(s, 1, 2, 1, 12, &v)))
          return nullptr;
        tm->tm_hour = v % 12;
        break;
      case 'j':
        s = Space(s);
        if (!(s = Number(s, 1, 3, 1, 366, &v)))
          return nullptr;
        tm->tm_yday = v - 1;
        break;
      case 'm':
        s = Space(s);
        if (!(s = Number(s, 1, 2, 1, 12, &v)))
          return nullptr;
        tm->tm_mon = v - 1;
        break;
      case 'M':
        s = Space(s);
        if (!(s = Number(s, 1, 2, 0, 59, &v)))
          return nullptr;
        tm->tm_min = v;
        break;
      case 'p':
      {
        s = Space(s);
        char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
        char c1 = static_cast<char>(std::tolower(static_cast<unsigned char>(s[1])));
        if (c1 != 'm' || (c0 != 'a' && c0 != 'p'))
          return nullptr;
        pm = (c0 == 'p');
        s += 2;
        break;
      }
      case 'r':
        if (!(s = strptime(s, "%I:%M:%S %p", tm)))
          return nullptr;
        break;
      case 'R':
        if (!(s = strptime(s, "%H:%M", tm)))
          return nullptr;
        break;
      case 'S':
        s = Space(s);
        if (!(s = Number(s, 1, 2, 0, 61, &v)))
          return nullptr;
        tm->tm_sec = v;
        break;
      case 'T':
        if (!(s = strptime(s, "%H:%M:%S", tm)))
          return nullptr;
        break;
      case 'U':
      case 'W':
      case 'V':
        s = Space(s);
        if (!(s = Number(s, 1, 2, 0, 53, &v)))
          return nullptr;
        break;
      case 'w':
        s = Space(s);
        if (!(s = Number(s, 1, 1, 0, 6, &v)))
          return nullptr;
        tm->tm_wday = v;
        break;
      case 'y':
        s = Space(s);
        if (!(s = Number(s, 1, 2, 0, 99, &v)))
          return nullptr;
        tm->tm_year = (century >= 0 ? century * 100 + v : (v < 69 ? 2000 + v : 1900 + v)) - 1900;
        haveYear = true;
        break;
      case 'Y':
        s = Space(s);
        if (!(s = Number(s, 1, 4, 0, 9999, &v)))
          return nullptr;
        tm->tm_year = v - 1900;
        haveYear = true;
        break;
      case 'z':
      {
        s = Space(s);
        if (*s == 'Z' || *s == 'z')
        {
          ++s;
          break;
        }
        if (*s != '+' && *s != '-')
          return nullptr;
        const char* q = s + 1;
        int hh = 0, mm = 0;
        if (!(q = Number(q, 2, 2, 0, 23, &hh)))
          return nullptr;
        if (*q == ':')
          ++q;
        if (std::isdigit(static_cast<unsigned char>(*q)))
        {
          if (!(q = Number(q, 2, 2, 0, 59, &mm)))
            return nullptr;
        }
        s = q; // offset is accepted but not applied (glibc behaviour: tm_gmtoff)
        break;
      }
      case 'Z':
        s = Space(s);
        while (std::isalpha(static_cast<unsigned char>(*s)))
          ++s;
        break;
      case 'c':
        if (!(s = strptime(s, "%a %b %e %H:%M:%S %Y", tm)))
          return nullptr;
        break;
      case 'x':
        if (!(s = strptime(s, "%m/%d/%y", tm)))
          return nullptr;
        break;
      case 'X':
        if (!(s = strptime(s, "%H:%M:%S", tm)))
          return nullptr;
        break;
      case 's':
      {
        s = Space(s);
        char* end = nullptr;
        long long secs = std::strtoll(s, &end, 10);
        if (end == s)
          return nullptr;
        time_t t = static_cast<time_t>(secs);
        localtime_r(&t, tm);
        s = end;
        break;
      }
      default:
        return nullptr;
    }
  }

  if (pm == 1 && tm->tm_hour < 12)
    tm->tm_hour += 12;
  else if (pm == 0 && tm->tm_hour == 12)
    tm->tm_hour = 0;
  if (!haveYear && century >= 0)
    tm->tm_year = century * 100 - 1900;

  return const_cast<char*>(s);
}
