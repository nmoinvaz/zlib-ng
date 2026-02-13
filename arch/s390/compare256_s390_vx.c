/* compare256_s390_vx.c - S390 VX version of compare256
 * Copyright (C) 2024 Contributors to the zlib-ng project
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifdef S390_VX

#include <vecintrin.h>
#include "zbuild.h"
#include "zmemory.h"
#include "deflate.h"

/* S390 is big-endian: first byte is in the MSB, so use CLZ to find the first mismatch */
static inline uint32_t compare256_s390_vx_static(const uint8_t *src0, const uint8_t *src1) {
    uint32_t len = 0;

    do {
        vector unsigned char a, b, cmp;
        uint64_t lane;

        a = vec_xl(0, src0);
        b = vec_xl(0, src1);

        cmp = (vector unsigned char)vec_xor(a, b);

        lane = vec_extract((vector unsigned long long)cmp, 0);
        if (lane)
            return len + (uint32_t)__builtin_clzll(lane) / 8;
        len += 8;
        lane = vec_extract((vector unsigned long long)cmp, 1);
        if (lane)
            return len + (uint32_t)__builtin_clzll(lane) / 8;
        len += 8;

        src0 += 16;
        src1 += 16;
    } while (len < 256);

    return 256;
}

Z_INTERNAL uint32_t compare256_s390_vx(const uint8_t *src0, const uint8_t *src1) {
    return compare256_s390_vx_static(src0, src1);
}

#define LONGEST_MATCH       longest_match_s390_vx
#define COMPARE256          compare256_s390_vx_static

#include "match_tpl.h"

#define LONGEST_MATCH_SLOW
#define LONGEST_MATCH       longest_match_slow_s390_vx
#define COMPARE256          compare256_s390_vx_static

#include "match_tpl.h"

#endif /* S390_VX */
