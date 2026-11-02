/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The drive (0.9.24, rewritten from scratch; two types since 0.9.30): TUBE DRIVE and DISTORTION (a JCM900 style two-stage
 * clip). Each one is a short chain, in integer:
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
 * tools/gen_drive_tables.py. No 64-bit multiply; latency 0. Patch byte OP_DRV_TYPE: 3 DISTORTION, any other TUBE DRIVE (0 and 1,
 * TUBE SCREAM and TUBE PREAMP before 0.9.30, play as TUBE DRIVE). */
#ifndef ENG_DRIVE_C
#define ENG_DRIVE_C
#include "eng_drive_tables.h"

typedef struct {
    int32_t type;                                   /* 0 TUBE DRIVE, 1 DISTORTION */
    int32_t ig;                                     /* pre-gain, Q10 */
    int32_t trim;                                   /* output trim (gain compensation x output level), Q8 */
    int32_t hx, hy;                                 /* the input high-pass: the last input and output (Q12) */
    int32_t tl;                                     /* the tone's low-pass state (Q15) */
    int32_t bx, by;                                 /* the DC blocker after the shaper: the last input and output (Q15) */
    int32_t tc, tk, tmode;                          /* the tone: low-pass coefficient (Q15), the emphasis (Q12), 1 bright */
    int32_t hc;                                     /* the high-pass coefficient (Q15) */
} dr_t;

#define DR_DCB 32721                                /* the DC blocker's pole, Q15: 1 - 2 pi 10 Hz / 44.1 kHz */

static inline uint32_t dr_type_of(uint32_t b)       /* the patch byte (OP_DRV_TYPE) -> 0 TUBE DRIVE, 1 DISTORTION */
{
    return (b & 3u) == 3u;
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
    dr_clear(d);
}

/* the user's settings: the type byte (OP_DRV_TYPE: 3 DISTORTION, else TUBE DRIVE), gain 0..100 (tenths: 10.0 is the top; 0 must
 * not reach here: the caller bypasses), tone 0..8 (4 flat), output 0..8 (4 = 0 dB, 2 dB a step: OUT_G, Q12) */
static void dr_user(dr_t *d, uint32_t type, uint32_t gain, uint32_t tone, uint32_t out)
{
    static const uint16_t OUT_G[9] = {1630, 2053, 2584, 3254, 4096, 5157, 6492, 8173, 10289};   /* -8 .. +8 dB, Q12 */
    uint32_t g = gain > 100u ? 100u : gain, t = tone > 8u ? 8u : tone, o = out > 8u ? 8u : out, ty = dr_type_of(type);
    d->type = (int32_t)ty;
    d->ig = DR_PRE[ty][g];
    d->hc = DR_HP[ty];
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

/* (the trim is large only where the pre-gain is small, so the shaper's output w stays small there: w x trim, the output in
 * Q23, stays far inside 32 bits at every step; the output clamp is 4.0, Q15) */
static __attribute__((noinline)) void dr_chain(dr_t *d, const int32_t *in, int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t hx = d->hx, hy = d->hy, tl = d->tl, bx = d->bx, by = d->by, hc = d->hc, ig = d->ig, trim = d->trim, tc = d->tc, tk = d->tk;
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
        by = w - bx + ((by * DR_DCB + 16384) >> 15);   /* the DC blocker (10 Hz): the asymmetric curves' even harmonics */
        bx = w;                                     /* bring a DC that grows with the drive, as a tube's coupling capacitor */
        w = by;
        tl += ((w - tl) * tc + 16384) >> 15;
        o = bright ? w + (((w - tl) * tk + 2048) >> 12) : tl;
        o = (o * trim + 128) >> 8;                  /* Q15 x Q8 -> Q15 */
        out[i] = o > 131071 ? 131071 : o < -131072 ? -131072 : o;   /* (4.0: the buffer is 32 bits) */
    }
    d->hx = hx;
    d->hy = hy;
    d->tl = tl;
    d->bx = bx;
    d->by = by;
}

static void dr_run_ex(dr_t *d, const int32_t *in, int32_t *out, uint32_t n)
{
    dr_chain(d, in, out, n);
}

#endif /* ENG_DRIVE_C */
