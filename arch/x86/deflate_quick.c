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

#include "../../zbuild.h"
#include <immintrin.h>
#ifdef _MSC_VER
#  include <nmmintrin.h>
#endif
#include "../../deflate.h"
#include "../../deflate_p.h"
#include "../../functable.h"
#include "../../memcopy.h"

extern Pos quick_insert_string_sse(deflate_state *const s, const Pos str);
extern void fill_window_sse(deflate_state *s);

extern void flush_pending(PREFIX3(stream) *strm);

extern void zng_tr_emit_lit(deflate_state *s, const ct_data *ltree, unsigned c);
extern void zng_tr_emit_tree(deflate_state *s, int type, const int last);
extern void zng_tr_emit_end_block(deflate_state *s, const ct_data *ltree, const int last);
extern void zng_tr_emit_dist(deflate_state *s, const ct_data *ltree, const ct_data *dtree, 
    uint32_t lc, uint32_t dist);

ZLIB_INTERNAL block_state deflate_quick(deflate_state *s, int flush) {
    IPos hash_head;
    unsigned dist, match_len, last;

    if (s->block_open == 0) {
        last = (flush == Z_FINISH) ? 1 : 0;
        zng_tr_emit_tree(s, STATIC_TREES, last);
    }

    do {
        if (s->pending + 4 >= s->pending_buf_size) {
            flush_pending(s->strm);
            if (s->strm->avail_in == 0 && flush != Z_FINISH) {
                return need_more;
            }
        }

        if (s->lookahead < MIN_LOOKAHEAD) {
            fill_window_sse(s);
            if (s->lookahead < MIN_LOOKAHEAD && flush == Z_NO_FLUSH) {
                zng_tr_emit_end_block(s, static_ltree, 0);
                s->block_start = s->strstart;
                flush_pending(s->strm);
                return need_more;
            }
            if (s->lookahead == 0)
                break;
        }

        if (s->lookahead >= MIN_MATCH) {
            hash_head = quick_insert_string_sse(s, s->strstart);
            dist = s->strstart - hash_head;

            if (dist > 0 && (dist-1) < s->w_size) {
                match_len = functable.compare258(s->window + s->strstart, s->window + s->strstart - dist);

                if (match_len >= MIN_MATCH) {
                    if (match_len > s->lookahead)
                        match_len = s->lookahead;

                    if (match_len > MAX_MATCH)
                        match_len = MAX_MATCH;

                    zng_tr_emit_dist(s, static_ltree, static_dtree, match_len - MIN_MATCH, s->strstart - hash_head);
                    s->lookahead -= match_len;
                    s->strstart += match_len;
                    continue;
                }
            }
        }

        zng_tr_emit_lit(s, static_ltree, s->window[s->strstart]);
        s->strstart++;
        s->lookahead--;
    } while (s->strm->avail_out != 0);

    if (s->strm->avail_out == 0 && flush != Z_FINISH)
        return need_more;

    s->insert = s->strstart < MIN_MATCH - 1 ? s->strstart : MIN_MATCH-1;

    last = (flush == Z_FINISH) ? 1 : 0;
    zng_tr_emit_end_block(s, static_ltree, last);
    s->block_start = s->strstart;
    flush_pending(s->strm);

    if (last) {
        if (s->strm->avail_out == 0)
            return s->strm->avail_in == 0 ? finish_started : need_more;
        else
            return finish_done;
    }

    return block_done;
}
