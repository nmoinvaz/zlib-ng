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

/* Multiply low 64-bit of a with high 64-bit of b (PCLMULQDQ imm8=0x01) */
static inline uint64x2_t pmull_01(uint64x2_t a, uint64x2_t b) {
    return vreinterpretq_u64_p128(vmull_p64(
        vget_lane_p64(vreinterpret_p64_u64(vget_low_u64(a)), 0),
        vget_lane_p64(vreinterpret_p64_u64(vget_high_u64(b)), 0)));
}

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

Z_INTERNAL Z_TARGET_CRC uint32_t crc32_armv8_pmull(uint32_t crc, const uint8_t *buf, size_t len) {
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
