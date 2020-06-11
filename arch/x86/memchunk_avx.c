/* memchunk_avx.c -- AVX inline functions to copy small data chunks.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */
#ifndef MEMCHUNK_AVX_H_
#define MEMCHUNK_AVX_H_

#include "zbuild.h"
#include "zutil.h"

#ifdef X86_AVX_MEMCHUNK
#include <immintrin.h>

typedef __m128i memchunk_t;

#define HAVE_CHUNKMEMSET_1
#define HAVE_CHUNKMEMSET_4
#define HAVE_CHUNKMEMSET_8

static inline void chunkmemset_1(uint8_t *from, memchunk_t *chunk) {
    *chunk = _mm_broadcastb_epi8(_mm_loadu_si16((int8_t *)from));
}

static inline void chunkmemset_4(uint8_t *from, memchunk_t *chunk) {
    *chunk = _mm_broadcastd_epi32(_mm_loadu_si32((int32_t *)from));
}

static inline void chunkmemset_8(uint8_t *from, memchunk_t *chunk) {
    *chunk = _mm_broadcastq_epi64(_mm_loadu_si64((int64_t *)from));
}

static inline void loadchunk(uint8_t const *s, memchunk_t *chunk) {
    *chunk = _mm_loadu_si128((__m128i *)s);
}

static inline void storechunk(uint8_t *out, memchunk_t *chunk) {
    _mm_storeu_si128((__m128i *)out, *chunk);
}

#define CHUNKSIZE        chunksize_avx
#define CHUNKCOPY        chunkcopy_avx
#define CHUNKCOPY_SAFE   chunkcopy_safe_avx
#define CHUNKUNROLL      chunkunroll_avx
#define CHUNKMEMSET      chunkmemset_avx
#define CHUNKMEMSET_SAFE chunkmemset_safe_avx

#include "memchunk_tpl.h"

#endif
#endif
