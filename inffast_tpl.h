/* inffast.c -- fast decoding
 * Copyright (C) 1995-2017 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "zbuild.h"
#include "zutil.h"
#include "inftrees.h"
#include "inflate.h"
#include "inflate_p.h"
#include "functable.h"

/* Result code from the outlined window-copy helper. */
typedef enum {
    INFFAST_WIN_OK = 0,           /* normal completion, fall through */
    INFFAST_WIN_BAD_DISTANCE = 1, /* SET_BAD then break the outer loop */
    INFFAST_WIN_DONEXT = 2,       /* skip remaining iter body, hit back-edge */
} inffast_window_result_t;

/* Outline of the cold copy-from-window path. The locals (w_have, w_next, from)
   are otherwise live across the entire inflate_fast body and impose
   register-allocation constraints on the hot in-output chunkcopy path. The
   only call site is gated by UNLIKELY(dist > op) so the call cost is rare. */
static Z_INTERNAL __attribute__((noinline)) uint8_t *
inflate_fast_window_copy_outlined(
    struct inflate_state *state,
    uint8_t *out, unsigned dist, unsigned len,
    unsigned op_in,
    uint8_t *safe, int safe_mode,
    inffast_window_result_t *result)
{
    unsigned w_have = state->whave;
    unsigned w_next = state->wnext;
    unsigned char *w_base = state->window;
    unsigned char *from;
    unsigned op = op_in;             /* distance back in window */

    if (UNLIKELY(op > w_have)) {
#ifdef INFLATE_ALLOW_INVALID_DISTANCE_TOOFAR_ARRR
        if (LIKELY(state->sane)) {
            *result = INFFAST_WIN_BAD_DISTANCE;
            return out;
        }
        unsigned gap = op - w_have;
        unsigned zeros = MIN(len, gap);
        memset(out, 0, zeros);
        out += zeros;
        len -= zeros;
        if (UNLIKELY(len == 0)) {
            *result = INFFAST_WIN_DONEXT;
            return out;
        }
        op = w_have;
        if (UNLIKELY(op == 0)) {
            out = chunkcopy_safe(out, out - dist, len, safe);
            *result = INFFAST_WIN_DONEXT;
            return out;
        }
#else
        *result = INFFAST_WIN_BAD_DISTANCE;
        return out;
#endif
    }
    from = w_base;
    if (LIKELY(w_next == 0)) {                    /* very common case */
        from += state->wsize - op;
    } else if (LIKELY(w_next >= op)) {            /* contiguous in window */
        from += w_next - op;
    } else {                                      /* wrap around window */
        op -= w_next;
        from += state->wsize - op;
        if (UNLIKELY(op < len)) {                 /* some from end of window */
            len -= op;
            out = CHUNKCOPY_SAFE(out, from, op, safe);
            from = w_base;                        /* more from start of window */
            op = w_next;
        }
    }
    if (UNLIKELY(op < len)) {                     /* still need some from output */
        len -= op;
        if (LIKELY(!safe_mode)) {
            out = CHUNKCOPY_SAFE(out, from, op, safe);
            out = CHUNKUNROLL(out, &dist, &len);
            out = CHUNKCOPY_SAFE(out, out - dist, len, safe);
        } else {
#ifdef HAVE_MASKED_READWRITE
            out = CHUNKCOPY_SAFE(out, from, op, safe);
            out = CHUNKCOPY_SAFE(out, out - dist, len, safe);
#else
            out = chunkcopy_safe(out, from, op, safe);
            out = chunkcopy_safe(out, out - dist, len, safe);
#endif
        }
    } else {
#ifdef HAVE_MASKED_READWRITE
        out = CHUNKCOPY_SAFE(out, from, len, safe);
#else
        if (LIKELY(!safe_mode))
            out = CHUNKCOPY_SAFE(out, from, len, safe);
        else
            out = chunkcopy_safe(out, from, len, safe);
#endif
    }
    *result = INFFAST_WIN_OK;
    return out;
}

/*
   Decode literal, length, and distance codes and write out the resulting
   literal and match bytes until either not enough input or output is
   available, an end-of-block is encountered, or a data error is encountered.
   When large enough input and output buffers are supplied to inflate(), for
   example, a 16K input buffer and a 64K output buffer, more than 95% of the
   inflate execution time is spent in this routine.

   Entry assumptions:

        state->mode == LEN
        strm->avail_in >= INFLATE_FAST_MIN_HAVE
        strm->avail_out >= INFLATE_FAST_MIN_LEFT
        start >= strm->avail_out
        state->bits < 8

   On return, state->mode is one of:

        LEN -- ran out of enough output space or enough available input
        TYPE -- reached end of block code, inflate() to interpret next block
        BAD -- error in block data

   Notes:

    - The maximum input bits used by a length/distance pair is 15 bits for the
      length code, 5 bits for the length extra, 15 bits for the distance code,
      and 13 bits for the distance extra.  This totals 48 bits, or six bytes.
      Therefore if strm->avail_in >= 6, then there is enough input to avoid
      checking for available input while decoding.

    - On some architectures, it can be significantly faster (e.g. up to 1.2x
      faster on x86_64) to load from strm->next_in 64 bits, or 8 bytes, at a
      time, so INFLATE_FAST_MIN_HAVE == 8.

    - The maximum bytes that a single length/distance pair can output is 258
      bytes, which is the maximum length that can be coded.  inflate_fast()
      requires strm->avail_out >= 258 for each loop to avoid checking for
      output space.
 */
void Z_INTERNAL INFLATE_FAST(PREFIX3(stream) *strm, uint32_t start, int safe_mode) {
    /* start: inflate()'s starting value for strm->avail_out */
    struct inflate_state *state;
    z_const unsigned char *in;  /* local strm->next_in */
    const unsigned char *last;  /* have enough input while in < last */
    unsigned char *out;         /* local strm->next_out */
    unsigned char *beg;         /* inflate()'s initial strm->next_out */
    unsigned char *end;         /* while out < end, enough space available */
    unsigned char *safe;        /* can use chunkcopy provided out < safe */

    /* hold is a local copy of strm->hold. By default, hold satisfies the same
       invariants that strm->hold does, namely that (hold >> bits) == 0. This
       invariant is kept by loading bits into hold one byte at a time, like:

       hold |= next_byte_of_input << bits; in++; bits += 8;

       If we need to ensure that bits >= 15 then this code snippet is simply
       repeated. Over one iteration of the outermost do/while loop, this
       happens up to six times (48 bits of input), as described in the NOTES
       above.

       However, on some little endian architectures, it can be significantly
       faster to load 64 bits once instead of 8 bits six times:

       if (bits <= 16) {
         hold |= next_8_bytes_of_input << bits; in += 6; bits += 48;
       }

       Unlike the simpler one byte load, shifting the next_8_bytes_of_input
       by bits will overflow and lose those high bits, up to 2 bytes' worth.
       The conservative estimate is therefore that we have read only 6 bytes
       (48 bits). Again, as per the NOTES above, 48 bits is sufficient for the
       rest of the iteration, and we will not need to load another 8 bytes.

       Inside this function, we no longer satisfy (hold >> bits) == 0, but
       this is not problematic, even if that overflow does not land on an 8 bit
       byte boundary. Those excess bits will eventually shift down lower as the
       Huffman decoder consumes input, and when new input bits need to be loaded
       into the bits variable, the same input bits will be or'ed over those
       existing bits. A bitwise or is idempotent: (a | b | b) equals (a | b).
       Note that we therefore write that load operation as "hold |= etc" and not
       "hold += etc".

       Outside that loop, at the end of the function, hold is bitwise and'ed
       with (1<<bits)-1 to drop those excess bits so that, on function exit, we
       keep the invariant that (state->hold >> state->bits) == 0.
    */
    bits_t bits;                /* local strm->bits */
    uint64_t hold;              /* local strm->hold */
    unsigned lmask;             /* mask for first level of length codes */
    unsigned dmask;             /* mask for first level of distance codes */
    code const *lcode;          /* local strm->lencode */
    code const *dcode;          /* local strm->distcode */
    code here;                  /* retrieved table entry */
    unsigned op;                /* code bits, operation, extra bits, or */
                                /*  window position, window bytes to copy */
    unsigned len;               /* match length, unused bytes */
    unsigned dist;              /* match distance */
    uint64_t old;               /* look-behind buffer for extra bits */

    /* copy state to local variables */
    state = (struct inflate_state *)strm->state;
    in = strm->next_in;
    last = in + (strm->avail_in - (INFLATE_FAST_MIN_HAVE - 1));
    out = strm->next_out;
    beg = out - (start - strm->avail_out);
    safe = out + strm->avail_out;
    end = safe - (safe_mode ? INFLATE_FAST_MIN_SAFE : INFLATE_FAST_MIN_LEFT) + 1;
    hold = state->hold;
    bits = (bits_t)state->bits;
    lcode = state->lencode;
    dcode = state->distcode;
    lmask = (1U << state->lenbits) - 1;
    dmask = (1U << state->distbits) - 1;

    /* decode literals and length/distances until end-of-block or not enough
       input data or output space.

       here.bits doubles as a "preloaded entry pending" flag: zero means the
       next iteration must REFILL+lookup, non-zero means here already holds a
       speculatively-decoded entry from the previous iteration. */
    here.bits = 0;
#ifdef _MSC_VER
    here.op = 0;        /* silence C4701 potentially uninitialized */
    here.val = 0;
    old = 0;
#endif
    do {
        if (here.bits == 0) {
            REFILL();
            here = lcode[hold & lmask];
            Z_TOUCH(here);
            old = hold;
            DROPBITS(here.bits);
        }
        if (LIKELY(here.op == 0)) {
            TRACE_LITERAL(here.val);
            *out++ = (unsigned char)(here.val);
            here = lcode[hold & lmask];
            Z_TOUCH(here);
            old = hold;
            DROPBITS(here.bits);
            if (LIKELY(here.op == 0)) {
                TRACE_LITERAL(here.val);
                *out++ = (unsigned char)(here.val);
                here = lcode[hold & lmask];
                Z_TOUCH(here);
            dolen:
                old = hold;
                DROPBITS(here.bits);
                if (LIKELY(here.op == 0)) {
                    TRACE_LITERAL(here.val);
                    *out++ = (unsigned char)(here.val);
                    here.bits = 0;
                    continue;
                }
            }
        }
        op = here.op;
        if (LIKELY(op & 16)) {                  /* length base */
            len = here.val + EXTRA_BITS(old, here, op);
            TRACE_LENGTH(len);
            here = dcode[hold & dmask];
            Z_TOUCH(here);
            if (UNLIKELY(bits < MAX_BITS + MAX_DIST_EXTRA_BITS)) {
                REFILL();
            }
          dodist:
            old = hold;
            DROPBITS(here.bits);
            op = here.op;
            here.bits = 0;                      /* clear preloaded sentinel; speculative preload below
                                                   restores it from the next lcode entry's bit count */
            if (LIKELY(op & 16)) {              /* distance base */
                dist = here.val + EXTRA_BITS(old, here, op);
#ifdef INFLATE_STRICT
                if (UNLIKELY(dist > state->dmax)) {
                    SET_BAD("invalid distance too far back");
                    break;
                }
#endif
                TRACE_DISTANCE(dist);

                /* In safe mode, if there isn't enough output space for the full copy,
                   bail to the slow path's MATCH state which handles partial copies. */
                if (UNLIKELY(safe_mode && len > (unsigned)(safe - out))) {
                    state->mode = MATCH;
                    state->length = len;
                    state->offset = dist;
                    break;
                }

                /* Preload the next iteration's literal/length code so its lookup
                   latency overlaps with the chunk-copy below. REFILL is idempotent
                   when bits is already saturated, so this is safe regardless of
                   whether the early conditional refill above fired. */
                REFILL();
                here = lcode[hold & lmask];
                Z_TOUCH(here);
                old = hold;
                DROPBITS(here.bits);

                op = (unsigned)(out - beg);     /* max distance in output */
                if (UNLIKELY(dist > op)) {      /* see if copy from window */
                    inffast_window_result_t r;
                    out = inflate_fast_window_copy_outlined(
                        state, out, dist, len, dist - op, safe, safe_mode, &r);
                    if (UNLIKELY(r == INFFAST_WIN_BAD_DISTANCE)) {
                        SET_BAD("invalid distance too far back");
                        break;
                    }
                    if (UNLIKELY(r == INFFAST_WIN_DONEXT))
                        continue;
                } else if (LIKELY(!safe_mode)) {
                    /* Whole reference is in range of current output.  No range checks are
                       necessary because we start with room for at least 258 bytes of output,
                       so unroll and roundoff operations can write beyond `out+len` so long
                       as they stay within 258 bytes of `out`.
                    */
                    if (LIKELY(dist >= len || dist >= CHUNKSIZE()))
                        out = CHUNKCOPY(out, out - dist, len);
                    else
                        out = CHUNKMEMSET(out, out - dist, len);
                } else {
#ifdef HAVE_MASKED_READWRITE
                    out = CHUNKCOPY_SAFE(out, out - dist, len, safe);
#else
                    out = chunkcopy_safe(out, out - dist, len, safe);
#endif
                }
            } else if (UNLIKELY((op & 64) == 0)) {          /* 2nd level distance code */
                here = dcode[here.val + BITS(op)];
                Z_TOUCH(here);
                goto dodist;
            } else {
                SET_BAD("invalid distance code");
                break;
            }
        } else if (UNLIKELY((op & 64) == 0)) {              /* 2nd level length code */
            here = lcode[here.val + BITS(op)];
            Z_TOUCH(here);
            goto dolen;
        } else if (UNLIKELY(op & 32)) {                     /* end-of-block */
            TRACE_END_OF_BLOCK();
            state->mode = TYPE;
            here.bits = 0;
            break;
        } else {
            SET_BAD("invalid literal/length code");
            here.bits = 0;
            break;
        }
    } while (in < last && out < end);

    /* undo preload if we exited the loop with a preloaded symbol pending */
    if (here.bits) {
        hold = old;
        bits += here.bits;
    }

    /* return unused bytes (on entry, bits < 8, so in won't go too far back) */
    len = bits >> 3;
    in -= len;
    bits -= (bits_t)(len << 3);
    hold &= (UINT64_C(1) << bits) - 1;

    /* update state and return */
    strm->next_in = in;
    strm->next_out = out;
    strm->avail_in = (unsigned)(in < last ? (INFLATE_FAST_MIN_HAVE - 1) + (last - in)
                                          : (INFLATE_FAST_MIN_HAVE - 1) - (in - last));
    strm->avail_out = (unsigned)(safe - out);

    Assert(bits <= 32, "Remaining bits greater than 32");
    state->hold = (uint32_t)hold;
    state->bits = bits;
    return;
}

/*
   inflate_fast() speedups that turned out slower (on a PowerPC G3 750CXe):
   - Using bit fields for code structure
   - Different op definition to avoid & for extra bits (do & for table bits)
   - Three separate decoding do-loops for direct, window, and wnext == 0
   - Special case for distance > 1 copies to do overlapped load and store copy
   - Explicit branch predictions (based on measured branch probabilities)
   - Deferring match copy and interspersed it with decoding subsequent codes
   - Swapping literal/length else
   - Swapping window/direct else
   - Larger unrolled copy loops (three is about right)
   - Moving len -= 3 statement into middle of loop
 */

 // Cleanup
#undef CHUNKCOPY
#undef CHUNKMEMSET
#undef CHUNKMEMSET_SAFE
#undef CHUNKSIZE
#undef CHUNKUNROLL
#undef HAVE_CHUNKCOPY
#undef HAVE_CHUNKMEMSET_2
#undef HAVE_CHUNKMEMSET_4
#undef HAVE_CHUNKMEMSET_8
#undef HAVE_CHUNKMEMSET_16
#undef HAVE_CHUNK_MAG
#undef HAVE_HALFCHUNKCOPY
#undef HAVE_HALF_CHUNK
#undef HAVE_MASKED_READWRITE
#undef INFLATE_FAST
