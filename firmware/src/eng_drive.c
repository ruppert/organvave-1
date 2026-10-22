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
 * samples. The input gain is limited to 10 (Q12 40960): with the input at 4.0 the FIR's output times the gain stays under 32. */
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
    /* 0.9.2: the user's part (dr_user, dr_run_ex): the type, the tone and the output level around either curve */
    int32_t type;                                   /* 0 DISTORTION (the setBfree preamp above), 1 TUBE */
    int32_t tone_c, out_g;                          /* the tone's tilt (Q12: -4096 = low-pass, 0 flat, + = treble up), the output gain (Q12) */
    int32_t tlp, tdc;                               /* the tone's low-pass state (Q15), the tube's DC estimate (Q15, x 64) */
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

/* the state emptied, the settings kept */
static void dr_clear(dr_t *d)
{
    uint32_t i;
    d->sagz = d->z = d->w = d->g = 0;
    for (i = 0; i < 8 + DR_MAX; i++)
        d->hx[i] = 0;
    for (i = 0; i < 32 + DR_MAX; i++)
        d->hy[i] = 0;
    d->tlp = d->tdc = 0;
}
static void dr_init(dr_t *d)
{
    dr_defaults(d);
    d->type = 0;
    d->tone_c = 0;
    d->out_g = 4096;
    dr_clear(d);
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
        u = ((s >> 14) * ig) >> 10;                 /* (s Q28: >> 14 is Q14, times ig Q12 is Q26: |value x ig| < 32) */
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

/* ---------------------------------------------------------------- 0.9.2: TUBE, TONE, OUTPUT ---
 * TUBE is a valve stage's soft saturation, far gentler than the preamp above: a cubic curve y = v - v^3 / 3 (v in units of
 * the knee, 8192 in Q13), slope 1 at the origin, flat at the knee (|v| >= 1 gives +-2/3: the curve and its slope are
 * continuous, no corner), with the asymmetry of a triode: the negative half meets the knee 1.25 times sooner, which adds the
 * even harmonics (the octave and the fourth: a warm, "musical" distortion); the DC that this asymmetry leaves is followed and
 * taken out. The input gain is the drive's GAIN halved (dr_user). No memory but the DC estimate and no latency.
 * TONE is a tilt around 1 kHz: out = x + c (x - lp), lp a one-pole low-pass at 1 kHz; c = -1 leaves only the low-pass (dark),
 * 0 is flat, + c lifts the treble (up to +1.5: +8 dB at the top). OUTPUT is a gain. Both follow either type. */
#define DR_TONE_A 4352                              /* the tone low-pass, Q15: 1 - exp(-2 pi 1000 / 44100) */
static const int16_t DR_TONE_C[9] = {-4096, -3072, -2048, -1024, 0, 1536, 3072, 4608, 6144};
static const uint16_t DR_OUT_G[9] = {1630, 2053, 2584, 3254, 4096, 5157, 6492, 8173, 10289};   /* -8 .. +8 dB, 2 dB a step */

/* the user's settings: type 0/1, the tone and output indexes 0..8 (4 the default); the state is kept */
static void dr_user(dr_t *d, uint32_t type, uint32_t tone, uint32_t out)
{
    d->type = (int32_t)(type ? 1 : 0);
    d->tone_c = DR_TONE_C[tone % 9u];
    d->out_g = DR_OUT_G[out % 9u];
}

/* the tube stage: in Q13 (1.0 = 8192) times the gain ig (Q12) / 2, out Q15 (the knee is 2/3 of 16384 out) */
static __attribute__((noinline)) void dr_tube(dr_t *d, const int32_t *in, int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t ig = d->ig >> 1, dc = d->tdc;
    for (i = 0; i < n; i++) {
        int32_t v = (in[i] * ig) >> 12, t2, t3, y;
        if (v < 0)
            v = v * 5 >> 2;                         /* (the negative half saturates sooner) */
        v = v > 8192 ? 8192 : v < -8192 ? -8192 : v;
        t2 = (v * v) >> 13;
        t3 = (t2 * v) >> 13;
        y = v - ((t3 * 21845) >> 16);               /* v - v^3 / 3 */
        if (y < 0)
            y = (y * 3277) >> 12;                   /* (back to the same scale: x 0.8) */
        y *= 2;                                     /* Q13 -> Q15 would be x4; x2: a TUBE at the default gain is as loud as DISTORTION (OUTPUT trims it) */
        dc += y - (dc >> 6);                        /* the DC (a one-pole at about 7 Hz), x 64 */
        out[i] = y - (dc >> 6);
    }
    d->tdc = dc;
}

/* the tone and the output level, in place */
static __attribute__((noinline)) void dr_post(dr_t *d, int32_t *x, uint32_t n)
{
    uint32_t i;
    int32_t lp = d->tlp, c = d->tone_c, g = d->out_g;
    for (i = 0; i < n; i++) {
        int32_t v = x[i], y;
        lp += ((v - lp) >> 3) * DR_TONE_A >> 12;    /* (v - lp) / 8 * 4352 / 4096: a ~ 0.13 */
        y = v + (((v - lp) >> 2) * c >> 10);        /* v + c (v - lp), c Q12 */
        x[i] = ((y >> 3) * g) >> 9;                 /* x g / 4096 (y >> 3 first: y may be 2^17) */
    }
    d->tlp = lp;
}

/* what the organ calls: either type, then the tone and the output. in Q13, out Q15 (in and out may be the same buffer) */
static void dr_run_ex(dr_t *d, const int32_t *in, int32_t *out, uint32_t n)
{
    if (d->type)
        dr_tube(d, in, out, n);
    else
        dr_run(d, in, out, n);
    if (d->tone_c != 0 || d->out_g != 4096)
        dr_post(d, out, n);
}
#endif
