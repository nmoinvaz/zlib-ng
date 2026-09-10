/* crc32_armv8.c -- compute the CRC-32 of a data stream
 * Copyright (C) 1995-2006, 2010, 2011, 2012 Mark Adler
 * Copyright (C) 2016 Yang Zhang
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifdef ARM_CRC32

#include "zbuild.h"
#include "acle_intrins.h"
#include "crc32_armv8_p.h"

#include "arch/shared/crc32_hw_copy_impl_tpl.h"

/* The crc32 instruction retires one dependent operation every few cycles, so
 * a single chain leaves the unit mostly idle. Run three chains over adjacent
 * lanes and merge them with precomputed x^n mod P constants. The multiply
 * runs in plain shift and xor arithmetic, this kernel serves CPUs without
 * PMULL, and the products reduce through one final crc32 instruction, so the
 * constants carry the same x^-33 adjustment the wide kernel's tables use.
 * Two fixed lane lengths keep the constant set at four words while engaging
 * within the 16 to 32 KiB calls the streaming paths make. */

/* {x^((2*L+8)*8-33), x^((L+8)*8-33)} mod P for the two lane lengths,
   regenerate with makecrct.c if the polynomial ever changes */
#define BRAID_LANE_LONG  4096u
#define BRAID_LANE_SHORT 1024u
static const uint32_t crc_braid_shift_long[2]  = {0xb566f6e2, 0x5ad8a92c};
static const uint32_t crc_braid_shift_short[2] = {0x88b6ba63, 0xf891f16f};

Z_FORCEINLINE static Z_TARGET_CRC uint64_t crc_braid_mul(uint32_t crc, uint32_t c) {
    uint64_t r = 0;
    for (int i = 0; i < 32; i++)
        if (c & (1u << i))
            r ^= (uint64_t)crc << i;
    return r;
}

/* One 3*lane+8 byte pass, returns the raw running CRC */
Z_FORCEINLINE static Z_TARGET_CRC uint32_t crc_braid_pass(uint32_t c, uint8_t *dst, const uint8_t *src,
                                                          size_t lane, const uint32_t shift[2], const int COPY) {
    uint32_t c0 = c, c1 = 0, c2 = 0;
    const uint8_t *p1 = src + lane, *p2 = src + 2 * lane;

    for (size_t i = 0; i < lane; i += 8) {
        uint64_t v0, v1, v2;
        memcpy(&v0, src + i, 8);
        memcpy(&v1, p1 + i, 8);
        memcpy(&v2, p2 + i, 8);
        if (COPY) {
            memcpy(dst + i, &v0, 8);
            memcpy(dst + lane + i, &v1, 8);
            memcpy(dst + 2 * lane + i, &v2, 8);
        }
        c0 = __crc32d(c0, v0);
        c1 = __crc32d(c1, v1);
        c2 = __crc32d(c2, v2);
    }
    uint64_t vc = crc_braid_mul(c0, shift[0]) ^ crc_braid_mul(c1, shift[1]);
    uint64_t vf;
    memcpy(&vf, src + 3 * lane, 8);
    if (COPY)
        memcpy(dst + 3 * lane, &vf, 8);
    return __crc32d(c2, vf ^ vc);
}

Z_FORCEINLINE static Z_TARGET_CRC uint32_t crc32_braid_impl(uint32_t crc, uint8_t *dst, const uint8_t *src,
                                                            size_t len, const int COPY) {
    uint32_t c = ~crc;

    while (len >= 3 * BRAID_LANE_LONG + 8) {
        c = crc_braid_pass(c, dst, src, BRAID_LANE_LONG, crc_braid_shift_long, COPY);
        src += 3 * BRAID_LANE_LONG + 8;
        if (COPY)
            dst += 3 * BRAID_LANE_LONG + 8;
        len -= 3 * BRAID_LANE_LONG + 8;
    }
    while (len >= 3 * BRAID_LANE_SHORT + 8) {
        c = crc_braid_pass(c, dst, src, BRAID_LANE_SHORT, crc_braid_shift_short, COPY);
        src += 3 * BRAID_LANE_SHORT + 8;
        if (COPY)
            dst += 3 * BRAID_LANE_SHORT + 8;
        len -= 3 * BRAID_LANE_SHORT + 8;
    }
    return crc32_hw_copy_impl(~c, dst, src, len, COPY);
}

Z_INTERNAL Z_TARGET_CRC uint32_t crc32_armv8(uint32_t crc, const uint8_t *buf, size_t len) {
    return crc32_braid_impl(crc, NULL, buf, len, 0);
}

Z_INTERNAL Z_TARGET_CRC uint32_t crc32_copy_armv8(uint32_t crc, uint8_t *dst, const uint8_t *src, size_t len) {
#if OPTIMAL_CMP >= 32
    return crc32_braid_impl(crc, dst, src, len, 1);
#else
    /* Without unaligned access, interleaved stores get decomposed into byte ops */
    crc = crc32_armv8(crc, src, len);
    memcpy(dst, src, len);
    return crc;
#endif
}
#endif
