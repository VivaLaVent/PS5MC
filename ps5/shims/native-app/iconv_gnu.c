/* iconv for a PS5 title.
 *
 * The console's libc exports iconv_open/iconv/iconv_close, but it only knows
 * the Unicode encodings (UTF-8, UTF-16LE/BE, UTF-32LE/BE). Kodi's charset
 * converter is fine with that - the GUI and its own strings are all UTF-8 /
 * UTF-16 - except for one caller: the zip reader. Zip entries without the
 * UTF-8 flag are named in CP437, so iconv_open("UTF-8", "CP437") fails with
 * EINVAL, every file name in an add-on package converts to "", and Kodi
 * rejects the package as broken.
 *
 * Linked with --wrap=iconv_open --wrap=iconv --wrap=iconv_close
 * (scripts/30-deploy.sh), like pthread_create, pipe and fcntl: every call in
 * Kodi and the static libraries lands here first. CP437 -> UTF-8 is converted
 * from the table below; every other pair goes to the system implementation
 * through __real_iconv_open, untouched. (dlsym is not an option in a title:
 * there is no runtime loader, see weak_shims.c.)
 *
 * A descriptor from here is either our own CP437 state or a wrapper around
 * the system's descriptor, so iconv() and iconv_close() can tell them apart. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp, strncasecmp */

/* The system's functions under the names --wrap gives them. iconv_t is a
 * void* on this libc; the prototypes are ours, so no header is involved. */
void* __real_iconv_open(const char* tocode, const char* fromcode);
size_t __real_iconv(void* cd, char** inbuf, size_t* inbytesleft, char** outbuf, size_t* outbytesleft);
int __real_iconv_close(void* cd);

/* CP437 -> Unicode, the high half (0x80-0xFF); 0x00-0x7F is ASCII.
 * Source: the Unicode Consortium's CP437 mapping. */
static const uint16_t cp437_hi[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, /* 80 */
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, /* 88 */
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, /* 90 */
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, /* 98 */
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, /* A0 */
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, /* A8 */
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, /* B0 */
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, /* B8 */
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, /* C0 */
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, /* C8 */
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, /* D0 */
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, /* D8 */
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, /* E0 */
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, /* E8 */
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, /* F0 */
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0, /* F8 */
};

static int cp_to_utf8(uint32_t cp, unsigned char* buf)
{
  if (cp < 0x80)
  {
    buf[0] = (unsigned char)cp;
    return 1;
  }
  if (cp < 0x800)
  {
    buf[0] = 0xC0 | (cp >> 6);
    buf[1] = 0x80 | (cp & 0x3F);
    return 2;
  }
  buf[0] = 0xE0 | (cp >> 12);
  buf[1] = 0x80 | ((cp >> 6) & 0x3F);
  buf[2] = 0x80 | (cp & 0x3F);
  return 3; /* nothing in CP437 is above U+FFFF */
}

enum
{
  CD_CP437_TO_UTF8 = 1, /* converted here */
  CD_SYSTEM = 2, /* forwarded to the system's descriptor */
};

typedef struct
{
  int kind;
  void* sys;
} ps5_iconv_cd;

/* Charset names as Kodi and GNU iconv spell them; a "//TRANSLIT" or
 * "//IGNORE" suffix does not change the encoding. */
static int name_is(const char* name, const char* want)
{
  if (!name)
    return 0;
  size_t n = strlen(want);
  return strncasecmp(name, want, n) == 0 && (name[n] == '\0' || (name[n] == '/' && name[n + 1] == '/'));
}

static int is_cp437(const char* s)
{
  return name_is(s, "CP437") || name_is(s, "IBM437") || name_is(s, "437") || name_is(s, "CSPC8CODEPAGE437");
}

static int is_utf8(const char* s)
{
  return name_is(s, "UTF-8") || name_is(s, "UTF8");
}

void* __wrap_iconv_open(const char* tocode, const char* fromcode)
{
  ps5_iconv_cd* cd = (ps5_iconv_cd*)malloc(sizeof *cd);
  if (!cd)
  {
    errno = ENOMEM;
    return (void*)-1;
  }
  if (is_cp437(fromcode) && is_utf8(tocode))
  {
    cd->kind = CD_CP437_TO_UTF8;
    cd->sys = NULL;
    return cd;
  }
  void* sys = __real_iconv_open(tocode, fromcode);
  if (sys == (void*)-1)
  {
    int saved = errno;
    free(cd);
    errno = saved;
    return (void*)-1;
  }
  cd->kind = CD_SYSTEM;
  cd->sys = sys;
  return cd;
}

size_t __wrap_iconv(void* handle, char** inbuf, size_t* inbytesleft, char** outbuf, size_t* outbytesleft)
{
  ps5_iconv_cd* cd = (ps5_iconv_cd*)handle;
  if (!cd || cd == (ps5_iconv_cd*)-1)
  {
    errno = EBADF;
    return (size_t)-1;
  }
  if (cd->kind == CD_SYSTEM)
    return __real_iconv(cd->sys, inbuf, inbytesleft, outbuf, outbytesleft);

  /* CP437 -> UTF-8. A NULL inbuf (or *inbuf) resets the conversion state;
   * CP437 is stateless, so there is nothing to flush. */
  if (!inbuf || !*inbuf)
    return 0;
  while (*inbytesleft > 0)
  {
    unsigned char b = (unsigned char)**inbuf;
    uint32_t cp = b < 0x80 ? b : cp437_hi[b - 0x80];
    unsigned char tmp[4];
    int n = cp_to_utf8(cp, tmp);
    if ((size_t)n > *outbytesleft)
    {
      errno = E2BIG; /* the pointers already reflect what was converted */
      return (size_t)-1;
    }
    memcpy(*outbuf, tmp, (size_t)n);
    *outbuf += n;
    *outbytesleft -= (size_t)n;
    (*inbuf)++;
    (*inbytesleft)--;
  }
  return 0; /* number of irreversible conversions: every CP437 byte has a code point */
}

int __wrap_iconv_close(void* handle)
{
  ps5_iconv_cd* cd = (ps5_iconv_cd*)handle;
  if (!cd || cd == (ps5_iconv_cd*)-1)
  {
    errno = EBADF;
    return -1;
  }
  int r = 0;
  if (cd->kind == CD_SYSTEM)
    r = __real_iconv_close(cd->sys);
  free(cd);
  return r;
}
