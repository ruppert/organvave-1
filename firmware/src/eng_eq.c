/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The organ's equaliser (0.5.0): LOW, a low shelf at 120 Hz; MID, a peak (Q 0.8) at one of twelve frequencies, 160 Hz to
 * 7.2 kHz; HIGH, a high shelf at 4 kHz; each -12 .. +12 dB in 1 dB steps. Three biquads (direct form I, Q14) a channel, run on
 * the stereo output after MASTER and before the limiter: it is linear, so where it sits does not change what it does, and
 * the limiter sees what the boost makes. The coefficients are the tables of tools/gen_eq_tables.py (a cookbook formula, no
 * floating point on the target).
 * The patch bytes are "zero is flat", like every byte since 0.3.0 (an older patch has zeros there): a gain byte s (0..24) is
 * s dB up to 12 and s - 25 dB above (13 is -12 dB, 24 is -1 dB); the frequency byte f (0..11) is the peak's index (f + 5) % 12
 * of the ascending list (0 is 900 Hz). eq_db / eq_byte and eq_fidx / eq_fbyte convert for the panel.
 * The states are the samples and outputs of the last two steps; a sample is clamped at +-300,000 (the Q14 multiply is safe
 * to 2^22 and the biquad's Q3 values are 8 times that). With no sound at the input for long the states are emptied (the rounding of a filter this low leaves a faint
 * tone running): see fx.c. */
#ifndef ENG_EQ_C
#define ENG_EQ_C
#include "eng_eq_tables.h"

typedef struct {
    int32_t c[3][5];                    /* the three bands' coefficients: b0 b1 b2 a1 a2 */
    int32_t s[2][3][4];                 /* a channel, a band: x1 x2 (raw) y1 y2 (Q3) */
    uint32_t on;                        /* the bands that are not flat (bit 0 LOW, 1 MID, 2 HIGH): the others are skipped */
} eq_t;

static inline int32_t eq_db(uint32_t s) { return s <= 12u ? (int32_t)s : (int32_t)s - 25; }
static inline uint32_t eq_byte(int32_t db) { return (uint32_t)(db >= 0 ? db : db + 25); }
static inline uint32_t eq_fidx(uint32_t f) { return (f + 5u) % 12u; }
static inline uint32_t eq_fbyte(uint32_t idx) { return (idx + 7u) % 12u; }

static void eq_clear(eq_t *e)
{
    uint32_t i;
    for (i = 0; i < sizeof e->s / sizeof(int32_t); i++)
        ((int32_t *)e->s)[i] = 0;
}
/* the coefficients for the three gain bytes and the frequency byte (the states are kept) */
static void eq_set(eq_t *e, uint32_t low, uint32_t mid, uint32_t freq, uint32_t high)
{
    uint32_t i, g;
    e->on = (low % 25u ? 1u : 0u) | (mid % 25u ? 2u : 0u) | (high % 25u ? 4u : 0u);
    g = (low + 12u) % 25u;
    for (i = 0; i < 5u; i++)
        e->c[0][i] = EQ_LS[g][i];
    g = (mid + 12u) % 25u;
    for (i = 0; i < 5u; i++)
        e->c[1][i] = EQ_PK[eq_fidx(freq % 12u)][g][i];
    g = (high + 12u) % 25u;
    for (i = 0; i < 5u; i++)
        e->c[2][i] = EQ_HS[g][i];
}
static void eq_init(eq_t *e)
{
    eq_clear(e);
    eq_set(e, 0, 0, 0, 0);
}

/* a biquad step: x raw, the states x1 x2 raw and y1 y2 with three fractional bits (the feedback of a filter this low needs them:
 * with the output rounded to whole numbers a 120 Hz shelf at +12 dB stays at a constant of a few hundred, a cycle of a few tens;
 * with them 8 times less). The Q3 values stay under 2.4 million, which the Q14 multiply takes with the largest coefficient. */
static inline int32_t eq_bq(int32_t *s, const int32_t *c, int32_t x)
{
    int32_t y = mul14(x << 3, c[0]) + mul14(s[0] << 3, c[1]) + mul14(s[1] << 3, c[2]) - mul14(s[2], c[3]) - mul14(s[3], c[4]);
    y = y > 2400000 ? 2400000 : y < -2400000 ? -2400000 : y;
    s[1] = s[0];
    s[0] = x;
    s[3] = s[2];
    s[2] = y;
    return (y + 4) >> 3;
}
/* a block (n samples, in place) through the bands that are not flat; a band a block (the states of a band are its own) */
static void eq_block(eq_t *e, int32_t *l, int32_t *r, uint32_t n) __attribute__((noinline));
static void eq_block(eq_t *e, int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i, k;
    for (i = 0; i < n; i++) {
        l[i] = l[i] > 300000 ? 300000 : l[i] < -300000 ? -300000 : l[i];
        r[i] = r[i] > 300000 ? 300000 : r[i] < -300000 ? -300000 : r[i];
    }
    for (k = 0; k < 3u; k++) {
        if (!((e->on >> k) & 1u))
            continue;
        for (i = 0; i < n; i++) {
            l[i] = eq_bq(e->s[0][k], e->c[k], l[i]);
            r[i] = eq_bq(e->s[1][k], e->c[k], r[i]);
        }
    }
}
#endif
