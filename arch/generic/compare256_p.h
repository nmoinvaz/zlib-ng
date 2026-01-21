/* compare256_p.h -- 256 byte memory comparison with match length return
 * Copyright (C) 2020 Nathan Moinvaziri
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zmemory.h"
#include "deflate.h"
#include "fallback_builtins.h"

/* 8-bit integer comparison - uses 32-bit loads with byte extraction on mismatch */
static inline uint32_t compare256_8(const uint8_t *src0, const uint8_t *src1) {
    uint32_t len = 0;

    do {
        uint32_t sv = zng_memread_4(src0);
        uint32_t mv = zng_memread_4(src1);
        if (sv != mv) {
            /* Find first differing byte by checking each byte */
            if ((sv & 0xFF) != (mv & 0xFF))
                return len;
            if (((sv >> 8) & 0xFF) != ((mv >> 8) & 0xFF))
                return len + 1;
            if (((sv >> 16) & 0xFF) != ((mv >> 16) & 0xFF))
                return len + 2;
            return len + 3;
        }
        src0 += 4, src1 += 4, len += 4;

        sv = zng_memread_4(src0);
        mv = zng_memread_4(src1);
        if (sv != mv) {
            if ((sv & 0xFF) != (mv & 0xFF))
                return len;
            if (((sv >> 8) & 0xFF) != ((mv >> 8) & 0xFF))
                return len + 1;
            if (((sv >> 16) & 0xFF) != ((mv >> 16) & 0xFF))
                return len + 2;
            return len + 3;
        }
        src0 += 4, src1 += 4, len += 4;
    } while (len < 256);

    return 256;
}

/* 16-bit integer comparison - uses 32-bit loads with 16-bit checks on mismatch */
static inline uint32_t compare256_16(const uint8_t *src0, const uint8_t *src1) {
    uint32_t len = 0;

    do {
        uint32_t sv = zng_memread_4(src0);
        uint32_t mv = zng_memread_4(src1);
        if (sv != mv) {
            if ((sv & 0xFFFF) != (mv & 0xFFFF))
                return len + ((sv & 0xFF) == (mv & 0xFF));
            return len + 2 + (((sv >> 16) & 0xFF) == ((mv >> 16) & 0xFF));
        }
        src0 += 4, src1 += 4, len += 4;

        sv = zng_memread_4(src0);
        mv = zng_memread_4(src1);
        if (sv != mv) {
            if ((sv & 0xFFFF) != (mv & 0xFFFF))
                return len + ((sv & 0xFF) == (mv & 0xFF));
            return len + 2 + (((sv >> 16) & 0xFF) == ((mv >> 16) & 0xFF));
        }
        src0 += 4, src1 += 4, len += 4;
    } while (len < 256);

    return 256;
}

#ifdef HAVE_BUILTIN_CTZ
/* 32-bit integer comparison */
static inline uint32_t compare256_32(const uint8_t *src0, const uint8_t *src1) {
    uint32_t len = 0;

    do {
        uint32_t sv, mv, diff;

        sv = zng_memread_4(src0);
        mv = zng_memread_4(src1);

        diff = sv ^ mv;
        if (diff) {
#  if BYTE_ORDER == LITTLE_ENDIAN
            uint32_t match_byte = __builtin_ctz(diff) / 8;
#  else
            uint32_t match_byte = __builtin_clz(diff) / 8;
#  endif
            return len + match_byte;
        }

        src0 += 4, src1 += 4, len += 4;
    } while (len < 256);

    return 256;
}
#endif

#ifdef HAVE_BUILTIN_CTZLL
/* 64-bit integer comparison */
static inline uint32_t compare256_64(const uint8_t *src0, const uint8_t *src1) {
    uint32_t len = 0;

    do {
        uint64_t sv, mv, diff;

        sv = zng_memread_8(src0);
        mv = zng_memread_8(src1);

        diff = sv ^ mv;
        if (diff) {
#  if BYTE_ORDER == LITTLE_ENDIAN
            uint64_t match_byte = __builtin_ctzll(diff) / 8;
#  else
            uint64_t match_byte = __builtin_clzll(diff) / 8;
#  endif
            return len + (uint32_t)match_byte;
        }

        src0 += 8, src1 += 8, len += 8;
    } while (len < 256);

    return 256;
}
#endif
