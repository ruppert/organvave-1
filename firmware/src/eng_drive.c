/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The drive (0.9.24, rewritten from scratch): four types, TUBE SCREAM, TUBE PREAMP, TUBE DRIVE and DISTORTION (a JCM900 style
 * two-stage clip). Each one is a short chain, in integer:
 *   input high-pass (one pole, DR_HP) -> pre-gain (DR_PRE, the GAIN knob 1..10; 0 is bypass and the caller takes the drive
 *   out of the mix) -> waveshaper (DR_WS: 512 steps over -8 .. +8; DISTORTION runs a second one after a 2x gain) -> tone
 *   (dark: a one-pole low-pass per TONE step 0..3; bright: the treble emphasis (the unfiltered signal plus a share of the
 *   difference to its low-pass) for TONE 5..8; TONE 4 is flat) -> output trim (DR_TRIM: the level compensation of the GAIN
 *   knob, times the OUTPUT level).
 * Formats: input Q13 (1.0 = 8192), output Q15 (1.0 = 32768); inside, Q12 (1.0 = 4096). Coefficients Q15, the pre-gain Q8.
 * The tables come from tools/gen_drive_tables.py (the curves are inspired by James Stubbs' TS-808-Ultra, GPL-3.0, and by
 * D4p0up's ValveLord, the 12AU7 preamp circuit; see CREDITS.md). No 64-bit multiply; latency 0. */
#ifndef ENG_DRIVE_C
#define ENG_DRIVE_C
#include "eng_drive_tables.h"

#ifndef DR_MAX
#define DR_MAX 64                                   /* the longest block */
#endif

typedef struct {
    int32_t type;                                   /* 0 TUBE SCREAM, 1 TUBE PREAMP, 2 TUBE DRIVE, 3 DISTORTION */
    int32_t ig;                                     /* pre-gain, Q8 */
    int32_t trim;                                   /* output trim (gain compensation x output level), Q12 */
    int32_t hx, hy;                                 /* the input high-pass: the last input and output (Q12) */
    int32_t tl;                                     /* the tone's low-pass state (Q12) */
    int32_t tc, tk, tmode;                          /* the tone: low-pass coefficient (Q15), the emphasis (Q12), 1 bright */
    int32_t hc;                                     /* the high-pass coefficient (Q15) */
} dr_t;

static inline int32_t dr_ws(uint32_t stage, int32_t x)     /* x Q12 (+-8 at +-32768 in Q12 units x 8): the waveshaper, out Q12 */
{
    int32_t u = x + 32768;
    uint32_t i = (uint32_t)u >> 7;
    int32_t f = u & 127, a, b;
    if (i > 511u)
        i = 511u;
    a = DR_WS[stage][i];
    b = DR_WS[stage][i < 511u ? i + 1u : 511u];
    return (a + (((b - a) * f) >> 7)) >> 3;
}

static void dr_clear(dr_t *d)
{
    d->hx = d->hy = d->tl = 0;
}

static void dr_init(dr_t *d)
{
    d->type = 0;
    d->ig = DR_PRE[0][1];
    d->trim = 4096;
    d->tc = 32767;
    d->tk = 0;
    d->tmode = 0;
    d->hc = DR_HP[0];
    dr_clear(d);
}

/* the user's settings: type 0..3, gain 0..100 (tenths: 10.0 is the top; 0 must not reach here: the caller bypasses), tone 0..8 (4 flat), output 0..8
 * (4 = 0 dB, 2 dB a step: DR_OUT_G, Q12) */
static void dr_user(dr_t *d, uint32_t type, uint32_t gain, uint32_t tone, uint32_t out)
{
    static const uint16_t OUT_G[9] = {1630, 2053, 2584, 3254, 4096, 5157, 6492, 8173, 10289};   /* -8 .. +8 dB, Q12 */
    uint32_t g = gain > 100u ? 100u : gain, t = tone > 8u ? 8u : tone, o = out > 8u ? 8u : out;
    d->type = (int32_t)(type & 3u);
    d->ig = DR_PRE[type & 3u][g];
    d->hc = DR_HP[type & 3u];
    d->trim = (int32_t)(((uint32_t)DR_TRIM[type & 3u][g] * OUT_G[o]) >> 12);
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

static __attribute__((noinline)) void dr_chain(dr_t *d, const int32_t *in, int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t hx = d->hx, hy = d->hy, tl = d->tl, hc = d->hc, ig = d->ig, trim = d->trim, tc = d->tc, tk = d->tk;
    uint32_t dist = (uint32_t)d->type == 3u, bright = (uint32_t)d->tmode;
    uint32_t stage1 = (uint32_t)d->type;        /* TS 0, PREAMP 1, DRIVE 2, DISTORTION 3: the table index is the type */
    for (i = 0; i < n; i++) {
        int32_t xq = in[i] >> 1, g, w, o;
        hy = ((hy + xq - hx) * hc) >> 15;           /* the high-pass: y = a (y + x - x_prev) */
        hx = xq;
        g = (hy * ig) >> 8;
        g = g > 32767 ? 32767 : g < -32768 ? -32768 : g;
        w = dr_ws(stage1, g);                       /* DR_WS: 0 TS, 1 PREAMP, 2 DRIVE, 3 DISTORTION 1 */
        if (dist) {
            int32_t g2 = w * 2;                     /* the second stage driven 2x (the float model) */
            g2 = g2 > 32767 ? 32767 : g2 < -32768 ? -32768 : g2;
            w = dr_ws(4u, g2);                      /* DR_WS 4: DISTORTION 2 */
        }
        tl += ((w - tl) * tc) >> 15;
        o = bright ? w + (((w - tl) * tk) >> 12) : tl;
        o = (o * trim) >> 9;                          /* Q12 x Q12 -> Q15 */
        out[i] = o > 32767 ? 32767 : o < -32768 ? -32768 : o;   /* (the trim can take the output past full scale) */
    }
    d->hx = hx;
    d->hy = hy;
    d->tl = tl;
}

static void dr_run_ex(dr_t *d, const int32_t *in, int32_t *out, uint32_t n)
{
    dr_chain(d, in, out, n);
}

#endif /* ENG_DRIVE_C */
