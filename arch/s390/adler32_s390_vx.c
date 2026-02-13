/* Adler32 for S390 using VX (Vector Extension) instructions.
 * Copyright (C) 2024 Contributors to the zlib-ng project
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 * Based on adler32_power8.c by Rogerio Alves <rcardoso@linux.ibm.com>
 * Ported to S390 VX intrinsics.
 */

#ifdef S390_VX

#include "zbuild.h"
#include "adler32_p.h"

#include <vecintrin.h>

/* Vector across sum unsigned int (horizontal sum of 4 uint32 lanes). */
static inline vector unsigned int vec_sumsu(vector unsigned int __a, vector unsigned int __b) {
    __b = vec_sld(__a, __a, 8);
    __b = vec_add(__b, __a);
    __a = vec_sld(__b, __b, 4);
    __a = vec_add(__a, __b);
    return __a;
}

Z_FORCEINLINE static uint32_t adler32_impl(uint32_t adler, const uint8_t *buf, size_t len) {
    uint32_t s1 = adler & 0xffff;
    uint32_t s2 = (adler >> 16) & 0xffff;

    /* in case user likes doing a byte at a time, keep it fast */
    if (UNLIKELY(len == 1))
        return adler32_copy_len_1(s1, NULL, buf, s2, 0);

    /* This is faster than VX code for len < 64. */
    if (len < 64)
        return adler32_copy_len_64(s1, NULL, buf, len, s2, 0);

    /* Use S390 VX instructions for len >= 64. */
    const vector unsigned int v_zeros = { 0, 0, 0, 0 };
    const vector unsigned char v_mul = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7,
         6, 5, 4, 3, 2, 1};
    const vector unsigned char vsh = vec_splat_u8(4);
    const vector unsigned int vmask = {0xffffffff, 0x0, 0x0, 0x0};
    vector unsigned int vs1 = { 0, 0, 0, 0 };
    vector unsigned int vs2 = { 0, 0, 0, 0 };
    vector unsigned int vs1_save = { 0, 0, 0, 0 };
    vector unsigned int vsum1, vsum2;
    vector unsigned char vbuf;
    int n;

    vs1 = vec_insert(s1, vs1, 0);
    vs2 = vec_insert(s2, vs2, 0);

    /* Do length bigger than NMAX in blocks of NMAX size. */
    while (len >= NMAX) {
        len -= NMAX;
        n = NMAX / 16;
        do {
            vbuf = vec_xl(0, (unsigned char *)buf);
            vsum1 = vec_sum4(vbuf, v_zeros);          /* sum(i=1 to 16) buf[i]. */
            vsum2 = vec_msum(vbuf, v_mul, v_zeros);   /* sum(i=1 to 16) buf[i]*(16-i+1). */
            /* Save vs1. */
            vs1_save = vec_add(vs1_save, vs1);
            /* Accumulate the sums. */
            vs1 = vec_add(vsum1, vs1);
            vs2 = vec_add(vsum2, vs2);

            buf += 16;
        } while (--n);
        /* Once each block of NMAX size. */
        vs1 = vec_sumsu(vs1, vsum1);
        vs1_save = vec_sll(vs1_save, vsh); /* 16*vs1_save. */
        vs2 = vec_add(vs1_save, vs2);
        vs2 = vec_sumsu(vs2, vsum2);

        /* vs1[0] = (s1_i + sum(i=1 to 16)buf[i]) mod 65521. */
        s1 = vec_extract(vs1, 0) % BASE;
        /* vs2[0] = s2_i + 16*s1_save +
           sum(i=1 to 16)(16-i+1)*buf[i] mod 65521. */
        s2 = vec_extract(vs2, 0) % BASE;

        vs1 = vec_and(vec_insert(s1, v_zeros, 0), vmask);
        vs2 = vec_and(vec_insert(s2, v_zeros, 0), vmask);
        vs1_save = v_zeros;
    }

    /* len is less than NMAX one modulo is needed. */
    if (len >= 16) {
        while (len >= 16) {
            len -= 16;

            vbuf = vec_xl(0, (unsigned char *)buf);

            vsum1 = vec_sum4(vbuf, v_zeros);          /* sum(i=1 to 16) buf[i]. */
            vsum2 = vec_msum(vbuf, v_mul, v_zeros);   /* sum(i=1 to 16) buf[i]*(16-i+1). */
            /* Save vs1. */
            vs1_save = vec_add(vs1_save, vs1);
            /* Accumulate the sums. */
            vs1 = vec_add(vsum1, vs1);
            vs2 = vec_add(vsum2, vs2);

            buf += 16;
        }
        /* Since the size will be always less than NMAX we do this once. */
        vs1 = vec_sumsu(vs1, vsum1);
        vs1_save = vec_sll(vs1_save, vsh); /* 16*vs1_save. */
        vs2 = vec_add(vs1_save, vs2);
        vs2 = vec_sumsu(vs2, vsum2);
    }
    /* Copy result back to s1, s2 (mod 65521). */
    s1 = vec_extract(vs1, 0) % BASE;
    s2 = vec_extract(vs2, 0) % BASE;

    /* Process tail (len < 16). */
    return adler32_copy_len_16(s1, NULL, buf, len, s2, 0);
}

Z_INTERNAL uint32_t adler32_s390_vx(uint32_t adler, const uint8_t *buf, size_t len) {
    return adler32_impl(adler, buf, len);
}

Z_INTERNAL uint32_t adler32_copy_s390_vx(uint32_t adler, uint8_t *dst, const uint8_t *buf, size_t len) {
    adler = adler32_impl(adler, buf, len);
    memcpy(dst, buf, len);
    return adler;
}

#endif /* S390_VX */
