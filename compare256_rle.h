/* compare256_rle.h -- 256 byte run-length encoding comparison
 * Copyright (C) 2022 Nathan Moinvaziri
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zbuild.h"
#include "zmemory.h"
#include "fallback_builtins.h"

typedef uint32_t (*compare256_rle_func)(const uint8_t* src0, const uint8_t* src1);

/* Helper to find first differing byte in a 64-bit word using best available method */
static inline uint32_t compare256_rle_match_byte(uint64_t diff) {
#ifdef HAVE_BUILTIN_CTZLL
#  if BYTE_ORDER == LITTLE_ENDIAN
    return __builtin_ctzll(diff) / 8;
#  else
    return __builtin_clzll(diff) / 8;
#  endif
#elif defined(HAVE_BUILTIN_CTZ)
    uint32_t lo = (uint32_t)diff;
    if (lo) {
#  if BYTE_ORDER == LITTLE_ENDIAN
        return __builtin_ctz(lo) / 8;
#  else
        return __builtin_clz(lo) / 8;
#  endif
    }
    uint32_t hi = (uint32_t)(diff >> 32);
#  if BYTE_ORDER == LITTLE_ENDIAN
    return 4 + __builtin_ctz(hi) / 8;
#  else
    return 4 + __builtin_clz(hi) / 8;
#  endif
#else
    uint32_t lo_diff = (uint32_t)diff;
    if (lo_diff) {
        if (lo_diff & 0xFF) return 0;
        if (lo_diff & 0xFF00) return 1;
        if (lo_diff & 0xFF0000) return 2;
        return 3;
    }
    uint32_t hi_diff = (uint32_t)(diff >> 32);
    if (hi_diff & 0xFF) return 4;
    if (hi_diff & 0xFF00) return 5;
    if (hi_diff & 0xFF0000) return 6;
    return 7;
#endif
}

/* 64-bit RLE comparison - uses 64-bit loads */
static inline uint32_t compare256_rle_64(const uint8_t *src0, const uint8_t *src1) {
    uint32_t src0_cmp32, len = 0;
    uint16_t src0_cmp;
    uint64_t sv, mv, diff;

    src0_cmp = zng_memread_2(src0);
    src0_cmp32 = ((uint32_t)src0_cmp << 16) | src0_cmp;
    sv = ((uint64_t)src0_cmp32 << 32) | src0_cmp32;

    do {
        mv = zng_memread_8(src1);
        diff = sv ^ mv;
        if (diff)
            return len + compare256_rle_match_byte(diff);
        src1 += 8, len += 8;
    } while (len < 256);

    return 256;
}

/* 8-bit RLE comparison for hardware without unaligned loads */
static inline uint32_t compare256_rle_8(const uint8_t *src0, const uint8_t *src1) {
    uint32_t len = 0;
    uint8_t val = *src0;

    do {
        if (val != src1[0])
            return len;
        if (val != src1[1])
            return len + 1;
        if (val != src1[2])
            return len + 2;
        if (val != src1[3])
            return len + 3;
        if (val != src1[4])
            return len + 4;
        if (val != src1[5])
            return len + 5;
        if (val != src1[6])
            return len + 6;
        if (val != src1[7])
            return len + 7;
        src1 += 8, len += 8;
    } while (len < 256);

    return 256;
}

static inline uint32_t compare256_rle_16(const uint8_t *src0, const uint8_t *src1) {
    return compare256_rle_64(src0, src1);
}

static inline uint32_t compare256_rle_32(const uint8_t *src0, const uint8_t *src1) {
    return compare256_rle_64(src0, src1);
}
