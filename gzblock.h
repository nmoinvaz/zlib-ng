/* gzblock.h -- gzip members made of independent deflate blocks, written and read in parallel
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef GZBLOCK_H_
#define GZBLOCK_H_

#include "zbuild.h"
#include <stddef.h>
#include <stdint.h>

/* Internal to the library, the gz layer sits on top of this, see gzsetblocksize() and
   gzsetthreads(). Errors are reported with a zlib return code and a message. */

/* Largest block size accepted, from a caller or from a file's header. Bounds what a member can make
   the reader allocate, two slots of input and output at this size stay within the ring budget. */
#define GZBLOCK_MAX_BLOCK (256u << 20)

/* I/O callbacks. read returns the bytes read, 0 at end of input, (size_t)-1 on error. write returns
   the bytes written, anything short of len is an error. */
typedef size_t (*gzblock_read_fn)(void *ctx, uint8_t *buf, size_t len);
typedef size_t (*gzblock_write_fn)(void *ctx, const uint8_t *buf, size_t len);

/* Parse a gzip header. Returns 1 and sets *hdr_len when complete, 0 when more bytes are needed, -1
   when buf is not a gzip header. *block_size receives the value of a "ZB" extra subfield, or 0. */
int Z_INTERNAL gzblock_parse_header(const uint8_t *buf, size_t len, size_t *hdr_len, uint32_t *block_size);

/* Writer. Produces one gzip member whose deflate stream is cut into independent blocks of
   block_size input bytes, deflated on nthreads threads, with the block size recorded in a "ZB"
   header extra subfield. nthreads of 0 picks the number of CPUs, 1 does the work on the calling
   thread. */
typedef struct gzblock_writer_s gzblock_writer;

gzblock_writer Z_INTERNAL *gzblock_wopen(gzblock_write_fn write, void *ctx, int level, int strategy,
                              uint32_t block_size, int nthreads);
int Z_INTERNAL gzblock_write(gzblock_writer *w, const uint8_t *buf, size_t len);   /* 0, or -1 on error */
int Z_INTERNAL gzblock_wsetparams(gzblock_writer *w, int level, int strategy);  /* for the blocks to come */
int Z_INTERNAL gzblock_wflush(gzblock_writer *w);    /* end the current block early and write everything out */
int Z_INTERNAL gzblock_wfinish(gzblock_writer *w);   /* write the last block and the trailer */
const char Z_INTERNAL *gzblock_werror(const gzblock_writer *w);
int Z_INTERNAL gzblock_werrcode(const gzblock_writer *w);    /* Z_ERRNO, Z_MEM_ERROR, ... */
void Z_INTERNAL gzblock_wclose(gzblock_writer *w);   /* free, without finishing if that has not happened */

/* Reader. Decodes gzip data, member by member. A member whose header records a block size, or
   any member when block_size is nonzero, is inflated as independent blocks on nthreads threads at
   once, nthreads of 0 picking the number of CPUs and 1 doing the work on the calling thread.
   Other members are streamed through plain inflate. Input that is not gzip at all is passed through unchanged, trailing garbage after the
   last member is ignored, both as gzread() does. head holds bytes already taken from the input
   that come before what read() returns, or NULL. */
typedef struct gzblock_reader_s gzblock_reader;

gzblock_reader Z_INTERNAL *gzblock_ropen(gzblock_read_fn read, void *ctx, const uint8_t *head, size_t head_len,
                              uint32_t block_size, int nthreads);
int Z_INTERNAL gzblock_read(gzblock_reader *r, uint8_t *buf, size_t len, size_t *got);   /* 0, or -1 on error */
/* Hand out the next piece of output without copying. *p and *n describe bytes owned by the reader,
   valid until the next gzblock_read() or gzblock_rnext() call, *n is 0 at the end of the data. */
int Z_INTERNAL gzblock_rnext(gzblock_reader *r, const uint8_t **p, size_t *n);             /* 0, or -1 on error */
const char Z_INTERNAL *gzblock_rerror(const gzblock_reader *r);
int Z_INTERNAL gzblock_rerrcode(const gzblock_reader *r);    /* Z_ERRNO, Z_DATA_ERROR, Z_BUF_ERROR, ... */
void Z_INTERNAL gzblock_rclose(gzblock_reader *r);

#endif /* GZBLOCK_H_ */
