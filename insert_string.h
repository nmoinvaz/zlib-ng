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
    uint32_t val, hm, h;

#ifdef UNALIGNED_OK
    val = *(uint32_t *)(s->window + str);
#else
    val  = ((uint32_t)s->window[str]);
    val |= ((uint32_t)s->window[str+1] << 8);
    val |= ((uint32_t)s->window[str+2] << 16);
    val |= ((uint32_t)s->window[str+3] << 24);
#endif
    h = 0;
    UPDATE_HASH(s, h, val);
    hm = h & s->hash_mask;
    s->ins_h = h;
    s->prev[str & s->w_mask] = ret = s->head[hm];
    s->head[hm] = str;
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
    Pos idx, ret;
    uint8_t *strstart, *strend;
    uint32_t h;

    if (UNLIKELY(count == 0)) {
        return s->prev[str & s->w_mask];
    }

    h = s->ins_h;
    strstart = s->window + str;
    strend = strstart + count - 1; /* last position */

    for (ret = 0, idx = str; strstart <= strend; idx++, strstart++) {
        uint32_t val, hm;

#ifdef UNALIGNED_OK
        val = *(uint32_t *)(strstart);
#else
        val  = ((uint32_t)(strstart));
        val |= ((uint32_t)(strstart+1) << 8);
        val |= ((uint32_t)(strstart+2) << 16);
        val |= ((uint32_t)(strstart+3) << 24);
#endif
        h = 0;
        val &= s->level_mask;
        UPDATE_HASH(s, h, val);
        hm = h & s->hash_mask;

        Pos head = s->head[hm];
        if (head != idx) {
            s->prev[idx & s->w_mask] = head;
            s->head[hm] = idx;
            if (strstart == strend)
                ret = head;
        } else if (strstart == strend) {
            ret = idx;
        }
    }
    s->ins_h = h;
    return ret;
}
#endif
