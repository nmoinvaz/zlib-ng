/* compare256_p.h -- 256 byte memory comparison with match length return
 * Copyright (C) 2020 Nathan Moinvaziri
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zmemory.h"
#include "deflate.h"
#include "fallback_builtins.h"

/* Helper to find first differing byte in a 64-bit word using best available method */
static inline uint32_t compare256_match_byte(uint64_t diff) {
#ifdef HAVE_BUILTIN_CTZLL
    /* Best: use 64-bit ctzll directly */
#  if BYTE_ORDER == LITTLE_ENDIAN
    return __builtin_ctzll(diff) / 8;
#  else
    return __builtin_clzll(diff) / 8;
#  endif
#elif defined(HAVE_BUILTIN_CTZ)
    /* Fallback: use 32-bit ctz on each half */
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
    /* Fallback: byte-by-byte extraction */
    uint32_t lo_diff = (uint32_t)diff;
    if (lo_diff) {
        if (lo_diff & 0xFF)
            return 0;
        if (lo_diff & 0xFF00)
            return 1;
        if (lo_diff & 0xFF0000)
            return 2;
        return 3;
    }
    uint32_t hi_diff = (uint32_t)(diff >> 32);
    if (hi_diff & 0xFF)
        return 4;
    if (hi_diff & 0xFF00)
        return 5;
    if (hi_diff & 0xFF0000)
        return 6;
    return 7;
#endif
}

/* 64-bit integer comparison - uses 64-bit loads with best available mismatch detection */
static inline uint32_t compare256_64(const uint8_t *src0, const uint8_t *src1) {
    uint32_t len = 0;

    do {
        uint64_t sv = zng_memread_8(src0);
        uint64_t mv = zng_memread_8(src1);
        uint64_t diff = sv ^ mv;
        if (diff)
            return len + compare256_match_byte(diff);
        src0 += 8, src1 += 8, len += 8;

        sv = zng_memread_8(src0);
        mv = zng_memread_8(src1);
        diff = sv ^ mv;
        if (diff)
            return len + compare256_match_byte(diff);
        src0 += 8, src1 += 8, len += 8;
    } while (len < 256);

    return 256;
}

/* Provide legacy function names that all use the unified 64-bit implementation */
static inline uint32_t compare256_8(const uint8_t *src0, const uint8_t *src1) {
    return compare256_64(src0, src1);
}

static inline uint32_t compare256_16(const uint8_t *src0, const uint8_t *src1) {
    return compare256_64(src0, src1);
}

static inline uint32_t compare256_32(const uint8_t *src0, const uint8_t *src1) {
    return compare256_64(src0, src1);
}
