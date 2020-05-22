/* adler32_intrin.h -- compute the Adler-32 checksum of a data stream
 * Copyright (C) 1995-2011 Mark Adler
 * Copyright (C) 2017 Mika T. Lindqvist
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef __ADLER32_INTRIN__
#define __ADLER32_INTRIN__

#ifdef _MSC_VER
#  include <intrin.h>
#else
#  include <x86intrin.h>
#endif

/* These functions mimic intrinsics not available on earlier processors and extend them */

#ifndef _MSC_VER
typedef uint8_t   v1si __attribute__ ((vector_size (16)));
typedef uint16_t  v2si __attribute__ ((vector_size (16)));
typedef uint32_t  v4si __attribute__ ((vector_size (16)));
typedef uint64_t  v8si __attribute__ ((vector_size (16)));
#endif

static inline uint32_t extract_epi32(__m128i a, int index)
{
#ifdef _MSC_VER
    return a.m128i_u32[index];
#else
    v4si b = (v4si)a;
    return b[index];
#endif
}

static inline int64_t haddq_epu64(__m128i a)
{
#ifdef _MSC_VER
    return a.m128i_u64[0] + a.m128i_u64[1];
#else
    v8si b = (v8si)a;
    return b[0] + b[1];
#endif
}

static inline __m128i haddq_epu32(__m128i a)
{
#ifdef _MSC_VER
    __m128i ret;
    ret.m128i_u64[0] = a.m128i_u32[0] + a.m128i_u32[1];
    ret.m128i_u64[1] = a.m128i_u32[2] + a.m128i_u32[3];
    return ret;
#else
    v8si ret;
    v4si b = (v4si)a;
    ret[0] = b[0] + b[1];
    ret[1] = b[2] + b[3];
    return (__m128i)ret;
#endif
}

static inline __m128i haddd_epu16(__m128i a)
{
#ifdef _MSC_VER
    __m128i ret;
    ret.m128i_u32[0] = a.m128i_u16[0] + a.m128i_u16[1];
    ret.m128i_u32[1] = a.m128i_u16[2] + a.m128i_u16[3];
    ret.m128i_u32[2] = a.m128i_u16[4] + a.m128i_u16[5];
    ret.m128i_u32[3] = a.m128i_u16[6] + a.m128i_u16[7];
    return ret;
#else
    v2si b = (v2si)a;
    v4si ret;
    ret[0] = b[0] + b[1];
    ret[1] = b[2] + b[3];
    ret[2] = b[4] + b[5];
    ret[3] = b[6] + b[7];
    return (__m128i)ret;
#endif
}

static inline __m128i haddw_epu8(__m128i a)
{
#ifdef _MSC_VER
    __m128i ret;
    ret.m128i_u16[0] = a.m128i_u8[ 0] + a.m128i_u8[ 1];
    ret.m128i_u16[1] = a.m128i_u8[ 2] + a.m128i_u8[ 3];
    ret.m128i_u16[2] = a.m128i_u8[ 4] + a.m128i_u8[ 5];
    ret.m128i_u16[3] = a.m128i_u8[ 6] + a.m128i_u8[ 7];
    ret.m128i_u16[4] = a.m128i_u8[ 8] + a.m128i_u8[ 9];
    ret.m128i_u16[5] = a.m128i_u8[10] + a.m128i_u8[11];
    ret.m128i_u16[6] = a.m128i_u8[12] + a.m128i_u8[13];
    ret.m128i_u16[7] = a.m128i_u8[14] + a.m128i_u8[15];
    return ret;
#else
    v1si b = (v1si)a;
    v2si ret;
    ret[0] = b[ 0] + b[ 1];
    ret[1] = b[ 2] + b[ 3];
    ret[2] = b[ 4] + b[ 5];
    ret[3] = b[ 6] + b[ 7];
    ret[4] = b[ 8] + b[ 9];
    ret[5] = b[10] + b[11];
    ret[6] = b[12] + b[13];
    ret[7] = b[14] + b[15];
    return (__m128i)ret;
#endif
}

static inline __m128i haddd_epu8(__m128i a)
{
#ifdef _MSC_VER
    __m128i ret;
    ret.m128i_u32[0] = (a.m128i_u8[ 0] + a.m128i_u8[ 1])
                    + (a.m128i_u8[ 2] + a.m128i_u8[ 3]);
    ret.m128i_u32[1] = (a.m128i_u8[ 4] + a.m128i_u8[ 5])
                    + (a.m128i_u8[ 6] + a.m128i_u8[ 7]);
    ret.m128i_u32[2] = (a.m128i_u8[ 8] + a.m128i_u8[ 9])
                    + (a.m128i_u8[10] + a.m128i_u8[11]);
    ret.m128i_u32[3] = (a.m128i_u8[12] + a.m128i_u8[13])
                    + (a.m128i_u8[14] + a.m128i_u8[15]);
    return ret;
#else
    v1si b = (v1si)a;
    v4si ret;
    ret[0] = (b[ 0] + b[ 1]) + (b[ 2] + b[ 3]);
    ret[1] = (b[ 4] + b[ 5]) + (b[ 6] + b[ 7]);
    ret[2] = (b[ 8] + b[ 9]) + (b[10] + b[11]);
    ret[3] = (b[12] + b[13]) + (b[14] + b[15]);
    return (__m128i)ret;
#endif
}
static inline __m128i cvtepu8_epi16(__m128i a)
{
#ifdef _MSC_VER
    __m128i ret;
    ret.m128i_i16[0] = a.m128i_u8[0];
    ret.m128i_i16[1] = a.m128i_u8[1];
    ret.m128i_i16[2] = a.m128i_u8[2];
    ret.m128i_i16[3] = a.m128i_u8[3];
    ret.m128i_i16[4] = a.m128i_u8[4];
    ret.m128i_i16[5] = a.m128i_u8[5];
    ret.m128i_i16[6] = a.m128i_u8[6];
    ret.m128i_i16[7] = a.m128i_u8[7];
    return ret;
#else
    v1si b = (v1si)a;
    v2si ret;
    ret[0] = b[0];
    ret[1] = b[1];
    ret[2] = b[2];
    ret[3] = b[3];
    ret[4] = b[4];
    ret[5] = b[5];
    ret[6] = b[6];
    ret[7] = b[7];
    return (__m128i)ret;
#endif
}

static inline __m128i cvtepu8hi_epi16(__m128i a)
{
#ifdef _MSC_VER
    __m128i ret;
    ret.m128i_i16[0] = a.m128i_u8[ 8];
    ret.m128i_i16[1] = a.m128i_u8[ 9];
    ret.m128i_i16[2] = a.m128i_u8[10];
    ret.m128i_i16[3] = a.m128i_u8[11];
    ret.m128i_i16[4] = a.m128i_u8[12];
    ret.m128i_i16[5] = a.m128i_u8[13];
    ret.m128i_i16[6] = a.m128i_u8[14];
    ret.m128i_i16[7] = a.m128i_u8[15];
    return ret;
#else
    v1si b = (v1si)a;
    v2si ret;
    ret[0] = b[ 8];
    ret[1] = b[ 9];
    ret[2] = b[10];
    ret[3] = b[11];
    ret[4] = b[12];
    ret[5] = b[13];
    ret[6] = b[14];
    ret[7] = b[15];
    return (__m128i)ret;
#endif
}

/* This instruction was added in SSSE3 */
/* Using union removes need for redundant shuffling data around in stack */
#ifdef X86
__m64 hadd_pi32(__m64 a, __m64 b)
{
    union ui32x2_m64 {
        uint32_t u32[2];
        __m64    m64;
    };
    register ui32x2_m64 *ai, *bi;
    ai = (ui32x2_m64 *)&a;
    bi = (ui32x2_m64 *)&b;
    ai->u32[0] += ai->u32[1];
    bi->u32[0] += bi->u32[1];
    ai->u32[1]  = bi->u32[0];
    return ai->m64;
}
#endif

static inline __m128i insert_epi32(__m128i s, uint32_t a, int index)
{
#ifdef _MSC_VER
    s.m128i_u32[index] = a;
    return s;
#else
    v4si ret = (v4si)s;
    ret[index] = a;
    return (__m128i)ret;
#endif
}

#endif
