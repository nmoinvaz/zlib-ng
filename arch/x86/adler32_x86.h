/* adler32_x86.h -- compute the Adler-32 checksum of a data stream
 * Copyright (C) 1995-2011 Mark Adler
 * Copyright (C) 2017 Mika T. Lindqvist
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef __ADLER32_X86__
#define __ADLER32_X86__

static inline void sse_handle_head_or_tail(uint32_t *pair, const unsigned char *buf, size_t len)
{
    unsigned int i;
    for (i = 0; i < len; ++i) {
        pair[0] += buf[i];
        pair[1] += pair[0];
    }
}

static inline void sse_accum32(uint32_t *s, const unsigned char *buf, size_t len)
{
    const uint8_t tc0[16] = {16, 15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1};

    __m128i t0 = _mm_load_si128((__m128i *)tc0);
    __m128i adacc, s2acc;
    adacc = insert_epi32(_mm_setzero_si128(), s[0], 0);
    s2acc = insert_epi32(_mm_setzero_si128(), s[1], 0);

    while (len > 0) {
        __m128i d0 = _mm_load_si128((__m128i *)buf);
        __m128i sum2;
        sum2  =                      _mm_mullo_epi16(  cvtepu8_epi16(t0),   cvtepu8_epi16(d0));
        sum2  = _mm_maddubs_epi16(d0, t0);
        s2acc = _mm_add_epi32(s2acc, _mm_slli_epi32(adacc, 4));
        s2acc = _mm_add_epi32(s2acc, _mm_madd_epi16(sum2, _mm_set1_epi16(1)));
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

#endif
