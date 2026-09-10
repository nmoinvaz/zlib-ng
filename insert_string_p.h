/* insert_string_p.h -- static single and batch hash insert functions
 *
 * Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */
#ifndef INSERT_STRING_P_H_
#define INSERT_STRING_P_H_

#define KNUTH_SHIFT (32 - HASH_BITS)
#define UPDATE_HASH_KNUTH(h,val) h = (((val) * 2654435761U) >> KNUTH_SHIFT)

#if (HASH_SIZE) > 65536u
#  define ROLL_HASH_SIZE 65536
#else
#  define ROLL_HASH_SIZE HASH_SIZE
#endif
#if defined(__aarch64__) && defined(__ARM_NEON)
#  include <arm_neon.h>
#endif
#define ROLL_MASK ((HASH_SIZE / 2) - 1u))
#define UPDATE_HASH_ROLL(h,val) h = (((h << 5) ^ ((uint8_t)(val))) & ROLL_MASK

/* ===========================================================================
 * Update a hash value with the given input byte
 * IN  assertion: all calls to UPDATE_HASH are made with consecutive
 *    input characters, so that a running hash key can be computed from the
 *    previous key instead of complete recalculation each time.
 */
Z_FORCEINLINE static uint32_t update_hash_roll(uint32_t h, uint32_t val) {
    UPDATE_HASH_ROLL(h, val);
    return h;
}

/* ===========================================================================
 * Insert string str in the dictionary using a pre-read value and set match_head
 * to the previous head of the hash chain (the most recent string with same hash key).
 * Return the previous length of the hash chain.
 */
Z_FORCEINLINE static uint32_t insert_knuth_val(deflate_state *const s, uint32_t str, uint32_t val) {
    uint32_t h, head;

    UPDATE_HASH_KNUTH(h, val);

    head = s->head[h];
    if (LIKELY(head != str)) {
        s->prev[str & W_MASK(s)] = (Pos)head;
        s->head[h] = (Pos)str;
    }
    return head;
}

/* ===========================================================================
 * Insert string str using a pre-read value, returning the previous head of the
 * hash chain. The prev link is left untouched since deflate_quick only inspects
 * the chain head and never walks the chain.
 */
Z_FORCEINLINE static uint32_t insert_knuth_val_head(deflate_state *const s, uint32_t str, uint32_t val) {
    uint32_t h, head;

    UPDATE_HASH_KNUTH(h, val);

    head = s->head[h];
    s->head[h] = (Pos)str;
    return head;
}

/* ===========================================================================
 * Insert string str in the dictionary and set match_head to the previous head
 * of the hash chain (the most recent string with same hash key). Return
 * the previous length of the hash chain.
 */
Z_FORCEINLINE static uint32_t insert_knuth(deflate_state *const s, unsigned char *window, uint32_t str) {
    uint8_t *strstart = window + str;
    uint32_t val, h, head;

    val = Z_U32_FROM_LE(zng_memread_4(strstart));
    UPDATE_HASH_KNUTH(h, val);

    head = s->head[h];
    if (LIKELY(head != str)) {
        s->prev[str & W_MASK(s)] = (Pos)head;
        s->head[h] = (Pos)str;
    }
    return head;
}

/* ===========================================================================
 * Insert string str read from the window, returning the previous head of the
 * hash chain. Like insert_knuth but leaves the prev link untouched for
 * deflate_quick, which only inspects the chain head.
 */
Z_FORCEINLINE static uint32_t insert_knuth_head(deflate_state *const s, unsigned char *window, uint32_t str) {
    uint8_t *strstart = window + str;
    uint32_t val, h, head;

    val = Z_U32_FROM_LE(zng_memread_4(strstart));
    UPDATE_HASH_KNUTH(h, val);

    head = s->head[h];
    s->head[h] = (Pos)str;
    return head;
}

Z_FORCEINLINE static uint32_t insert_roll(deflate_state *const s, unsigned char *window, uint32_t str) {
    uint8_t *strstart = window + str + (STD_MIN_MATCH-1);
    uint32_t h, head;

    h = s->ins_h;
    UPDATE_HASH_ROLL(h, strstart[0]);
    s->ins_h = h;

    head = s->head[h];
    if (LIKELY(head != str)) {
        s->prev[str & W_MASK(s)] = (Pos)head;
        s->head[h] = (Pos)str;
    }
    return head;
}

/* ===========================================================================
 * Insert string str in the dictionary and set match_head to the previous head
 * of the hash chain (the most recent string with same hash key). Return
 * the previous length of the hash chain.
 * IN  assertion: all calls to insert_knuth_batch are made with consecutive
 *    input characters and the first STD_MIN_MATCH bytes of str are valid
 *    (except for the last STD_MIN_MATCH-1 bytes of the input file).
 */
Z_FORCEINLINE static void insert_knuth_batch_static(deflate_state *const s, unsigned char *window, uint32_t str, uint32_t count) {
    uint8_t *strstart = window + str;
    uint8_t *strend = strstart + count;

    /* Local pointers to avoid indirection */
    Pos *headp = s->head;
    Pos *prevp = s->prev;
    const unsigned int w_mask = W_MASK(s);

    for (uint32_t idx = str; strstart < strend; idx++, strstart++) {
        uint32_t val, h, head;

        val = Z_U32_FROM_LE(zng_memread_4(strstart));
        UPDATE_HASH_KNUTH(h, val);

        head = headp[h];
        if (LIKELY(head != idx)) {
            prevp[idx & w_mask] = (Pos)head;
            headp[h] = (Pos)idx;
        }
    }
}

/* ===========================================================================
 * Insert count strings read from the window, leaving the prev links untouched.
 * Used by fill_window during deflate_quick, which only inspects the chain head.
 */
Z_FORCEINLINE static void insert_knuth_batch_head_static(deflate_state *const s, unsigned char *window, uint32_t str, uint32_t count) {
    uint8_t *strstart = window + str;
    uint8_t *strend = strstart + count;
    Pos *headp = s->head;

    for (uint32_t idx = str; strstart < strend; idx++, strstart++) {
        uint32_t val, h;

        val = Z_U32_FROM_LE(zng_memread_4(strstart));
        UPDATE_HASH_KNUTH(h, val);

        headp[h] = (Pos)idx;
    }
}

Z_FORCEINLINE static void insert_roll_batch_static(deflate_state *const s, unsigned char *window, uint32_t str, uint32_t count) {
    /* Local pointers to avoid indirection */
    Pos *headp = s->head;
    Pos *prevp = s->prev;
    const unsigned int w_mask = W_MASK(s);
    uint32_t idx = str;

    /* The rolling hash forgets everything past its mask after three shifts,
     * so position idx hashes exactly the bytes idx..idx+2 and every hash in
     * the batch computes independently of the running state. Recomputing
     * breaks the serial dependency the rolling form imposes, and on NEON
     * sixteen hashes fall out of three shifted byte vectors. */
#if defined(__aarch64__) && defined(__ARM_NEON)
    for (; count >= 16; count -= 16, idx += 16) {
        const uint8_t *p = window + idx;
        uint8x16_t b0 = vld1q_u8(p);
        uint8x16_t b1 = vld1q_u8(p + 1);
        uint8x16_t b2 = vld1q_u8(p + 2);
        uint16x8_t mask = vdupq_n_u16((uint16_t)((HASH_SIZE / 2) - 1u));
        uint16x8_t lo = vandq_u16(veorq_u16(veorq_u16(
            vshlq_n_u16(vmovl_u8(vget_low_u8(b0)), 10),
            vshlq_n_u16(vmovl_u8(vget_low_u8(b1)), 5)),
            vmovl_u8(vget_low_u8(b2))), mask);
        uint16x8_t hi = vandq_u16(veorq_u16(veorq_u16(
            vshlq_n_u16(vmovl_u8(vget_high_u8(b0)), 10),
            vshlq_n_u16(vmovl_u8(vget_high_u8(b1)), 5)),
            vmovl_u8(vget_high_u8(b2))), mask);
        uint16_t hs[16];
        vst1q_u16(hs, lo);
        vst1q_u16(hs + 8, hi);
        for (int j = 0; j < 16; j++) {
            uint32_t h = hs[j], head = headp[h];
            uint32_t pos = idx + (uint32_t)j;
            if (LIKELY(head != pos)) {
                prevp[pos & w_mask] = (Pos)head;
                headp[h] = (Pos)pos;
            }
        }
    }
#endif
    for (; count > 0; count--, idx++) {
        uint32_t h = update_hash_roll(update_hash_roll(update_hash_roll(0,
                         window[idx]), window[idx + 1]), window[idx + 2]);
        uint32_t head = headp[h];
        if (LIKELY(head != idx)) {
            prevp[idx & w_mask] = (Pos)head;
            headp[h] = (Pos)idx;
        }
    }
    if (idx > str)
        s->ins_h = update_hash_roll(update_hash_roll(update_hash_roll(0,
                       window[idx - 1]), window[idx]), window[idx + 1]);
}

#endif
