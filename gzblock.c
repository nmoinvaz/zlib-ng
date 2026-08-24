/* gzblock.c -- gzip members made of independent deflate blocks, written and read in parallel
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/*
 * Format. One ordinary gzip member whose deflate stream does a full flush every block_size input
 * bytes. Each block therefore ends with an empty stored block, the bytes 00 00 FF FF, and the next
 * block references nothing before it, so blocks can be inflated on their own. The gzip header
 * records the block size in an extra subfield with the ID "ZB", LEN 4, and the block size as a
 * 32-bit little-endian value, so readers can learn the layout from the header. Any deflate stream
 * built the same way decodes here, pigz -i output included given its block size.
 *
 * Threads. A pool of workers runs deflate or inflate over a ring of slots. The main thread fills
 * the slots in order and drains them in order, so output order is slot order and memory is
 * bounded by the ring. The writer cuts its input into blocks. The reader cuts the compressed
 * stream into candidate segments at every marker. A marker pattern can also occur by chance
 * inside compressed data. Such a false start shows up as a segment that ends before it has
 * produced block_size bytes, which the main thread repairs by inflating from that point serially
 * across as many following pieces as the real block spans.
 */

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE   /* sysconf(_SC_NPROCESSORS_ONLN) is hidden under strict POSIX */
#endif

#include "zbuild.h"
#ifdef ZLIB_COMPAT
#  include "zlib.h"
#else
#  include "zlib-ng.h"
#endif
#include "gzblock.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GZBLOCK_THREADS
#  include <pthread.h>
#  include <unistd.h>
#endif

#define IO_CHUNK      (256 * 1024)
#define GZ_TRAILER    8
#define RING_BYTES    (1ULL << 30)   /* upper bound on block buffers held in flight */
#define MSG_LEN       128

/* ===========================================================================
 * Helpers
 */

static void set_msg(char *msg, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, MSG_LEN, fmt, ap);
    va_end(ap);
}

static int default_threads(void) {
#if defined(GZBLOCK_THREADS) && defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0)
        return n > 64 ? 64 : (int)n;
#endif
    return 1;
}

/* Growable byte buffer. */
typedef struct {
    uint8_t *p;
    size_t len, cap;
} membuf;

static int membuf_reserve(membuf *m, size_t need) {
    if (need > m->cap) {
        size_t ncap = m->cap ? m->cap : (1 << 16);
        uint8_t *grown;
        while (ncap < need)
            ncap *= 2;
        grown = (uint8_t *)realloc(m->p, ncap);
        if (grown == NULL)
            return -1;
        m->p = grown;
        m->cap = ncap;
    }
    return 0;
}

static int membuf_append(membuf *m, const uint8_t *data, size_t n) {
    if (membuf_reserve(m, m->len + n) != 0)
        return -1;
    memcpy(m->p + m->len, data, n);
    m->len += n;
    return 0;
}

static void membuf_drop(membuf *m, size_t n) {
    memmove(m->p, m->p + n, m->len - n);
    m->len -= n;
}

/* Read through the callback until the buffer holds at least want bytes or the input ends, which
   sets *eof. Returns -1 on a read error. */
static int membuf_fill(membuf *m, gzblock_read_fn read, void *ctx, size_t want, int *eof) {
    while (m->len < want && !*eof) {
        size_t got;
        if (m->len == m->cap && membuf_reserve(m, m->cap + 1) != 0)
            return -1;
        got = read(ctx, m->p + m->len, m->cap - m->len);
        if (got == (size_t)-1)
            return -1;
        if (got == 0) {
            *eof = 1;
            break;
        }
        m->len += got;
    }
    return 0;
}

/* ===========================================================================
 * gzip header
 */

/* Returns the header length, 0 if more bytes are needed, (size_t)-1 if this is not a gzip header. */
static size_t parse_header(const uint8_t *buf, size_t len, uint32_t *block_size) {
    size_t pos = 10;
    uint8_t flags;

    *block_size = 0;
    if (len < 10)
        return 0;
    if (buf[0] != 0x1f || buf[1] != 0x8b || buf[2] != 8)
        return (size_t)-1;
    flags = buf[3];
    if (flags & 0xe0)
        return (size_t)-1;

    if (flags & 4) {   /* FEXTRA */
        size_t xlen, end;
        if (len < pos + 2)
            return 0;
        xlen = buf[pos] | ((size_t)buf[pos + 1] << 8);
        pos += 2;
        end = pos + xlen;
        if (len < end)
            return 0;
        while (pos + 4 <= end) {
            size_t sublen = buf[pos + 2] | ((size_t)buf[pos + 3] << 8);
            if (buf[pos] == 'Z' && buf[pos + 1] == 'B' && sublen == 4 && pos + 8 <= end)
                *block_size = (uint32_t)buf[pos + 4] | ((uint32_t)buf[pos + 5] << 8) |
                              ((uint32_t)buf[pos + 6] << 16) | ((uint32_t)buf[pos + 7] << 24);
            pos += 4 + sublen;
        }
        pos = end;
    }
    if (flags & 8) {   /* FNAME */
        while (pos < len && buf[pos] != 0)
            pos++;
        if (pos >= len)
            return 0;
        pos++;
    }
    if (flags & 16) {  /* FCOMMENT */
        while (pos < len && buf[pos] != 0)
            pos++;
        if (pos >= len)
            return 0;
        pos++;
    }
    if (flags & 2) {   /* FHCRC */
        if (len < pos + 2)
            return 0;
        pos += 2;
    }
    return pos;
}

int Z_INTERNAL gzblock_parse_header(const uint8_t *buf, size_t len, size_t *hdr_len, uint32_t *block_size) {
    size_t n = parse_header(buf, len, block_size);
    if (n == (size_t)-1)
        return -1;
    if (n == 0)
        return 0;
    *hdr_len = n;
    return 1;
}

/* ===========================================================================
 * One block, inflated piece by piece
 */

enum { SEG_FULL, SEG_END, SEG_SHORT, SEG_OVERFLOW, SEG_ERROR };

typedef struct {
    PREFIX3(stream) *z;
    int want_marker;    /* output complete, the trailing empty stored block is still to come */
} block_dec;

static void block_begin(block_dec *d, PREFIX3(stream) *z, uint8_t *out, uint32_t block_size) {
    d->z = z;
    d->want_marker = 0;
    PREFIX(inflateReset)(z);
    z->next_in = NULL;
    z->avail_in = 0;
    z->next_out = out;
    z->avail_out = block_size;
}

/* Feed the next piece of a block. Returns SEG_SHORT when the block needs more input, SEG_FULL once the
   block's output and its trailing marker are consumed, SEG_END when the deflate stream ends,
   SEG_OVERFLOW when the block wants more than block_size bytes of output, or SEG_ERROR on invalid
   data. *used receives how much of this piece was consumed. */
static int block_feed(block_dec *d, const uint8_t *in, size_t in_len, size_t *used) {
    PREFIX3(stream) *z = d->z;
    size_t left = in_len, start_in = (size_t)z->total_in;
    int err, boundary, aligned, exhausted, status;

    z->next_in = (z_const uint8_t *)in;
    z->avail_in = 0;
    for (;;) {
        if (z->avail_in == 0 && left != 0) {
            uint32_t chunk = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
            z->avail_in = chunk;
            left -= chunk;
        }
        /* Z_BLOCK returns at every block boundary, where data_type reports the position. */
        err = PREFIX(inflate)(z, Z_BLOCK);
        if (err == Z_OK && (z->data_type & (64 | 128)) == (64 | 128))
            err = PREFIX(inflate)(z, Z_BLOCK);   /* past the final block, conclude the stream */
        if (err == Z_STREAM_END) {
            status = SEG_END;
            break;
        }
        if (err != Z_OK) {
            status = SEG_ERROR;
            break;
        }
        boundary = (z->data_type & 128) != 0;   /* just finished a deflate block */
        aligned = (z->data_type & 7) == 0;      /* and landed on a byte boundary */
        exhausted = (z->avail_in == 0 && left == 0);

        if (d->want_marker) {
            /* Only empty stored blocks may follow a full block, one from a full flush, two when pigz -i
               wrote it. */
            if (!boundary && z->avail_in == 0) {
                if (exhausted) {
                    status = SEG_SHORT;     /* the marker continues in the next piece */
                    break;
                }
                continue;
            }
            if (!(boundary && aligned)) {
                status = SEG_OVERFLOW;
                break;
            }
            if (exhausted) {
                status = SEG_FULL;
                break;
            }
            continue;                       /* another empty stored block */
        }
        if (z->avail_out != 0) {
            if (exhausted) {
                status = SEG_SHORT;
                break;
            }
            continue;       /* more deflate blocks to go */
        }
        /* The output is full. */
        if (!boundary) {
            status = exhausted ? SEG_SHORT : SEG_OVERFLOW;
            break;
        }
        if (exhausted) {
            status = aligned ? SEG_FULL : SEG_SHORT;
            break;
        }
        d->want_marker = 1;
    }
    *used = (size_t)z->total_in - start_in;
    return status;
}

static const char *seg_status_name(int status) {
    switch (status) {
    case SEG_FULL:     return "complete";
    case SEG_END:      return "end of stream";
    case SEG_SHORT:    return "truncated";
    case SEG_OVERFLOW: return "larger than the block size";
    default:           return "corrupt";
    }
}

/* ===========================================================================
 * Pool, a ring of slots worked on by threads
 */

enum { SLOT_FREE, SLOT_FILLED, SLOT_CLAIMED, SLOT_DONE };
enum { POOL_INFLATE, POOL_DEFLATE };

typedef struct {
    uint8_t *in;         /* input block or compressed segment, owned by the slot */
    size_t in_len, in_cap;
    int last;            /* final piece of the input */
    uint8_t *out;
    int level, strategy; /* deflate settings for this block */
    int status;          /* SEG_* for inflate, 0 or -1 for deflate */
    size_t out_len, in_used;
    uint32_t crc;        /* crc32 of the uncompressed side */
    int state;
} slot_t;

typedef struct {
    int mode;            /* POOL_INFLATE or POOL_DEFLATE */
    uint32_t block_size;
    int level, strategy; /* deflate settings */
    size_t out_cap;      /* bytes in each slot's out buffer */
    slot_t *ring;
    size_t nring;
    slot_t **queue;      /* filled slots in fill order, at most nring */
    size_t qhead, qtail;
    int abort;
    PREFIX3(stream) z;   /* stream for working slots on the calling thread */
    int inline_run;      /* no worker threads, slots are worked on demand */
#ifdef GZBLOCK_THREADS
    pthread_mutex_t mu;
    pthread_cond_t work_cv;   /* a slot was queued, or abort */
    pthread_cond_t done_cv;   /* a slot became done */
    pthread_t *threads;
    int started;
#endif
} pool_t;

static void run_segment(PREFIX3(stream) *z, slot_t *slot, uint32_t block_size) {
    block_dec d;
    block_begin(&d, z, slot->out, block_size);
    slot->status = block_feed(&d, slot->in, slot->in_len, &slot->in_used);
    slot->out_len = (size_t)z->total_out;
    slot->crc = (uint32_t)PREFIX(crc32_z)(0, slot->out, slot->out_len);
}

/* Deflate one block on a fresh raw stream. A full flush ends it on a byte boundary with the empty
   stored block marker, the last block ends the deflate stream instead. */
static void run_block(PREFIX3(stream) *z, slot_t *slot, size_t out_cap) {
    int err;
    PREFIX(deflateReset)(z);
    PREFIX(deflateParams)(z, slot->level, slot->strategy);
    z->next_in = (z_const uint8_t *)slot->in;
    z->avail_in = (uint32_t)slot->in_len;
    z->next_out = slot->out;
    z->avail_out = (uint32_t)out_cap;
    err = PREFIX(deflate)(z, slot->last ? Z_FINISH : Z_FULL_FLUSH);
    slot->out_len = out_cap - z->avail_out;
    slot->in_used = slot->in_len - z->avail_in;
    slot->status = slot->last ? (err == Z_STREAM_END ? 0 : -1)
                              : (err == Z_OK && z->avail_in == 0 && z->avail_out != 0 ? 0 : -1);
    slot->crc = (uint32_t)PREFIX(crc32_z)(0, slot->in, slot->in_len);
}

static int stream_init(pool_t *p, PREFIX3(stream) *z) {
    memset(z, 0, sizeof(*z));
    if (p->mode == POOL_DEFLATE)
        return PREFIX(deflateInit2)(z, p->level, Z_DEFLATED, -MAX_WBITS, 8, p->strategy);
    return PREFIX(inflateInit2)(z, -MAX_WBITS);
}

static void stream_end(pool_t *p, PREFIX3(stream) *z) {
    if (p->mode == POOL_DEFLATE)
        PREFIX(deflateEnd)(z);
    else
        PREFIX(inflateEnd)(z);
}

static void run_slot(pool_t *p, PREFIX3(stream) *z, slot_t *slot) {
    if (p->mode == POOL_DEFLATE)
        run_block(z, slot, p->out_cap);
    else
        run_segment(z, slot, p->block_size);
}

static slot_t *pool_slot(pool_t *p, size_t i) {
    return &p->ring[i % p->nring];
}

/* Allocate the ring, nthreads * 4 slots of in_cap + out_cap bytes, within RING_BYTES. */
static int pool_alloc(pool_t *p, int nthreads, size_t in_cap, size_t out_cap) {
    size_t i;
#ifdef GZBLOCK_THREADS
    p->nring = nthreads <= 1 ? 1 : (size_t)nthreads * 4;
    while (p->nring > 2 && (unsigned long long)p->nring * (in_cap + out_cap) > RING_BYTES)
        p->nring /= 2;
#else
    (void)nthreads;
    p->nring = 1;
#endif
    p->out_cap = out_cap;
    p->ring = (slot_t *)calloc(p->nring, sizeof(slot_t));
    p->queue = (slot_t **)calloc(p->nring, sizeof(slot_t *));
    if (p->ring == NULL || p->queue == NULL)
        return -1;
    for (i = 0; i < p->nring; i++) {
        p->ring[i].out = (uint8_t *)malloc(out_cap);
        if (p->ring[i].out == NULL)
            return -1;
        if (in_cap != 0) {
            p->ring[i].in = (uint8_t *)malloc(in_cap);
            p->ring[i].in_cap = in_cap;
            if (p->ring[i].in == NULL)
                return -1;
        }
    }
    return 0;
}

static void pool_free(pool_t *p) {
    size_t i;
    if (p->ring != NULL) {
        for (i = 0; i < p->nring; i++) {
            free(p->ring[i].in);
            free(p->ring[i].out);
        }
        free(p->ring);
    }
    free(p->queue);
    p->ring = NULL;
    p->queue = NULL;
    p->nring = 0;
    p->qhead = p->qtail = 0;
    p->abort = 0;
}

/* Without worker threads the slots are worked on demand by the calling thread. */
static int pool_start_inline(pool_t *p) {
    p->inline_run = 1;
    return stream_init(p, &p->z) == Z_OK ? 0 : -1;
}

static void pool_stop_inline(pool_t *p) {
    stream_end(p, &p->z);
}

static void slot_wait_inline(pool_t *p, slot_t *slot) {
    if (slot->state == SLOT_FILLED)
        run_slot(p, &p->z, slot);
    slot->state = SLOT_DONE;
}

#ifdef GZBLOCK_THREADS

static void *worker(void *arg) {
    pool_t *p = (pool_t *)arg;
    PREFIX3(stream) z;

    if (stream_init(p, &z) != Z_OK)
        return NULL;
    for (;;) {
        slot_t *slot;

        pthread_mutex_lock(&p->mu);
        while (!p->abort && p->qhead == p->qtail)
            pthread_cond_wait(&p->work_cv, &p->mu);
        if (p->abort) {
            pthread_mutex_unlock(&p->mu);
            break;
        }
        slot = p->queue[p->qhead++ % p->nring];
        slot->state = SLOT_CLAIMED;
        pthread_mutex_unlock(&p->mu);

        run_slot(p, &z, slot);

        pthread_mutex_lock(&p->mu);
        slot->state = SLOT_DONE;
        pthread_cond_broadcast(&p->done_cv);
        pthread_mutex_unlock(&p->mu);
    }
    stream_end(p, &z);
    return NULL;
}

static int pool_start(pool_t *p, int nthreads) {
    if (nthreads <= 1)
        return pool_start_inline(p);
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->work_cv, NULL);
    pthread_cond_init(&p->done_cv, NULL);
    p->threads = (pthread_t *)calloc((size_t)nthreads, sizeof(pthread_t));
    if (p->threads == NULL)
        return -1;
    for (p->started = 0; p->started < nthreads; p->started++) {
        if (pthread_create(&p->threads[p->started], NULL, worker, p) != 0)
            break;
    }
    return p->started > 0 ? 0 : -1;
}

static void pool_stop(pool_t *p) {
    int i;
    if (p->inline_run) {
        pool_stop_inline(p);
        p->inline_run = 0;
        return;
    }
    if (p->threads == NULL)
        return;
    pthread_mutex_lock(&p->mu);
    p->abort = 1;
    pthread_cond_broadcast(&p->work_cv);
    pthread_mutex_unlock(&p->mu);
    for (i = 0; i < p->started; i++)
        pthread_join(p->threads[i], NULL);
    free(p->threads);
    p->threads = NULL;
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->work_cv);
    pthread_cond_destroy(&p->done_cv);
}

static void slot_submit(pool_t *p, slot_t *slot) {
    if (p->inline_run) {
        slot->state = SLOT_FILLED;
        return;
    }
    pthread_mutex_lock(&p->mu);
    slot->state = SLOT_FILLED;
    p->queue[p->qtail++ % p->nring] = slot;
    pthread_cond_signal(&p->work_cv);
    pthread_mutex_unlock(&p->mu);
}

static void slot_wait(pool_t *p, slot_t *slot) {
    if (p->inline_run) {
        slot_wait_inline(p, slot);
        return;
    }
    pthread_mutex_lock(&p->mu);
    while (slot->state != SLOT_DONE)
        pthread_cond_wait(&p->done_cv, &p->mu);
    pthread_mutex_unlock(&p->mu);
}

static void slot_release(pool_t *p, slot_t *slot) {
    if (p->inline_run) {
        slot->state = SLOT_FREE;
        return;
    }
    pthread_mutex_lock(&p->mu);
    slot->state = SLOT_FREE;
    pthread_mutex_unlock(&p->mu);
}

#else /* !GZBLOCK_THREADS */

static int pool_start(pool_t *p, int nthreads) {
    (void)nthreads;
    return pool_start_inline(p);
}

static void pool_stop(pool_t *p) {
    pool_stop_inline(p);
    p->inline_run = 0;
}

static void slot_submit(pool_t *p, slot_t *slot) {
    (void)p;
    slot->state = SLOT_FILLED;
}

static void slot_wait(pool_t *p, slot_t *slot) {
    slot_wait_inline(p, slot);
}

static void slot_release(pool_t *p, slot_t *slot) {
    (void)p;
    slot->state = SLOT_FREE;
}

#endif

/* ===========================================================================
 * Writer
 */

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
    set_msg(w->msg, "%s", msg);
    w->err = err;
    w->failed = 1;
    return -1;
}

static int w_out(gzblock_writer *w, const uint8_t *buf, size_t len) {
    if (w->write(w->ctx, buf, len) != len)
        return w_fail(w, Z_ERRNO, "write error");
    return 0;
}

/* gzip header with FEXTRA carrying the "ZB" block size subfield. */
static int w_header(gzblock_writer *w) {
    uint8_t hdr[10 + 2 + 8];
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
    hdr[10] = 8;
    hdr[12] = 'Z';
    hdr[13] = 'B';
    hdr[14] = 4;
    hdr[16] = (uint8_t)w->block_size;
    hdr[17] = (uint8_t)(w->block_size >> 8);
    hdr[18] = (uint8_t)(w->block_size >> 16);
    hdr[19] = (uint8_t)(w->block_size >> 24);
    w->hdr_written = 1;
    return w_out(w, hdr, sizeof(hdr));
}

/* Write out the next compressed block in order. */
static int w_drain(gzblock_writer *w) {
    slot_t *slot = pool_slot(&w->pool, w->next_emit);

    slot_wait(&w->pool, slot);
    if (slot->status != 0)
        return w_fail(w, Z_STREAM_ERROR, "deflate failed");
    if (w_header(w) != 0 || w_out(w, slot->out, slot->out_len) != 0)
        return -1;
    w->crc = (uint32_t)PREFIX(crc32_combine)(w->crc, slot->crc, (z_off64_t)slot->in_len);
    w->total += slot->in_len;
    slot_release(&w->pool, slot);
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
    if (w_inline_out(w, last ? Z_FINISH : Z_FULL_FLUSH) != 0)
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
        slot_release(&w->pool, slot);
    }
    return 0;
}

/* Take the next free slot to fill, draining finished ones to make room. */
static int w_acquire(gzblock_writer *w) {
    slot_t *slot;
    while ((slot = pool_slot(&w->pool, w->next_produce))->state != SLOT_FREE) {
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
    slot_submit(&w->pool, w->cur);
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
    w->nthreads = nthreads > 0 ? nthreads : default_threads();

    /* Room for a whole block's worst case plus the flush marker. */
    memset(&bound, 0, sizeof(bound));
    if (PREFIX(deflateInit2)(&bound, level, Z_DEFLATED, -MAX_WBITS, 8, strategy) != Z_OK) {
        free(w);
        return NULL;
    }
    out_cap = PREFIX(deflateBound)(&bound, block_size) + 16;
    PREFIX(deflateEnd)(&bound);

    w->pool.mode = POOL_DEFLATE;
    w->pool.block_size = block_size;
    w->pool.level = level;
    w->pool.strategy = strategy;
    w->obuf = (uint8_t *)malloc(IO_CHUNK);
    if (w->obuf == NULL || pool_alloc(&w->pool, w->nthreads, block_size, out_cap) != 0 ||
        pool_start(&w->pool, w->nthreads) != 0) {
        pool_free(&w->pool);
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

int Z_INTERNAL gzblock_wsetparams(gzblock_writer *w, int level, int strategy) {
    if (w->failed || w->finished)
        return -1;
    if (level == w->level && strategy == w->strategy)
        return 0;
    /* Input already taken for the current block keeps the old settings. deflateParams() applies
       them to it and switches mid-stream, so the block stays one stream. */
    if (w->inline_active || (w->cur != NULL && w->cur->in_len != 0)) {
        int err;
        if (!w->inline_active && w_inline_begin(w) != 0)
            return -1;
        if (w->inline_active) {
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

int Z_INTERNAL gzblock_wflush(gzblock_writer *w) {
    if (w->failed || w->finished)
        return -1;
    /* A partly filled block goes inline, so it can be flushed without ending early. */
    if (!w->inline_active && w->cur != NULL && w->cur->in_len != 0 && w_inline_begin(w) != 0)
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
        pool_stop(&w->pool);
    pool_free(&w->pool);
    if (w->iz_init)
        PREFIX(deflateEnd)(&w->iz);
    free(w->obuf);
    free(w);
}

/* ===========================================================================
 * Reader
 */

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
    size_t max_seg;           /* longest a compressed block can be */
    membuf hdr;               /* this member's header, kept for the fallback */
    size_t scanned;           /* bytes of buf already scanned for markers */
    int cut_all;              /* the scanner handed out the member's last segment */
    membuf seg;               /* segment most recently cut out of buf */
    int seg_last;
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
    vsnprintf(r->msg, MSG_LEN, fmt, ap);
    va_end(ap);
    r->err = err;
    r->state = R_ERROR;
    return -1;
}

static int r_fill(gzblock_reader *r, size_t want) {
    if (membuf_fill(&r->buf, r->read, r->ctx, want, &r->eof) != 0)
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
    r->z.next_in = (z_const uint8_t *)r->buf.p;
    r->z.avail_in = (uint32_t)feed;
    r->z.next_out = r->obuf;
    r->z.avail_out = IO_CHUNK;
    err = PREFIX(inflate)(&r->z, Z_NO_FLUSH);
    membuf_drop(&r->buf, feed - r->z.avail_in);
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
        memcpy(r->obuf, r->buf.p, n);
        membuf_drop(&r->buf, n);
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

#if defined(_MSC_VER) && !defined(__clang__)
#  include <intrin.h>
static __forceinline unsigned marker_ctz32(unsigned x) { unsigned long i; _BitScanForward(&i, x); return (unsigned)i; }
static __forceinline unsigned marker_ctz64(unsigned long long x) { unsigned long i; _BitScanForward64(&i, x); return (unsigned)i; }
#else
#  define marker_ctz32(x) ((unsigned)__builtin_ctz(x))
#  define marker_ctz64(x) ((unsigned)__builtin_ctzll(x))
#endif

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
                unsigned i = marker_ctz64(mask) >> 2;
                const uint8_t *q = p + i;
                if (q[1] == 0 && q[2] == 0xff && q[3] == 0xff)
                    return q;
                mask &= ~(0xfull << (i * 4));
            } while (mask != 0);
        }
        p += 16;
    }
    while (p < end && (p = (const uint8_t *)memchr(p, 0, (size_t)(end - p))) != NULL) {
        if (p[1] == 0 && p[2] == 0xff && p[3] == 0xff)
            return p;
        p++;
    }
    return NULL;
}

#elif !defined(GZBLOCK_NO_SIMD) && (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))

#include <emmintrin.h>

static const uint8_t *find_marker(const uint8_t *p, const uint8_t *end) {
    const __m128i zero = _mm_setzero_si128();
    while (end - p >= 16) {
        unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)p), zero));
        while (mask != 0) {
            unsigned i = marker_ctz32(mask);
            const uint8_t *q = p + i;
            if (q[1] == 0 && q[2] == 0xff && q[3] == 0xff)
                return q;
            mask &= mask - 1;
        }
        p += 16;
    }
    while (p < end && (p = (const uint8_t *)memchr(p, 0, (size_t)(end - p))) != NULL) {
        if (p[1] == 0 && p[2] == 0xff && p[3] == 0xff)
            return p;
        p++;
    }
    return NULL;
}

#else

static const uint8_t *find_marker(const uint8_t *p, const uint8_t *end) {
    while (p < end && (p = (const uint8_t *)memchr(p, 0, (size_t)(end - p))) != NULL) {
        if (p[1] == 0 && p[2] == 0xff && p[3] == 0xff)
            return p;
        p++;
    }
    return NULL;
}

#endif

/* Move the first n bytes of the input buffer into seg. */
static int cut_segment(gzblock_reader *r, size_t n, int last) {
    r->seg.len = 0;
    if (membuf_append(&r->seg, r->buf.p, n) != 0)
        return r_oom(r);
    r->seg_last = last;
    membuf_drop(&r->buf, n);
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
        const uint8_t *hit = r->scanned < limit ? find_marker(b->p + r->scanned, b->p + limit) : NULL;
        if (hit != NULL) {
            size_t n = (size_t)(hit - b->p) + 4;
            /* Empty stored blocks right after the marker belong to this segment too, pigz -i writes a
               second one. Their bytes must be in hand to tell. */
            for (;;) {
                if (n + 5 > b->len) {
                    if (r->eof)
                        break;
                    r->scanned = (size_t)(hit - b->p);
                    goto read_more;
                }
                if (memcmp(b->p + n, "\0\0\0\xff\xff", 5) != 0)
                    break;
                n += 5;
            }
            if (n > r->max_seg)
                return -2;
            return cut_segment(r, n, 0) != 0 ? -1 : 1;
        }
        r->scanned = limit;
        if (b->len > r->max_seg + 3)
            return -2;
        if (r->eof) {
            if (b->len == 0)
                return 0;
            return cut_segment(r, b->len, 1) != 0 ? -1 : 1;
        }
read_more:
        if (r_fill(r, b->len + IO_CHUNK) != 0)
            return -1;
    }
}

/* Enter block mode for a member whose header (the first hdr_len bytes of buf) records, or -b
   supplies, a block size. */
static int r_start_blocks(gzblock_reader *r, size_t hdr_len, uint32_t block_size) {
    r->hdr.len = 0;
    if (membuf_append(&r->hdr, r->buf.p, hdr_len) != 0)
        return r_oom(r);
    membuf_drop(&r->buf, hdr_len);

    if (r->pool_up && r->block_size != block_size) {
        pool_stop(&r->pool);
        pool_free(&r->pool);
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
        if (r->tmp == NULL || pool_alloc(&r->pool, r->nthreads, 0, block_size) != 0)
            return r_oom(r);
        if (pool_start(&r->pool, r->nthreads) != 0)
            return r_fail(r, Z_MEM_ERROR, "cannot start threads");
        r->pool_up = 1;
    }
    if (!r->mz_init) {
        memset(&r->mz, 0, sizeof(r->mz));
        if (PREFIX(inflateInit2)(&r->mz, -MAX_WBITS) != Z_OK)
            return r_oom(r);
        r->mz_init = 1;
    }
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
    membuf all = { NULL, 0, 0 };
    size_t i;

    if (membuf_append(&all, r->hdr.p, r->hdr.len) != 0)
        return r_oom(r);
    for (i = r->next_emit; i < r->next_produce; i++) {
        slot_t *slot = pool_slot(&r->pool, i);
        slot_wait(&r->pool, slot);
        if (membuf_append(&all, slot->in, slot->in_len) != 0)
            return r_oom(r);
        slot_release(&r->pool, slot);
    }
    if (membuf_append(&all, r->buf.p, r->buf.len) != 0)
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
        slot_t *slot = pool_slot(&r->pool, r->next_produce);
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
        r->seg.p = swap.p;
        r->seg.cap = swap.cap;
        r->seg.len = 0;
        slot_submit(&r->pool, slot);
        r->next_produce++;
    }
    return 0;
}

/* The member's final block was inflated. rest is what followed it in its piece, the trailer and
   possibly more members, which together with any segments cut after it and the input in hand goes
   back to the front of the input. slot, if not NULL, held rest and is released afterwards. */
static int r_member_end(gzblock_reader *r, const uint8_t *rest, size_t rest_n, slot_t *slot) {
    membuf all = { NULL, 0, 0 };
    size_t i;

    if (membuf_append(&all, rest, rest_n) != 0)
        return r_oom(r);
    if (slot != NULL)
        slot_release(&r->pool, slot);
    for (i = r->next_emit; i < r->next_produce; i++) {
        slot_t *s = pool_slot(&r->pool, i);
        slot_wait(&r->pool, s);
        if (membuf_append(&all, s->in, s->in_len) != 0)
            return r_oom(r);
        slot_release(&r->pool, s);
    }
    if (membuf_append(&all, r->buf.p, r->buf.len) != 0)
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
    int last = first->last, status;
    slot_t *ps = first;

    block_begin(&m, &r->mz, r->tmp, r->block_size);
    for (;;) {
        status = block_feed(&m, piece, piece_len, &used);
        r->next_emit++;
        if (status == SEG_SHORT) {
            if (ps != NULL)
                slot_release(&r->pool, ps);
            if (last)
                return r_fail(r, Z_BUF_ERROR, "block %zu is truncated", r->next_emit - 1);
            if (r->next_emit < r->next_produce) {
                /* The next piece is already in the ring, wait for its worker and take it from there. */
                ps = pool_slot(&r->pool, r->next_emit);
                slot_wait(&r->pool, ps);
                piece = ps->in;
                piece_len = ps->in_len;
                last = ps->last;
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
                slot_release(&r->pool, ps);
            return 0;
        }
        if (ps != NULL)
            slot_release(&r->pool, ps);
        if (status == SEG_FULL)
            return r_fail(r, last ? Z_BUF_ERROR : Z_DATA_ERROR, last ? "unexpected end of file" : "block %zu has trailing data", r->next_emit - 1);
        return r_fail(r, status == SEG_SHORT ? Z_BUF_ERROR : Z_DATA_ERROR, "block %zu is %s", r->next_emit - 1, seg_status_name(status));
    }
}

/* Hand out the next block in order. */
static int r_drain(gzblock_reader *r) {
    slot_t *slot = pool_slot(&r->pool, r->next_emit);

    slot_wait(&r->pool, slot);
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
    return r_fail(r, slot->status == SEG_SHORT ? Z_BUF_ERROR : Z_DATA_ERROR, "block %zu is %s", r->next_emit, seg_status_name(slot->status));
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
    t = r->buf.p;
    want_crc = (uint32_t)t[0] | ((uint32_t)t[1] << 8) | ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
    want_size = (uint32_t)t[4] | ((uint32_t)t[5] << 8) | ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 24);
    if (r->crc != want_crc)
        return r_fail(r, Z_DATA_ERROR, "crc mismatch in the gzip trailer");
    if (want_size != (uint32_t)r->total)
        return r_fail(r, Z_DATA_ERROR, "length mismatch in the gzip trailer");
    membuf_drop(&r->buf, GZ_TRAILER);
    r->members++;
    r->state = R_HEADER;
    return 0;
}

/* Decide how to decode what comes next: a gzip member in block mode or plain, pass-through for data
   that is not gzip, or the end. */
static int r_header(gzblock_reader *r) {
    size_t want = 1024, hdr_len;
    uint32_t hdr_block_size;

    for (;;) {
        if (r_fill(r, want) != 0)
            return -1;
        if (r->buf.len < 2 || r->buf.p[0] != 0x1f || r->buf.p[1] != 0x8b) {
            if (r->buf.len == 0 && r->eof)
                r->state = R_END;
            else if (r->members == 0)
                r->state = R_PASSTHRU;   /* not gzip, pass it through like gzread() */
            else
                r->state = R_END;        /* trailing garbage, ignored like gzread() */
            return 0;
        }
        hdr_len = parse_header(r->buf.p, r->buf.len, &hdr_block_size);
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
    if (hdr_block_size == 0)
        hdr_block_size = r->block_hint;
    /* Nothing to parallelize, or a block size that would cost more memory than is sensible. */
    if (hdr_block_size == 0 || hdr_block_size > GZBLOCK_MAX_BLOCK)
        return r_start_stream(r);
    return r_start_blocks(r, hdr_len, hdr_block_size);
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
    r->nthreads = nthreads > 0 ? nthreads : default_threads();
    r->obuf = (uint8_t *)malloc(IO_CHUNK);
    if (r->obuf == NULL || (head_len != 0 && membuf_append(&r->buf, head, head_len) != 0)) {
        gzblock_rclose(r);
        return NULL;
    }
    r->state = R_HEADER;
    return r;
}

/* Output handed out earlier has been consumed, the slot holding it can go back to the pool. */
static void r_done_pending(gzblock_reader *r) {
    if (r->out_n == 0 && r->out_slot != NULL) {
        slot_release(&r->pool, r->out_slot);
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
        pool_stop(&r->pool);
    pool_free(&r->pool);
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
