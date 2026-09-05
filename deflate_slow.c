/* deflate_slow.c -- compress data using the slow strategy of deflation algorithm
 *
 * Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zbuild.h"
#include "zmemory.h"
#include "deflate.h"
#include "deflate_p.h"
#include "functable.h"
#include "insert_string_p.h"
#include "trees.h"

/* ===========================================================================
 * Same as deflate_medium, but achieves better compression. We use a lazy
 * evaluation for matches: a match is finally adopted only if there is
 * no better match at the next window position.
 */
/* Minimum-length matches beyond this distance cost more bits than the three
   literals they replace. */
#define TOO_FAR 4096

/* Blocks shorter than this never split, their tree headers cost too much. */
#define SPLIT_MIN_BLOCK 5000

/* Coarse symbol classes for the split statistics: literals keyed by their top
   two and low bits, matches split into short and long. */
Z_FORCEINLINE static void split_observe_lit(deflate_state *s, uint8_t c) {
    s->split_new[((c >> 5) & 0x6) | (c & 1)]++;
    s->split_num_new++;
}

Z_FORCEINLINE static void split_observe_match(deflate_state *s, uint32_t len) {
    s->split_new[SPLIT_LIT_TYPES + (len >= 9)]++;
    s->split_num_new++;
}

/* End the block when the distribution of the symbols observed since the last
   check diverges from the block's overall distribution, so tree boundaries
   land where the data changes character instead of where the buffer fills. */
static int split_should_end(deflate_state *s, uint32_t block_len) {
    if (s->split_num_obs > 0) {
        uint32_t total_delta = 0;
        uint32_t num_items, cutoff;
        int i;

        /* Sum of absolute probability differences, scaled by
           split_num_obs * split_num_new to stay in integers. */
        for (i = 0; i < SPLIT_TYPES; i++) {
            uint32_t expected = s->split_obs[i] * s->split_num_new;
            uint32_t actual = s->split_new[i] * s->split_num_obs;
            total_delta += (actual > expected) ? actual - expected : expected - actual;
        }

        num_items = s->split_num_obs + s->split_num_new;
        /* Diverged when the summed probability delta reaches 200/512, with an
           extra penalty while the block is still short. */
        cutoff = s->split_num_new * 200 / 512 * s->split_num_obs;
        if (block_len < 10000 && num_items < 8192)
            cutoff += (uint32_t)((uint64_t)cutoff * (8192 - num_items) / 8192);

        if (total_delta + (block_len / 4096) * s->split_num_obs >= cutoff)
            return 1;
    }
    for (int i = 0; i < SPLIT_TYPES; i++) {
        s->split_obs[i] += s->split_new[i];
        s->split_new[i] = 0;
    }
    s->split_num_obs += s->split_num_new;
    s->split_num_new = 0;
    return 0;
}

Z_INTERNAL block_state deflate_slow(deflate_state *s, int flush) {
    longest_match_func longest_match = s->longest_match;
    insert_batch_func insert_batch = s->insert_batch;
    unsigned char *window = s->window;
    int bflush;              /* set if current block must be flushed */
    int level = s->level;
    /* Carrying the scan state in locals keeps it in callee-saved registers
       across the longest_match, insert and flush calls, synced back only
       where a callee reads it. */
    uint32_t strstart = s->strstart;
    uint32_t lookahead = s->lookahead;
    uint32_t prev_length = s->prev_length;
    unsigned int match_available = s->match_available;
    const int64_t max_dist = (int64_t)MAX_DIST(s);
    const uint32_t max_lazy = s->max_lazy_match;

    /* Matches this long or shorter are discarded. Z_FILTERED ignores lengths up to 5, other
     * strategies only ignore lengths below STD_MIN_MATCH. */
    const uint32_t match_discard = (s->strategy == Z_FILTERED) ? 5 : STD_MIN_MATCH - 1;
    /* Same floor for longest_match, kept below good_match. */
    const uint32_t match_floor = MIN(match_discard, s->good_match - 1);

#define SLOW_SYNC_STATE() do { \
        s->strstart = strstart; \
        s->lookahead = lookahead; \
        s->prev_length = prev_length; \
        s->match_available = match_available; \
    } while (0)

    /* Process the input block. */
    for (;;) {
        /* Make sure that we always have enough lookahead, except
         * at the end of the input file. We need STD_MAX_MATCH bytes
         * for the next match, plus WANT_MIN_MATCH bytes to insert the
         * string following the next match.
         */
        if (UNLIKELY(lookahead < MIN_LOOKAHEAD)) {
            SLOW_SYNC_STATE();
            PREFIX(fill_window)(s);
            strstart = s->strstart;
            lookahead = s->lookahead;
            if (UNLIKELY(lookahead < MIN_LOOKAHEAD && flush == Z_NO_FLUSH)) {
                return need_more;
            }
            if (UNLIKELY(lookahead == 0))
                break; /* flush the current block */
        }

        /* Insert the string window[strstart .. strstart+2] in the
         * dictionary, and set hash_head to the head of the hash chain:
         */
        uint32_t hash_head = 0;
        if (LIKELY(lookahead >= WANT_MIN_MATCH)) {
            if (level >= 9)
                hash_head = insert_roll(s, window, strstart);
            else
                hash_head = insert_knuth(s, window, strstart);
        }

        /* Find the longest match, discarding those <= prev_length.
         */
        s->prev_match = s->match_start;
        uint32_t match_len = STD_MIN_MATCH - 1;
        int64_t dist = (int64_t)strstart - hash_head;

        if (dist <= max_dist && dist > 0 && prev_length < max_lazy && hash_head != 0) {
            /* To simplify the code, we prevent matches with the string
             * of window index 0 (in particular we have to avoid a match
             * of the string with itself at the start of the input file).
             */

            if (UNLIKELY(dist == 1 &&
                zng_memread_4(window + strstart) == zng_memread_4(window + strstart - 1))) {
                /* A run of one repeated byte is its own best match at distance
                   one. The hash chain holds every earlier position of the run,
                   so the chain walk would inspect them all only to converge on
                   the same overlapping match. */
                match_len = FUNCTABLE_CALL(compare256)(window + strstart + 2, window + strstart + 1) + 2;
                match_len = MIN(match_len, lookahead);
                s->match_start = strstart - 1;
            } else {
                /* longest_match only looks for matches longer than s->prev_length.
                   The floor pairs with the discard below so a phantom seeded length
                   can never be accepted as a real match. Level 9 stays pure
                   maximum compression, so the adaptive floor applies below it. */
                uint32_t adaptive = level < 9 ? s->match_floor : STD_MIN_MATCH - 1;
                uint32_t floor = MIN(MAX(match_floor, adaptive), s->good_match - 1);
                uint32_t discard = MAX(match_discard, adaptive);
                s->strstart = strstart;
                s->lookahead = lookahead;
                s->prev_length = MAX(prev_length, floor);
                match_len = longest_match(s, hash_head);
                /* longest_match() sets match_start; the locals stay authoritative */

                if (match_len <= discard ||
                    (match_len == STD_MIN_MATCH && strstart - s->match_start > TOO_FAR)) {
                    /* Match not long enough, treat it as no match found, which makes a garbage
                     * match_start that is harmless. */
                    match_len = STD_MIN_MATCH - 1;
                } else if (UNLIKELY(match_len <= 6) && s->lit_cost_q3 != 0 && level >= 7) {
                    /* Price the short match against the exact literals it
                       replaces with the previous block's code lengths, which
                       persist across the block flush. Only lengths up to six
                       can cost more than their literals. */
                    uint32_t lc = zng_length_code[match_len - STD_MIN_MATCH];
                    uint32_t dc = d_code(strstart - s->match_start);
                    uint32_t lbits = s->dyn_ltree[lc + LITERALS + 1].Len;
                    uint32_t dbits = s->dyn_dtree[dc].Len;
                    uint32_t match_bits = (lbits ? lbits : 13) + (uint32_t)extra_lbits[lc] +
                                          (dbits ? dbits : 13) + (uint32_t)extra_dbits[dc];
                    uint32_t lit_bits = 0;
                    for (uint32_t i = 0; i < match_len; i++) {
                        uint32_t l = s->dyn_ltree[window[strstart + i]].Len;
                        lit_bits += l ? l : 13;
                    }
                    if (match_bits > lit_bits)
                        match_len = STD_MIN_MATCH - 1;
                }
            }
        }
        /* If there was a match at the previous step and the current
         * match is not better, output the previous match:
         */
        if (prev_length >= STD_MIN_MATCH && match_len <= prev_length) {
            unsigned int max_insert = strstart + lookahead - STD_MIN_MATCH;
            /* Do not insert strings in hash table beyond this. */

            Assert((strstart-1) <= UINT16_MAX, "strstart-1 should fit in uint16_t");
            check_match(s, strstart - 1, s->prev_match, prev_length);

            bflush = zng_tr_tally_dist(s, strstart -1 - s->prev_match, prev_length - STD_MIN_MATCH);
            split_observe_match(s, prev_length);

            /* Insert in hash table all strings up to the end of the match.
             * strstart-1 and strstart are already inserted. If there is not
             * enough lookahead, the last two strings are not inserted in
             * the hash table.
             */
            prev_length -= 1;
            lookahead -= prev_length;

            unsigned int mov_fwd = prev_length - 1;
            if (max_insert > strstart) {
                unsigned int insert_cnt = mov_fwd;
                if (UNLIKELY(insert_cnt > max_insert - strstart))
                    insert_cnt = max_insert - strstart;
                insert_batch(s, window, strstart + 1, insert_cnt);
            }
            prev_length = 0;
            match_available = 0;
            strstart += mov_fwd + 1;

            if (UNLIKELY(bflush)) {
                SLOW_SYNC_STATE();
                FLUSH_BLOCK(s, window, 0);
            } else if (UNLIKELY(s->split_num_new >= SPLIT_CHECK_SYMS)) {
                uint32_t block_len = (uint32_t)((int)strstart - s->block_start);
                if (block_len >= SPLIT_MIN_BLOCK && split_should_end(s, block_len)) {
                    SLOW_SYNC_STATE();
                    FLUSH_BLOCK(s, window, 0);
                }
            }

        } else if (match_available) {
            /* If there was no match at the previous position, output a
             * single literal. If there was a match but the current match
             * is longer, truncate the previous match to a single literal.
             */
            bflush = zng_tr_tally_lit(s, window[strstart-1]);
            split_observe_lit(s, window[strstart-1]);
            if (UNLIKELY(bflush)) {
                SLOW_SYNC_STATE();
                FLUSH_BLOCK_ONLY(s, window, 0);
            } else if (UNLIKELY(s->split_num_new >= SPLIT_CHECK_SYMS)) {
                uint32_t block_len = (uint32_t)((int)strstart - s->block_start);
                if (block_len >= SPLIT_MIN_BLOCK && split_should_end(s, block_len)) {
                    SLOW_SYNC_STATE();
                    FLUSH_BLOCK_ONLY(s, window, 0);
                }
            }
            prev_length = match_len;
            strstart++;
            lookahead--;
            if (UNLIKELY(s->strm->avail_out == 0)) {
                SLOW_SYNC_STATE();
                return need_more;
            }
        } else {
            /* There is no previous match to compare with, wait for
             * the next step to decide.
             */
            prev_length = match_len;
            match_available = 1;
            strstart++;
            lookahead--;
        }
    }
    SLOW_SYNC_STATE();
#undef SLOW_SYNC_STATE
    Assert(flush != Z_NO_FLUSH, "no flush?");
    if (UNLIKELY(s->match_available)) {
        Z_UNUSED(zng_tr_tally_lit(s, window[s->strstart-1]));
        s->match_available = 0;
    }
    s->insert = s->strstart < (STD_MIN_MATCH - 1) ? s->strstart : (STD_MIN_MATCH - 1);
    if (UNLIKELY(flush == Z_FINISH)) {
        FLUSH_BLOCK(s, window, 1);
        return finish_done;
    }
    if (UNLIKELY(s->sym_next))
        FLUSH_BLOCK(s, window, 0);
    return block_done;
}
