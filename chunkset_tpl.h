/* chunkset_tpl.h -- inline functions to copy small data chunks.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* Copies a partial chunk, all bytes minus one */
static inline uint8_t* CSUFFIX(chunkcopy_partial)(uint8_t *out, uint8_t const *from, unsigned len) {
    int32_t use_chunk16 = sizeof(chunk_t) > 16 && (len & 16);
    if (use_chunk16) {
        memcpy(out, from, 16);
        out += 16;
        from += 16;
    }
    if (len & 8) {
        memcpy(out, from, 8);
        out += 8;
        from += 8;
    }
    if (len & 4) {
        memcpy(out, from, 4);
        out += 4;
        from += 4;
    }
    if (len & 2) {
        memcpy(out, from, 2);
        out += 2;
        from += 2;
    }
    if (len & 1) {
        *out++ = *from++;
    }
    return out;
}

/* Returns the chunk size */
Z_INTERNAL uint32_t CSUFFIX(chunksize)(void) {
    return sizeof(chunk_t);
}

/* Copy memory in chunks when possible and avoid writing beyond legal output */
static inline uint8_t* CSUFFIX(chunkcopy_static)(uint8_t *out, uint8_t const *from, unsigned len, uint8_t *safe) {
    chunk_t chunk;
    unsigned align = MIN(len, (unsigned)(safe - out) + 1) % sizeof(chunk_t);

    if (align != 0) {
        out = CSUFFIX(chunkcopy_partial)(out, from, align);
        len -= align;
        if (len == 0)
            return out;
        from += align;
    }
    do {
        loadchunk(from, &chunk);
        storechunk(out, &chunk);
        out += sizeof(chunk_t);
        from += sizeof(chunk_t);
        len -= sizeof(chunk_t);
    }
    while (len > 0);
    return out;
}
Z_INTERNAL uint8_t* CSUFFIX(chunkcopy)(uint8_t *out, uint8_t const *from, unsigned len, uint8_t *safe) {
    return CSUFFIX(chunkcopy_static)(out, from, len, safe);
}

/* Perform short copies until distance can be rewritten as being at least
   sizeof chunk_t.

   This assumes that it's OK to overwrite at least the first
   2*sizeof(chunk_t) bytes of output even if the copy is shorter than this.
   This assumption holds because inflate_fast() starts every iteration with at
   least 258 bytes of output space available (258 being the maximum length
   output from a single token; see inflate_fast()'s assumptions below). */
static inline uint8_t* CSUFFIX(chunkunroll_static)(uint8_t *out, unsigned *dist, unsigned *len) {
    unsigned char const *from = out - *dist;
    chunk_t chunk;
    while (*dist < *len && *dist < sizeof(chunk_t)) {
        loadchunk(from, &chunk);
        storechunk(out, &chunk);
        out += *dist;
        *len -= *dist;
        *dist += *dist;
    }
    return out;
}
Z_INTERNAL uint8_t* CSUFFIX(chunkunroll)(uint8_t *out, unsigned *dist, unsigned *len) {
    return CSUFFIX(chunkunroll_static)(out, dist, len);
}

/* Copy DIST bytes from OUT - DIST into OUT + DIST * k, for 0 <= k < LEN/DIST.
   Return OUT + LEN. */
Z_INTERNAL uint8_t* CSUFFIX(chunkmemset)(uint8_t *out, unsigned dist, unsigned len, unsigned left) {
    /* Debug performance related issues when len < sizeof(uint64_t):
       Assert(len >= sizeof(uint64_t), "chunkmemset should be called on larger chunks"); */
    Assert(dist > 0, "cannot have a distance 0");
    unsigned char *from = out - dist;
    chunk_t chunk;
    unsigned sz = sizeof(chunk);

    if (left < 3 * sz) {
        while (len > 0) {
            *out++ = *from++;
            --len;
        }
        return out;
    }
    len = MIN(len, left);
    if (len < sz) {
        if (dist >= len || dist >= sz)
            return CSUFFIX(chunkcopy_partial)(out, from, len);

        do {
            *out++ = *from++;
            --len;
        } while (len != 0);
        return out;
    }

#ifdef HAVE_CHUNKMEMSET_1
    if (dist == 1) {
        chunkmemset_1(from, &chunk);
    } else
#endif
#ifdef HAVE_CHUNKMEMSET_2
    if (dist == 2) {
        chunkmemset_2(from, &chunk);
    } else
#endif
#ifdef HAVE_CHUNKMEMSET_4
    if (dist == 4) {
        chunkmemset_4(from, &chunk);
    } else
#endif
#ifdef HAVE_CHUNKMEMSET_8
    if (dist == 8) {
        chunkmemset_8(from, &chunk);
    } else
#endif
    if (dist == sz) {
        loadchunk(from, &chunk);
    } else if (dist < sz) {
        unsigned char *end = out + len - 1;
        while (len > dist) {
            out = CSUFFIX(chunkcopy_static)(out, from, dist, end);
            len -= dist;
        }
        if (len > 0) {
            out = CSUFFIX(chunkcopy_static)(out, from, len, end);
        }
        return out;
    } else {
        unsigned char *end = out + len - 1;
        out = CSUFFIX(chunkunroll_static)(out, &dist, &len);
        return CSUFFIX(chunkcopy_static)(out, out - dist, len, end);
    }

    unsigned rem = len % sz;
    len -= rem;
    while (len) {
        storechunk(out, &chunk);
        out += sz;
        len -= sz;
    }

    /* Last, deal with the case when LEN is not a multiple of SZ. */
    if (rem) {
        memcpy(out, from, rem);
        out += rem;
    }

    return out;
}
