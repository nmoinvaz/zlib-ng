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

    while (len + 4 * vl <= 256) {
        svuint8_t v0 = svld1_u8(pg_all, src0);
        svuint8_t v1 = svld1_u8(pg_all, src1);
        svuint8_t v2 = svld1_u8(pg_all, src0 + vl);
        svuint8_t v3 = svld1_u8(pg_all, src1 + vl);
        svuint8_t v4 = svld1_u8(pg_all, src0 + 2 * vl);
        svuint8_t v5 = svld1_u8(pg_all, src1 + 2 * vl);
        svuint8_t v6 = svld1_u8(pg_all, src0 + 3 * vl);
        svuint8_t v7 = svld1_u8(pg_all, src1 + 3 * vl);

        svbool_t cmp0 = svcmpne_u8(pg_all, v0, v1);
        svbool_t cmp1 = svcmpne_u8(pg_all, v2, v3);
        svbool_t cmp2 = svcmpne_u8(pg_all, v4, v5);
        svbool_t cmp3 = svcmpne_u8(pg_all, v6, v7);

        svbool_t cmp_01 = svorr_b_z(pg_all, cmp0, cmp1);
        svbool_t cmp_23 = svorr_b_z(pg_all, cmp2, cmp3);
        svbool_t cmp_all = svorr_b_z(pg_all, cmp_01, cmp_23);

        if (svptest_any(pg_all, cmp_all)) {
            if (svptest_any(pg_all, cmp0))
                return len + (uint32_t)svcntp_b8(pg_all, svbrkb_b_z(pg_all, cmp0));
            if (svptest_any(pg_all, cmp1))
                return len + (uint32_t)vl + (uint32_t)svcntp_b8(pg_all, svbrkb_b_z(pg_all, cmp1));
            if (svptest_any(pg_all, cmp2))
                return len + (uint32_t)(2 * vl) + (uint32_t)svcntp_b8(pg_all, svbrkb_b_z(pg_all, cmp2));
            return len + (uint32_t)(3 * vl) + (uint32_t)svcntp_b8(pg_all, svbrkb_b_z(pg_all, cmp3));
        }

        src0 += 4 * vl;
        src1 += 4 * vl;
        len += (uint32_t)(4 * vl);
    }

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
    }

    if (len < 256) {
        svbool_t pg = svwhilelt_b8_u32(len, 256);
        svuint8_t v0 = svld1_u8(pg, src0);
        svuint8_t v1 = svld1_u8(pg, src1);
        svbool_t cmp = svcmpne_u8(pg, v0, v1);

        if (svptest_any(pg, cmp)) {
            return len + (uint32_t)svcntp_b8(pg, svbrkb_b_z(pg, cmp));
        }
    }

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
