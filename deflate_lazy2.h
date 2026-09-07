/* deflate_lazy2.h -- two-step lazy match probe for the slow levels
 * Copyright (C) 2026 Nathan Moinvaziri
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 * Before adopting a pending match that just beat the current position's
 * match, the slow levels can look one position further. The probe peeks at
 * the chain head without inserting, so backing out costs nothing, and it
 * prices the deferred match against the pending one before accepting.
 * Emitting the two literals that buy the deferred match stays with the
 * caller, that path is fused to deflate_slow's block flushing.
 */
#ifndef DEFLATE_LAZY2_H
#define DEFLATE_LAZY2_H

/* The slow levels look one position past the lazy match before committing it.
   Deferring costs an extra literal, so the deferred match has to gain more
   than the pending one is worth. */
#define LAZY2_MIN_GAIN 6

/* ===========================================================================
 * Estimated bit gain from taking a match of new_len at new_dist over one of
 * cur_len at cur_dist. A length byte is worth about four bits, and a distance
 * costs its magnitude in extra bits, so a shorter distance offsets a smaller
 * length.
 */
static inline int lazy_gain(uint32_t new_len, uint32_t cur_len, uint32_t new_dist, uint32_t cur_dist) {
    return 4 * (int)(new_len - cur_len) + (int)zng_clz32(new_dist) - (int)zng_clz32(cur_dist);
}

/* ===========================================================================
 * Probe one position past the pending match. Returns the deferred match's
 * length when it wins, 0 otherwise. On a win s->match_start points at the
 * deferred match. s->strstart, s->lookahead and s->prev_length are left as the
 * probe's scratch either way, the caller resyncs them from its locals. The
 * rolling hash levels peek by extending the running state, the Knuth levels
 * hash the four bytes directly. A hit-rate throttle retires the probing on
 * streams that never produce worthwhile wins, helped below the maximum level
 * by a higher gain floor that rejects marginal one-byte wins.
 */
Z_FORCEINLINE static uint32_t lazy2_probe(deflate_state *s, unsigned char *window, int level,
                                          longest_match_func longest_match,
                                          uint32_t strstart, uint32_t lookahead,
                                          uint32_t prev_length, uint32_t max_lazy,
                                          int64_t max_dist) {
    if (lookahead <= MIN_LOOKAHEAD || prev_length >= max_lazy ||
        (s->lazy2_probes >= 256 && s->lazy2_hits * 16 < s->lazy2_probes))
        return 0;

    uint32_t next_pos = strstart + 1;
    uint32_t hash_head2;
    if (level >= MIN_ROLL_LEVEL) {
        hash_head2 = s->head[update_hash_roll(s->ins_h, window[next_pos + STD_MIN_MATCH - 1])];
    } else {
        uint32_t h2;
        UPDATE_HASH_KNUTH(h2, Z_U32_FROM_LE(zng_memread_4(window + next_pos)));
        hash_head2 = s->head[h2];
    }

    int64_t dist2 = (int64_t)next_pos - hash_head2;
    if (dist2 > max_dist || dist2 <= 0 || hash_head2 == 0)
        return 0;

    uint32_t cur_dist = strstart - 1 - s->prev_match;
    uint32_t saved_match_start = s->match_start;

    /* longest_match() scans from strstart, floors at prev_length and clamps
     * to lookahead, so probe with both stepped. */
    s->strstart = next_pos;
    s->lookahead = lookahead - 1;
    s->prev_length = prev_length;
    uint32_t match_len2 = longest_match(s, hash_head2);
    s->lazy2_probes++;

    /* Below the maximum level, marginal one-byte wins are not worth the
       probe, and rejecting them lets the hit-rate throttle retire the
       probing on streams full of them. */
    int min_gain = level >= 9 ? LAZY2_MIN_GAIN : LAZY2_MIN_GAIN + 4;
    if (match_len2 > prev_length && !(match_len2 <= 5 && s->strategy == Z_FILTERED) &&
        lazy_gain(match_len2, prev_length, next_pos - s->match_start, cur_dist) > min_gain)
        return match_len2;

    s->match_start = saved_match_start;
    return 0;
}

#endif
