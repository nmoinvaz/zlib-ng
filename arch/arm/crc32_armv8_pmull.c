/* crc32_armv8_pmull.c -- ARMv8 CRC32 using PMULL (without EOR3)
 * Based on Chromium's zlib PMULL implementation
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zbuild.h"
#include "zutil.h"
#include <string.h>
#include "acle_intrins.h"
#include "neon_intrins.h"
#include "crc32.h"

/* Helper functions for carryless multiplication matching PCLMULQDQ semantics */

/* Multiply low 64-bit of both (PCLMULQDQ imm8=0x00) */
static inline uint64x2_t pmull_lo(uint64x2_t a, uint64x2_t b) {
    return vreinterpretq_u64_p128(vmull_p64(
        vget_lane_p64(vreinterpret_p64_u64(vget_low_u64(a)), 0),
        vget_lane_p64(vreinterpret_p64_u64(vget_low_u64(b)), 0)));
}

/* Multiply high 64-bit of both (PCLMULQDQ imm8=0x11) */
static inline uint64x2_t pmull_hi(uint64x2_t a, uint64x2_t b) {
    return vreinterpretq_u64_p128(vmull_high_p64(vreinterpretq_p64_u64(a), vreinterpretq_p64_u64(b)));
}

Z_INTERNAL Z_TARGET_PMULL uint32_t crc32_armv8_pmull_2acc(uint32_t crc, const uint8_t *buf, size_t len) {
    /* Folding constants for PMULL operations */
    /* k_32: constants for 32-byte folding (x^256 mod P) */
    static const uint64_t ALIGNED_(16) k_32[] = { 0xf1da05aa, 0x81256527 };
    /* k3k4: constants for 16-byte folding (x^128 mod P) */
    static const uint64_t ALIGNED_(16) k3k4[] = { 0x01751997d0, 0x00ccaa009e };

    uint32_t c = ~crc;

    /* Need at least 32 bytes + alignment overhead for PMULL path */
    if (len < 32 + 16) {
        /* Use scalar CRC for small inputs - copied from crc32_armv8.c */
        if ((ptrdiff_t)buf & (sizeof(uint64_t) - 1)) {
            if (len && ((ptrdiff_t)buf & 1)) {
                c = __crc32b(c, *buf++);
                len--;
            }

            if ((len >= sizeof(uint16_t)) && ((ptrdiff_t)buf & (sizeof(uint32_t) - 1))) {
                uint16_t buf2;
                memcpy(&buf2, buf, sizeof(uint16_t));
                c = __crc32h(c, buf2);
                buf += sizeof(uint16_t);
                len -= sizeof(uint16_t);
            }

            if ((len >= sizeof(uint32_t)) && ((ptrdiff_t)buf & (sizeof(uint64_t) - 1))) {
                uint32_t buf4;
                memcpy(&buf4, buf, sizeof(uint32_t));
                c = __crc32w(c, buf4);
                len -= sizeof(uint32_t);
                buf += sizeof(uint32_t);
            }
        }

        while (len >= sizeof(uint64_t)) {
            uint64_t buf8;
            memcpy(&buf8, buf, sizeof(uint64_t));
            c = __crc32d(c, buf8);
            len -= sizeof(uint64_t);
            buf += sizeof(uint64_t);
        }

        if (len & sizeof(uint32_t)) {
            uint32_t buf4;
            memcpy(&buf4, buf, sizeof(uint32_t));
            c = __crc32w(c, buf4);
            buf += sizeof(uint32_t);
        }

        if (len & sizeof(uint16_t)) {
            uint16_t buf2;
            memcpy(&buf2, buf, sizeof(uint16_t));
            c = __crc32h(c, buf2);
            buf += sizeof(uint16_t);
        }

        if (len & sizeof(uint8_t)) {
            c = __crc32b(c, *buf);
        }

        return ~c;
    }

    /* Align to 16 bytes using scalar CRC */
    while (len && ((uintptr_t)buf & 0xF)) {
        c = __crc32b(c, *buf++);
        len--;
    }

    /* Optimized for single PMULL unit: use 2 accumulators processing 32 bytes/iteration */
    uint64x2_t x0, x1, x2, x3, x4, x5;

    /* Load first 32 bytes */
    x1 = vld1q_u64((const uint64_t *)(buf + 0x00));
    x2 = vld1q_u64((const uint64_t *)(buf + 0x10));

    /* XOR initial CRC into first block */
    x1 = veorq_u64(x1, (uint64x2_t)vsetq_lane_u32(c, vdupq_n_u32(0), 0));

    /* Load folding constants k_32 for 32-byte folds */
    x0 = vld1q_u64(k_32);

    buf += 32;
    len -= 32;

    /* Main folding loop: process 32 bytes per iteration with k_32 */
    while (len >= 32) {
        /* Load next 32 bytes */
        x3 = vld1q_u64((const uint64_t *)(buf + 0x00));
        x4 = vld1q_u64((const uint64_t *)(buf + 0x10));

        /* Fold x1 */
        x5 = pmull_lo(x1, x0);
        x1 = pmull_hi(x1, x0);
        x1 = veorq_u64(x1, x5);
        x1 = veorq_u64(x1, x3);

        /* Fold x2 */
        x5 = pmull_lo(x2, x0);
        x2 = pmull_hi(x2, x0);
        x2 = veorq_u64(x2, x5);
        x2 = veorq_u64(x2, x4);

        buf += 32;
        len -= 32;
    }

    /* Switch to k3k4 for folding x1 and x2 together */
    x0 = vld1q_u64(k3k4);

    /* Fold x1 by 16 bytes and combine with x2 */
    x5 = pmull_lo(x1, x0);
    x1 = pmull_hi(x1, x0);
    x1 = veorq_u64(x1, x5);
    x1 = veorq_u64(x1, x2);

    /* Process remaining 16-byte blocks with k3k4 */
    while (len >= 16) {
        x2 = vld1q_u64((const uint64_t *)buf);
        x5 = pmull_lo(x1, x0);
        x1 = pmull_hi(x1, x0);
        x1 = veorq_u64(x1, x2);
        x1 = veorq_u64(x1, x5);
        buf += 16;
        len -= 16;
    }

    /* Final reduction from 128-bit to 32-bit using scalar CRC32 instructions */
    /* This is much simpler than Barrett reduction - just 2 CRC32 operations */
    c = __crc32d(0, vgetq_lane_u64(x1, 0));
    c = __crc32d(c, vgetq_lane_u64(x1, 1));

    /* Process remaining bytes with scalar CRC */
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, buf, 8);
        c = __crc32d(c, v);
        buf += 8;
        len -= 8;
    }
    if (len >= 4) {
        uint32_t v;
        memcpy(&v, buf, 4);
        c = __crc32w(c, v);
        buf += 4;
        len -= 4;
    }
    if (len >= 2) {
        uint16_t v;
        memcpy(&v, buf, 2);
        c = __crc32h(c, v);
        buf += 2;
        len -= 2;
    }
    if (len) {
        c = __crc32b(c, *buf);
    }

    return ~c;
}

/* 4-accumulator version for CPUs with dual PMULL units */
Z_INTERNAL Z_TARGET_PMULL uint32_t crc32_armv8_pmull_4acc(uint32_t crc, const uint8_t *buf, size_t len) {
    /* Folding constants for PMULL operations */
    /* k1k2: constants for 64-byte folding (x^512 mod P) */
    static const uint64_t ALIGNED_(16) k1k2[] = { 0x0154442bd4, 0x01c6e41596 };
    /* k3k4: constants for 16-byte folding (x^128 mod P) */
    static const uint64_t ALIGNED_(16) k3k4[] = { 0x01751997d0, 0x00ccaa009e };

    uint32_t c = ~crc;

    /* Need at least 64 bytes + alignment overhead for PMULL path */
    if (len < 64 + 16) {
        /* Use scalar CRC for small inputs */
        if ((ptrdiff_t)buf & (sizeof(uint64_t) - 1)) {
            if (len && ((ptrdiff_t)buf & 1)) {
                c = __crc32b(c, *buf++);
                len--;
            }

            if ((len >= sizeof(uint16_t)) && ((ptrdiff_t)buf & (sizeof(uint32_t) - 1))) {
                uint16_t buf2;
                memcpy(&buf2, buf, sizeof(uint16_t));
                c = __crc32h(c, buf2);
                buf += sizeof(uint16_t);
                len -= sizeof(uint16_t);
            }

            if ((len >= sizeof(uint32_t)) && ((ptrdiff_t)buf & (sizeof(uint64_t) - 1))) {
                uint32_t buf4;
                memcpy(&buf4, buf, sizeof(uint32_t));
                c = __crc32w(c, buf4);
                len -= sizeof(uint32_t);
                buf += sizeof(uint32_t);
            }
        }

        while (len >= sizeof(uint64_t)) {
            uint64_t buf8;
            memcpy(&buf8, buf, sizeof(uint64_t));
            c = __crc32d(c, buf8);
            len -= sizeof(uint64_t);
            buf += sizeof(uint64_t);
        }

        if (len & sizeof(uint32_t)) {
            uint32_t buf4;
            memcpy(&buf4, buf, sizeof(uint32_t));
            c = __crc32w(c, buf4);
            buf += sizeof(uint32_t);
        }

        if (len & sizeof(uint16_t)) {
            uint16_t buf2;
            memcpy(&buf2, buf, sizeof(uint16_t));
            c = __crc32h(c, buf2);
            buf += sizeof(uint16_t);
        }

        if (len & sizeof(uint8_t)) {
            c = __crc32b(c, *buf);
        }

        return ~c;
    }

    /* Align to 16 bytes using scalar CRC */
    while (len && ((uintptr_t)buf & 0xF)) {
        c = __crc32b(c, *buf++);
        len--;
    }

    /* Use 4 accumulators processing 64 bytes/iteration for dual PMULL units */
    uint64x2_t x0, x1, x2, x3, x4, x5, x6, x7, x8;

    /* Load first 64 bytes */
    x1 = vld1q_u64((const uint64_t *)(buf + 0x00));
    x2 = vld1q_u64((const uint64_t *)(buf + 0x10));
    x3 = vld1q_u64((const uint64_t *)(buf + 0x20));
    x4 = vld1q_u64((const uint64_t *)(buf + 0x30));

    /* XOR initial CRC into first block */
    x1 = veorq_u64(x1, (uint64x2_t)vsetq_lane_u32(c, vdupq_n_u32(0), 0));

    /* Load folding constants k1k2 for 64-byte folds */
    x0 = vld1q_u64(k1k2);

    buf += 64;
    len -= 64;

    /* Main folding loop: process 64 bytes per iteration with k1k2 */
    while (len >= 64) {
        uint64x2_t y5, y6, y7, y8;

        /* Perform carryless multiplication on all 4 accumulators */
        x5 = pmull_lo(x1, x0);
        x6 = pmull_lo(x2, x0);
        x7 = pmull_lo(x3, x0);
        x8 = pmull_lo(x4, x0);

        /* Load next 64 bytes */
        y5 = vld1q_u64((const uint64_t *)(buf + 0x00));
        y6 = vld1q_u64((const uint64_t *)(buf + 0x10));
        y7 = vld1q_u64((const uint64_t *)(buf + 0x20));
        y8 = vld1q_u64((const uint64_t *)(buf + 0x30));

        x1 = pmull_hi(x1, x0);
        x2 = pmull_hi(x2, x0);
        x3 = pmull_hi(x3, x0);
        x4 = pmull_hi(x4, x0);

        /* XOR the results together */
        x1 = veorq_u64(x1, x5);
        x2 = veorq_u64(x2, x6);
        x3 = veorq_u64(x3, x7);
        x4 = veorq_u64(x4, x8);

        /* XOR with loaded data */
        x1 = veorq_u64(x1, y5);
        x2 = veorq_u64(x2, y6);
        x3 = veorq_u64(x3, y7);
        x4 = veorq_u64(x4, y8);

        buf += 64;
        len -= 64;
    }

    /* Switch to k3k4 for folding the 4 accumulators together */
    x0 = vld1q_u64(k3k4);

    /* Fold x1 with x2 */
    x5 = pmull_lo(x1, x0);
    x1 = pmull_hi(x1, x0);
    x1 = veorq_u64(x1, x2);
    x1 = veorq_u64(x1, x5);

    /* Fold x1 with x3 */
    x5 = pmull_lo(x1, x0);
    x1 = pmull_hi(x1, x0);
    x1 = veorq_u64(x1, x3);
    x1 = veorq_u64(x1, x5);

    /* Fold x1 with x4 */
    x5 = pmull_lo(x1, x0);
    x1 = pmull_hi(x1, x0);
    x1 = veorq_u64(x1, x4);
    x1 = veorq_u64(x1, x5);

    /* Process remaining 16-byte blocks with k3k4 */
    while (len >= 16) {
        x2 = vld1q_u64((const uint64_t *)buf);
        x5 = pmull_lo(x1, x0);
        x1 = pmull_hi(x1, x0);
        x1 = veorq_u64(x1, x2);
        x1 = veorq_u64(x1, x5);
        buf += 16;
        len -= 16;
    }

    /* Final reduction from 128-bit to 32-bit using scalar CRC32 instructions */
    c = __crc32d(0, vgetq_lane_u64(x1, 0));
    c = __crc32d(c, vgetq_lane_u64(x1, 1));

    /* Process remaining bytes with scalar CRC */
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, buf, 8);
        c = __crc32d(c, v);
        buf += 8;
        len -= 8;
    }
    if (len >= 4) {
        uint32_t v;
        memcpy(&v, buf, 4);
        c = __crc32w(c, v);
        buf += 4;
        len -= 4;
    }
    if (len >= 2) {
        uint16_t v;
        memcpy(&v, buf, 2);
        c = __crc32h(c, v);
        buf += 2;
        len -= 2;
    }
    if (len) {
        c = __crc32b(c, *buf);
    }

    return ~c;
}

/* Carryless multiply of two 32-bit scalars: a * b (returns 64-bit result in 128-bit vector) */
static inline uint64x2_t clmul_scalar(uint32_t a, uint32_t b) {
  return vreinterpretq_u64_p128(vmull_p64((poly64_t)a, (poly64_t)b));
}

static inline uint32_t xnmodp(uint64_t n) /* x^n mod P, in log(n) time */ {
  uint64_t stack = ~(uint64_t)1;
  uint32_t acc, low;
  for (; n > 191; n = (n >> 1) - 16) {
    stack = (stack << 1) + (n & 1);
  }
  stack = ~stack;
  acc = ((uint32_t)0x80000000) >> (n & 31);
  for (n >>= 5; n; --n) {
    acc = __crc32w(acc, 0);
  }
  while ((low = stack & 1), stack >>= 1) {
    poly8x8_t x = vreinterpret_p8_u64(vmov_n_u64(acc));
    uint64_t y = vgetq_lane_u64(vreinterpretq_u64_p16(vmull_p8(x, x)), 0);
    acc = __crc32d(0, y << low);
  }
  return acc;
}

static inline uint64x2_t crc_shift(uint32_t crc, size_t nbytes) {
  return clmul_scalar(crc, xnmodp(nbytes * 8 - 33));
}

Z_INTERNAL Z_TARGET_PMULL uint32_t crc32_armv8_pmull_3s4x2e(uint32_t crc, const uint8_t *buf, size_t len) {
    uint32_t crc0 = ~crc;

    /* Align to 16-byte boundary for vector path */
    if ((ptrdiff_t)buf & 15) {
        if (len && ((ptrdiff_t)buf & 1)) {
            crc0 = __crc32b(crc0, *buf++);
            len--;
        }

        if ((len >= sizeof(uint16_t)) && ((ptrdiff_t)buf & (sizeof(uint32_t) - 1))) {
            crc0 = __crc32h(crc0, *((uint16_t*)buf));
            buf += sizeof(uint16_t);
            len -= sizeof(uint16_t);
        }

        if ((len >= sizeof(uint32_t)) && ((ptrdiff_t)buf & (sizeof(uint64_t) - 1))) {
            crc0 = __crc32w(crc0, *((uint32_t*)buf));
            len -= sizeof(uint32_t);
            buf += sizeof(uint32_t);
        }

        if (len >= sizeof(uint64_t) && ((ptrdiff_t)buf & (sizeof(uint64_t)))) {
            crc0 = __crc32d(crc0, *((uint64_t*)buf));
            buf += sizeof(uint64_t);
            len -= sizeof(uint64_t);
        }
    }

    /* 4-way scalar CRC + 3-way PMULL folding (112 bytes/iter) */
    if (len >= 112) {
        const uint8_t *end = buf + len;
        size_t blk = len / 112;               /* Number of 112-byte blocks */
        size_t klen = blk * 16;                  /* Scalar stride per CRC lane */
        const uint8_t *buf2 = buf + klen * 4;    /* Vector data starts after scalar lanes */
        const uint8_t *limit = buf + klen - 32;
        uint32_t crc1 = 0, crc2 = 0, crc3 = 0;
        uint64x2_t vc0, vc1, vc2, vc3;
        uint64_t vc;

        /* Load first 3 vector chunks (48 bytes) */
        uint64x2_t x0 = vld1q_u64((const uint64_t*)buf2), y0;
        uint64x2_t x1 = vld1q_u64((const uint64_t*)(buf2 + 16)), y1;
        uint64x2_t x2 = vld1q_u64((const uint64_t*)(buf2 + 32)), y2;
        uint64x2_t k;
        /* k = {x^384 mod P, x^384+64 mod P} for 48-byte fold */
        { static const uint64_t ALIGNED_(16) k_[] = {0x3db1ecdc, 0xaf449247}; k = vld1q_u64(k_); }
        buf2 += 48;

        /* Prefetch first iteration's data */
        uint64x2_t y3 = vld1q_u64((const uint64_t *)(buf2 + 0x00));
        uint64x2_t y4 = vld1q_u64((const uint64_t *)(buf2 + 0x10));
        uint64x2_t y5 = vld1q_u64((const uint64_t *)(buf2 + 0x20));

        /* Main loop: fold vectors + 4-way parallel scalar CRC */
        while (buf <= limit) {
            /* Perform carryless multiplication on all 3 accumulators */
            y0 = pmull_lo(x0, k);
            y1 = pmull_lo(x1, k);
            y2 = pmull_lo(x2, k);

            x0 = pmull_hi(x0, k);
            x1 = pmull_hi(x1, k);
            x2 = pmull_hi(x2, k);

            /* XOR the results together */
            x0 = veorq_u64(x0, y0);
            x1 = veorq_u64(x1, y1);
            x2 = veorq_u64(x2, y2);

            /* XOR with loaded data (from previous iteration) */
            x0 = veorq_u64(x0, y3);
            x1 = veorq_u64(x1, y4);
            x2 = veorq_u64(x2, y5);

            /* 4-way parallel scalar CRC (16 bytes each) */
            crc0 = __crc32d(crc0, *(const uint64_t*)buf);
            crc1 = __crc32d(crc1, *(const uint64_t*)(buf + klen));
            crc2 = __crc32d(crc2, *(const uint64_t*)(buf + klen * 2));
            crc3 = __crc32d(crc3, *(const uint64_t*)(buf + klen * 3));
            crc0 = __crc32d(crc0, *(const uint64_t*)(buf + 8));
            crc1 = __crc32d(crc1, *(const uint64_t*)(buf + klen + 8));
            crc2 = __crc32d(crc2, *(const uint64_t*)(buf + klen * 2 + 8));
            crc3 = __crc32d(crc3, *(const uint64_t*)(buf + klen * 3 + 8));

            buf += 16;
            buf2 += 48;

            /* Prefetch next 48 bytes early for next iteration */
            y3 = vld1q_u64((const uint64_t *)(buf2 + 0x00));
            y4 = vld1q_u64((const uint64_t *)(buf2 + 0x10));
            y5 = vld1q_u64((const uint64_t *)(buf2 + 0x20));
        }

        /* Reduce 3 vectors to 1: x0 = fold(x0, x1), then x0 = fold(x0, x2) */
        { static const uint64_t ALIGNED_(16) k_[] = {0xae689191, 0xccaa009e}; k = vld1q_u64(k_); }

        /* Fold x0 with x1 */
        y0 = pmull_lo(x0, k);
        x0 = pmull_hi(x0, k);
        x0 = veorq_u64(x0, x1);
        x0 = veorq_u64(x0, y0);

        /* Fold x0 with x2 */
        y0 = pmull_lo(x0, k);
        x0 = pmull_hi(x0, k);
        x0 = veorq_u64(x0, x2);
        x0 = veorq_u64(x0, y0);

        /* Process final scalar chunk */
        crc0 = __crc32d(crc0, *(const uint64_t*)buf);
        crc1 = __crc32d(crc1, *(const uint64_t*)(buf + klen));
        crc2 = __crc32d(crc2, *(const uint64_t*)(buf + klen * 2));
        crc3 = __crc32d(crc3, *(const uint64_t*)(buf + klen * 3));
        crc0 = __crc32d(crc0, *(const uint64_t*)(buf + 8));
        crc1 = __crc32d(crc1, *(const uint64_t*)(buf + klen + 8));
        crc2 = __crc32d(crc2, *(const uint64_t*)(buf + klen * 2 + 8));
        crc3 = __crc32d(crc3, *(const uint64_t*)(buf + klen * 3 + 8));

        /* Shift and combine 4 scalar CRCs */
        vc0 = crc_shift(crc0, klen * 3 + blk * 48);
        vc1 = crc_shift(crc1, klen * 2 + blk * 48);
        vc2 = crc_shift(crc2, klen + blk * 48);
        vc3 = crc_shift(crc3, blk * 48);
        vc = vgetq_lane_u64(veorq_u64(veorq_u64(vc0, vc1), veorq_u64(vc2, vc3)), 0);

        /* Final reduction: 128-bit vector + scalar CRCs -> 32-bit */
        crc0 = __crc32d(0, vgetq_lane_u64(x0, 0));
        crc0 = __crc32d(crc0, vc ^ vgetq_lane_u64(x0, 1));
        buf = buf2;
        len = end - buf;
    }
#if 0
    /* Medium buffer path: 2-way PMULL folding (32 bytes/iter) */
    if (len >= 32) {
        uint64x2_t x0 = vld1q_u64((const uint64_t*)buf), y0;
        uint64x2_t x1 = vld1q_u64((const uint64_t*)(buf + 16)), y1;
        uint64x2_t k;
        /* k = {x^32 mod P, x^32+64 mod P} for 32-byte fold */
        { static const uint64_t ALIGNED_(16) k_[] = {0xf1da05aa, 0x81256527}; k = vld1q_u64(k_); }
        x0 = veorq_u64((uint64x2_t){crc0, 0}, x0);  /* Mix in current CRC */
        buf += 32;
        len -= 32;

        /* Fold 32 bytes at a time */
        while (len >= 32) {
            uint64x2_t y2, y3;

            y2 = vld1q_u64((const uint64_t*)buf);
            y3 = vld1q_u64((const uint64_t*)(buf + 16));

            y0 = pmull_lo(x0, k);
            y1 = pmull_lo(x1, k);
            x0 = pmull_hi(x0, k);
            x1 = pmull_hi(x1, k);

            x0 = veorq_u64(x0, y0);
            x1 = veorq_u64(x1, y1);
            x0 = veorq_u64(x0, y2);
            x1 = veorq_u64(x1, y3);

            buf += 32;
            len -= 32;
        }

        /* Reduce 2 vectors to 1 */
        { static const uint64_t ALIGNED_(16) k_[] = {0xae689191, 0xccaa009e}; k = vld1q_u64(k_); }
        y0 = pmull_lo(x0, k);
        x0 = pmull_hi(x0, k);
        x0 = veorq_u64(x0, x1);
        x0 = veorq_u64(x0, y0);

        /* Final reduction: 128-bit -> 32-bit */
        crc0 = __crc32d(0, vgetq_lane_u64(x0, 0));
        crc0 = __crc32d(crc0, vgetq_lane_u64(x0, 1));
    }
#endif
    /* Process remaining bytes */
    while (len >= sizeof(uint64_t)) {
        crc0 = __crc32d(crc0, *((uint64_t*)buf));
        len -= sizeof(uint64_t);
        buf += sizeof(uint64_t);
    }

    if (len & sizeof(uint32_t)) {
        crc0 = __crc32w(crc0, *((uint32_t*)buf));
        buf += sizeof(uint32_t);
    }

    if (len & sizeof(uint16_t)) {
        crc0 = __crc32h(crc0, *((uint16_t*)buf));
        buf += sizeof(uint16_t);
    }

    if (len & sizeof(uint8_t)) {
        crc0 = __crc32b(crc0, *buf);
    }

    return ~crc0;
}
