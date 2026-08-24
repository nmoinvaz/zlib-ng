/* gzblockwrite.c -- the parallel writer for gzip members made of independent deflate blocks
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "gzblock_p.h"


struct gzblock_writer_s {
    gzblock_write_fn write;
    void *ctx;
    uint32_t block_size;
    int level, strategy, nthreads;
    pool_t pool;
    int pool_up;
    size_t next_produce, next_emit;
    slot_t *cur;            /* slot being filled */
    uint32_t crc;
    size_t total;
    int hdr_written, finished, failed;
    int err;                /* zlib error code once failed */
    char msg[MSG_LEN];

    /* A block that has to be flushed part way continues on the calling thread as one deflate
       stream, so it still decodes to exactly block_size bytes. */
    PREFIX3(stream) iz;
    int iz_init, inline_active;
    size_t inline_fill;     /* input bytes of the inline block so far */
    uint32_t inline_crc;
    uint8_t *obuf;          /* IO_CHUNK of output space for the inline stream */
};

static int w_fail(gzblock_writer *w, int err, const char *msg) {
    gzblk_msg(w->msg, "%s", msg);
    w->err = err;
    w->failed = 1;
    return -1;
}

static int w_out(gzblock_writer *w, const uint8_t *buf, size_t len) {
    if (w->write(w->ctx, buf, len) != len)
        return w_fail(w, Z_ERRNO, "write error");
    return 0;
}

/* gzip header with FEXTRA carrying the "ZB" subfield. Fixed 21-byte layout, the block size at
   offsets 16..19 and the flags byte at 20, which tests and tools rely on. */
static int w_header(gzblock_writer *w) {
    uint8_t hdr[10 + 2 + 9];
    if (w->hdr_written)
        return 0;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 0x1f;
    hdr[1] = 0x8b;
    hdr[2] = 8;
    hdr[3] = 4;
    hdr[8] = (uint8_t)(w->level == 9 ? 2 :
                       (w->strategy >= Z_HUFFMAN_ONLY || (w->level >= 0 && w->level < 2) ? 4 : 0));
#ifdef _WIN32
    hdr[9] = 0;
#else
    hdr[9] = 3;
#endif
    hdr[10] = 9;
    hdr[12] = 'Z';
    hdr[13] = 'B';
    hdr[14] = 5;
    hdr[16] = (uint8_t)w->block_size;
    hdr[17] = (uint8_t)(w->block_size >> 8);
    hdr[18] = (uint8_t)(w->block_size >> 16);
    hdr[19] = (uint8_t)(w->block_size >> 24);
    hdr[20] = ZB_PAIRED;
    w->hdr_written = 1;
    return w_out(w, hdr, sizeof(hdr));
}

/* Write out the next compressed block in order. */
static int w_drain(gzblock_writer *w) {
    slot_t *slot = gzblk_pool_slot(&w->pool, w->next_emit);

    gzblk_slot_wait(&w->pool, slot);
    if (slot->status != 0)
        return w_fail(w, Z_STREAM_ERROR, "deflate failed");
    if (w_header(w) != 0 || w_out(w, slot->out, slot->out_len) != 0)
        return -1;
    w->crc = (uint32_t)PREFIX(crc32_combine)(w->crc, slot->crc, (z_off64_t)slot->in_len);
    w->total += slot->in_len;
    gzblk_slot_release(&w->pool, slot);
    w->next_emit++;
    return 0;
}

/* Run the inline stream with flush until its output is drained to the file. */
static int w_inline_out(gzblock_writer *w, int flush) {
    int err;
    do {
        size_t have;
        w->iz.next_out = w->obuf;
        w->iz.avail_out = IO_CHUNK;
        err = PREFIX(deflate)(&w->iz, flush);
        if (err == Z_STREAM_ERROR)
            return w_fail(w, Z_STREAM_ERROR, "deflate failed");
        have = IO_CHUNK - w->iz.avail_out;
        if (have != 0 && w_out(w, w->obuf, have) != 0)
            return -1;
    } while (w->iz.avail_out == 0);
    return 0;
}

/* The inline block is complete, seal it the way the pool does and account for it. */
static int w_inline_end(gzblock_writer *w, int last) {
    if (w_inline_out(w, last ? Z_FINISH : Z_SYNC_FLUSH) != 0)
        return -1;
    if (!last && w_inline_out(w, Z_FULL_FLUSH) != 0)
        return -1;
    w->crc = (uint32_t)PREFIX(crc32_combine)(w->crc, w->inline_crc, (z_off64_t)w->inline_fill);
    w->total += w->inline_fill;
    w->inline_active = 0;
    return 0;
}

/* Feed len bytes, at most what is left of the block, to the inline stream. */
static int w_inline_feed(gzblock_writer *w, const uint8_t *buf, size_t len) {
    w->iz.next_in = (z_const uint8_t *)buf;
    w->iz.avail_in = (uint32_t)len;
    w->inline_crc = (uint32_t)PREFIX(crc32_z)(w->inline_crc, buf, len);
    w->inline_fill += len;
    if (w_inline_out(w, Z_NO_FLUSH) != 0)
        return -1;
    if (w->inline_fill == w->block_size)
        return w_inline_end(w, 0);
    return 0;
}

static int w_drain(gzblock_writer *w);

/* Move the block being filled onto the inline stream. Everything before it goes to the file first,
   so the inline output can follow directly. */
static int w_inline_begin(gzblock_writer *w) {
    while (w->next_emit < w->next_produce) {
        if (w_drain(w) != 0)
            return -1;
    }
    if (w_header(w) != 0)
        return -1;
    if (!w->iz_init) {
        memset(&w->iz, 0, sizeof(w->iz));
        if (PREFIX(deflateInit2)(&w->iz, w->level, Z_DEFLATED, -MAX_WBITS, 8, w->strategy) != Z_OK)
            return w_fail(w, Z_MEM_ERROR, "out of memory");
        w->iz_init = 1;
    } else {
        PREFIX(deflateReset)(&w->iz);
        PREFIX(deflateParams)(&w->iz, w->level, w->strategy);
    }
    w->inline_active = 1;
    w->inline_fill = 0;
    w->inline_crc = 0;
    if (w->cur != NULL) {
        slot_t *slot = w->cur;
        w->cur = NULL;
        if (slot->in_len != 0 && w_inline_feed(w, slot->in, slot->in_len) != 0)
            return -1;
        gzblk_slot_release(&w->pool, slot);
    }
    return 0;
}

/* Take the next free slot to fill, draining finished ones to make room. */
static int w_acquire(gzblock_writer *w) {
    slot_t *slot;
    while ((slot = gzblk_pool_slot(&w->pool, w->next_produce))->state != SLOT_FREE) {
        if (w_drain(w) != 0)
            return -1;
    }
    slot->in_len = 0;
    w->cur = slot;
    return 0;
}

static void w_submit(gzblock_writer *w, int last) {
    w->cur->last = last;
    w->cur->level = w->level;
    w->cur->strategy = w->strategy;
    gzblk_slot_submit(&w->pool, w->cur);
    w->cur = NULL;
    w->next_produce++;
}

gzblock_writer Z_INTERNAL *gzblock_wopen(gzblock_write_fn write, void *ctx, int level, int strategy,
                                         uint32_t block_size, int nthreads) {
    gzblock_writer *w;
    PREFIX3(stream) bound;
    size_t out_cap;

    if (write == NULL || block_size == 0 || block_size > GZBLOCK_MAX_BLOCK)
        return NULL;
    w = (gzblock_writer *)calloc(1, sizeof(*w));
    if (w == NULL)
        return NULL;
    w->write = write;
    w->ctx = ctx;
    w->block_size = block_size;
    w->level = level;
    w->strategy = strategy;
    w->nthreads = nthreads > 0 ? nthreads : gzblk_default_threads();

    /* Room for a whole block's worst case plus the flush marker. */
    memset(&bound, 0, sizeof(bound));
    if (PREFIX(deflateInit2)(&bound, level, Z_DEFLATED, -MAX_WBITS, 8, strategy) != Z_OK) {
        free(w);
        return NULL;
    }
    out_cap = PREFIX(deflateBound)(&bound, block_size) + 32;
    PREFIX(deflateEnd)(&bound);

    w->pool.mode = POOL_DEFLATE;
    w->pool.block_size = block_size;
    w->pool.level = level;
    w->pool.strategy = strategy;
    w->obuf = (uint8_t *)malloc(IO_CHUNK);
    if (w->obuf == NULL || gzblk_pool_alloc(&w->pool, w->nthreads, block_size, out_cap) != 0 ||
        gzblk_pool_start(&w->pool, w->nthreads) != 0) {
        gzblk_pool_free(&w->pool);
        free(w->obuf);
        free(w);
        return NULL;
    }
    w->pool_up = 1;
    return w;
}

int Z_INTERNAL gzblock_write(gzblock_writer *w, const uint8_t *buf, size_t len) {
    if (w->failed || w->finished)
        return -1;
    while (len != 0) {
        size_t take;
        if (w->inline_active) {
            take = w->block_size - w->inline_fill;
            if (take > len)
                take = len;
            if (w_inline_feed(w, buf, take) != 0)
                return -1;
            buf += take;
            len -= take;
            continue;
        }
        if (w->cur == NULL && w_acquire(w) != 0)
            return -1;
        take = w->block_size - w->cur->in_len;
        if (take > len)
            take = len;
        memcpy(w->cur->in + w->cur->in_len, buf, take);
        w->cur->in_len += take;
        buf += take;
        len -= take;
        if (w->cur->in_len == w->block_size)
            w_submit(w, 0);
    }
    return 0;
}

static int w_inline_migrate(gzblock_writer *w);

int Z_INTERNAL gzblock_wsetparams(gzblock_writer *w, int level, int strategy) {
    if (w->failed || w->finished)
        return -1;
    if (level == w->level && strategy == w->strategy)
        return 0;
    /* Input already taken for the current block keeps the old settings. deflateParams() applies
       them to it and switches mid-stream, so the block stays one stream. */
    if (w_inline_migrate(w) != 0)
        return -1;
    if (w->inline_active) {
        int err;
        {
            for (;;) {
                size_t have;
                w->iz.next_out = w->obuf;
                w->iz.avail_out = IO_CHUNK;
                err = PREFIX(deflateParams)(&w->iz, level, strategy);
                have = IO_CHUNK - w->iz.avail_out;
                if (have != 0 && w_out(w, w->obuf, have) != 0)
                    return -1;
                if (err != Z_BUF_ERROR)
                    break;
            }
            if (err != Z_OK)
                return w_fail(w, Z_STREAM_ERROR, "deflateParams failed");
        }
    }
    w->level = level;
    w->strategy = strategy;
    return 0;
}

/* A partly filled block moves to the inline stream, so it can be flushed or reconfigured without
   ending early. No-op when there is no such block. */
static int w_inline_migrate(gzblock_writer *w) {
    if (!w->inline_active && w->cur != NULL && w->cur->in_len != 0)
        return w_inline_begin(w);
    return 0;
}

int Z_INTERNAL gzblock_wflush(gzblock_writer *w) {
    if (w->failed || w->finished)
        return -1;
    if (w_inline_migrate(w) != 0)
        return -1;
    if (w->inline_active)
        return w_inline_out(w, Z_SYNC_FLUSH);
    while (w->next_emit < w->next_produce) {
        if (w_drain(w) != 0)
            return -1;
    }
    return w_header(w);
}

int Z_INTERNAL gzblock_wfinish(gzblock_writer *w) {
    uint8_t trailer[GZ_TRAILER];

    if (w->failed)
        return -1;
    if (w->finished)
        return 0;
    if (w->inline_active) {
        /* The inline block is the last one and ends the stream itself. */
        if (w_inline_end(w, 1) != 0)
            return -1;
    } else {
        /* The last block ends the deflate stream, an empty one if the input ended on a boundary. */
        if (w->cur == NULL && w_acquire(w) != 0)
            return -1;
        w_submit(w, 1);
        while (w->next_emit < w->next_produce) {
            if (w_drain(w) != 0)
                return -1;
        }
    }
    trailer[0] = (uint8_t)w->crc;
    trailer[1] = (uint8_t)(w->crc >> 8);
    trailer[2] = (uint8_t)(w->crc >> 16);
    trailer[3] = (uint8_t)(w->crc >> 24);
    trailer[4] = (uint8_t)w->total;
    trailer[5] = (uint8_t)(w->total >> 8);
    trailer[6] = (uint8_t)(w->total >> 16);
    trailer[7] = (uint8_t)(w->total >> 24);
    if (w_out(w, trailer, sizeof(trailer)) != 0)
        return -1;
    w->finished = 1;
    return 0;
}

const char Z_INTERNAL *gzblock_werror(const gzblock_writer *w) {
    return w->msg;
}

int Z_INTERNAL gzblock_werrcode(const gzblock_writer *w) {
    return w->err;
}

void Z_INTERNAL gzblock_wclose(gzblock_writer *w) {
    if (w == NULL)
        return;
    if (w->pool_up)
        gzblk_pool_stop(&w->pool);
    gzblk_pool_free(&w->pool);
    if (w->iz_init)
        PREFIX(deflateEnd)(&w->iz);
    free(w->obuf);
    free(w);
}

