/* gzblockread.c -- the parallel reader for gzip members made of independent deflate blocks
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "gzblock_p.h"
#include "fallback_builtins.h"


enum { R_HEADER, R_PASSTHRU, R_STREAM, R_BLOCKS, R_MEMBER_END, R_END, R_ERROR };

struct gzblock_reader_s {
    gzblock_read_fn read;
    void *ctx;
    int nthreads;
    uint32_t block_hint;      /* block size to assume when a header records none */
    int state;
    int members;              /* gzip members finished so far */
    int err;                  /* zlib error code once failed */
    char msg[MSG_LEN];

    membuf buf;               /* input in hand, not yet consumed by the current stage */
    int eof;                  /* the read callback returned 0 */

    const uint8_t *out_p;     /* output being handed out */
    size_t out_n;
    slot_t *out_slot;         /* slot to release once out_p is consumed, or NULL */

    PREFIX3(stream) z;        /* plain inflate */
    int z_init;
    uint8_t *obuf;            /* IO_CHUNK, output of z, or bytes passed through */

    uint32_t block_size;      /* block mode */
    int paired;               /* boundaries are marker pairs, lone markers are not candidates */
    size_t max_seg;           /* longest a compressed block can be */
    membuf hdr;               /* this member's header, kept for the fallback */
    size_t scanned;           /* bytes of buf already scanned for markers */
    int cut_all;              /* the scanner handed out the member's last segment */
    membuf seg;               /* segment most recently cut out of buf */
    int seg_last;
    int seg_pair;             /* the segment ends with a marker pair */
    size_t next_produce, next_emit;
    pool_t pool;
    int pool_up;
    PREFIX3(stream) mz;       /* for repairing false splits on this thread */
    int mz_init;
    uint8_t *tmp;             /* block_size bytes for repaired and final blocks */
    uint32_t crc;             /* running crc and length of the member's output */
    size_t total;
};

static int r_fail(gzblock_reader *r, int err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    gzblk_msgv(r->msg, fmt, ap);
    va_end(ap);
    r->err = err;
    r->state = R_ERROR;
    return -1;
}

static int r_fill(gzblock_reader *r, size_t want) {
    if (gzblk_buf_fill(&r->buf, r->read, r->ctx, want, &r->eof) != 0)
        return r_fail(r, Z_ERRNO, "read error");
    return 0;
}

static int r_oom(gzblock_reader *r) {
    return r_fail(r, Z_MEM_ERROR, "out of memory");
}

static void r_handout(gzblock_reader *r, const uint8_t *p, size_t n, slot_t *slot) {
    r->out_p = p;
    r->out_n = n;
    r->out_slot = slot;
}

/* Plain inflate of a member, starting with whatever is in buf. */
static int r_start_stream(gzblock_reader *r) {
    if (!r->z_init) {
        memset(&r->z, 0, sizeof(r->z));
        if (PREFIX(inflateInit2)(&r->z, MAX_WBITS + 16) != Z_OK)
            return r_oom(r);
        r->z_init = 1;
    } else {
        PREFIX(inflateReset)(&r->z);
    }
    r->state = R_STREAM;
    return 0;
}

static int r_stream(gzblock_reader *r) {
    size_t feed;
    int err;

    if (r->buf.len == 0) {
        if (r_fill(r, 1) != 0)
            return -1;
        if (r->buf.len == 0)
            return r_fail(r, Z_BUF_ERROR, "unexpected end of file");
    }
    feed = r->buf.len > UINT32_MAX ? UINT32_MAX : r->buf.len;
    r->z.next_in = (z_const uint8_t *)GZBLK_BUF(&r->buf);
    r->z.avail_in = (uint32_t)feed;
    r->z.next_out = r->obuf;
    r->z.avail_out = IO_CHUNK;
    err = PREFIX(inflate)(&r->z, Z_NO_FLUSH);
    gzblk_buf_drop(&r->buf, feed - r->z.avail_in);
    if (err != Z_OK && err != Z_STREAM_END)
        return r_fail(r, Z_DATA_ERROR, "inflate failed: %s", r->z.msg ? r->z.msg : "data error");
    r_handout(r, r->obuf, IO_CHUNK - r->z.avail_out, NULL);
    if (err == Z_STREAM_END) {
        r->members++;
        r->state = R_HEADER;
    }
    return 0;
}

/* Not gzip at all, copied through unchanged the way gzread() does. */
static int r_passthru(gzblock_reader *r) {
    size_t n;
    if (r->buf.len != 0) {
        n = r->buf.len < IO_CHUNK ? r->buf.len : IO_CHUNK;
        memcpy(r->obuf, GZBLK_BUF(&r->buf), n);
        gzblk_buf_drop(&r->buf, n);
        r_handout(r, r->obuf, n, NULL);
        return 0;
    }
    if (r->eof) {
        r->state = R_END;
        return 0;
    }
    n = r->read(r->ctx, r->obuf, IO_CHUNK);
    if (n == (size_t)-1)
        return r_fail(r, Z_ERRNO, "read error");
    if (n == 0) {
        r->eof = 1;
        r->state = R_END;
        return 0;
    }
    r_handout(r, r->obuf, n, NULL);
    return 0;
}


/* The scalar scanner, also the tail behind the vector ones. Pointer to the first 00 00 FF FF
   starting in [p, end), or NULL. end + 3 must be readable. */
static const uint8_t *find_marker_scalar(const uint8_t *p, const uint8_t *end) {
    while (p < end && (p = (const uint8_t *)memchr(p, 0, (size_t)(end - p))) != NULL) {
        if (p[1] == 0 && p[2] == 0xff && p[3] == 0xff)
            return p;
        p++;
    }
    return NULL;
}

/* Pointer to the first 00 00 FF FF starting in [p, end), or NULL. end + 3 must be readable.
   The vector versions filter for the zero byte the way libc memchr does, one load and one
   reduction per 16 bytes, but stay inline so the roughly one hit per 256 bytes that compressed
   data produces costs a few cycles instead of a call boundary. Candidates get the full four-byte
   check, real markers are tens of kilobytes apart. */
#if !defined(GZBLOCK_NO_SIMD) && (defined(__aarch64__) || defined(_M_ARM64))

#include <arm_neon.h>

static const uint8_t *find_marker(const uint8_t *p, const uint8_t *end) {
    while (end - p >= 16) {
        uint8x16_t v = vld1q_u8(p);
        if (vminvq_u8(v) == 0) {
            /* four mask bits per lane, the usual movemask substitute */
            uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(
                                vshrn_n_u16(vreinterpretq_u16_u8(vceqzq_u8(v)), 4)), 0);
            do {
                unsigned i = zng_ctz64(mask) >> 2;
                const uint8_t *q = p + i;
                if (q[1] == 0 && q[2] == 0xff && q[3] == 0xff)
                    return q;
                mask &= ~(0xfull << (i * 4));
            } while (mask != 0);
        }
        p += 16;
    }
    return find_marker_scalar(p, end);
}

#elif !defined(GZBLOCK_NO_SIMD) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))

#include <emmintrin.h>

static const uint8_t *find_marker(const uint8_t *p, const uint8_t *end) {
    const __m128i zero = _mm_setzero_si128();
    while (end - p >= 16) {
        unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)p), zero));
        while (mask != 0) {
            unsigned i = zng_ctz32(mask);
            const uint8_t *q = p + i;
            if (q[1] == 0 && q[2] == 0xff && q[3] == 0xff)
                return q;
            mask &= mask - 1;
        }
        p += 16;
    }
    return find_marker_scalar(p, end);
}

#else

static const uint8_t *find_marker(const uint8_t *p, const uint8_t *end) {
    return find_marker_scalar(p, end);
}

#endif

/* Move the first n bytes of the input buffer into seg. */
static int cut_segment(gzblock_reader *r, size_t n, int last, int pair) {
    r->seg.len = 0;
    if (gzblk_buf_append(&r->seg, GZBLK_BUF(&r->buf), n) != 0)
        return r_oom(r);
    r->seg_last = last;
    r->seg_pair = pair;
    gzblk_buf_drop(&r->buf, n);
    r->scanned = 0;
    return 0;
}

/* Cut the next candidate segment out of the input into seg. Returns 1 when there is one, 0 once the
   input is used up, -1 on an error already recorded, -2 when the data in hand is longer than any
   block could be. */
static int next_segment(gzblock_reader *r) {
    membuf *b = &r->buf;

    for (;;) {
        size_t limit = b->len >= 3 ? b->len - 3 : 0;
        const uint8_t *bp = GZBLK_BUF(b);
        const uint8_t *hit = r->scanned < limit ? find_marker(bp + r->scanned, bp + limit) : NULL;
        if (hit != NULL) {
            size_t n = (size_t)(hit - bp) + 4;
            int empties = 0;
            /* Empty stored blocks right after the marker belong to this segment too, the second one
               is what makes a boundary when the header says pairs. Their bytes must be in hand. */
            for (;;) {
                if (n + 5 > b->len) {
                    if (r->eof)
                        break;
                    r->scanned = (size_t)(hit - bp);
                    goto read_more;
                }
                if (memcmp(bp + n, "\0\0\0\xff\xff", 5) != 0)
                    break;
                n += 5;
                empties++;
            }
            if (r->paired && empties == 0) {
                /* a lone marker, a flush inside a block or data that happens to match */
                r->scanned = (size_t)(hit - bp) + 1;
                continue;
            }
            if (n > r->max_seg)
                return -2;
            return cut_segment(r, n, 0, empties > 0) != 0 ? -1 : 1;
        }
        r->scanned = limit;
        if (b->len > r->max_seg + 3)
            return -2;
        if (r->eof) {
            if (b->len == 0)
                return 0;
            return cut_segment(r, b->len, 1, 0) != 0 ? -1 : 1;
        }
read_more:
        if (r_fill(r, b->len + IO_CHUNK) != 0)
            return -1;
    }
}

/* Enter block mode for a member whose header (the first hdr_len bytes of buf) records, or -b
   supplies, a block size. */
static int r_start_blocks(gzblock_reader *r, size_t hdr_len, uint32_t block_size, uint32_t zb_flags) {
    r->hdr.len = 0;
    if (gzblk_buf_append(&r->hdr, GZBLK_BUF(&r->buf), hdr_len) != 0)
        return r_oom(r);
    gzblk_buf_drop(&r->buf, hdr_len);

    if (r->pool_up && r->block_size != block_size) {
        gzblk_pool_stop(&r->pool);
        gzblk_pool_free(&r->pool);
        r->pool_up = 0;
        free(r->tmp);
        r->tmp = NULL;
    }
    r->block_size = block_size;
    r->max_seg = (size_t)block_size + (block_size >> 8) + 1024;   /* stored blocks plus the markers */
    if (!r->pool_up) {
        r->pool.mode = POOL_INFLATE;
        r->pool.block_size = block_size;
        /* Segments are swapped in from the scanner, so slots start without an in buffer. */
        r->tmp = (uint8_t *)malloc(block_size);
        if (r->tmp == NULL || gzblk_pool_alloc(&r->pool, r->nthreads, 0, block_size) != 0)
            return r_oom(r);
        if (gzblk_pool_start(&r->pool, r->nthreads) != 0)
            return r_fail(r, Z_MEM_ERROR, "cannot start threads");
        r->pool_up = 1;
    }
    if (!r->mz_init) {
        memset(&r->mz, 0, sizeof(r->mz));
        if (PREFIX(inflateInit2)(&r->mz, -MAX_WBITS) != Z_OK)
            return r_oom(r);
        r->mz_init = 1;
    }
    r->paired = (zb_flags & ZB_PAIRED) != 0;
    r->scanned = 0;
    r->cut_all = 0;
    r->next_produce = r->next_emit = 0;
    r->crc = 0;
    r->total = 0;
    r->state = R_BLOCKS;
    return 0;
}

/* Put the header, the segments cut so far, and the input in hand back together and stream the member
   through plain inflate instead. Only valid before any of its output was handed out. */
static int r_fallback(gzblock_reader *r) {
    membuf all = { NULL, 0, 0, 0 };
    size_t i;

    if (gzblk_buf_append(&all, r->hdr.p, r->hdr.len) != 0)
        return r_oom(r);
    for (i = r->next_emit; i < r->next_produce; i++) {
        slot_t *slot = gzblk_pool_slot(&r->pool, i);
        gzblk_slot_wait(&r->pool, slot);
        if (gzblk_buf_append(&all, slot->in, slot->in_len) != 0)
            return r_oom(r);
        gzblk_slot_release(&r->pool, slot);
    }
    if (gzblk_buf_append(&all, GZBLK_BUF(&r->buf), r->buf.len) != 0)
        return r_oom(r);
    free(r->buf.p);
    r->buf = all;
    r->hdr.len = 0;
    r->next_produce = r->next_emit = 0;
    r->scanned = 0;
    r->cut_all = 0;
    return r_start_stream(r);
}

/* Keep the pool fed, cutting segments into free slots until the ring is full or the input is done. */
static int r_produce(gzblock_reader *r) {
    while (!r->cut_all) {
        slot_t *slot = gzblk_pool_slot(&r->pool, r->next_produce);
        membuf swap;
        int rc;

        if (slot->state != SLOT_FREE)
            break;
        rc = next_segment(r);
        if (rc == 0) {
            r->cut_all = 1;
            break;
        }
        if (rc == -1)
            return -1;
        if (rc == -2) {
            if (r->next_produce == 0 && r->next_emit == 0)
                return r_fallback(r);     /* no block structure at this size */
            return r_fail(r, Z_DATA_ERROR, "block %zu is larger than the block size", r->next_produce);
        }
        /* Swap buffers rather than copy, the slot keeps the segment and seg reuses the old one. */
        swap.p = slot->in;
        swap.cap = slot->in_cap;
        slot->in = r->seg.p;
        slot->in_cap = r->seg.cap;
        slot->in_len = r->seg.len;
        slot->last = r->seg_last;
        slot->pair = r->seg_pair;
        r->seg.p = swap.p;
        r->seg.cap = swap.cap;
        r->seg.len = 0;
        gzblk_slot_submit(&r->pool, slot);
        r->next_produce++;
    }
    return 0;
}

/* The member's final block was inflated. rest is what followed it in its piece, the trailer and
   possibly more members, which together with any segments cut after it and the input in hand goes
   back to the front of the input. slot, if not NULL, held rest and is released afterwards. */
static int r_member_end(gzblock_reader *r, const uint8_t *rest, size_t rest_n, slot_t *slot) {
    membuf all = { NULL, 0, 0, 0 };
    size_t i;

    if (gzblk_buf_append(&all, rest, rest_n) != 0)
        return r_oom(r);
    if (slot != NULL)
        gzblk_slot_release(&r->pool, slot);
    for (i = r->next_emit; i < r->next_produce; i++) {
        slot_t *s = gzblk_pool_slot(&r->pool, i);
        gzblk_slot_wait(&r->pool, s);
        if (gzblk_buf_append(&all, s->in, s->in_len) != 0)
            return r_oom(r);
        gzblk_slot_release(&r->pool, s);
    }
    if (gzblk_buf_append(&all, GZBLK_BUF(&r->buf), r->buf.len) != 0)
        return r_oom(r);
    free(r->buf.p);
    r->buf = all;
    r->scanned = 0;
    r->cut_all = 0;
    r->next_produce = r->next_emit = 0;
    r->state = R_MEMBER_END;
    return 0;
}

/* Hand out a finished block's output and account for it. */
static void r_block_out(gzblock_reader *r, const uint8_t *out, size_t out_len, uint32_t crc, slot_t *slot) {
    r->crc = (uint32_t)PREFIX(crc32_combine)(r->crc, crc, (z_off64_t)out_len);
    r->total += out_len;
    r_handout(r, out, out_len, slot);
}

/* A false marker split the block in first. Inflate it again from there on this thread, feeding the
   following pieces until the real block completes. */
static int r_repair(gzblock_reader *r, slot_t *first) {
    block_dec m;
    const uint8_t *piece = first->in;
    size_t piece_len = first->in_len, used;
    int last = first->last, pair = first->pair, status;
    slot_t *ps = first;

    gzblk_block_begin(&m, &r->mz, r->tmp, r->block_size);
    for (;;) {
        m.accept_partial = pair;
        status = gzblk_block_feed(&m, piece, piece_len, &used);
        r->next_emit++;
        if (status == SEG_SHORT) {
            if (ps != NULL)
                gzblk_slot_release(&r->pool, ps);
            if (last)
                return r_fail(r, Z_BUF_ERROR, "block %zu is truncated", r->next_emit - 1);
            if (r->next_emit < r->next_produce) {
                /* The next piece is already in the ring, wait for its worker and take it from there. */
                ps = gzblk_pool_slot(&r->pool, r->next_emit);
                gzblk_slot_wait(&r->pool, ps);
                piece = ps->in;
                piece_len = ps->in_len;
                last = ps->last;
                pair = ps->pair;
            } else {
                /* Not cut yet, take it straight from the input, it never needs a slot. */
                int rc = next_segment(r);
                if (rc == -1)
                    return -1;
                if (rc != 1)
                    return r_fail(r, rc == 0 ? Z_BUF_ERROR : Z_DATA_ERROR, rc == 0 ? "unexpected end of file" : "block %zu is larger than the block size", r->next_emit);
                ps = NULL;
                piece = r->seg.p;
                piece_len = r->seg.len;
                last = r->seg_last;
                pair = r->seg_pair;
                r->next_produce++;
            }
            continue;
        }
        if (status == SEG_END) {
            r_block_out(r, r->tmp, (size_t)r->mz.total_out, (uint32_t)PREFIX(crc32_z)(0, r->tmp, (size_t)r->mz.total_out), NULL);
            return r_member_end(r, piece + used, piece_len - used, ps);
        }
        if (status == SEG_FULL && used == piece_len && !last) {
            r_block_out(r, r->tmp, (size_t)r->mz.total_out, (uint32_t)PREFIX(crc32_z)(0, r->tmp, (size_t)r->mz.total_out), NULL);
            if (ps != NULL)
                gzblk_slot_release(&r->pool, ps);
            return 0;
        }
        if (ps != NULL)
            gzblk_slot_release(&r->pool, ps);
        if (status == SEG_FULL)
            return r_fail(r, last ? Z_BUF_ERROR : Z_DATA_ERROR, last ? "unexpected end of file" : "block %zu has trailing data", r->next_emit - 1);
        return r_fail(r, status == SEG_SHORT ? Z_BUF_ERROR : Z_DATA_ERROR, "block %zu is %s", r->next_emit - 1, gzblk_seg_name(status));
    }
}

/* Hand out the next block in order. */
static int r_drain(gzblock_reader *r) {
    slot_t *slot = gzblk_pool_slot(&r->pool, r->next_emit);

    gzblk_slot_wait(&r->pool, slot);
    if (slot->status == SEG_FULL && slot->in_used == slot->in_len && !slot->last) {
        r->next_emit++;
        r_block_out(r, slot->out, slot->out_len, slot->crc, slot);
        return 0;
    }
    if (slot->status == SEG_END) {
        /* The final block. Its output goes out from tmp so the slot can be recycled right away. */
        memcpy(r->tmp, slot->out, slot->out_len);
        r_block_out(r, r->tmp, slot->out_len, slot->crc, NULL);
        r->next_emit++;
        return r_member_end(r, slot->in + slot->in_used, slot->in_len - slot->in_used, slot);
    }
    if (slot->status == SEG_OVERFLOW && r->next_emit == 0)
        return r_fallback(r);
    if (slot->status == SEG_SHORT && !slot->last)
        return r_repair(r, slot);
    if (slot->status == SEG_FULL)
        return r_fail(r, slot->last ? Z_BUF_ERROR : Z_DATA_ERROR, slot->last ? "unexpected end of file" : "block %zu has trailing data", r->next_emit);
    return r_fail(r, slot->status == SEG_SHORT ? Z_BUF_ERROR : Z_DATA_ERROR, "block %zu is %s", r->next_emit, gzblk_seg_name(slot->status));
}

static int r_blocks(gzblock_reader *r) {
    if (r_produce(r) != 0)
        return -1;
    if (r->state != R_BLOCKS)
        return 0;                    /* fell back to plain inflate */
    if (r->next_emit < r->next_produce)
        return r_drain(r);
    return r_fail(r, Z_BUF_ERROR, "unexpected end of file");
}

/* The 8 trailer bytes follow the final block, then the next member or the end. */
static int r_member_end_step(gzblock_reader *r) {
    const uint8_t *t;
    uint32_t want_crc, want_size;

    if (r_fill(r, GZ_TRAILER) != 0)
        return -1;
    if (r->buf.len < GZ_TRAILER)
        return r_fail(r, Z_BUF_ERROR, "unexpected end of file");
    t = GZBLK_BUF(&r->buf);
    want_crc = (uint32_t)t[0] | ((uint32_t)t[1] << 8) | ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
    want_size = (uint32_t)t[4] | ((uint32_t)t[5] << 8) | ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 24);
    if (r->crc != want_crc)
        return r_fail(r, Z_DATA_ERROR, "crc mismatch in the gzip trailer");
    if (want_size != (uint32_t)r->total)
        return r_fail(r, Z_DATA_ERROR, "length mismatch in the gzip trailer");
    gzblk_buf_drop(&r->buf, GZ_TRAILER);
    r->members++;
    r->state = R_HEADER;
    return 0;
}

/* Decide how to decode what comes next: a gzip member in block mode or plain, pass-through for data
   that is not gzip, or the end. */
static int r_header(gzblock_reader *r) {
    size_t want = 1024, hdr_len;
    uint32_t hdr_block_size, zb_flags;

    for (;;) {
        if (r_fill(r, want) != 0)
            return -1;
        if (r->buf.len < 2 || GZBLK_BUF(&r->buf)[0] != 0x1f || GZBLK_BUF(&r->buf)[1] != 0x8b) {
            if (r->buf.len == 0 && r->eof)
                r->state = R_END;
            else if (r->members == 0)
                r->state = R_PASSTHRU;   /* not gzip, pass it through like gzread() */
            else
                r->state = R_END;        /* trailing garbage, ignored like gzread() */
            return 0;
        }
        hdr_len = gzblk_header_parse(GZBLK_BUF(&r->buf), r->buf.len, &hdr_block_size, &zb_flags);
        if (hdr_len == (size_t)-1)
            return r_fail(r, Z_DATA_ERROR, "not in gzip format");
        if (hdr_len != 0)
            break;
        if (r->eof)
            return r_fail(r, Z_BUF_ERROR, "unexpected end of file");
        /* A header that does not fit in a megabyte is not one worth buffering, inflate takes it
           piece by piece. */
        if (want >= (1u << 20))
            return r_start_stream(r);
        want *= 2;
    }
    if (hdr_block_size == 0) {
        hdr_block_size = r->block_hint;
        zb_flags = 0;                  /* a guessed size implies nothing about the markers */
    }
    /* Nothing to parallelize, or a block size that would cost more memory than is sensible. */
    if (hdr_block_size == 0 || hdr_block_size > GZBLOCK_MAX_BLOCK)
        return r_start_stream(r);
    return r_start_blocks(r, hdr_len, hdr_block_size, zb_flags);
}

gzblock_reader Z_INTERNAL *gzblock_ropen(gzblock_read_fn read, void *ctx, const uint8_t *head, size_t head_len,
                                         uint32_t block_size, int nthreads) {
    gzblock_reader *r;

    if (read == NULL)
        return NULL;
    r = (gzblock_reader *)calloc(1, sizeof(*r));
    if (r == NULL)
        return NULL;
    r->read = read;
    r->ctx = ctx;
    r->block_hint = block_size > GZBLOCK_MAX_BLOCK ? 0 : block_size;
    r->nthreads = nthreads > 0 ? nthreads : gzblk_default_threads();
    r->obuf = (uint8_t *)malloc(IO_CHUNK);
    if (r->obuf == NULL || (head_len != 0 && gzblk_buf_append(&r->buf, head, head_len) != 0)) {
        gzblock_rclose(r);
        return NULL;
    }
    r->state = R_HEADER;
    return r;
}

/* Output handed out earlier has been consumed, the slot holding it can go back to the pool. */
static void r_done_pending(gzblock_reader *r) {
    if (r->out_n == 0 && r->out_slot != NULL) {
        gzblk_slot_release(&r->pool, r->out_slot);
        r->out_slot = NULL;
    }
}

/* Advance until there is output to hand out or the data ends. Returns 0 with r->out_n set or the
   state at R_END, -1 on error. */
static int r_advance(gzblock_reader *r) {
    int rc;
    while (r->out_n == 0) {
        switch (r->state) {
        case R_HEADER:     rc = r_header(r); break;
        case R_PASSTHRU:   rc = r_passthru(r); break;
        case R_STREAM:     rc = r_stream(r); break;
        case R_BLOCKS:     rc = r_blocks(r); break;
        case R_MEMBER_END: rc = r_member_end_step(r); break;
        case R_END:        return 0;
        default:           return -1;
        }
        if (rc != 0)
            return -1;
    }
    return 0;
}

int Z_INTERNAL gzblock_read(gzblock_reader *r, uint8_t *buf, size_t len, size_t *got) {
    size_t done = 0;

    r_done_pending(r);
    while (done < len) {
        size_t n;
        if (r_advance(r) != 0)
            return -1;
        if (r->out_n == 0)
            break;                  /* end of the data */
        n = r->out_n < len - done ? r->out_n : len - done;
        memcpy(buf + done, r->out_p, n);
        done += n;
        r->out_p += n;
        r->out_n -= n;
        r_done_pending(r);
    }
    *got = done;
    return 0;
}

int Z_INTERNAL gzblock_rnext(gzblock_reader *r, const uint8_t **p, size_t *n) {
    r_done_pending(r);
    if (r_advance(r) != 0)
        return -1;
    *p = r->out_p;
    *n = r->out_n;
    /* Consumed as far as the reader is concerned, the slot goes back on the next call. */
    r->out_p += r->out_n;
    r->out_n = 0;
    return 0;
}

const char Z_INTERNAL *gzblock_rerror(const gzblock_reader *r) {
    return r->msg;
}

int Z_INTERNAL gzblock_rerrcode(const gzblock_reader *r) {
    return r->err;
}

void Z_INTERNAL gzblock_rclose(gzblock_reader *r) {
    if (r == NULL)
        return;
    if (r->pool_up)
        gzblk_pool_stop(&r->pool);
    gzblk_pool_free(&r->pool);
    if (r->z_init)
        PREFIX(inflateEnd)(&r->z);
    if (r->mz_init)
        PREFIX(inflateEnd)(&r->mz);
    free(r->tmp);
    free(r->obuf);
    free(r->seg.p);
    free(r->hdr.p);
    free(r->buf.p);
    free(r);
}
