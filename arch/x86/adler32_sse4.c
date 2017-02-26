/* adler32_sse4.c -- compute the Adler-32 checksum of a data stream
 * Copyright (C) 1995-2011 Mark Adler
 * Copyright (C) 2017 Mika T. Lindqvist
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* @(#) $Id$ */

#ifdef X86_SSE4_ADLER32

#include "adler32_sse4.h"
#include "adler32_intrin.h"
#include "adler32_x86.h"

#ifdef _MSC_VER
#  include <intrin.h>
#else
#  include <x86intrin.h>
#endif

static void sse_accum32(uint32_t *s, const unsigned char *buf, size_t len)
{
  const uint8_t tc0[16] = {16, 15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1};

  __m128i t0 = _mm_load_si128((__m128i *)tc0);
  __m128i adacc, s2acc;
  adacc = _mm_insert_epi32(_mm_setzero_si128(), s[0], 0);
  s2acc = _mm_insert_epi32(_mm_setzero_si128(), s[1], 0);

  while (len > 0) {
    __m128i d0 = _mm_load_si128((__m128i *)buf);
    __m128i sum2;
    sum2  =                      _mm_mullo_epi16(_mm_cvtepu8_epi16(t0),
                                                 _mm_cvtepu8_epi16(d0));
    sum2  = _mm_add_epi16(sum2,  _mm_mullo_epi16(_mm_cvtepu8_epi16(_mm_srli_si128(t0, 8)),
                                                 _mm_cvtepu8_epi16(_mm_srli_si128(d0, 8))));
    s2acc = _mm_add_epi32(s2acc, _mm_slli_epi32(adacc, 4));
    s2acc = _mm_add_epi32(s2acc, haddd_epu16(sum2));
    adacc = _mm_add_epi32(adacc, haddd_epu8(d0));
    buf += 16;
    len--;
  }

  {
#ifdef X86
    /* i686 can't handle _mm_extract_epi64() so we convert __m128 -> __m64x2 */
    __m64 adacc2[2], s2acc2[2];
    _mm_store_si128((__m128i *)adacc2, adacc);
    _mm_store_si128((__m128i *)s2acc2, s2acc);
    __m64 adacc3 = _mm_hadd_pi32(adacc2[0], adacc2[1]);
    __m64 s2acc3 = _mm_hadd_pi32(s2acc2[0], s2acc2[1]);
    __m64 as     = _mm_hadd_pi32(adacc3,    s2acc3);
    _mm_stream_pi((__m64 *)s, as);
#else
    /* We widen from 32-bit to 64-bit because x86_64 doesn't support __m64 */
    __m128i adacc2 = haddq_epu32(adacc);
    __m128i s2acc2 = haddq_epu32(s2acc);
    s[0] = (uint32_t)haddq_epu64(adacc2); /* Horizontal add */
    s[1] = (uint32_t)haddq_epu64(s2acc2); /* Horizontal add */
#endif
  }
}

uint32_t adler32_sse4(uint32_t adler, const unsigned char *buf, size_t len)
{
  /* The largest prime smaller than 65536. */
  const uint32_t M_BASE = 65521;
  /* This is the threshold where doing accumulation may overflow. */
  const int M_NMAX = 5552;

  uint32_t sum2;
  uint32_t pair[2];
  int n = M_NMAX;
  unsigned int done = 0, i;
  unsigned int al = 0;

  if (buf == NULL)
    return 1L;

  /* Split Adler-32 into component sums, it can be supplied by
   * the caller sites (e.g. in a PNG file).
   */
  sum2 = (adler >> 16) & 0xffff;
  adler &= 0xffff;
  pair[0] = adler;
  pair[1] = sum2;

  /* Align buffer */
  if ((uintptr_t)buf & 0xf) {
    al = 16-((uintptr_t)buf & 0xf);
    if (al > len) {
      al = len;
    }
    sse_handle_head_or_tail(pair, buf, al);
    pair[0] %= M_BASE;
    pair[1] %= M_BASE;

    done += al;
  }
  for (i = al; i < len; i += n) {
    if ((i + n) > len)
      n = (int)(len - i);

    if (n < 16)
      break;

    sse_accum32(pair, buf + i, n / 16);
    pair[0] %= M_BASE;
    pair[1] %= M_BASE;

    done += (n / 16) * 16;
  }

  /* Handle the tail elements. */
  if (done < len) {
    sse_handle_head_or_tail(pair, (buf + done), len - done);
    pair[0] %= M_BASE;
    pair[1] %= M_BASE;
  }

  /* D = B * 65536 + A, see: https://en.wikipedia.org/wiki/Adler-32. */
  return (pair[1] << 16) | pair[0];
}
#endif
