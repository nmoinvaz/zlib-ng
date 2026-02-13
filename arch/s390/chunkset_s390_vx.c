/* chunkset_s390_vx.c -- S390 VX inline functions to copy small data chunks.
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifdef S390_VX

#include "zbuild.h"
#include "zmemory.h"

#include <vecintrin.h>

typedef vector unsigned char chunk_t;

#define HAVE_CHUNKMEMSET_2
#define HAVE_CHUNKMEMSET_4
#define HAVE_CHUNKMEMSET_8

static inline void chunkmemset_2(uint8_t *from, chunk_t *chunk) {
    *chunk = (vector unsigned char)vec_splats(zng_memread_2(from));
}

static inline void chunkmemset_4(uint8_t *from, chunk_t *chunk) {
    *chunk = (vector unsigned char)vec_splats(zng_memread_4(from));
}

static inline void chunkmemset_8(uint8_t *from, chunk_t *chunk) {
    *chunk = (vector unsigned char)vec_splats((unsigned long long)zng_memread_8(from));
}

static inline void loadchunk(uint8_t const *s, chunk_t *chunk) {
    *chunk = vec_xl(0, s);
}

static inline void storechunk(uint8_t *out, chunk_t *chunk) {
    vec_xst(*chunk, 0, out);
}

#define CHUNKSIZE        chunksize_s390_vx
#define CHUNKCOPY        chunkcopy_s390_vx
#define CHUNKUNROLL      chunkunroll_s390_vx
#define CHUNKMEMSET      chunkmemset_s390_vx
#define CHUNKMEMSET_SAFE chunkmemset_safe_s390_vx

#include "chunkset_tpl.h"

#define INFLATE_FAST     inflate_fast_s390_vx

#include "inffast_tpl.h"

#endif /* S390_VX */
