/* adler32_ssse3.c -- compute the Adler-32 checksum of a data stream
 * Copyright (C) 1995-2011 Mark Adler
 * Copyright (C) 2017 Mika T. Lindqvist
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifdef X86_SSSE3_ADLER32

#include "zbuild.h"
#include "zutil.h"

#include "adler32_p.h"
#include "adler32_ssse3.h"
#include "adler32_intrin.h"
#include "adler32_x86.h"

#ifdef _MSC_VER
#  include <intrin.h>
#else
#  include <x86intrin.h>
#endif

uint32_t adler32_ssse3(uint32_t adler, const unsigned char *buf, size_t len) {
    uint32_t sum2;
    uint32_t pair[2];
    int n = NMAX;
    unsigned int done = 0, i;
    unsigned int al = 0;

    /* initial Adler-32 value (deferred check for len == 1 speed) */
    if (UNLIKELY(buf == NULL))
        return 1L;
    
    /* split Adler-32 into component sums */
    sum2 = (adler >> 16) & 0xffff;
    adler &= 0xffff;

    pair[0] = adler;
    pair[1] = sum2;

    /* Align buffer */
    if ((uintptr_t)buf & 0xf) {
        al = 16-((uintptr_t)buf & 0xf);
        if (al > len) {
            al = len;
        }
        sse_handle_head_or_tail(pair, buf, al);
        pair[0] %= BASE;
        pair[1] %= BASE;

        done += al;
    }
    for (i = al; i < len; i += n) {
        if ((i + n) > len)
            n = (int)(len - i);

        if (n < 16)
            break;

        sse_accum32(pair, buf + i, n / 16);
        pair[0] %= BASE;
        pair[1] %= BASE;

        done += (n / 16) * 16;
    }

    /* Handle the tail elements. */
    if (done < len) {
        sse_handle_head_or_tail(pair, (buf + done), len - done);
        pair[0] %= BASE;
        pair[1] %= BASE;
    }

    /* D = B * 65536 + A, see: https://en.wikipedia.org/wiki/Adler-32. */
    return (pair[1] << 16) | pair[0];
}
#endif
