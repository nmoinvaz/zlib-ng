/* compare256_neon.c - NEON version of compare256
 * Copyright (C) 2022 Nathan Moinvaziri
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zbuild.h"
#include "zmemory.h"
#include "deflate.h"
#include "zendian.h"
#include "fallback_builtins.h"

#if defined(ARM_NEON) && defined(HAVE_BUILTIN_CTZLL)
#include "neon_intrins.h"

static inline uint32_t compare256_neon_v2_static(const uint8_t *src0, const uint8_t *src1) {
    uint32_t len = 0;
    uint64_t sv, mv, diff;

    sv = zng_memread_8(src0);
    mv = zng_memread_8(src1);
    diff = sv ^ mv;

    if (diff) {
#if BYTE_ORDER == LITTLE_ENDIAN
        uint32_t match_byte = (uint32_t)__builtin_ctzll(diff) / 8;
#else
        uint32_t match_byte = (uint32_t)__builtin_clzll(diff) / 8;
#endif
        return match_byte;
    }

    src0 += 8, src1 += 8, len += 8;

    do {
        uint8x16_t a, b, cmp;
        uint64_t lane0, lane1;

        a = vld1q_u8(src0);
        b = vld1q_u8(src1);
        sv = zng_memread_8(src0 + 16);
        mv = zng_memread_8(src1 + 16);

        cmp = veorq_u8(a, b);
        diff = sv ^ mv;

        lane0 = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
        lane1 = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);

        if ((lane0 | lane1 | diff) != 0) {
            if (lane0) {
                uint32_t match_byte = (uint32_t)__builtin_ctzll(lane0) / 8;
                return len + match_byte;
            }
            if (lane1) {
                uint32_t match_byte = (uint32_t)__builtin_ctzll(lane1) / 8;
                return len + 8 + match_byte;
            }
#if BYTE_ORDER == LITTLE_ENDIAN
            uint32_t match_byte = (uint32_t)__builtin_ctzll(diff) / 8;
#else
            uint32_t match_byte = (uint32_t)__builtin_clzll(diff) / 8;
#endif
            return len + 16 + match_byte;
        }

        src0 += 24, src1 += 24, len += 24;
    } while (len < 248);

    sv = zng_memread_8(src0);
    mv = zng_memread_8(src1);
    diff = sv ^ mv;

    if (diff) {
#if BYTE_ORDER == LITTLE_ENDIAN
        uint32_t match_byte = (uint32_t)__builtin_ctzll(diff) / 8;
#else
        uint32_t match_byte = (uint32_t)__builtin_clzll(diff) / 8;
#endif
        return 240 + match_byte;
    }

    return 256;
}


static inline uint32_t compare256_v3_neon_static(const uint8_t *src0, const uint8_t *src1) {
    uint8x16_t a, b, cmp;
    uint64_t lane;

    /* Do the first load unaligned, than all subsequent ones we have at least
     * one aligned load. Sadly aligning both loads is probably unrealistic */
    a = vld1q_u8(src0);
    b = vld1q_u8(src1);
    cmp = veorq_u8(a, b);

    lane = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
    if (lane) {
        uint32_t match_byte = (uint32_t)__builtin_ctzll(lane) / 8;
        return  match_byte;
    }
    lane = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
    if (lane) {
        uint32_t match_byte = (uint32_t)__builtin_ctzll(lane) / 8;
        return 8 + match_byte;
    }

    const uint8_t *last0 = src0 + 240;
    const uint8_t *last1 = src1 + 240;
    int align_offset = ((uintptr_t)src0) & 15;
    int align_adv = 16 - align_offset;
    size_t len = align_adv;
    src0 += align_adv;
    src1 += align_adv;

    for (int i = 0; i < 15; ++i) {
        a = vld1q_u8(src0);
        b = vld1q_u8(src1);
        cmp = veorq_u8(a, b);

        lane = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
        if (lane) {
            uint32_t match_byte = (uint32_t)__builtin_ctzll(lane) / 8;
            return len + match_byte;
        }
        lane = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
        if (lane) {
            uint32_t match_byte = (uint32_t)__builtin_ctzll(lane) / 8;
            return len + 8 + match_byte;
        }

        len += 16, src0 += 16, src1 += 16;
    }

    if (align_offset) {
        a = vld1q_u8(last0);
        b = vld1q_u8(last1);
        cmp = veorq_u8(a, b);

        lane = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
        if (lane) {
            uint32_t match_byte = (uint32_t)__builtin_ctzll(lane) / 8;
            return 240 + match_byte;
        }
        lane = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
        if (lane) {
            uint32_t match_byte = (uint32_t)__builtin_ctzll(lane) / 8;
            return 248 + match_byte;
        }
    }

    return 256;
}

static inline uint32_t compare256_neon_static(const uint8_t *src0, const uint8_t *src1) {
    uint32_t len = 0;

    do {
        uint8x16_t a, b, cmp;
        uint64_t lane;

        a = vld1q_u8(src0);
        b = vld1q_u8(src1);

        cmp = veorq_u8(a, b);

        lane = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
        if (lane) {
            uint32_t match_byte = (uint32_t)__builtin_ctzll(lane) / 8;
            return len + match_byte;
        }
        len += 8;
        lane = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
        if (lane) {
            uint32_t match_byte = (uint32_t)__builtin_ctzll(lane) / 8;
            return len + match_byte;
        }
        len += 8;

        src0 += 16, src1 += 16;
    } while (len < 256);

    return 256;
}

Z_INTERNAL uint32_t compare256_neon(const uint8_t *src0, const uint8_t *src1) {
    return compare256_neon_static(src0, src1);
}

Z_INTERNAL uint32_t compare256_v2_neon(const uint8_t *src0, const uint8_t *src1) {
    return compare256_neon_v2_static(src0, src1);
}

Z_INTERNAL uint32_t compare256_v3_neon(const uint8_t *src0, const uint8_t *src1) {
    return compare256_v3_neon_static(src0, src1);
}

#define LONGEST_MATCH       longest_match_neon
#define COMPARE256          compare256_neon_static

#include "match_tpl.h"

#define LONGEST_MATCH_SLOW
#define LONGEST_MATCH       longest_match_slow_neon
#define COMPARE256          compare256_neon_static

#include "match_tpl.h"

#endif
