/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The drive: the organ's preamp, ported from setBfree's b_overdrive/overdrive.c (GPL-2.0-or-later; ovt_biased, the
 * default constants), in integer. What setBfree's loop does, sample by sample:
 *   xin = IG x; sag = 0.991 sag + |xin|; bias = B0 - 0.0094 sag
 *   u = (9-tap FIR of x, times IG) + 0.6214 g          g: the last output, the global feedback
 *   u = u - z; z = u + 0.5821 z (old z)                the pre-differentiator
 *   v = sign(u) (D(|u| + bias) - D(bias))              the biased curve, D(x) = x^2 / (1 + x^2)
 *   y = v - (1 - 0.999) z1; z1 = v + 0.999 z1          the post-differentiator, written as a one-pole low-pass w:
 *                                                      y = v - w; w = w + 0.001 (v - w)
 *   out = OG (33-tap FIR of y, odd taps and the centre)
 * There is no oversampling in the signal path: setBfree sums the four polyphase rows of its interpolation filter for
 * every input sample (a 9-tap FIR at the input rate) and the "decimation" FIR is applied at the same rate.
 * Formats: input Q13 (1.0 = 8192, setBfree's units: a loud chord is several times 1.0), output Q15 (1.0 = 32768); inside Q16 (u, v, y, the sag in Q16); the post-
 * differentiator's w in Q28 (its coefficient is tiny, so it needs the bits); a coefficient that multiplies a state of
 * more than 22 bits is a mantissa and a shift (dr_cf: c = m / 2^n, 12 bits for the sag, 8 for the post-differentiator: error under 0.05 % and 0.4 %). The
 * pre-differentiator's state is clamped at 3,000,000 (below 2^22; its gain is 1 / (1 - fb): at fb 0.999 it is 1000, which is far out of
 * the usable range: the clamp only acts on absurd settings). No 64-bit multiply anywhere.
 * The curve is the table DR_D (1/64 steps up to |x| = 16, linear interpolation, error under 7e-5). Latency: 16 + 4
 * samples. Needs the input at or below 1.0 times a gain such that |u| stays under 64 (IG up to 10 with a full-scale
 * input is 10: fine). */
#ifndef ENG_DRIVE_C
#define ENG_DRIVE_C
#include "eng_drive_tables.h"

#ifndef DR_MAX
#define DR_MAX 64                                   /* the longest block */
#endif

typedef struct {
    int32_t ig, og;                                 /* input and output gain, Q12 */
    int32_t fb24, g24;                              /* pre-differentiator feedback, global feedback (positive), Q24 */
    int32_t bias;                                   /* the bias without sag, Q16 */
    int32_t zgb_m, zgb_n, sag_m, sag_n, c2_m, c2_n; /* sag-to-bias, 1 - sag pole, 1 - post pole (dr_cf) */
    int32_t sagz, z, w, g;                          /* sag state Q16, pre-diff state Q16, post-diff w Q28, last output Q16 */
    int32_t hx[8 + DR_MAX];                         /* the input history (Q15): hx[8 + n - k] is x[n - k] */
    int32_t hy[32 + DR_MAX];                        /* the curve's output history (Q15) */
} dr_t;

/* c24 (Q24, 0 < c < 1) -> mantissa (mx / 2 .. mx - 1) and shift: c ~ m / 2^n */
static inline void dr_cf(int32_t c24, int32_t *m, int32_t *n, int32_t mx)
{
    int32_t k = 24;
    if (c24 < 1)
        c24 = 1;
    while (c24 >= mx) {
        c24 >>= 1;
        k--;
    }
    while (c24 < mx / 2 && k < 40) {
        c24 <<= 1;
        k++;
    }
    *m = c24;
    *n = k;
}
/* a * c for a Q24 coefficient c (< 1) and |a| < 2^22: c in three parts, each rounded (the same as the rotary's mul24w) */
static inline int32_t dr_m24w(int32_t a, int32_t c)
{
    int32_t t2 = a * (c >> 16), t1 = a * ((c >> 8) & 255), t0 = a * (c & 255);
    return (t2 + ((t1 + ((t0 + 128) >> 8) + 128) >> 8) + 128) >> 8;
}
/* z * c for c = m / 2^n (n >= 8), z * m below 2^38 before the 8-bit shift: a state up to 2^26 with a 12-bit mantissa, up to
 * 2^29 with an 8-bit one */
static inline int32_t dr_mc(int32_t z, int32_t m, int32_t n)
{
    return ((z >> 8) * m) >> (n - 8);
}

/* Settings in setBfree's own terms (Q24 for the constants below 1): ig / og gains (Q12), fb the pre-differentiator's
 * pole (0.5821), fb2 the post-differentiator's pole (0.999), gfb the global feedback (0.6214, positive here), bias
 * (0.5347 in Q16), zgb (0.0094), sag pole (0.991). The state is kept. */
static void dr_set(dr_t *d, int32_t ig, int32_t og, int32_t fb24, int32_t fb2_24, int32_t gfb24, int32_t bias16,
                   int32_t zgb24, int32_t sagfb24)
{
    d->ig = ig;
    d->og = og;
    d->fb24 = fb24;
    d->g24 = gfb24;
    d->bias = bias16;
    dr_cf(zgb24, &d->zgb_m, &d->zgb_n, 4096);
    dr_cf((1 << 24) - sagfb24, &d->sag_m, &d->sag_n, 4096);
    dr_cf((1 << 24) - fb2_24, &d->c2_m, &d->c2_n, 256);
}

/* setBfree's defaults (overdrive.inputgain 3.5675, outputgain 0.8795, fb 0.5821, fb2 0.999, gfb 0.6214, bias 0.5347,
 * sagtobias 0.0094, sag pole 0.991) */
static void dr_defaults(dr_t *d)
{
    dr_set(d, 14612, 3603, 9766000, 16760438, 10425000, 35043, 157700, 16626000);
}

static void dr_init(dr_t *d)
{
    uint32_t i;
    dr_defaults(d);
    d->sagz = d->z = d->w = d->g = 0;
    for (i = 0; i < 8 + DR_MAX; i++)
        d->hx[i] = 0;
    for (i = 0; i < 32 + DR_MAX; i++)
        d->hy[i] = 0;
}

/* D(x) for x >= 0 in Q16 (x in Q16): the table, interpolated */
static inline int32_t dr_D(uint32_t x)
{
    uint32_t i = x >> 10;
    int32_t a, b;
    if (i >= 1024u)
        return DR_D[1024];
    a = DR_D[i];
    b = DR_D[i + 1u];
    return a + (((b - a) * (int32_t)(x & 1023u)) >> 10);
}

/* n <= DR_MAX samples, in Q13 and out Q15 (in and out may be the same buffer) */
static void dr_run(dr_t *d, const int32_t *in, int32_t *out, uint32_t n)
__attribute__((noinline));
static void dr_run(dr_t *d, const int32_t *in, int32_t *out, uint32_t n)
{
    int32_t i, nn = (int32_t)n;
    int32_t *hx = d->hx + 8, *hy = d->hy + 32;
    int32_t ig = d->ig, og = d->og;
    for (i = 0; i < nn; i++) {
        int32_t x = in[i], ax, u, v, y, bias, s, t;
        hx[i] = x;
        /* sag and bias, from the unfiltered input */
        ax = x < 0 ? -x : x;
        d->sagz += ((ax * ig) >> 9) - dr_mc(d->sagz, d->sag_m, d->sag_n);
        bias = d->bias - dr_mc(d->sagz, d->zgb_m, d->zgb_n);
        /* the input FIR (9 taps), then the gain */
        s = DR_IN[0] * hx[i] + DR_IN[1] * hx[i - 1] + DR_IN[2] * hx[i - 2] + DR_IN[3] * hx[i - 3] + DR_IN[4] * hx[i - 4]
            + DR_IN[5] * hx[i - 5] + DR_IN[6] * hx[i - 6] + DR_IN[7] * hx[i - 7];
        /* DR_IN is stored as {c0..c8} of x[n]..x[n-8]: its tap 8 is 0 */
        u = ((s >> 12) * ig) >> 12;
        /* the global feedback */
        u += dr_m24w(d->g, d->g24);
        /* the pre-differentiator */
        t = u - d->z;
        s = u + dr_m24w(d->z, d->fb24);
        d->z = s > 3000000 ? 3000000 : s < -3000000 ? -3000000 : s;
        u = t;
        /* the biased curve: odd, D(|u| + bias) - D(bias) */
        {
            int32_t xb = (u < 0 ? -u : u) + bias;
            v = dr_D((uint32_t)(xb < 0 ? -xb : xb)) - dr_D((uint32_t)(bias < 0 ? -bias : bias));
        }
        if (u < 0)
            v = -v;
        /* the post-differentiator */
        y = v - (d->w >> 12);
        d->w += dr_mc((v << 12) - d->w, d->c2_m, d->c2_n);
        d->g = y;
        hy[i] = y >> 1;
    }
    for (i = 0; i < nn; i++) {
        /* the output FIR: centre tap 16, pairs (15, 17) .. (1, 31) */
        int32_t s = DR_OUT[0] * hy[i - 16] + DR_OUT[1] * (hy[i - 15] + hy[i - 17])
                    + DR_OUT[2] * (hy[i - 13] + hy[i - 19])
                    + DR_OUT[3] * (hy[i - 11] + hy[i - 21])
                    + DR_OUT[4] * (hy[i - 9] + hy[i - 23])
                    + DR_OUT[5] * (hy[i - 7] + hy[i - 25])
                    + DR_OUT[6] * (hy[i - 5] + hy[i - 27])
                    + DR_OUT[7] * (hy[i - 3] + hy[i - 29])
                    + DR_OUT[8] * (hy[i - 1] + hy[i - 31]);
        out[i] = ((s >> 15) * og) >> 12;
    }
    for (i = 0; i < 8; i++)
        d->hx[i] = d->hx[nn + i];
    for (i = 0; i < 32; i++)
        d->hy[i] = d->hy[nn + i];
}
#endif
