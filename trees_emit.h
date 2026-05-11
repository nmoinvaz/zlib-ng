#ifndef TREES_EMIT_H_
#define TREES_EMIT_H_

#include "zbuild.h"
#include "trees.h"

#ifdef ZLIB_DEBUG
#  include <ctype.h>
#  include <inttypes.h>
#endif


/* trees.h */
extern Z_INTERNAL const ct_data static_ltree[L_CODES+2];
extern Z_INTERNAL const ct_data static_dtree[D_CODES];

extern const unsigned char Z_INTERNAL zng_dist_code[DIST_CODE_LEN];
extern const unsigned char Z_INTERNAL zng_length_code[STD_MAX_MATCH-STD_MIN_MATCH+1];

/* Combined base + extra_bits tables for single-lookup optimization */
extern Z_INTERNAL const uint16_t lbase_extra[LENGTH_CODES];
extern Z_INTERNAL const uint32_t dbase_extra[D_CODES];

/* Bit buffer and deflate code stderr tracing */
#ifdef ZLIB_DEBUG
#  define send_bits_trace(value, length) { \
        Tracevv((stderr, " l %2d v %4llx ", (int)(length), (long long)(value))); \
        Assert(length > 0 && length <= BIT_BUF_SIZE, "invalid length"); \
    }
#  define send_code_trace(c) \
    if (z_verbose > 2) { \
        fprintf(stderr, "\ncd %3d ", (c)); \
    }
#else
#  define send_bits_trace(value, length)
#  define send_code_trace(c)
#endif

/* If not enough room in bi_buf, use (valid) bits from bi_buf and
 * (64 - bi_valid) bits from value, leaving (width - (64-bi_valid))
 * unused bits in value.
 *
 * NOTE: Static analyzers can't evaluate value of total_bits, so we
 *       also need to make sure bi_valid is within acceptable range,
 *       otherwise the shifts will overflow.
 */
#define send_bits(t_val, t_len, e) {\
    uint64_t val = (uint64_t)t_val;\
    uint32_t len = (uint32_t)t_len;\
    uint32_t total_bits = (e)->bi_valid + len;\
    send_bits_trace(val, len);\
    sent_bits_add(s, len);\
    if (total_bits < BIT_BUF_SIZE && (e)->bi_valid < BIT_BUF_SIZE) {\
        (e)->bi_buf |= val << (e)->bi_valid;\
        (e)->bi_valid = total_bits;\
    } else if ((e)->bi_valid >= BIT_BUF_SIZE) {\
        put_uint64(e, (e)->bi_buf);\
        (e)->bi_buf = val;\
        (e)->bi_valid = len;\
    } else {\
        (e)->bi_buf |= val << (e)->bi_valid;\
        put_uint64(e, (e)->bi_buf);\
        (e)->bi_buf = val >> (BIT_BUF_SIZE - (e)->bi_valid);\
        (e)->bi_valid = total_bits - BIT_BUF_SIZE;\
    }\
}

/* Send a code of the given tree. c and tree must not have side effects */
#ifdef ZLIB_DEBUG
#  define send_code(c, tree, e) { \
    send_code_trace(c); \
    send_bits(tree[c].Code, tree[c].Len, e); \
}
#else
#  define send_code(c, tree, e) \
    send_bits(tree[c].Code, tree[c].Len, e)
#endif

/* ===========================================================================
 * Flush the bit buffer and align the output on a byte boundary
 */
static inline void bi_windup(deflate_state *s) {
    deflate_emit_hot e;
    DEFLATE_EMIT_HOT_LOAD(s, e);

    if (e.bi_valid > 56) {
        put_uint64(&e, e.bi_buf);
    } else {
        if (e.bi_valid > 24) {
            put_uint32(&e, (uint32_t)e.bi_buf);
            e.bi_buf >>= 32;
            e.bi_valid -= 32;
        }
        if (e.bi_valid > 8) {
            put_short(&e, (uint16_t)e.bi_buf);
            e.bi_buf >>= 16;
            e.bi_valid -= 16;
        }
        if (e.bi_valid > 0) {
            put_byte(&e, e.bi_buf);
        }
    }

    s->pending = e.pending;
    s->bi_buf = 0;
    s->bi_valid = 0;
}

/* ===========================================================================
 * Emit literal code
 */
static inline void zng_emit_lit(deflate_state *s, const ct_data *ltree, unsigned c,
                                deflate_emit_hot *e) {
    send_code(c, ltree, e);
    Tracecv(isgraph(c & 0xff), (stderr, " '%c' ", c));
    Z_UNUSED(s);
}

/* ===========================================================================
 * Emit match distance/length code
 */
static inline uint32_t zng_emit_dist(deflate_state *s, const ct_data *ltree, const ct_data *dtree,
                                     uint32_t lc, uint32_t dist, deflate_emit_hot *e) {
    uint32_t c, extra, lext;
    uint8_t code;
    uint64_t match_bits;
    uint32_t match_bits_len;

    /* Send the length code, len is the match length - STD_MIN_MATCH */
    code = zng_length_code[lc];
    c = code+LITERALS+1;
    Assert(c < L_CODES, "bad l_code");
    send_code_trace(c);

    match_bits = ltree[c].Code;
    match_bits_len = ltree[c].Len;
    /* Get extra bits count and subtract base length from match length */
    lext = lbase_extra[code];
    extra = lext >> 8;
    lc -= lext & 0xff;
    match_bits |= ((uint64_t)(lc & ((1U << extra) - 1)) << match_bits_len);
    match_bits_len += extra;

    dist--; /* dist is now the match distance - 1 */
    code = d_code(dist);
    Assert(code < D_CODES, "bad d_code");
    send_code_trace(code);

    /* Send the distance code */
    match_bits |= ((uint64_t)dtree[code].Code << match_bits_len);
    match_bits_len += dtree[code].Len;
    /* Get extra bits count and subtract base distance */
    lext = dbase_extra[code];
    extra = lext >> 16;
    dist -= lext & 0xffff;
    match_bits |= ((uint64_t)(dist & ((1U << extra) - 1)) << match_bits_len);
    match_bits_len += extra;

    send_bits(match_bits, match_bits_len, e);

    Z_UNUSED(s);
    return match_bits_len;
}

/* ===========================================================================
 * Emit end block
 */
static inline void zng_emit_end_block(deflate_state *s, const ct_data *ltree, const int last,
                                      deflate_emit_hot *e) {
    send_code(END_BLOCK, ltree, e);
    Tracev((stderr, "\n+++ Emit End Block: Last: %u Pending: %u Total Out: %" PRIu64 "\n",
        last, s->pending, (uint64_t)s->strm->total_out));
    Z_UNUSED(last);
    Z_UNUSED(s);
}

/* ===========================================================================
 * Emit literal and count bits
 */
static inline void zng_tr_emit_lit(deflate_state *s, const ct_data *ltree, unsigned c) {
    deflate_emit_hot e;
    DEFLATE_EMIT_HOT_LOAD(s, e);
    zng_emit_lit(s, ltree, c, &e);
    DEFLATE_EMIT_HOT_STORE(s, e);
    cmpr_bits_add(s, ltree[c].Len);
}

/* ===========================================================================
 * Emit match and count bits
 */
static inline void zng_tr_emit_dist(deflate_state *s, const ct_data *ltree, const ct_data *dtree,
    uint32_t lc, uint32_t dist) {
    deflate_emit_hot e;
    DEFLATE_EMIT_HOT_LOAD(s, e);
    uint32_t bits = zng_emit_dist(s, ltree, dtree, lc, dist, &e);
    DEFLATE_EMIT_HOT_STORE(s, e);
    cmpr_bits_add(s, bits);
}

/* ===========================================================================
 * Emit start of block
 */
static inline void zng_tr_emit_tree(deflate_state *s, int type, const int last) {
    deflate_emit_hot e;
    DEFLATE_EMIT_HOT_LOAD(s, e);
    uint32_t header_bits = (type << 1) + last;
    send_bits(header_bits, 3, &e);
    cmpr_bits_add(s, 3);
    DEFLATE_EMIT_HOT_STORE(s, e);
    Tracev((stderr, "\n--- Emit Tree: Last: %u\n", last));
}

/* ===========================================================================
 * Align bit buffer on a byte boundary and count bits
 */
static inline void zng_tr_emit_align(deflate_state *s) {
    bi_windup(s); /* align on byte boundary */
    sent_bits_align(s);
}

/* ===========================================================================
 * Emit an end block and align bit buffer if last block
 */
static inline void zng_tr_emit_end_block(deflate_state *s, const ct_data *ltree, const int last) {
    deflate_emit_hot e;
    DEFLATE_EMIT_HOT_LOAD(s, e);
    zng_emit_end_block(s, ltree, last, &e);
    DEFLATE_EMIT_HOT_STORE(s, e);
    cmpr_bits_add(s, 7);
    if (last)
        zng_tr_emit_align(s);
}

#endif
