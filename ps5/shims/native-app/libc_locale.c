/*
 *  C-locale implementation of the FreeBSD "xlocale" API that libc++ (built
 *  for FreeBSD) and a few libraries call. The console's system libc
 *  (SceLibcInternal) has the plain functions but none of the *_l variants,
 *  and the title only ever runs in the C locale - so every locale_t is the
 *  same C locale and each *_l forwards to its plain counterpart.
 *
 *  Deliberately includes no libc headers: they declare these functions with
 *  locale_t and mbstate_t types; pointers are passed as void* here, which is
 *  the same ABI.
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stddef.h>

typedef int ps5_wint_t;
typedef void* ps5_locale_t;

/* ---- the one locale ------------------------------------------------------ */

static struct { int dummy; } g_c_locale;

ps5_locale_t newlocale(int mask, const char* name, ps5_locale_t base)
{
  (void)mask; (void)name;
  return base ? base : (ps5_locale_t)&g_c_locale;
}
ps5_locale_t duplocale(ps5_locale_t loc) { return loc ? loc : (ps5_locale_t)&g_c_locale; }
void freelocale(ps5_locale_t loc) { (void)loc; }
ps5_locale_t uselocale(ps5_locale_t loc) { (void)loc; return (ps5_locale_t)&g_c_locale; }

/* ---- FreeBSD rune table (C locale), as <xlocale/_ctype.h> reads it -------- */

typedef struct { int nranges; void* ranges; } ps5_rune_range;
typedef struct
{
  char magic[8];
  char encoding[32];
  void* sgetrune;
  void* sputrune;
  int invalid_rune;
  unsigned long runetype[256];
  int maplower[256];
  int mapupper[256];
  ps5_rune_range runetype_ext, maplower_ext, mapupper_ext;
  void* variable;
  int variable_len;
} ps5_rune_locale;

static ps5_rune_locale g_runes = {
    "RuneMag", "NONE", NULL, NULL, 0xFFFD,
    {0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x24200UL,0x4200UL,0x4200UL,0x4200UL,0x4200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x200UL,0x40064000UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40450c00UL,0x40450c01UL,0x40450c02UL,0x40450c03UL,0x40450c04UL,0x40450c05UL,0x40450c06UL,0x40450c07UL,0x40450c08UL,0x40450c09UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x4005890aUL,0x4005890bUL,0x4005890cUL,0x4005890dUL,0x4005890eUL,0x4005890fUL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40048900UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x4005190aUL,0x4005190bUL,0x4005190cUL,0x4005190dUL,0x4005190eUL,0x4005190fUL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40041900UL,0x40042800UL,0x40042800UL,0x40042800UL,0x40042800UL,0x200UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL,0x0UL},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,123,124,125,126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255},
    {0, NULL}, {0, NULL}, {0, NULL}, NULL, 0};

void* __runes_for_locale(ps5_locale_t loc, int* mb_sc_max)
{
  (void)loc;
  if (mb_sc_max)
    *mb_sc_max = 1;
  return &g_runes;
}
unsigned long ___runetype_l(int c, ps5_locale_t loc) { (void)c; (void)loc; return 0; }
int ___tolower_l(int c, ps5_locale_t loc) { (void)loc; return c; }
int ___toupper_l(int c, ps5_locale_t loc) { (void)loc; return c; }
size_t ___mb_cur_max_l(ps5_locale_t loc) { (void)loc; return 1; }

/* ---- message catalogues: none ------------------------------------------- */

void* catopen(const char* name, int flag) { (void)name; (void)flag; return (void*)-1; }
char* catgets(void* cat, int set, int num, const char* s) { (void)cat; (void)set; (void)num; return (char*)s; }
int catclose(void* cat) { (void)cat; return 0; }

/* ---- plain functions from SceLibcInternal --------------------------------- */

double strtod(const char*, char**);
float strtof(const char*, char**);
long double strtold(const char*, char**);
long long strtoll(const char*, char**, int);
unsigned long long strtoull(const char*, char**, int);
size_t strftime(char*, size_t, const char*, const void*);
int strcoll(const char*, const char*);
size_t strxfrm(char*, const char*, size_t);
int wcscoll(const wchar_t*, const wchar_t*);
size_t wcsxfrm(wchar_t*, const wchar_t*, size_t);
size_t mbrtowc(wchar_t*, const char*, size_t, void*);
size_t mbrlen(const char*, size_t, void*);
size_t mbsrtowcs(wchar_t*, const char**, size_t, void*);
int mbtowc(wchar_t*, const char*, size_t);
size_t wcrtomb(char*, wchar_t, void*);
int wctob(ps5_wint_t);
ps5_wint_t btowc(int);
int iswctype(ps5_wint_t, unsigned long);
int vsnprintf(char*, size_t, const char*, va_list);
int vasprintf(char**, const char*, va_list);
int vsscanf(const char*, const char*, va_list);

/* ---- *_l forwarders ----------------------------------------------------- */

double strtod_l(const char* s, char** e, ps5_locale_t l) { (void)l; return strtod(s, e); }
float strtof_l(const char* s, char** e, ps5_locale_t l) { (void)l; return strtof(s, e); }
long double strtold_l(const char* s, char** e, ps5_locale_t l) { (void)l; return strtold(s, e); }
long long strtoll_l(const char* s, char** e, int b, ps5_locale_t l) { (void)l; return strtoll(s, e, b); }
unsigned long long strtoull_l(const char* s, char** e, int b, ps5_locale_t l) { (void)l; return strtoull(s, e, b); }
size_t strftime_l(char* s, size_t n, const char* f, const void* tm, ps5_locale_t l) { (void)l; return strftime(s, n, f, tm); }
int strcoll_l(const char* a, const char* b, ps5_locale_t l) { (void)l; return strcoll(a, b); }
size_t strxfrm_l(char* d, const char* s, size_t n, ps5_locale_t l) { (void)l; return strxfrm(d, s, n); }
int wcscoll_l(const wchar_t* a, const wchar_t* b, ps5_locale_t l) { (void)l; return wcscoll(a, b); }
size_t wcsxfrm_l(wchar_t* d, const wchar_t* s, size_t n, ps5_locale_t l) { (void)l; return wcsxfrm(d, s, n); }
size_t mbrtowc_l(wchar_t* w, const char* s, size_t n, void* ps, ps5_locale_t l) { (void)l; return mbrtowc(w, s, n, ps); }
size_t mbrlen_l(const char* s, size_t n, void* ps, ps5_locale_t l) { (void)l; return mbrlen(s, n, ps); }
size_t mbsrtowcs_l(wchar_t* d, const char** s, size_t n, void* ps, ps5_locale_t l) { (void)l; return mbsrtowcs(d, s, n, ps); }
int mbtowc_l(wchar_t* w, const char* s, size_t n, ps5_locale_t l) { (void)l; return mbtowc(w, s, n); }
size_t wcrtomb_l(char* s, wchar_t w, void* ps, ps5_locale_t l) { (void)l; return wcrtomb(s, w, ps); }
int wctob_l(ps5_wint_t c, ps5_locale_t l) { (void)l; return wctob(c); }
ps5_wint_t btowc_l(int c, ps5_locale_t l) { (void)l; return btowc(c); }
int iswctype_l(ps5_wint_t c, unsigned long t, ps5_locale_t l) { (void)l; return iswctype(c, t); }
/*
 * localeconv(): the system libc's struct lconv has a different field order
 * from FreeBSD's (the monetary fields come first), so FreeBSD-compiled code
 * reading ->decimal_point got an empty string - nlohmann::json then put a NUL
 * where the decimal point goes and asserted on the first float (Kodi's
 * JSON-RPC schema). Return the C locale in FreeBSD's layout instead.
 */
struct ps5_lconv
{
  char* decimal_point;
  char* thousands_sep;
  char* grouping;
  char* int_curr_symbol;
  char* currency_symbol;
  char* mon_decimal_point;
  char* mon_thousands_sep;
  char* mon_grouping;
  char* positive_sign;
  char* negative_sign;
  char int_frac_digits;
  char frac_digits;
  char p_cs_precedes;
  char p_sep_by_space;
  char n_cs_precedes;
  char n_sep_by_space;
  char p_sign_posn;
  char n_sign_posn;
  char int_p_cs_precedes;
  char int_n_cs_precedes;
  char int_p_sep_by_space;
  char int_n_sep_by_space;
  char int_p_sign_posn;
  char int_n_sign_posn;
};

static struct ps5_lconv g_c_lconv = {
    (char*)".", (char*)"", (char*)"", (char*)"", (char*)"", (char*)"", (char*)"", (char*)"",
    (char*)"", (char*)"",
    /* CHAR_MAX = "not available" for every numeric field, as in the C locale */
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127};

void* localeconv(void) { return &g_c_lconv; }
void* localeconv_l(ps5_locale_t l) { (void)l; return &g_c_lconv; }

int snprintf_l(char* s, size_t n, ps5_locale_t l, const char* f, ...)
{
  (void)l;
  va_list ap;
  va_start(ap, f);
  int r = vsnprintf(s, n, f, ap);
  va_end(ap);
  return r;
}
int asprintf_l(char** s, ps5_locale_t l, const char* f, ...)
{
  (void)l;
  va_list ap;
  va_start(ap, f);
  int r = vasprintf(s, f, ap);
  va_end(ap);
  return r;
}
int sscanf_l(const char* s, ps5_locale_t l, const char* f, ...)
{
  (void)l;
  va_list ap;
  va_start(ap, f);
  int r = vsscanf(s, f, ap);
  va_end(ap);
  return r;
}

/* ---- the two "n" conversions SceLibcInternal lacks ------------------------ */

/* FreeBSD mbstate_t is 128 bytes; used when the caller passes no state. */
static _Alignas(8) unsigned char g_mbs_state[128];
static _Alignas(8) unsigned char g_wcs_state[128];

size_t mbsnrtowcs(wchar_t* dst, const char** src, size_t nms, size_t len, void* ps)
{
  if (!ps)
    ps = g_mbs_state;
  const char* s = *src;
  size_t count = 0;
  wchar_t wc;
  while (nms > 0 && (dst == NULL || count < len))
  {
    size_t r = mbrtowc(&wc, s, nms, ps);
    if (r == (size_t)-1)
    {
      if (dst)
        *src = s;
      return (size_t)-1;
    }
    if (r == (size_t)-2) /* incomplete sequence at the end of the input */
    {
      s += nms;
      break;
    }
    if (dst)
      dst[count] = wc;
    if (wc == 0)
    {
      s = NULL;
      break;
    }
    s += r;
    nms -= r;
    ++count;
  }
  if (dst)
    *src = s;
  return count;
}

size_t wcsnrtombs(char* dst, const wchar_t** src, size_t nwc, size_t len, void* ps)
{
  if (!ps)
    ps = g_wcs_state;
  const wchar_t* s = *src;
  size_t count = 0;
  char buf[16]; /* >= MB_LEN_MAX */
  while (nwc > 0)
  {
    size_t r = wcrtomb(buf, *s, ps);
    if (r == (size_t)-1)
    {
      if (dst)
        *src = s;
      return (size_t)-1;
    }
    if (dst)
    {
      if (count + r > len)
        break;
      for (size_t i = 0; i < r; ++i)
        dst[count + i] = buf[i];
    }
    if (*s == 0)
    {
      s = NULL;
      break;
    }
    count += r;
    ++s;
    --nwc;
  }
  if (dst)
    *src = s;
  return count;
}

size_t mbsnrtowcs_l(wchar_t* d, const char** s, size_t nms, size_t n, void* ps, ps5_locale_t l) { (void)l; return mbsnrtowcs(d, s, nms, n, ps); }
size_t wcsnrtombs_l(char* d, const wchar_t** s, size_t nwc, size_t n, void* ps, ps5_locale_t l) { (void)l; return wcsnrtombs(d, s, nwc, n, ps); }
