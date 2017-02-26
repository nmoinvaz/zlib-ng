/* adler32_x86.c -- compute the Adler-32 checksum of a data stream
 * Copyright (C) 1995-2011 Mark Adler
 * Copyright (C) 2017 Mika T. Lindqvist
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* @(#) $Id$ */

#include "adler32_x86.h"
#include "x86.h"

extern uint32_t adler32_c(uint32_t adler, const unsigned char *buf, size_t len);
#ifdef X86_SSE3_ADLER32
extern uint32_t adler32_sse3(uint32_t adler, const unsigned char *buf, size_t len);
#endif
#ifdef X86_SSSE3_ADLER32
extern uint32_t adler32_ssse3(uint32_t adler, const unsigned char *buf, size_t len);
#endif
#ifdef X86_SSE4_ADLER32
extern uint32_t adler32_sse4(uint32_t adler, const unsigned char *buf, size_t len);
#endif
#ifdef X86_XOP_ADLER32
extern uint32_t adler32_xop(uint32_t adler, const unsigned char *buf, size_t len);
#endif

uint32_t adler32_x86(uint32_t adler, const unsigned char *buf, size_t len) {
#ifdef X86_XOP_ADLER32
    if (x86_cpu_has_xop) {
        return adler32_xop(adler, buf, len);
    }
#endif
#ifdef X86_SSE4_ADLER32
    if (x86_cpu_has_sse41) {
        return adler32_sse4(adler, buf, len);
    }
#endif
#ifdef X86_SSSE3_ADLER32
    if (x86_cpu_has_ssse3) {
        return adler32_ssse3(adler, buf, len);
    }
#endif
#ifdef X86_SSE3_ADLER32
    if (x86_cpu_has_sse3) {
        return adler32_sse3(adler, buf, len);
    }
#endif

    return adler32_c(adler, buf, len);
}
