#ifndef INSERT_STRING_H_
#define INSERT_STRING_H_

/* insert_string.h -- Private insert_string functions shared with more than
 *                    one insert string implementation
 *
 * Copyright (C) 1995-2013 Jean-loup Gailly and Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 */

ZLIB_INTERNAL Pos QUICK_INSERT_STRING(deflate_state *const s, const Pos str) {
    Pos ret;
    unsigned int h = 0;
    uint32_t val;

#ifdef UNALIGNED_OK
    val = *(uint32_t *)(s->window + str);
#else
    val  = ((uint32_t)s->window[str]);
    val |= ((uint32_t)s->window[str+1] << 8);
    val |= ((uint32_t)s->window[str+2] << 16);
    val |= ((uint32_t)s->window[str+3] << 24);
#endif
    UPDATE_HASH(s, h, val);
    ret = s->head[h & s->hash_mask];
    s->head[h & s->hash_mask] = str;
    return ret;
}

/* ===========================================================================
 * Insert string str in the dictionary and set match_head to the previous head
 * of the hash chain (the most recent string with same hash key). Return
 * the previous length of the hash chain.
 * IN  assertion: all calls to to INSERT_STRING are made with consecutive
 *    input characters and the first MIN_MATCH bytes of str are valid
 *    (except for the last MIN_MATCH-1 bytes of the input file).
 */

ZLIB_INTERNAL Pos INSERT_STRING(deflate_state *const s, const Pos str, unsigned int count) {
    Pos str_idx, str_end, ret;

    if (UNLIKELY(count == 0)) {
        return s->prev[str & s->w_mask];
    }

    ret = 0;
    str_end = str + count - 1; /* last position */

    for (str_idx = str; str_idx <= str_end; str_idx++) {
        uint32_t val, hm;

#ifdef UNALIGNED_OK
        val = *(uint32_t *)(&s->window[str_idx]);
#else
        val  = ((uint32_t)s->window[str_idx]);
        val |= ((uint32_t)s->window[str_idx+1] << 8);
        val |= ((uint32_t)s->window[str_idx+2] << 16);
        val |= ((uint32_t)s->window[str_idx+3] << 24);
#endif

        if (s->level >= TRIGGER_LEVEL)
            val &= ~0xFF;

        UPDATE_HASH(s, s->ins_h, val);
        hm = s->ins_h & s->hash_mask;

        Pos head = s->head[hm];
        if (head != str_idx) {
            s->prev[str_idx & s->w_mask] = head;
            s->head[hm] = str_idx;
            if (str_idx == str_end)
                ret = head;
        } else if (str_idx == str_end) {
            ret = str_idx;
        }
    }
    return ret;
}
#endif
