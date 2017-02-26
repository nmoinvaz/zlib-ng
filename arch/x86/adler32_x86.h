/* adler32_x86.h -- compute the Adler-32 checksum of a data stream
 * Copyright (C) 1995-2011 Mark Adler
 * Copyright (C) 2017 Mika T. Lindqvist
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* @(#) $Id$ */

#ifndef __ADLER32_X86__
#define __ADLER32_X86__

#include "zlib.h"

uint32_t adler32_x86(uint32_t adler, const unsigned char *buf, size_t len);

static inline void sse_handle_head_or_tail(uint32_t *pair, const unsigned char *buf, size_t len)
{
  unsigned int i;
  for (i = 0; i < len; ++i) {
    pair[0] += buf[i];
    pair[1] += pair[0];
  }
}

#endif
