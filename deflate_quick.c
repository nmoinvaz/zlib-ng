/*
 * The deflate_quick deflate strategy, designed to be used when cycles are
 * at a premium.
 *
 * Copyright (C) 2013 Intel Corporation. All rights reserved.
 * Authors:
 *  Wajdi Feghali   <wajdi.k.feghali@intel.com>
 *  Jim Guilford    <james.guilford@intel.com>
 *  Vinodh Gopal    <vinodh.gopal@intel.com>
 *     Erdinc Ozturk   <erdinc.ozturk@intel.com>
 *  Jim Kukunas     <james.t.kukunas@linux.intel.com>
 *
 * Portions are Copyright (C) 2016 12Sided Technology, LLC.
 * Author:
 *  Phil Vachon     <pvachon@12sidedtech.com>
 *
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zbuild.h"
#include "zmemory.h"
#include "deflate.h"
#include "deflate_p.h"
#include "functable.h"
#include "trees_emit.h"
#include "insert_string_p.h"

extern const ct_data static_ltree[L_CODES+2];
extern const ct_data static_dtree[D_CODES];

Z_FORCEINLINE static void quick_start_block(deflate_state *s, uint32_t strstart, int last) {
    zng_tr_emit_tree(s, STATIC_TREES, last);
    s->block_open = 1 + last;
    s->block_start = (int)strstart;
}

Z_FORCEINLINE static int quick_end_block(deflate_state *s, uint32_t strstart, int last) {
    if (s->block_open) {
        zng_tr_emit_end_block(s, static_ltree, last);
        s->block_open = 0;
        s->block_start = (int)strstart;
        PREFIX(flush_pending)(s->strm);
        return (s->strm->avail_out == 0);
    }
    return 0;
}

/* ===========================================================================
 * One match loop for both emission modes. static_emit is a compile-time
 * constant, so each instantiation keeps only its own emission code and the
 * shared single-probe search is written once.
 *
 * static_emit: Z_FIXED, the trees are known up front so every symbol goes
 * straight through the static tables, single pass with no symbol buffer.
 * Otherwise symbols are tallied and the block flush builds per-block trees.
 */
Z_FORCEINLINE static block_state deflate_quick_impl(deflate_state *s, int flush, const int static_emit) {
    unsigned char *window;
    /* Carrying the scan state in locals keeps it in callee-saved registers
       across the compare256 and flush calls, instead of a load and store
       pair through the state on every symbol. */
    uint32_t strstart = s->strstart;
    uint32_t lookahead = s->lookahead;
    unsigned last = (flush == Z_FINISH) ? 1 : 0;

    if (static_emit) {
        if (UNLIKELY(last && s->block_open != 2)) {
            /* Emit end of previous block */
            if (quick_end_block(s, strstart, 0))
                return need_more;
            /* Emit start of last block */
            quick_start_block(s, strstart, last);
        } else if (UNLIKELY(s->block_open == 0 && lookahead > 0)) {
            /* Start new block only when we have lookahead data, so that if no
               input data is given an empty block will not be written */
            quick_start_block(s, strstart, last);
        }
    }

    window = s->window;

    for (;;) {
        if (static_emit) {
            if (UNLIKELY(s->pending + ((BIT_BUF_SIZE + 7) >> 3) >= s->pending_buf_size)) {
                PREFIX(flush_pending)(s->strm);
                if (s->strm->avail_out == 0) {
                    s->lookahead = lookahead;
                    s->strstart = strstart;
                    return (last && s->strm->avail_in == 0 && s->bi_valid == 0 && s->block_open == 0) ? finish_started : need_more;
                }
            }
        }

        if (UNLIKELY(lookahead < MIN_LOOKAHEAD)) {
            s->lookahead = lookahead;
            s->strstart = strstart;
            PREFIX(fill_window)(s);
            lookahead = s->lookahead;
            strstart = s->strstart;
            if (UNLIKELY(lookahead < MIN_LOOKAHEAD && flush == Z_NO_FLUSH))
                return need_more;
            if (UNLIKELY(lookahead == 0))
                break;

            if (static_emit && UNLIKELY(s->block_open == 0)) {
                /* Start new block when we have lookahead data, so that if no
                   input data is given an empty block will not be written */
                quick_start_block(s, strstart, last);
            }
        }

        uint32_t str_val = Z_U32_FROM_LE(zng_memread_4(window + strstart));

        if (LIKELY(lookahead >= WANT_MIN_MATCH)) {
            uint32_t hash_head = insert_knuth_val_head(s, strstart, str_val);
            int64_t dist = (int64_t)strstart - hash_head;

            if (dist <= MAX_DIST(s) && dist > 0) {
                const uint8_t *match_start = window + hash_head;
                uint32_t match_val = Z_U32_FROM_LE(zng_memread_4(match_start));

                if (str_val == match_val) {
                    const uint8_t *scan_start = window + strstart;
                    uint32_t match_len = FUNCTABLE_CALL(compare256)(scan_start+2, match_start+2) + 2;

                    if (match_len >= WANT_MIN_MATCH) {
                        if (UNLIKELY(match_len > lookahead))
                            match_len = lookahead;

                        Assert(match_len <= STD_MAX_MATCH, "match too long");
                        check_match(s, strstart, hash_head, match_len);

                        if (static_emit) {
                            Assert(strstart <= UINT16_MAX, "strstart should fit in uint16_t");
                            zng_tr_emit_dist(s, static_ltree, static_dtree, match_len - STD_MIN_MATCH, (uint32_t)dist);
                            lookahead -= match_len;
                            strstart += match_len;
                        } else {
                            int bflush = zng_tr_tally_dist(s, (uint32_t)dist, match_len - STD_MIN_MATCH);
                            lookahead -= match_len;
                            strstart += match_len;
                            if (UNLIKELY(bflush)) {
                                s->strstart = strstart;
                                s->lookahead = lookahead;
                                FLUSH_BLOCK(s, window, 0);
                            }
                        }
                        continue;
                    }
                }
            }
        }

        if (static_emit) {
            zng_tr_emit_lit(s, static_ltree, (uint8_t)str_val);
            strstart++;
            lookahead--;
        } else {
            int bflush = zng_tr_tally_lit(s, (uint8_t)str_val);
            strstart++;
            lookahead--;
            if (UNLIKELY(bflush)) {
                s->strstart = strstart;
                s->lookahead = lookahead;
                FLUSH_BLOCK(s, window, 0);
            }
        }
    }

    s->lookahead = lookahead;
    s->strstart = strstart;
    s->insert = strstart < (STD_MIN_MATCH - 1) ? strstart : (STD_MIN_MATCH - 1);

    if (static_emit) {
        if (UNLIKELY(last)) {
            if (quick_end_block(s, strstart, 1))
                return finish_started;
            return finish_done;
        }
        if (quick_end_block(s, strstart, 0))
            return need_more;
        return block_done;
    }

    if (UNLIKELY(flush == Z_FINISH)) {
        FLUSH_BLOCK(s, window, 1);
        return finish_done;
    }
    if (UNLIKELY(s->sym_next))
        FLUSH_BLOCK(s, window, 0);
    return block_done;
}

/* Z_FIXED path: every symbol goes straight through the static tables. */
static block_state deflate_quick_static(deflate_state *s, int flush) {
    return deflate_quick_impl(s, flush, 1);
}

/* Default path: symbols are tallied so the flush builds per-block trees. */
static block_state deflate_quick_dynamic(deflate_state *s, int flush) {
    return deflate_quick_impl(s, flush, 0);
}

Z_INTERNAL block_state deflate_quick(deflate_state *s, int flush) {
    if (UNLIKELY(s->strategy == Z_FIXED))
        return deflate_quick_static(s, flush);
    return deflate_quick_dynamic(s, flush);
}
