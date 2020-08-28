
#include "zbuild.h"
#include "deflate.h"
#include "functable.h"

#ifndef BESTCMP_TYPE
#define BESTCMP_TYPE

#ifdef UNALIGNED_OK
#if MIN_MATCH >= 4
typedef uint32_t        bestcmp_t;
#elif MIN_MATCH >= 2
typedef uint16_t        bestcmp_t;
#else
typedef uint8_t         bestcmp_t;
#endif
#else
typedef uint8_t         bestcmp_t;
#endif

#endif

/*
 * Set match_start to the longest match starting at the given string and
 * return its length. Matches shorter or equal to prev_length are discarded,
 * in which case the result is equal to prev_length and match_start is garbage.
 *
 * IN assertions: cur_match is the head of the hash chain for the current
 * string (strstart) and its distance is <= MAX_DIST, and prev_length >=1
 * OUT assertion: the match length is not greater than s->lookahead
 */
ZLIB_INTERNAL int32_t LONGEST_MATCH(deflate_state *const s, Pos cur_match) {
    unsigned int strstart = s->strstart;
    const unsigned wmask = s->w_mask;
    ZLIB_REGISTER unsigned char *window = s->window;
    ZLIB_REGISTER unsigned char *scan = window + strstart;
    const Pos *prev = s->prev;
    unsigned chain_length;
    Pos limit;
    unsigned int best_len, nice_match;
    bestcmp_t scan_end, scan_start;

    /*
     * The code is optimized for HASH_BITS >= 8 and MAX_MATCH-2 multiple
     * of 16. It is easy to get rid of this optimization if necessary.
     */
    Assert(s->hash_bits >= 8 && MAX_MATCH == 258, "Code too clever");

    /*
     * Do not waste too much time if we already have a good match
     */
    best_len = s->prev_length;
    if (best_len == 0)
        best_len = 1;
    chain_length = s->max_chain_length;
    if (best_len >= s->good_match)
        chain_length >>= 2;
    unsigned char* match_base = s->window + cur_match;
    if (best_len >= MIN_MATCH)
        cur_match += offset;
    /*
     * Do not look for matches beyond the end of the input. This is
     * necessary to make deflate deterministic
     */
    nice_match = (unsigned int)s->nice_match > s->lookahead ? s->lookahead : (unsigned int)s->nice_match;

    /*
     * Stop when cur_match becomes <= limit. To simplify the code,
     * we prevent matches with the string of window index 0
     */
    
    limit = strstart > MAX_DIST(s) ? strstart - MAX_DIST(s) : 0;

    scan_start = *(bestcmp_t *)(scan);
    scan_end = *(bestcmp_t *)(scan+best_len-1);

    Assert((unsigned long)strstart <= s->window_size - MIN_LOOKAHEAD, "need lookahead");
    do {
        ZLIB_REGISTER unsigned char *match;
        ZLIB_REGISTER unsigned int len;
        int cont;
        if (cur_match >= strstart)
            break;

        /*
         * Skip to next match if the match length cannot increase
         * or if the match length is less than 2. Note that the checks
         * below for insufficient lookahead only occur occasionally
         * for performance reasons. Therefore uninitialized memory
         * will be accessed and conditional jumps will be made that
         * depend on those values. However the length of the match
         * is limited to the lookahead, so the output of deflate is not
         * affected by the uninitialized values.
         */
        cont = 1;
        do {
            match = window + cur_match;
            if (LIKELY(*(bestcmp_t *)(match+best_len-1) != scan_end ||
                       *(bestcmp_t *)(match) != scan_start)) {
                if ((cur_match = prev[cur_match & wmask]) > limit && --chain_length != 0) {
                    continue;
                }
                cont = 0;
            }
            break;
        } while (1);

        if (!cont)
            break;

#if MIN_MATCH >= 2 && defined(UNALIGNED_OK)
        len = COMPARE256(scan+2, match+2) + 2;
#else
        len = COMPARE258(scan, match);
#endif

        Assert(scan+len <= window+(unsigned)(s->window_size-1), "wild scan");

        if (len > best_len) {
            if (best_len >= MIN_MATCH)
                cur_match -= offset;
            s->match_start = cur_match;
            best_len = len;
            if (len >= nice_match)
                break;
            scan_end = *(bestcmp_t *)(scan+best_len-1);
             if (best_len >= MIN_MATCH) {
                cur_match += offset;
                /*Pos pos = functable.insert_string(s, cur_match, 3);
                if (pos > len && pos < cur_match) {
                    cur_match = pos;
                    if (cur_match > limit)
                        continue;
                    return best_len;
                }*/
                //continue;
             }
            
#if 0
            if (len > MIN_MATCH) {// && cur_match - offset + len < strstart) {
                Pos    pos, next_pos;
                register int i;
                register uInt hash;
                Bytef* scan_end;
#if 1
                /* go back to offset 0 */
                //cur_match -= offset;
                //offset = 0;
                next_pos = cur_match;
                for (i = 0; i <= len - MIN_MATCH; i++) {
                    pos = prev[(cur_match + i) & wmask];
                    if (pos < next_pos) {
                        /* this hash chain is more distant, use it */
                        if (pos <= limit + i) 
                            return best_len;
                        next_pos = pos;
                        //offset = i;
                    }
                }
                /* Switch cur_match to next_pos chain */
                cur_match = next_pos;
                //continue;
#else
                pos = functable.quick_insert_string(s, strstart+len-MIN_MATCH+1);
                if (pos > len && pos < cur_match) {
                    cur_match = pos-(len-MIN_MATCH);
                    if (cur_match > limit)
                        continue;
                    return best_len;
                }
#endif
            }
#endif
        } else {
            /*
             * The probability of finding a match later if we here
             * is pretty low, so for performance it's best to
             * outright stop here for the lower compression levels
             */
            if (s->level < TRIGGER_LEVEL)
                break;
        }
    } while ((cur_match = prev[cur_match & wmask]) > limit && --chain_length != 0);

    if (best_len <= s->lookahead)
        return best_len;
    return s->lookahead;
}

#undef LONGEST_MATCH
#undef COMPARE256
#undef COMPARE258
