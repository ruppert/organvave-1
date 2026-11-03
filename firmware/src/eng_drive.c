/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The drive (0.9.24, rewritten from scratch; two types since 0.9.30, six since 0.9.41): TUBE DRIVE and DISTORTION (a JCM900
 * style two-stage clip) run dr_chain below; SOFT TUBE, MID DRIVE, HARD CLIP and TIGHT TUBE (a 12AX7 stage's circuit model) run
 * dr_chain2 (its comment; tools/gen_drive_tables.py has the circuits). dr_chain, in integer:
 *   input high-pass (one pole, DR_HP) -> pre-gain (DR_PRE, the GAIN knob 0.1 .. 10.0; 0 is bypass and the caller takes the
 *   drive out of the mix) -> waveshaper (DR_WS: 512 steps over -8 .. +8; DISTORTION runs a second one after a 2x gain) -> tone
 *   (dark: a one-pole low-pass per TONE step 0..3; bright: the treble emphasis (the unfiltered signal plus a share of the
 *   difference to its low-pass) for TONE 5..8; TONE 4 is flat) -> output trim (DR_TRIM: the level compensation of the GAIN
 *   knob, times the OUTPUT level).
 * TUBE DRIVE's GAIN follows its harmonic distortion (0.9.30): from 0.2 % at 0.1 to 34 % at 10.0 (0.9.31) on an exponential law, so the
 * pre-gain is tiny at the bottom (0.04) and the trim large (39x): the shaper's output is kept in Q15 and the trim in Q8 for it.
 * Formats: input Q13 (1.0 = 8192), output Q15 up to 4.0 (0.9.30: +-131071 in the 32-bit buffer; the organ's chords come in
 * hot); inside, the shaper's input Q12 (1.0 = 4096), its output Q15. Every shift rounds to the nearest (0.9.30): with the trim
 * at 39x, a truncation's half-LSB bias was a DC of -9 % of the signal at GAIN 0.1. Coefficients Q15, the pre-gain Q10, the trim Q8. The tables come from
 * tools/gen_drive_tables.py. No 64-bit multiply; latency 0. Patch byte OP_DRV_TYPE: 2 TUBE DRIVE, 3 DISTORTION, 4 SOFT TUBE,
 * 5 MID DRIVE, 6 HARD CLIP, 7 TIGHT TUBE (0.9.41); 0 and 1 (TUBE SCREAM and TUBE PREAMP before 0.9.30) and any other byte play
 * as TUBE DRIVE. */
#ifndef ENG_DRIVE_C
#define ENG_DRIVE_C
#include "eng_drive_tables.h"

typedef struct {
    int32_t type;                                   /* 0 TUBE DRIVE, 1 DISTORTION, 2 SOFT TUBE, 3 MID DRIVE, 4 HARD CLIP, 5 TIGHT TUBE */
    int32_t ig;                                     /* pre-gain, Q10 */
    int32_t trim;                                   /* output trim (gain compensation x output level), Q8 */
    int32_t hx, hy;                                 /* the input high-pass: the last input and output (Q12) */
    int32_t tl;                                     /* the tone's low-pass state (Q15) */
    int32_t bx, by;                                 /* the DC blocker after the shaper: the last input and output (Q15) */
    int32_t tc, tk, tmode;                          /* the tone: low-pass coefficient (Q15), the emphasis (Q12), 1 bright */
    int32_t hc;                                     /* the high-pass coefficient (Q15) */
    /* dr_chain2's (0.9.41): the curve, the cathode shelf (its high-pass Q15, the lows' share Q15), the dry share into the clipper
     * (Q10), the grid's conduction point (Q12; 0 none), the DC blocker's pole (Q15), the fixed low-pass (Q15; 0 none); states: the
     * shelf's high-pass (output, last input), the bias shift (Q12, <= 0), the fixed low-pass */
    int32_t row, sh, kl, dry, xc, dcp, plp;
    int32_t h2, yp, bs, pl;
    int32_t be, te, pe;                             /* the remainders of the DC blocker, the tone and the fixed low-pass (0.9.41) */
} dr_t;

#define DR_DCB 32721                                /* the DC blocker's pole, Q15: 1 - 2 pi 10 Hz / 44.1 kHz */

static inline uint32_t dr_type_of(uint32_t b)       /* the patch byte (OP_DRV_TYPE) -> dr_t.type: 3 -> 1, 4..7 -> 2..5, else 0 */
{
    return b == 3u ? 1u : b >= 4u && b <= 7u ? b - 2u : 0u;
}

static inline int32_t dr_ws(uint32_t stage, int32_t x)     /* x Q12 (+-8 at +-32768): the waveshaper, out Q15 */
{
    int32_t u = x + 32768;
    uint32_t i = (uint32_t)u >> 7;
    int32_t f = u & 127, a, b;
    if (i > 511u)
        i = 511u;
    a = DR_WS[stage][i];
    b = DR_WS[stage][i < 511u ? i + 1u : 511u];
    return a + (((b - a) * f + 64) >> 7);
}

static void dr_clear(dr_t *d)
{
    d->hx = d->hy = d->tl = d->bx = d->by = 0;
    d->h2 = d->yp = d->bs = d->pl = 0;
    d->be = d->te = d->pe = 0;
}

static void dr_init(dr_t *d)
{
    d->type = 0;
    d->ig = DR_PRE[0][1];
    d->trim = 256;
    d->tc = 32767;
    d->tk = 0;
    d->tmode = 0;
    d->hc = DR_HP[0];
    d->row = 0;
    d->sh = d->dry = d->xc = d->plp = 0;
    d->kl = 32768;
    d->dcp = DR_DCB;
    dr_clear(d);
}

/* the user's settings: the type byte (OP_DRV_TYPE: dr_type_of), gain 0..100 (tenths: 10.0 is the top; 0 must
 * not reach here: the caller bypasses), tone 0..8 (4 flat), output 0..8 (4 = 0 dB, 2 dB a step: OUT_G, Q12) */
static void dr_user(dr_t *d, uint32_t type, uint32_t gain, uint32_t tone, uint32_t out)
{
    static const uint16_t OUT_G[9] = {1630, 2053, 2584, 3254, 4096, 5157, 6492, 8173, 10289};   /* -8 .. +8 dB, Q12 */
    uint32_t g = gain > 100u ? 100u : gain, t = tone > 8u ? 8u : tone, o = out > 8u ? 8u : out, ty = dr_type_of(type);
    d->type = (int32_t)ty;
    d->ig = DR_PRE[ty][g];
    d->hc = DR_HP[ty];
    d->row = DR_ROW[ty];
    d->sh = DR_SH[ty];
    d->kl = DR_KL[ty];
    d->dry = DR_DRY[ty];
    d->xc = DR_XC[ty];
    d->dcp = DR_DCP[ty];
    d->plp = DR_PLP[ty];
    d->trim = (int32_t)(((uint32_t)DR_TRIM[ty][g] * OUT_G[o]) >> 12);
    if (t < 4u) {                                   /* dark: the low-pass at 1.5 kHz x 2^(0.6 t) (step 0 .. 3: 1.5 k .. 5.2 k) */
        d->tc = (int32_t)DR_TONE_LP[t];
        d->tk = 0;
        d->tmode = 0;
    } else {                                        /* flat (4) or bright (5..8): the emphasis */
        d->tc = (int32_t)DR_LP_BRIGHT;
        d->tk = (int32_t)(t - 4u) * 2048;
        d->tmode = 1;
    }
}

/* the DC blocker's step (0.9.41): x - x_prev + by - leak, the leak by x (1 - pole) with its remainder carried to the next sample
 * (error feedback, *e in 0 .. 32767), so nothing is lost and by reaches 0 (a leak rounded to the nearest stopped at |by| < 349 for
 * the 10 Hz pole: a DC of about -40 dB stayed after the sound) */
static inline int32_t dr_dcb(int32_t w, int32_t bx, int32_t by, int32_t k, int32_t *e)
{
    int32_t m = by * k + *e, lk = m >> 15;
    *e = m - (lk << 15);
    return w - bx + by - lk;
}
/* a one-pole low-pass's step toward x (c Q15, >> sh), the remainder carried as above: it gets there (rounded, it stopped short by
 * up to a few LSB, which the trim, up to 107x, made a DC after the sound) */
static inline int32_t dr_lps(int32_t st, int32_t x, int32_t c, uint32_t sh, int32_t *e)
{
    int32_t m = (x - st) * c + *e, s = m >> sh;
    *e = m - (s << sh);
    return st + s;
}

/* (the trim is large only where the pre-gain is small, so the shaper's output w stays small there: w x trim, the output in
 * Q23, stays far inside 32 bits at every step; the output clamp is 4.0, Q15) */
static __attribute__((noinline)) void dr_chain(dr_t *d, const int32_t *in, int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t hx = d->hx, hy = d->hy, tl = d->tl, bx = d->bx, by = d->by, hc = d->hc, ig = d->ig, trim = d->trim, tc = d->tc, tk = d->tk;
    int32_t be = d->be, te = d->te;
    uint32_t dist = (uint32_t)d->type, bright = (uint32_t)d->tmode;
    for (i = 0; i < n; i++) {
        int32_t xq = in[i] >> 1, g, w, o;
        hy = ((hy + xq - hx) * hc + 16384) >> 15;   /* the high-pass: y = a (y + x - x_prev) */
        hx = xq;
        g = (hy * ig + 512) >> 10;
        g = g > 32767 ? 32767 : g < -32768 ? -32768 : g;
        w = dr_ws(dist, g);                         /* DR_WS: 0 TUBE DRIVE, 1 DISTORTION stage 1 */
        if (dist)
            w = dr_ws(2u, w >> 2);                  /* DISTORTION stage 2, driven 2x: Q15 x 2 -> Q12 */
        by = dr_dcb(w, bx, by, 32768 - DR_DCB, &be);     /* the DC blocker (10 Hz): the asymmetric curves' even harmonics */
        bx = w;                                     /* bring a DC that grows with the drive, as a tube's coupling capacitor */
        w = by;
        tl = dr_lps(tl, w, tc, 15u, &te);
        o = bright ? w + (((w - tl) * tk + 2048) >> 12) : tl;
        o = (o * trim + 128) >> 8;                  /* Q15 x Q8 -> Q15 */
        out[i] = o > 131071 ? 131071 : o < -131072 ? -131072 : o;   /* (4.0: the buffer is 32 bits) */
    }
    d->hx = hx;
    d->hy = hy;
    d->tl = tl;
    d->bx = bx;
    d->by = by;
    d->be = be;
    d->te = te;
}

/* x * c >> 15, rounded, for an x up to +-2^17 (the shelf's sums): split so that no product leaves 32 bits */
static inline int32_t dr_mq15(int32_t x, int32_t c)
{
    return (x >> 15) * c + (((x & 32767) * c + 16384) >> 15);
}

/* dr_chain2 (0.9.41): SOFT TUBE, MID DRIVE, HARD CLIP, TIGHT TUBE.
 *   input high-pass (DR_HP: the coupling capacitor) -> [the cathode shelf: the lows at DR_KL of the gain below DR_SH's corner]
 *   -> pre-gain (DR_PRE) [+ the dry input at DR_DRY: MID DRIVE's unity-gain lows] -> [the grid's bias shift: what goes past the
 *   conduction point DR_XC charges the coupling capacitor (DR_BIAS_KC) and pulls the bias down; it leaks back (DR_BIAS_KR)]
 *   -> the curve (DR_WS[DR_ROW]) -> DC blocker (DR_DCP: TIGHT TUBE's output coupling capacitor at 100 Hz) -> [fixed low-pass,
 *   DR_PLP] -> the tone -> the trim. Formats as dr_chain's. */
static __attribute__((noinline)) void dr_chain2(dr_t *d, const int32_t *in, int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t hx = d->hx, hy = d->hy, tl = d->tl, bx = d->bx, by = d->by, hc = d->hc, ig = d->ig, trim = d->trim, tc = d->tc, tk = d->tk;
    int32_t h2 = d->h2, yp = d->yp, bs = d->bs, pl = d->pl, sh = d->sh, kl = d->kl, dry = d->dry, xc = d->xc, dcp = 32768 - d->dcp, plp = d->plp >> 1;
    int32_t be = d->be, te = d->te, pe = d->pe;
    uint32_t row = (uint32_t)d->row, bright = (uint32_t)d->tmode;
    for (i = 0; i < n; i++) {
        int32_t xq = in[i] >> 1, dd, g, w, o;
        hy = ((hy + xq - hx) * hc + 16384) >> 15;   /* the coupling capacitor: y = a (y + x - x_prev) */
        hx = xq;
        dd = hy;
        if (sh) {                                   /* the cathode's partial bypass: the highs (above the corner) at full gain, */
            h2 = dr_mq15(h2 + hy - yp, sh);         /* the lows at kl */
            yp = hy;
            dd = h2 + (((hy - h2) * kl + 16384) >> 15);
            dd = dd > 32767 ? 32767 : dd < -32768 ? -32768 : dd;
        }
        g = ((dd * ig + 512) >> 10) + ((xq * dry + 512) >> 10);
        g = g > 32767 ? 32767 : g < -32768 ? -32768 : g;
        if (xc) {                                   /* the grid's bias shift */
            g += bs;
            if (g > xc)
                bs -= ((g - xc) * DR_BIAS_KC + 16384) >> 15;
            bs += (((-bs) * DR_BIAS_KR) >> 15) + (bs < 0);
            g = g > 32767 ? 32767 : g < -32768 ? -32768 : g;
        }
        w = dr_ws(row, g);
        by = dr_dcb(w, bx, by, dcp, &be);                /* the DC blocker / the output coupling capacitor */
        bx = w;
        w = by;
        if (plp) {                                  /* the type's own top end */
            pl = dr_lps(pl, w, plp, 14u, &pe);
            w = pl;
        }
        tl = dr_lps(tl, w, tc, 15u, &te);
        o = bright ? w + (((w - tl) * tk + 2048) >> 12) : tl;
        o = (o * trim + 128) >> 8;                  /* Q15 x Q8 -> Q15 */
        out[i] = o > 131071 ? 131071 : o < -131072 ? -131072 : o;
    }
    d->hx = hx;
    d->hy = hy;
    d->tl = tl;
    d->bx = bx;
    d->by = by;
    d->h2 = h2;
    d->yp = yp;
    d->bs = bs;
    d->pl = pl;
    d->be = be;
    d->te = te;
    d->pe = pe;
}

static void dr_run_ex(dr_t *d, const int32_t *in, int32_t *out, uint32_t n)
{
    if (d->type >= 2)
        dr_chain2(d, in, out, n);
    else
        dr_chain(d, in, out, n);
}

#endif /* ENG_DRIVE_C */
