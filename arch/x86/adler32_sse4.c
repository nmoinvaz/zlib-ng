/* adler32_sse4.c -- compute the Adler-32 checksum of a data stream
 * Copyright (C) 1995-2011 Mark Adler
 * Copyright (C) 2017 Mika T. Lindqvist
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifdef X86_SSE4_ADLER32

#include "zbuild.h"
#include "zutil.h"

#include "adler32_p.h"
#include "adler32_sse4.h"
#include "adler32_intrin.h"
#include "adler32_x86.h"

#ifdef _MSC_VER
#  include <intrin.h>
#else
#  include <x86intrin.h>
#endif

uint32_t adler32_sse4(uint32_t adler, const unsigned char *buf, size_t len) {
    uint32_t sum2;
    uint32_t pair[2];
    unsigned n;
    unsigned int done = 0, i;
    unsigned int al = 0;
  
    /* split Adler-32 into component sums */
    sum2 = (adler >> 16) & 0xffff;
    adler &= 0xffff;

    /* in case user likes doing a byte at a time, keep it fast */
    if (UNLIKELY(len == 1))
        return adler32_len_1(adler, buf, sum2);

    /* initial Adler-32 value (deferred check for len == 1 speed) */
    if (UNLIKELY(buf == NULL))
        return 1L;

    /* in case short lengths are provided, keep it somewhat fast */
    if (UNLIKELY(len < 16))
        return adler32_len_16(adler, buf, len, sum2);

    pair[0] = adler;
    pair[1] = sum2;

    /* Align buffer */
    if ((uintptr_t)buf & 0xf) {
        al = 16-((uintptr_t)buf & 0xf);
        if (al > len) {
            al = len;
        }
        sse_handle_head_or_tail(pair, buf, al);
        pair[0] %= BASE;
        pair[1] %= BASE;

        done += al;
    }
    n = NMAX;
    for (i = al; i < len; i += n) {
        if ((i + n) > len)
            n = (int)(len - i);

        if (n < 16)
            break;

        sse_accum32(pair, buf + i, n / 16);
        pair[0] %= BASE;
        pair[1] %= BASE;

        done += (n / 16) * 16;
    }

    /* Handle the tail elements. */
    if (done < len) {
        sse_handle_head_or_tail(pair, (buf + done), len - done);
        pair[0] %= BASE;
        pair[1] %= BASE;
    }

    /* D = B * 65536 + A, see: https://en.wikipedia.org/wiki/Adler-32. */
    return (pair[1] << 16) | pair[0];
}

uint32_t adler32_vec(uint32_t adler, const unsigned char *buf, size_t len) {
    unsigned long sum2;

    /* split Adler-32 into component sums */
    sum2 = (adler >> 16) & 0xffff;
    adler &= 0xffff;

    /* in case user likes doing a byte at a time, keep it fast */
    if (UNLIKELY(len == 1))
        return adler32_len_1(adler, buf, sum2);

    /* initial Adler-32 value (deferred check for len == 1 speed) */
    if (UNLIKELY(buf == NULL))
        return 1L;

    /* in case short lengths are provided, keep it somewhat fast */
    if (UNLIKELY(len < 16))
        return adler32_len_16(adler, buf, len, sum2);

    uint32_t ALIGNED_(16) s1[4], s2[4];
    s1[0] = s1[1] = s1[2] = 0; s1[3] = adler;
    s2[0] = s2[1] = s2[2] = 0; s2[3] = sum2;
    char ALIGNED_(16) dot1[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    __m128i dot1v = _mm_load_si128((__m128i*)dot1);
    char ALIGNED_(16) dot2[16] = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    __m128i dot2v = _mm_load_si128((__m128i*)dot2);
    short ALIGNED_(16) dot3[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    __m128i dot3v = _mm_load_si128((__m128i*)dot3);
    // We will need to multiply by 
    //char ALIGNED_(16) shift[4] = {0, 0, 0, 4}; //{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4};
    char ALIGNED_(16) shift[16] = {4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    __m128i shiftv = _mm_load_si128((__m128i*)shift);
    while (len >= 16) {
       //printf("Starting iteration with length %d\n", len);
       __m128i vs1 = _mm_load_si128((__m128i*)s1);
       __m128i vs2 = _mm_load_si128((__m128i*)s2);
       __m128i vs1_0 = vs1;
       int k = (len < NMAX ? (int)len : NMAX);
       k -= k % 16;
       len -= k;
       while (k >= 16) {
           /*
              vs1 = adler + sum(c[i])
              vs2 = sum2 + 16 vs1 + sum( (16-i+1) c[i] )
              NOTE: 256-bit equivalents are:
                _mm256_maddubs_epi16 <- operates on 32 bytes to 16 shorts
                _mm256_madd_epi16    <- Sums 16 shorts to 8 int32_t.
              We could rewrite the below to use 256-bit instructions instead of 128-bit.
           */
           __m128i vbuf = _mm_loadu_si128((__m128i*)buf);
           //printf("vbuf: [%d, %d, %d, %d; %d, %d, %d, %d; %d, %d, %d, %d; %d, %d, %d, %d]\n", buf[0], (unsigned char)buf[1], (unsigned char)buf[2], (unsigned char)buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
           buf += 16;
           k -= 16;
           __m128i v_short_sum1 = _mm_maddubs_epi16(vbuf, dot1v); // multiply-add, resulting in 8 shorts.
           //{short __attribute__((ALIGNED_(16) test[8]; _mm_store_si128((__m128i*)test, v_short_sum1); printf("v_short_sum1: [%d, %d, %d, %d; %d, %d, %d, %d]\n", test[0], test[1], test[2], test[3], test[4], test[5], test[6], test[7]);}
           __m128i vsum1 = _mm_madd_epi16(v_short_sum1, dot3v);  // sum 8 shorts to 4 int32_t;
           //{uint32_t __attribute__((ALIGNED_(16) t2[4]; _mm_store_si128((__m128i*)t2, vsum1); printf("vsum1: [%d, %d, %d, %d]\n", t2[0], t2[1], t2[2], t2[3]);}
           __m128i v_short_sum2 = _mm_maddubs_epi16(vbuf, dot2v);
           //{short __attribute__((ALIGNED_(16) test[8]; _mm_store_si128((__m128i*)test, v_short_sum2); printf("v_short_sum2: [%d, %d, %d, %d; %d, %d, %d, %d]\n", test[0], test[1], test[2], test[3], test[4], test[5], test[6], test[7]);}
           vs1 = _mm_add_epi32(vsum1, vs1);
           __m128i vsum2 = _mm_madd_epi16(v_short_sum2, dot3v);
           //{uint32_t __attribute__((ALIGNED_(16) t2[4]; _mm_store_si128((__m128i*)t2, vsum2); printf("vsum2: [%d, %d, %d, %d]\n", t2[0], t2[1], t2[2], t2[3]);}
           vs1_0 = _mm_sll_epi32(vs1_0, shiftv);
           //{uint32_t __attribute__((ALIGNED_(16) t2[4]; _mm_store_si128((__m128i*)t2, vs1_0); printf("16*vs1_0: [%d, %d, %d, %d]\n", t2[0], t2[1], t2[2], t2[3]);}
           vsum2 = _mm_add_epi32(vsum2, vs2);
           vs2   = _mm_add_epi32(vsum2, vs1_0);
           vs1_0 = vs1;
       }
       // At this point, we have partial sums stored in vs1 and vs2.  There are AVX512 instructions that
       // would allow us to sum these quickly (VP4DPWSSD).  For now, just unpack and move on.
       uint32_t ALIGNED_(32) s1_unpack[4];
       uint32_t ALIGNED_(32) s2_unpack[4];
       _mm_store_si128((__m128i*)s1_unpack, vs1);
       //{uint32_t __attribute__((ALIGNED_(16) t2[4]; _mm_store_si128((__m128i*)t2, vs1); printf("vs1: [%d, %d, %d, %d]\n", t2[0], t2[1], t2[2], t2[3]);}
       _mm_store_si128((__m128i*)s2_unpack, vs2);
       adler = (s1_unpack[0] % BASE) + (s1_unpack[1] % BASE) + (s1_unpack[2] % BASE) + (s1_unpack[3] % BASE);
       MOD(adler);
       s1[3] = adler;
       sum2 = (s2_unpack[0] % BASE) + (s2_unpack[1] % BASE) + (s2_unpack[2] % BASE) + (s2_unpack[3] % BASE);
       MOD(sum2);
       s2[3] = sum2;
    }

    while (len--) {
       //printf("Handling tail end.\n");
       adler += *buf++;
       sum2 += adler;
    }
    MOD(adler);
    MOD(sum2);

    /* return recombined sums */
    return adler | (sum2 << 16);
}

uint32_t adler32_avx(uint32_t adler, const unsigned char *buf, size_t len) {
    unsigned long sum2;

    /* split Adler-32 into component sums */
    sum2 = (adler >> 16) & 0xffff;
    adler &= 0xffff;

    /* in case user likes doing a byte at a time, keep it fast */
    if (UNLIKELY(len == 1))
        return adler32_len_1(adler, buf, sum2);

    /* initial Adler-32 value (deferred check for len == 1 speed) */
    if (UNLIKELY(buf == NULL))
        return 1L;

    /* in case short lengths are provided, keep it somewhat fast */
    if (UNLIKELY(len < 16))
        return adler32_len_16(adler, buf, len, sum2);

    uint32_t ALIGNED_(32) s1[8], s2[8];
    memset(s1, '\0', sizeof(uint32_t)*7); s1[7] = adler; // TODO: would a masked load be faster?
    memset(s2, '\0', sizeof(uint32_t)*7); s2[7] = sum2;
    char ALIGNED_(32) dot1[32] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    __m256i dot1v = _mm256_load_si256((__m256i*)dot1);
    char ALIGNED_(32) dot2[32] = {32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    __m256i dot2v = _mm256_load_si256((__m256i*)dot2);
    short ALIGNED_(32) dot3[16] = {1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1};
    __m256i dot3v = _mm256_load_si256((__m256i*)dot3);
    // We will need to multiply by 
    char ALIGNED_(16) shift[16] = {5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    __m128i shiftv = _mm_load_si128((__m128i*)shift);
    while (len >= 32) {
       //printf("Starting iteration with length %d\n", len);
       __m256i vs1 = _mm256_load_si256((__m256i*)s1);
       __m256i vs2 = _mm256_load_si256((__m256i*)s2);
       __m256i vs1_0 = vs1;
       int k = (len < NMAX ? (int)len : NMAX);
       k -= k % 32;
       len -= k;
       while (k >= 32) {
           /*
              vs1 = adler + sum(c[i])
              vs2 = sum2 + 16 vs1 + sum( (16-i+1) c[i] )
           */
           __m256i vbuf = _mm256_loadu_si256((__m256i*)buf);
           //printf("vbuf: [%d, %d, %d, %d; %d, %d, %d, %d; %d, %d, %d, %d; %d, %d, %d, %d]\n", buf[0], (unsigned char)buf[1], (unsigned char)buf[2], (unsigned char)buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
           buf += 32;
           k -= 32;
           __m256i v_short_sum1 = _mm256_maddubs_epi16(vbuf, dot1v); // multiply-add, resulting in 8 shorts.
           //{short ALIGNED_(16) test[8]; _mm_store_si128((__m128i*)test, v_short_sum1); printf("v_short_sum1: [%d, %d, %d, %d; %d, %d, %d, %d]\n", test[0], test[1], test[2], test[3], test[4], test[5], test[6], test[7]);}
           __m256i vsum1 = _mm256_madd_epi16(v_short_sum1, dot3v);  // sum 8 shorts to 4 int32_t;
           //{uint32_t ALIGNED_(16) t2[4]; _mm_store_si128((__m128i*)t2, vsum1); printf("vsum1: [%d, %d, %d, %d]\n", t2[0], t2[1], t2[2], t2[3]);}
           __m256i v_short_sum2 = _mm256_maddubs_epi16(vbuf, dot2v);
           //{short ALIGNED_(16) test[8]; _mm_store_si128((__m128i*)test, v_short_sum2); printf("v_short_sum2: [%d, %d, %d, %d; %d, %d, %d, %d]\n", test[0], test[1], test[2], test[3], test[4], test[5], test[6], test[7]);}
           vs1 = _mm256_add_epi32(vsum1, vs1);
           __m256i vsum2 = _mm256_madd_epi16(v_short_sum2, dot3v);
           //{uint32_t ALIGNED_(16) t2[4]; _mm_store_si128((__m128i*)t2, vsum2); printf("vsum2: [%d, %d, %d, %d]\n", t2[0], t2[1], t2[2], t2[3]);}
           vs1_0 = _mm256_sll_epi32(vs1_0, shiftv);
           //{uint32_t ALIGNED_(16) t2[4]; _mm_store_si128((__m128i*)t2, vs1_0); printf("16*vs1_0: [%d, %d, %d, %d]\n", t2[0], t2[1], t2[2], t2[3]);}
           vsum2 = _mm256_add_epi32(vsum2, vs2);
           vs2   = _mm256_add_epi32(vsum2, vs1_0);
           vs1_0 = vs1;
       }
       // At this point, we have partial sums stored in vs1 and vs2.  There are AVX512 instructions that
       // would allow us to sum these quickly (VP4DPWSSD).  For now, just unpack and move on.
       uint32_t ALIGNED_(32) s1_unpack[8];
       uint32_t ALIGNED_(32) s2_unpack[8];
       _mm256_store_si256((__m256i*)s1_unpack, vs1);
       //{uint32_t ALIGNED_(16) t2[4]; _mm_store_si128((__m128i*)t2, vs1); printf("vs1: [%d, %d, %d, %d]\n", t2[0], t2[1], t2[2], t2[3]);}
       _mm256_store_si256((__m256i*)s2_unpack, vs2);
       adler = (s1_unpack[0] % BASE) + (s1_unpack[1] % BASE) + (s1_unpack[2] % BASE) + (s1_unpack[3] % BASE) + (s1_unpack[4] % BASE) + (s1_unpack[5] % BASE) + (s1_unpack[6] % BASE) + (s1_unpack[7] % BASE);
       MOD(adler);
       s1[7] = adler;
       sum2 = (s2_unpack[0] % BASE) + (s2_unpack[1] % BASE) + (s2_unpack[2] % BASE) + (s2_unpack[3] % BASE) + (s2_unpack[4] % BASE) + (s2_unpack[5] % BASE) + (s2_unpack[6] % BASE) + (s2_unpack[7] % BASE);
       MOD(sum2);
       s2[7] = sum2;
    }

    while (len--) {
       //printf("Handling tail end.\n");
       adler += *buf++;
       sum2 += adler;
    }
    MOD(adler);
    MOD(sum2);

    /* return recombined sums */
    return adler | (sum2 << 16);
}
#endif
