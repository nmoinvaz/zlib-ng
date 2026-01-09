/* compare256_sve.c -- SVE version of compare256
 * Copyright (C) 2026 Nathan Moinvaziri
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zbuild.h"
#include "zmemory.h"
#include "deflate.h"

#if defined(ARM_SVE)
#include <arm_sve.h>

static inline uint32_t compare256_sve_static(const uint8_t *src0, const uint8_t *src1) {
    uint32_t len = 0;
    uint64_t vl = svcntb();
    svbool_t pg_all = svptrue_b8();

    while (len + vl <= 256) {
        svuint8_t v0 = svld1_u8(pg_all, src0);
        svuint8_t v1 = svld1_u8(pg_all, src1);
        svbool_t cmp = svcmpne_u8(pg_all, v0, v1);

        if (svptest_any(pg_all, cmp)) {
            return len + (uint32_t)svcntp_b8(pg_all, svbrkb_b_z(pg_all, cmp));
        }

        src0 += vl;
        src1 += vl;
        len += (uint32_t)vl;


        v0 = svld1_u8(pg_all, src0);
        v1 = svld1_u8(pg_all, src1);
        cmp = svcmpne_u8(pg_all, v0, v1);

        if (svptest_any(pg_all, cmp)) {
            return len + (uint32_t)svcntp_b8(pg_all, svbrkb_b_z(pg_all, cmp));
        }

        src0 += vl;
        src1 += vl;
        len += (uint32_t)vl;
    } while (len < 256);

    return 256;
}

Z_INTERNAL uint32_t compare256_sve(const uint8_t *src0, const uint8_t *src1) {
    return compare256_sve_static(src0, src1);
}

#define LONGEST_MATCH       longest_match_sve
#define COMPARE256          compare256_sve_static

#include "match_tpl.h"

#define LONGEST_MATCH_SLOW
#define LONGEST_MATCH       longest_match_slow_sve
#define COMPARE256          compare256_sve_static

#include "match_tpl.h"

#endif
