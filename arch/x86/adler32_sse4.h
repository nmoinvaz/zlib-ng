/* adler32_sse4.h -- compute the Adler-32 checksum of a data stream
 * Copyright (C) 1995-2011 Mark Adler
 * Copyright (C) 2017 Mika T. Lindqvist
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef __ADLER32_SSE4__
#define __ADLER32_SSE4__

uint32_t adler32_sse4(uint32_t adler, const unsigned char *buf, size_t len);

#endif
