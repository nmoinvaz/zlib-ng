/* gzblock.c -- gzip members made of independent deflate blocks, written and read in parallel
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/*
 * Format. One ordinary gzip member whose deflate stream is cut into independent blocks of
 * block_size input bytes. Each block ends with two empty stored blocks, the nine bytes
 * 00 00 FF FF 00 00 00 FF FF, the same shape pigz --independent writes, so blocks can be
 * inflated on their own and boundaries are hard to fake. The gzip header records the layout in an
 * extra subfield with the ID "ZB", the block size as a 32-bit little-endian value and a flags
 * byte whose bit 0 says the boundaries are marker pairs, which lets the reader ignore the single
 * markers that a flush inside a block or a chance pattern in stored data produce. Any deflate
 * stream built the same way decodes here, pigz -i output included given its block size, and
 * streams with single full flush markers are still read by scanning for those.
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

#include "gzblock_p.h"

#ifdef GZBLOCK_THREADS
#  include <unistd.h>
#endif

/* ===========================================================================
 * Helpers
 */

void Z_INTERNAL gzblk_msgv(char *msg, const char *fmt, va_list ap) {
    vsnprintf(msg, MSG_LEN, fmt, ap);
}

void Z_INTERNAL gzblk_msg(char *msg, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    gzblk_msgv(msg, fmt, ap);
    va_end(ap);
}

int Z_INTERNAL gzblk_default_threads(void) {
#if defined(GZBLOCK_THREADS) && defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0)
        return n > 64 ? 64 : (int)n;
#endif
    return 1;
}

int Z_INTERNAL gzblk_buf_reserve(membuf *m, size_t need) {
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

int Z_INTERNAL gzblk_buf_append(membuf *m, const uint8_t *data, size_t n) {
    if (gzblk_buf_reserve(m, m->len + n) != 0)
        return -1;
    memcpy(m->p + m->len, data, n);
    m->len += n;
    return 0;
}

void Z_INTERNAL gzblk_buf_drop(membuf *m, size_t n) {
    memmove(m->p, m->p + n, m->len - n);
    m->len -= n;
}

/* Read through the callback until the buffer holds at least want bytes or the input ends, which
   sets *eof. Returns -1 on a read error. */
int Z_INTERNAL gzblk_buf_fill(membuf *m, gzblock_read_fn read, void *ctx, size_t want, int *eof) {
    while (m->len < want && !*eof) {
        size_t got;
        if (m->len == m->cap && gzblk_buf_reserve(m, m->cap + 1) != 0)
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
size_t Z_INTERNAL gzblk_header_parse(const uint8_t *buf, size_t len, uint32_t *block_size, uint32_t *zb_flags) {
    size_t pos = 10;
    uint8_t flags;

    *block_size = 0;
    *zb_flags = 0;
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
            if (buf[pos] == 'Z' && buf[pos + 1] == 'B' && sublen >= 4 && pos + 4 + sublen <= end) {
                *block_size = (uint32_t)buf[pos + 4] | ((uint32_t)buf[pos + 5] << 8) |
                              ((uint32_t)buf[pos + 6] << 16) | ((uint32_t)buf[pos + 7] << 24);
                if (sublen >= 5)
                    *zb_flags = buf[pos + 8];
            }
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
    uint32_t zb_flags;
    size_t n = gzblk_header_parse(buf, len, block_size, &zb_flags);
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


void Z_INTERNAL gzblk_block_begin(block_dec *d, PREFIX3(stream) *z, uint8_t *out, uint32_t block_size) {
    d->z = z;
    d->want_marker = 0;
    d->accept_partial = 0;
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
int Z_INTERNAL gzblk_block_feed(block_dec *d, const uint8_t *in, size_t in_len, size_t *used) {
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
                /* A segment that ends at a marker pair is a block at whatever size it produced,
                   pairs do not occur by accident. Lone markers must land exactly on block_size. */
                status = (d->accept_partial && boundary && aligned) ? SEG_FULL : SEG_SHORT;
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

const char Z_INTERNAL *gzblk_seg_name(int status) {
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


static void run_segment(PREFIX3(stream) *z, slot_t *slot, uint32_t block_size) {
    block_dec d;
    gzblk_block_begin(&d, z, slot->out, block_size);
    d.accept_partial = slot->pair;
    slot->status = gzblk_block_feed(&d, slot->in, slot->in_len, &slot->in_used);
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
    err = PREFIX(deflate)(z, slot->last ? Z_FINISH : Z_SYNC_FLUSH);
    if (!slot->last && err == Z_OK)
        err = PREFIX(deflate)(z, Z_FULL_FLUSH);   /* the second marker makes it a boundary */
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

slot_t Z_INTERNAL *gzblk_pool_slot(pool_t *p, size_t i) {
    return &p->ring[i % p->nring];
}

/* Allocate the ring, nthreads * 4 slots of in_cap + out_cap bytes, within RING_BYTES. */
int Z_INTERNAL gzblk_pool_alloc(pool_t *p, int nthreads, size_t in_cap, size_t out_cap) {
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

void Z_INTERNAL gzblk_pool_free(pool_t *p) {
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

int Z_INTERNAL gzblk_pool_start(pool_t *p, int nthreads) {
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

void Z_INTERNAL gzblk_pool_stop(pool_t *p) {
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

void Z_INTERNAL gzblk_slot_submit(pool_t *p, slot_t *slot) {
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

void Z_INTERNAL gzblk_slot_wait(pool_t *p, slot_t *slot) {
    if (p->inline_run) {
        slot_wait_inline(p, slot);
        return;
    }
    pthread_mutex_lock(&p->mu);
    while (slot->state != SLOT_DONE)
        pthread_cond_wait(&p->done_cv, &p->mu);
    pthread_mutex_unlock(&p->mu);
}

void Z_INTERNAL gzblk_slot_release(pool_t *p, slot_t *slot) {
    if (p->inline_run) {
        slot->state = SLOT_FREE;
        return;
    }
    pthread_mutex_lock(&p->mu);
    slot->state = SLOT_FREE;
    pthread_mutex_unlock(&p->mu);
}

#else /* !GZBLOCK_THREADS */

int Z_INTERNAL gzblk_pool_start(pool_t *p, int nthreads) {
    (void)nthreads;
    return pool_start_inline(p);
}

void Z_INTERNAL gzblk_pool_stop(pool_t *p) {
    pool_stop_inline(p);
    p->inline_run = 0;
}

void Z_INTERNAL gzblk_slot_submit(pool_t *p, slot_t *slot) {
    (void)p;
    slot->state = SLOT_FILLED;
}

void Z_INTERNAL gzblk_slot_wait(pool_t *p, slot_t *slot) {
    slot_wait_inline(p, slot);
}

void Z_INTERNAL gzblk_slot_release(pool_t *p, slot_t *slot) {
    (void)p;
    slot->state = SLOT_FREE;
}

#endif

