/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* WORK IN PROGRESS (0.3.0). The rotary speaker, LIGHT model (ORGAN.md, "Rotary speaker: two models"): a crossover, the
 * horn with a direct and a reflected read tap per mic, the drum with one read tap per mic, each tap a delay and a gain
 * that follow the rotor's angle (the Doppler and the amplitude modulation). Our own model, with no reference of its
 * own: its geometry and gains (eng_rotary_light_params.h, tools/rotary_light/) are fitted to CLASSIC's sidebands, so
 * that the two models share the speeds, the ramps and the balance and LIGHT is the cheaper approximation of CLASSIC.
 *
 * The rotors are CLASSIC's (rot_t: angles, speeds, ramps), so a model switch keeps the rotation going. Per block of
 * CTL samples, int64 once: the six taps' delays (a distance: sqrt(P + Q cos(2 pi angle))) and gains
 * (a + b cos(2 pi angle)) at the block's end angle, and their linear ramps across the block. The per-sample loop is
 * int32 only: the crossover and the tone biquad (Q14, direct form I), two lines of 512 samples, linear interpolation
 * with a 12 bit fraction. */
#include "eng_rotary_light_params.h"

#define LT_LINE 512u
#define LT_MASK (LT_LINE - 1u)

typedef struct {
    int32_t hl[LT_LINE], dl[LT_LINE];        /* the horn's line (the crossover's high band, toned) and the drum's (the low band) */
    uint32_t w;                              /* write index */
    int32_t xo[4];                           /* the crossover's low-pass: x1, x2, y1, y2 */
    int32_t tn[4];                           /* the horn's tone biquad: x1, x2, y1, y2 */
    int32_t d[6], g[6];                      /* the taps' delay (Q16 samples) and gain (Q14) now */
    int32_t sd[6], sg[6];                    /* their steps per sample */
    uint32_t tone;                           /* the horn character: 0 default, 1 sharp (set from the rotor's hset every block) */
    int32_t hlev;                            /* the horn's level, Q14: LT_HLEV times the rotor's horn level over its default */
} lt_t;

static uint32_t lt_isqrt64(uint64_t v)
{
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return (uint32_t)r;
}

/* the six taps now: 0..3 the horn (mic m, tap k: index 2 m + k), 4 and 5 the drum (mic m: 4 + m) */
static void lt_targets(const rot_t *r, uint32_t n, int32_t d[6], int32_t g[6])
{
    uint32_t th_h = r->ph_h + n * r->inc_h, th_d = r->ph_d + n * r->inc_d, i;
    for (i = 0; i < 6; i++) {
        uint32_t m = i < 4 ? (i >> 1) : (i - 4u), k = i & 1u, u, ug;
        int64_t c, d2, gg;
        if (i < 4) {
            u = th_h + (m ? LT_PSI : 0u) + LT_PH[k];
            ug = th_h + (m ? LT_PSI : 0u) + LT_GPH[k];
            c = sine_i(u + 0x40000000u);                                    /* cos, Q15 */
            d2 = (int64_t)LT_HP + (((int64_t)LT_HQ * c) >> 15);             /* the distance squared, Q10 */
            d[i] = (int32_t)lt_isqrt64((uint64_t)(d2 < 1024 ? 1024 : d2) << 22) + LT_S[k];   /* Q16 */
            c = sine_i(ug + 0x40000000u);
            gg = LT_GA[k] + (((int64_t)LT_GB[k] * c) >> 15);
        } else {
            u = th_d + (m ? LT_DPSI : 0u);
            c = sine_i(u + 0x40000000u);
            d2 = (int64_t)LT_DP + (((int64_t)LT_DQ * c) >> 15);
            d[i] = (int32_t)lt_isqrt64((uint64_t)(d2 < 1024 ? 1024 : d2) << 22);
            c = sine_i(u + LT_DGPH + 0x40000000u);
            gg = LT_DA + (((int64_t)LT_DB * c) >> 15);
        }
        g[i] = (int32_t)(gg < 0 ? 0 : gg);
    }
}

static void lt_init(lt_t *t, const rot_t *r)
{
    uint32_t i;
    for (i = 0; i < sizeof(*t) / sizeof(int32_t); i++)
        ((int32_t *)t)[i] = 0;
    lt_targets(r, 0, t->d, t->g);
}

/* a read at a delay of d (Q16 samples, >= 1) from the line written up to index w */
static inline int32_t lt_read(const int32_t *ln, uint32_t w, int32_t d)
{
    uint32_t n = w - (uint32_t)(d >> 16);
    int32_t f = (d >> 4) & 4095, a = ln[n & LT_MASK], b = ln[(n - 1u) & LT_MASK];
    return a + (((b - a) * f) >> 12);
}
/* a direct form I biquad, Q14, over the state s = {x1, x2, y1, y2} */
static inline int32_t lt_bq(int32_t *s, const int32_t *c, int32_t x)
{
    int32_t y = mul14(x, c[0]) + mul14(s[0], c[1]) + mul14(s[1], c[2]) - mul14(s[2], c[3]) - mul14(s[3], c[4]);
    s[1] = s[0]; s[0] = x; s[3] = s[2]; s[2] = y;
    return y;
}

/* one block (n <= CTL): the rotors' speeds are already set for it (rot_speeds); the rotors advance */
#ifdef ROT_RAM
__attribute__((noinline))
#endif
static void lt_samples(rot_t *r, lt_t *t, const int32_t *in, int32_t *outL, int32_t *outR, uint32_t n)
{
    int32_t td[6], tg[6];
    uint32_t i, k;
    t->tone = r->hset ? 1u : 0u;
    t->hlev = (int32_t)(LT_HLEV * r->hl / 11469);       /* the horn level (11469: the default 0.7) */
    lt_targets(r, n, td, tg);
    for (k = 0; k < 6; k++) {
        t->sd[k] = (td[k] - t->d[k]) / (int32_t)n;
        t->sg[k] = (tg[k] - t->g[k]) / (int32_t)n;
    }
    for (i = 0; i < n; i++) {
        int32_t x = in[i], lo, hi, yl, yr;
        uint32_t w = t->w;
        x = x > 32767 ? 32767 : x < -32768 ? -32768 : x;
        lo = lt_bq(t->xo, LT_XO, x);
        hi = lt_bq(t->tn, LT_TONE[t->tone], x - lo);
        t->hl[w & LT_MASK] = hi;
        t->dl[w & LT_MASK] = lo;
        for (k = 0; k < 6; k++) {
            t->d[k] += t->sd[k];
            t->g[k] += t->sg[k];
        }
        yl = mul14(lt_read(t->hl, w, t->d[0]), t->g[0]) + mul14(lt_read(t->hl, w, t->d[1]), t->g[1]);
        yr = mul14(lt_read(t->hl, w, t->d[2]), t->g[2]) + mul14(lt_read(t->hl, w, t->d[3]), t->g[3]);
        outL[i] = mul14(yl, t->hlev) + mul14(mul14(lt_read(t->dl, w, t->d[4]), t->g[4]), LT_DLEV);
        outR[i] = mul14(yr, t->hlev) + mul14(mul14(lt_read(t->dl, w, t->d[5]), t->g[5]), LT_DLEV);
        t->w = w + 1u;
        r->ph_h += r->inc_h;
        r->ph_d += r->inc_d;
    }
}

/* The two models behind one call. The rotors are shared (rot_t): a switch keeps the rotation going. A switch fades over
 * ROT_FADE blocks (about 20 ms): the model coming in is emptied first (rot_flush / lt_init) and both run until the
 * fade ends, from the same angles; the model going out is not run again. */
#define ROT_FADE 28u
typedef struct {
    uint8_t model;                           /* 0 CLASSIC, 1 LIGHT: the model that plays (or is fading in) */
    uint8_t fade;                            /* blocks of the fade left (0: none) */
} rotmix_t;

/* the models: 0 CLASSIC, 1 LIGHT, 2 CLASSIC 2 (A+B), 3 CLASSIC 3 (D): the classic engine with a variant (rot_t.var) and
 * its level (ROT_VG, Q24, matched to CLASSIC's in dB). Only a change to or from LIGHT crossfades; between the classic
 * models the variant changes at once (the classic states carry over). */
static const uint8_t ROT_VAR[4] = {0u, 0u, ROT_V_NEAR | ROT_V_DRUM4, ROT_V_TAP4};
static const int32_t ROT_VG[4] = {1 << 24, 1 << 24, 1 << 24, 16944154};   /* CLASSIC 3: +0.09 dB (its level is -0.09 dB under CLASSIC) */
static const int32_t LT_VG = 13922586;                                  /* LIGHT, Q24: -1.62 dB (it was +1.6 dB over CLASSIC) */
static void lt_gain(int32_t *oL, int32_t *oR, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        oL[i] = mul24(oL[i], LT_VG);
        oR[i] = mul24(oR[i], LT_VG);
    }
}
static void rotary_model(rotmix_t *m, rot_t *r, lt_t *t, uint32_t model)
{
    if (model == m->model)
        return;
    if (model == 1u)
        lt_init(t, r);
    else if (m->model == 1u)
        rot_flush(r);
    r->var = ROT_VAR[model];
    r->vg = ROT_VG[model];
    if (model == 1u || m->model == 1u)
        m->fade = ROT_FADE;
    m->model = (uint8_t)model;
}

static void rotary_run(rotmix_t *m, rot_t *r, lt_t *t, const int32_t *in, int32_t *oL, int32_t *oR, uint32_t n)
{
    rot_speeds(r);
    if (!m->fade) {
        if (m->model == 1u) {
            lt_samples(r, t, in, oL, oR, n);
            lt_gain(oL, oR, n);
        } else
            rot_samples(r, in, oL, oR, n);
        return;
    }
    {
        int32_t aL[ROT_CTL], aR[ROT_CTL], bL[ROT_CTL], bR[ROT_CTL];   /* a: CLASSIC, b: LIGHT */
        uint32_t ph_h = r->ph_h, ph_d = r->ph_d, i;
        int32_t w0, w1;                                  /* LIGHT's share, Q15, at the block's start and end */
        rot_samples(r, in, aL, aR, n);
        r->ph_h = ph_h;
        r->ph_d = ph_d;
        lt_samples(r, t, in, bL, bR, n);
        lt_gain(bL, bR, n);
        m->fade--;
        w1 = (int32_t)(m->model == 1u ? ROT_FADE - m->fade : m->fade) * 32768 / (int32_t)ROT_FADE;
        w0 = (int32_t)(m->model == 1u ? ROT_FADE - m->fade - 1u : m->fade + 1u) * 32768 / (int32_t)ROT_FADE;
        for (i = 0; i < n; i++) {
            int32_t w = (w0 + (w1 - w0) * (int32_t)i / (int32_t)n) >> 3;   /* Q12 */
            oL[i] = aL[i] + (((bL[i] - aL[i]) * w) >> 12);
            oR[i] = aR[i] + (((bR[i] - aR[i]) * w) >> 12);
        }
    }
}
