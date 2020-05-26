/* functable.h -- Struct containing function pointers to optimized functions
 * Copyright (C) 2017 Hans Kristian Rosbach
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef FUNCTABLE_H_
#define FUNCTABLE_H_

#include "deflate.h"

struct functable_s {
    wpos_t   (* insert_string)      (deflate_state *const s, const wpos_t str, unsigned int count);
    wpos_t   (* quick_insert_string)(deflate_state *const s, const wpos_t str);
    uint32_t (* adler32)            (uint32_t adler, const unsigned char *buf, size_t len);
    uint32_t (* crc32)              (uint32_t crc, const unsigned char *buf, uint64_t len);
    void     (* slide_hash)         (deflate_state *s);
    wlen_t   (* compare258)         (const unsigned char *src0, const unsigned char *src1);
    wlen_t   (* longest_match)      (deflate_state *const s, wpos_t cur_match);
};

ZLIB_INTERNAL extern __thread struct functable_s functable;


#endif
