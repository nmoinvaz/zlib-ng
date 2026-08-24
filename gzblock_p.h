/* gzblock_p.h -- private interfaces shared by the gzblock core, reader, and writer
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef GZBLOCK_P_H_
#define GZBLOCK_P_H_

#include "zbuild.h"
#if defined(ZLIB_COMPAT)
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
#endif

#define IO_CHUNK      (256 * 1024)
#define GZ_TRAILER    8
#define RING_BYTES    (1ULL << 30)   /* upper bound on block buffers held in flight */
#define MSG_LEN       128

#define ZB_PAIRED 1     /* "ZB" flags bit, block boundaries are marker pairs */

void Z_INTERNAL gzblk_msgv(char *msg, const char *fmt, va_list ap);
void Z_INTERNAL gzblk_msg(char *msg, const char *fmt, ...);
int Z_INTERNAL gzblk_default_threads(void);

/* Growable byte buffer, consumed from the front by moving an offset rather than the bytes. The
   live data is GZBLK_BUF(m), len bytes at p + off, and compaction happens when space is needed. */
typedef struct {
    uint8_t *p;
    size_t len, cap;
    size_t off;
} membuf;

#define GZBLK_BUF(m) ((m)->p + (m)->off)

int Z_INTERNAL gzblk_buf_reserve(membuf *m, size_t need);
int Z_INTERNAL gzblk_buf_append(membuf *m, const uint8_t *data, size_t n);
void Z_INTERNAL gzblk_buf_drop(membuf *m, size_t n);
int Z_INTERNAL gzblk_buf_fill(membuf *m, gzblock_read_fn read, void *ctx, size_t want, int *eof);

/* Returns the header length, 0 if more bytes are needed, (size_t)-1 if this is not a gzip
   header. */
size_t Z_INTERNAL gzblk_header_parse(const uint8_t *buf, size_t len, uint32_t *block_size, uint32_t *zb_flags);

/* How one piece of a block ended, see gzblk_block_feed(). */
enum { SEG_FULL, SEG_END, SEG_SHORT, SEG_OVERFLOW, SEG_ERROR };

/* Incremental decoder for one independent block, fed one piece of input at a time. */
typedef struct {
    PREFIX3(stream) *z;
    int want_marker;    /* output complete, the trailing empty stored block is still to come */
    int accept_partial; /* the input ends at a marker pair, so any clean output size is a block */
} block_dec;

void Z_INTERNAL gzblk_block_begin(block_dec *d, PREFIX3(stream) *z, uint8_t *out, uint32_t block_size);
int Z_INTERNAL gzblk_block_feed(block_dec *d, const uint8_t *in, size_t in_len, size_t *used);
const char Z_INTERNAL *gzblk_seg_name(int status);

/* Ring of blocks, filled in order by the calling thread, worked on by the pool, drained in
   order. Decompression inflates segments into blocks, compression deflates blocks into pieces. */
enum { SLOT_FREE, SLOT_FILLED, SLOT_CLAIMED, SLOT_DONE };
enum { POOL_INFLATE, POOL_DEFLATE };

typedef struct {
    uint8_t *in;         /* input block or compressed segment, owned by the slot */
    size_t in_len, in_cap;
    int last;            /* final piece of the input */
    int pair;            /* the segment ends with a marker pair, a boundary in its own right */
    uint8_t *out;
    size_t out_cap;      /* grows past block_size for pair-terminated and final segments */
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

slot_t Z_INTERNAL *gzblk_pool_slot(pool_t *p, size_t i);
int Z_INTERNAL gzblk_pool_alloc(pool_t *p, int nthreads, size_t in_cap, size_t out_cap);
void Z_INTERNAL gzblk_pool_free(pool_t *p);
int Z_INTERNAL gzblk_pool_start(pool_t *p, int nthreads);
void Z_INTERNAL gzblk_pool_stop(pool_t *p);
void Z_INTERNAL gzblk_slot_submit(pool_t *p, slot_t *slot);
void Z_INTERNAL gzblk_slot_wait(pool_t *p, slot_t *slot);
void Z_INTERNAL gzblk_slot_release(pool_t *p, slot_t *slot);

#endif /* GZBLOCK_P_H_ */
