/* Optimized slide_hash for S390 processors with VX (Vector Extension)
 * Copyright (C) 2024 Contributors to the zlib-ng project
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifdef S390_VX

#include <vecintrin.h>
#include "zbuild.h"
#include "deflate.h"

static inline void slide_hash_chain(Pos *table, uint32_t entries, uint16_t wsize) {
    const vector unsigned short vmx_wsize = vec_splats(wsize);
    Pos *p = table;

    do {
        vector unsigned short value, result;

        value = vec_xl(0, p);
        /* Emulate saturating subtract: vec_sub(value, vec_min(value, vmx_wsize)) */
        result = vec_sub(value, vec_min(value, vmx_wsize));
        vec_xst(result, 0, p);

        p += 8;
        entries -= 8;
    } while (entries > 0);
}

void Z_INTERNAL slide_hash_s390_vx(deflate_state *s) {
    Assert(s->w_size <= UINT16_MAX, "w_size should fit in uint16_t");
    uint16_t wsize = (uint16_t)s->w_size;

    slide_hash_chain(s->head, HASH_SIZE, wsize);
    slide_hash_chain(s->prev, wsize, wsize);
}

#endif /* S390_VX */
