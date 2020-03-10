/* deflate_p.h -- Private inline functions and macros shared with more than
 *                one deflate method
 *
 * Copyright (C) 1995-2013 Jean-loup Gailly and Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 */

#ifndef DEFLATE_P_H
#define DEFLATE_P_H

/* Forward declare common non-inlined functions declared in deflate.c */

#ifdef ZLIB_DEBUG
void check_match(deflate_state *s, IPos start, IPos match, int length);
#else
#define check_match(s, start, match, length)
#endif
void flush_pending(PREFIX3(stream) *strm);

/* ===========================================================================
 * Flush the current block, with given end-of-file flag.
 * IN assertion: strstart is set to the end of the current match.
 */
#define FLUSH_BLOCK_ONLY(s, last) { \
    zng_tr_flush_block(s, (s->block_start >= 0L ? \
                   (char *)&s->window[(unsigned)s->block_start] : \
                   NULL), \
                   (unsigned long)((long)s->strstart - s->block_start), \
                   (last)); \
    s->block_start = s->strstart; \
    flush_pending(s->strm); \
    Tracev((stderr, "[FLUSH]")); \
}

/* Same but force premature exit if necessary. */
#define FLUSH_BLOCK(s, last) { \
    FLUSH_BLOCK_ONLY(s, last); \
    if (s->strm->avail_out == 0) return (last) ? finish_started : need_more; \
}

/* Maximum stored block length in deflate format (not including header). */
#define MAX_STORED 65535

/* Minimum of a and b. */
#define MIN(a, b) ((a) > (b) ? (b) : (a))

#endif
