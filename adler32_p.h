/* adler32_p.h -- Private inline functions and macros shared with
 *                different computation of the Adler-32 checksum
 *                of a data stream.
 * Copyright (C) 1995-2011, 2016 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef ADLER32_P_H
#define ADLER32_P_H

#define BASE 65521U     /* largest prime smaller than 65536 */
#define NMAX 5552
/* NMAX is the largest n such that 255n(n+1)/2 + (n+1)(BASE-1) <= 2^32-1 */

#define ADLER_DO1(sum1, sum2, buf, i)  {(sum1) += buf[(i)]; (sum2) += (sum1);}
#define ADLER_DO2(sum1, sum2, buf, i)  {ADLER_DO1(sum1, sum2, buf, i); ADLER_DO1(sum1, sum2, buf, i+1);}
#define ADLER_DO4(sum1, sum2, buf, i)  {ADLER_DO2(sum1, sum2, buf, i); ADLER_DO2(sum1, sum2, buf, i+2);}
#define ADLER_DO8(sum1, sum2, buf, i)  {ADLER_DO4(sum1, sum2, buf, i); ADLER_DO4(sum1, sum2, buf, i+4);}
#define ADLER_DO16(sum1, sum2, buf)    {ADLER_DO8(sum1, sum2, buf, 0); ADLER_DO8(sum1, sum2, buf, 8);}

Z_FORCEINLINE static void adler32_copy_small(uint32_t *Z_RESTRICT adler, uint8_t *dst, const uint8_t *buf, size_t len,
                                             uint32_t *Z_RESTRICT sum2, const int MAX_LEN, const int COPY) {
    /* GCC at -O2 on x86 hoists all byte loads in DO8/DO16 simultaneously, requiring 12+ GPRs
     * which overflows x86-64's 9 caller-saved registers and forces callee-saved spills.
     * Clang handles this fine, so only GCC is restricted to a DO4 loop. */
#if defined(ARCH_X86) && defined(__GNUC__) && !defined(__clang__)
    while (len >= 4) {
        if (COPY) {
            memcpy(dst, buf, 4);
            dst += 4;
        }
        len -= 4;
        ADLER_DO4(*adler, *sum2, buf, 0);
        buf += 4;
    }
#else
    if (MAX_LEN >= 16) {
        while (len >= 16) {
            if (COPY) {
                memcpy(dst, buf, 16);
                dst += 16;
            }
            len -= 16;
            ADLER_DO16(*adler, *sum2, buf);
            buf += 16;
        }
        if (len & 8) {
            if (COPY) {
                memcpy(dst, buf, 8);
                dst += 8;
            }
            ADLER_DO8(*adler, *sum2, buf, 0);
            buf += 8;
        }
    } else {
        while (len >= 8) {
            if (COPY) {
                memcpy(dst, buf, 8);
                dst += 8;
            }
            len -= 8;
            ADLER_DO8(*adler, *sum2, buf, 0);
            buf += 8;
        }
    }
    if (len & 4) {
        if (COPY) {
            memcpy(dst, buf, 4);
            dst += 4;
        }
        ADLER_DO4(*adler, *sum2, buf, 0);
        buf += 4;
    }
#endif
    if (len & 2) {
        if (COPY) {
            memcpy(dst, buf, 2);
            dst += 2;
        }
        ADLER_DO2(*adler, *sum2, buf, 0);
        buf += 2;
    }
    if (len & 1) {
        if (COPY)
            *dst = *buf;
        ADLER_DO1(*adler, *sum2, buf, 0);
    }
}

Z_FORCEINLINE static uint32_t adler32_copy_tail(uint32_t adler, uint8_t *dst, const uint8_t *buf, size_t len,
                                                uint32_t sum2, const int REBASE, const int MAX_LEN, const int COPY) {
    if (len) {
        adler32_copy_small(&adler, dst, buf, len, &sum2, MAX_LEN, COPY);
    }
    if (REBASE) {
        adler %= BASE;
        sum2 %= BASE;
    }
    /* D = B * 65536 + A, see: https://en.wikipedia.org/wiki/Adler-32. */
    return adler | (sum2 << 16);
}

#endif /* ADLER32_P_H */
