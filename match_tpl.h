/* match_tpl.h -- find longest match template for compare256 variants
 *
 * Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 * Portions copyright (C) 2014-2021 Konstantin Nosov
 *  Fast-zlib optimized longest_match
 *  https://github.com/gildor2/fast_zlib
 */

#include "insert_string_p.h"

#define EARLY_EXIT_TRIGGER_LEVEL 5

#define GOTO_NEXT_CHAIN \
    if (--chain_length && (cur_match = prev[cur_match & wmask]) > limit) \
        continue; \
    return best_len;

/* Set match_start to the longest match starting at the given string and
 * return its length. Matches shorter or equal to prev_length are discarded,
 * in which case the result is equal to prev_length and match_start is garbage.
 *
 * IN assertions: cur_match is the head of the hash chain for the current
 * string (strstart) and its distance is <= MAX_DIST, and prev_length >=1
 * OUT assertion: the match length is not greater than s->lookahead
 */
Z_INTERNAL uint32_t LONGEST_MATCH(deflate_state *const s, uint32_t cur_match) {
    const unsigned wmask = W_MASK(s);
    unsigned int strstart = s->strstart;
    const unsigned char *window = s->window;
    const Pos *prev = s->prev;
    const Pos *head = s->head;
    const unsigned char *scan;
    const unsigned char *mbase_start = window;
    const unsigned char *mbase_end;
    uint32_t limit;
    uint32_t limit_base;
#ifndef LONGEST_MATCH_ROLL
    int32_t early_exit;
#endif
    uint32_t chain_length = s->max_chain_length;
    uint32_t nice_match = (uint32_t)s->nice_match;
    uint32_t best_len, offset;
    uint32_t lookahead = s->lookahead;
    uint32_t match_offset = 0;
    uint64_t scan_start;
    uint64_t scan_end;

    /* The code is optimized for STD_MAX_MATCH-2 multiple of 16. */
    Assert(STD_MAX_MATCH == 258, "Code too clever");

    scan = window + strstart;
    best_len = s->prev_length ? s->prev_length : STD_MIN_MATCH-1;
#ifdef LONGEST_MATCH_ROLL
    /* Rolling-hash variant always runs the offset search; the compiler
     * folds the constant away in the post-match check below.
     */
    const int offset_search = 1;
#else
    /* Offset-search only pays off for lazy-evaluation callers (deflate_slow).
     * Non-lazy callers (deflate_fast, deflate_medium) would do the extra work
     * per accepted match with no corresponding win, so gate on whether we
     * entered with a prior best_len from lazy evaluation.
     */
    const int offset_search = (best_len >= STD_MIN_MATCH);
#endif

    /* Calculate read offset which should only extend an extra byte to find the
     * next best match length. When best_len is shorter than the read width, we
     * diff the mismatched bytes instead.
     */
    offset = best_len >= sizeof(uint64_t) ? best_len - 7 : 0;

    scan_start = zng_memread_8(scan);
    scan_end = zng_memread_8(scan+offset);
    mbase_end = (mbase_start+offset);

    /* Do not waste too much time if we already have a good match */
    if (best_len >= s->good_match)
        chain_length >>= 2;

    /* Stop when cur_match becomes <= limit. To simplify the code,
     * we prevent matches with the string of window index 0
     */
    limit = strstart > MAX_DIST(s) ? (strstart - MAX_DIST(s)) : 0;
    limit_base = limit;
#ifndef LONGEST_MATCH_ROLL
    early_exit = s->level < EARLY_EXIT_TRIGGER_LEVEL;
#endif
    if (offset_search) {
        /* We're continuing search (lazy evaluation). Find a most distant
         * chain by hashing substrings within the match area. We cannot use
         * s->prev[strstart+1,...] immediately because those strings are not
         * yet inserted into the hash table.
         */
#ifdef LONGEST_MATCH_ROLL
        uint32_t hash;
        uint32_t pos;

        hash = update_hash_roll(0, scan[1]);
        hash = update_hash_roll(hash, scan[2]);

        for (uint32_t i = 3; i <= best_len; i++) {
            hash = update_hash_roll(hash, scan[i]);
            pos = head[hash];
            if (pos < cur_match) {
                match_offset = i - 2;
                cur_match = pos;
            }
        }
#else /* 4-byte integer hash, fresh lookup per offset */
        for (uint32_t i = 1; i + (WANT_MIN_MATCH - 1) <= best_len; i++) {
            uint32_t val = Z_U32_FROM_LE(zng_memread_4(scan + i));
            uint32_t hash = update_hash(0, val);
            uint32_t pos = head[hash];
            if (pos < cur_match) {
                match_offset = i;
                cur_match = pos;
            }
        }
#endif

        /* Update offset-dependent variables */
        limit = limit_base+match_offset;
        if (cur_match <= limit)
            goto break_matching;
        mbase_start -= match_offset;
        mbase_end -= match_offset;
    }
    Assert((unsigned long)strstart <= s->window_size - MIN_LOOKAHEAD, "need lookahead");
    for (;;) {
        if (cur_match >= strstart)
            break;

        /* Skip to next match if the match length cannot increase or if the match length is
         * less than 2. Note that the checks below for insufficient lookahead only occur
         * occasionally for performance reasons.
         * Therefore uninitialized memory will be accessed and conditional jumps will be made
         * that depend on those values. However the length of the match is limited to the
         * lookahead, so the output of deflate is not affected by the uninitialized values.
         */
        uint32_t len;
        if (best_len < sizeof(uint64_t)) {
            uint64_t cand_start = zng_memread_8(mbase_start + cur_match);
            uint64_t diff = scan_start ^ cand_start;
            if (diff != 0) {
                len = zng_first_diff_byte64(diff);
                if (len <= best_len) {
                    GOTO_NEXT_CHAIN;
                }
                goto short_match_accept;
            }
            /* All 8 bytes match, fallthrough to compare256 for the tail. */
        } else {
            for (;;) {
                if (zng_memcmp_8(mbase_end+cur_match, &scan_end) == 0 &&
                    zng_memcmp_8(mbase_start+cur_match, &scan_start) == 0)
                    break;
                GOTO_NEXT_CHAIN;
            }
        }
        len = COMPARE256(scan+2, mbase_start+cur_match+2) + 2;
        Assert(scan+len <= window+(unsigned)(s->window_size-1), "wild scan");

        if (len > best_len)
short_match_accept:
        {
            uint32_t match_start = cur_match - match_offset;
            s->match_start = match_start;

            /* Do not look for better matches if the current match reaches
             * or exceeds the end of the input.
             */
            if (len >= lookahead)
                return lookahead;
            if (len >= nice_match)
                return len;

            best_len = len;

            offset = best_len >= sizeof(uint64_t) ? best_len - 7 : 0;

            scan_end = zng_memread_8(scan+offset);

            /* Look for a better string offset */
            if (offset_search && UNLIKELY(len > STD_MIN_MATCH && match_start + len < strstart)) {
                const unsigned char *scan_endstr;
                uint32_t hash;
                uint32_t pos, next_pos;

                /* Go back to offset 0 */
                cur_match -= match_offset;
                match_offset = 0;
                next_pos = cur_match;

                /* Walk prev[] for positions within the match. The loop bound
                 * must keep the hash key window within the match boundary,
                 * so the prev[] lookups look up chains that share bytes with
                 * the current match (not with unrelated data past its end).
                 */
#ifdef LONGEST_MATCH_ROLL
                for (uint32_t i = 0; i <= len - STD_MIN_MATCH; i++) {
#else /* 4-byte integer hash needs len - 4 bound */
                for (uint32_t i = 0; i + WANT_MIN_MATCH <= len; i++) {
#endif
                    pos = prev[(cur_match + i) & wmask];
                    if (pos < next_pos) {
                        /* Hash chain is more distant, use it */
                        if (pos <= limit_base + i)
                            goto break_matching;
                        next_pos = pos;
                        match_offset = i;
                    }
                }
                /* Switch cur_match to next_pos chain */
                cur_match = next_pos;

                /* Try hash head at a window covering the tail of the current
                 * match to find chains that extend farther back. The window
                 * width differs per variant: 3 bytes for rolling, 4 for integer.
                 */
#ifdef LONGEST_MATCH_ROLL
                scan_endstr = scan + len - (STD_MIN_MATCH+1);

                hash = update_hash_roll(0, scan_endstr[0]);
                hash = update_hash_roll(hash, scan_endstr[1]);
                hash = update_hash_roll(hash, scan_endstr[2]);

                pos = head[hash];
                if (pos < cur_match) {
                    match_offset = len - (STD_MIN_MATCH+1);
                    if (pos <= limit_base + match_offset)
                        goto break_matching;
                    cur_match = pos;
                }
#else
                scan_endstr = scan + len - WANT_MIN_MATCH;
                uint32_t val = Z_U32_FROM_LE(zng_memread_4(scan_endstr));
                hash = update_hash(0, val);

                pos = head[hash];
                if (pos < cur_match) {
                    match_offset = len - WANT_MIN_MATCH;
                    if (pos <= limit_base + match_offset)
                        goto break_matching;
                    cur_match = pos;
                }
#endif

                /* Update offset-dependent variables */
                limit = limit_base+match_offset;
                mbase_start = window-match_offset;
                mbase_end = (mbase_start+offset);
                continue;
            }
            mbase_end = (mbase_start+offset);
        }
#ifndef LONGEST_MATCH_ROLL
        else if (UNLIKELY(early_exit)) {
            /* The probability of finding a match later if we here is pretty low, so for
             * performance it's best to outright stop here for the lower compression levels
             */
            break;
        }
#endif
        GOTO_NEXT_CHAIN;
    }
    return best_len;

break_matching:

    if (best_len < lookahead)
        return best_len;

    return lookahead;
}

#undef LONGEST_MATCH_ROLL
#undef LONGEST_MATCH
