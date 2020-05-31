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
#include "../../trees_emit.h"

extern const ct_data static_ltree[L_CODES+2];
extern const ct_data static_dtree[D_CODES];

static inline void emit_block_start(deflate_state *s, int last) {
    zng_tr_emit_tree(s, STATIC_TREES, last);
    s->block_open = 1;
    s->block_start = s->strstart;
    s->sym_next = 6;
}

static inline void emit_block_end(deflate_state *s, int last) {
    zng_tr_emit_end_block(s, static_ltree, last);
    s->block_open = 0;
    s->block_start = s->strstart;
    s->sym_next = 0;
}

ZLIB_INTERNAL block_state deflate_quick(deflate_state *s, int flush) {
    IPos hash_head;
    Pos match_len = 0;
    int bflush = 0;
    int last;
    
    last = (flush == Z_FINISH) ? 1 : 0;
    
    for (;;) {
        if (s->block_open == 0) {
            emit_block_start(s, last);
        }

        if (s->lookahead < MIN_LOOKAHEAD) {
            fill_window(s);
            if (s->lookahead < MIN_LOOKAHEAD && flush == Z_NO_FLUSH) {
                QUICK_FLUSH_BLOCK(s, 0);
                return need_more;
            }
            if (s->lookahead == 0)
                break;
        }

        hash_head = 0;
        if (s->lookahead >= MIN_MATCH) {
            hash_head = functable.quick_insert_string(s, s->strstart);
        }

        if (hash_head != 0 && s->strstart - hash_head <= MAX_DIST(s)) {
            match_len = functable.compare258(s->window + s->strstart, s->window + hash_head);
        }

        if (match_len >= MIN_MATCH)  {
            if (match_len > s->lookahead)
                match_len = s->lookahead;

            check_match(s, s->strstart, hash_head, match_len);

            bflush = zng_tr_emit_dist(s, static_ltree, static_dtree, s->strstart - hash_head, match_len - MIN_MATCH);
            s->lookahead -= match_len;
            s->strstart += match_len;

            match_len = 0;
        } else {
            bflush = zng_tr_emit_lit(s, static_ltree, s->window[s->strstart]);
            s->strstart++;
            s->lookahead--;
        }

        if (bflush)
            QUICK_FLUSH_BLOCK(s, 0);
    }

    s->insert = s->strstart < MIN_MATCH-1 ? s->strstart : MIN_MATCH-1;
    QUICK_BLOCK_ONLY(s, last);
    if (last) {
        if (s->strm->avail_out == 0)
            return s->strm->avail_in == 0 ? finish_started : need_more;
        else
            return finish_done;
    }

    return block_done;
}
