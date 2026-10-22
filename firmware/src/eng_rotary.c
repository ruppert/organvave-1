/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* WORK IN PROGRESS (0.3.0, not in the build: felucca.c does not include it yet). The rotary speaker, CLASSIC model
 * (ORGAN.md, "Rotary speaker"): setBfree's whirl.c (whirlProc2, GPL-2.0-or-later) in integer arithmetic, sample for
 * sample: the horn's filters A and B, the leakage, six horn taps (two per reflection stage) read through the angle FIR,
 * the two reflections, six drum taps on the unfiltered input, the drum's high-shelf per mic; the rotors' angles advance
 * per sample and their speeds ramp per block (setBfree's exponential law, hornAcDc).
 * The angle tables are generated from setBfree's own code (eng_rotary_tables.h): 1024 angles, interpolated.
 *
 * Measured against tools/setbfree_ref/whirl_render (2 s, 440 Hz at 0.5, horn 400 rpm, drum stopped; the
 * reference's speed ramp is followed exactly):
 *   sidebands at 440 -/+ 6.67 Hz and -13.3 Hz: -16.2 / -27.0 / -30.5 dB, the same as setBfree (0.05 dB);
 *   RMS within 0.1%; with the rotor stopped there are no sidebands (below -240 dB);
 *   waveform SNR, rotating: 61 dB (L), 52 dB (R); stopped: 72 dB (L), 60 dB (R).
 *   The R channel is limited by the angle table, not by the arithmetic: the FIR of the horn's taps is sampled
 *   every 16 of 16384 angles (1024 entries). Float model of the same taps: 62 dB (R) with linear interpolation,
 *   63 with cubic, 64 with 2048 entries (about 74 KB more). Not taken; the owner's call.
 *   The five filters are in coupled form (rot_cpl): setBfree's direct form I rounds at 2^-14, and its poles near
 *   radius 0.99 with a DC gain up to 700 amplify that (the drum alone: 32 dB). Coupled: 73 to 79 dB per filter.
 *
 * Formats. Samples are Q15 (1.0 = 32768: the organ's output). Products never exceed 32 bits: a Q14 coefficient
 * multiplies a sample in two parts (mul14), a Q24 coupled-form coefficient in two parts (mul24), each part rounded.
 * The state of a coupled-form filter peaks near 2^18.3 for a full-scale input; mul24 holds |state| below 2^19, so
 * the input must stay at or below 1.0 (Q15). Angles are 32-bit phases (2^32 = one turn); the table index is the
 * phase in 16384ths (phase >> 18). The line is 1024 samples (setBfree: 2048; the default geometry needs at most
 * about 335). The speed ramp rounds (a floor left 0.5 / k below the target: 111 phase units at 400 rpm).
 *
 * Only the rotor speeds and their ramps are computed per block (CTL samples), with int64 once per block. The per-sample
 * loop is int32 only. Not yet in: the brake (STOP) of setBfree's hnBrakePos / drBrakePos, the mic angle (default 0), the
 * horn comb filter (disabled in setBfree's own build), the LIGHT model, the table copy to SRAM. */
#include <stdint.h>
#include "eng_rotary_tables.h"

#define ROT_LINE 1024u                 /* the delay lines: 1024 samples */
#define ROT_MASK (ROT_LINE - 1u)
#define ROT_CTL 32u                    /* the speed ramp's block (setBfree's bufferLengthSamples) */
#define ROT_FS 44100u

/* the taps' phases and spacings (setBfree's hornPhase / hornSpacing: horn 0..5, drum 6..11 in ROT_PHASE, ROT_SPACE) */

typedef struct {
    uint32_t ph_h, ph_d;               /* the rotors' angles, 2^32 = one turn */
    uint32_t inc_h, inc_d;             /* their speeds, phase per sample */
    uint32_t tgt_h, tgt_d;             /* their targets */
    int32_t k_ha, k_hd, k_da, k_dd;    /* the ramp's step per block, 1 - exp(-CTL / (FS * T)), Q30: acc / dec */
    int32_t rem_h, rem_d;              /* the ramps' carried fractions (Q30) */
    int8_t acdc_h, acdc_d;             /* +1 accelerating, -1 decelerating, 0 holding (setBfree's hornAcDc) */
    uint32_t mic_fw, mic_bw;           /* the mic angles' offsets (setBfree's micAngle * 0.25, and 1 - that), phase */
    int32_t hl, leak;                  /* the horn level and the leakage, Q14 (0.7 and 0.15 * 0.7) */
    int32_t hA[2], hB[2];              /* the horn filters' states (coupled form: u, v) */
    int32_t dL[2], dR[2];              /* the drum output filters' states (coupled form: u, v) */
    int32_t rf[4];                     /* the reflections' previous inputs: horn 0, 1; drum 2, 3 (FILTER_C) */
    int32_t hh[3][4];                  /* the horn's history for the three tap pairs (adx0..2), newest first */
    int32_t HL[ROT_LINE], HR[ROT_LINE], DL[ROT_LINE], DR[ROT_LINE];
    uint32_t outpos;
} rot_t;

/* a * c for a Q24 coefficient c and a state or sample a (|a| < 2^19): split into c's high and low parts */
static inline int32_t mul24(int32_t a, int32_t c)
{
    return (((a * (c >> 12)) + 2048) >> 12) + (((a * (c & 4095)) + (1 << 23)) >> 24);
}
/* a * c for a Q14 coefficient (|a| < 2^17, |c| < 2^15): split into c's high and low parts */
static inline int32_t mul14(int32_t a, int32_t c)
{
    return (((a * (c >> 7)) + 64) >> 7) + (((a * (c & 127)) + 8192) >> 14);
}

/* one of the five filters in coupled form (the table's ROT_CPL[k] = s, w, c0, c1, d; A = [[s, w], [-w, s]], B = (1, 0),
 * C = (c0, c1), D = d), z = {u, v} the state. The same filter as setBfree's biquad. Its rounding is far lower: the biquads
 * (direct form I) round at 2^-14 and their poles near radius 0.99 with a DC gain up to about 700 amplify that
 * (32 dB of error for the drum, 45 for horn B). Coupled, the error is 73 to 79 dB below the signal. The state peaks near
 * 2^18.3 for a full-scale input (Q15), which the mul24 split holds (it needs |state| < 2^19). */
static inline int32_t rot_cpl(int32_t *z, const int32_t *c, int32_t x)
{
    int32_t y = mul24(z[0], c[2]) + mul24(z[1], c[3]) + mul24(x, c[4]);
    int32_t u = mul24(z[0], c[0]) + mul24(z[1], c[1]) + x;
    int32_t v = mul24(z[1], c[0]) - mul24(z[0], c[1]);
    z[0] = u;
    z[1] = v;
    return y;
}

/* the two-sample reflection (setBfree's FILTER_C with 0.4, 0.4): y = 0.4 (x + previous x) */
static inline int32_t rot_refl(int32_t *prev, int32_t x)
{
    int32_t y = mul14(x, 6554) + mul14(*prev, 6554);
    *prev = x;
    return y;
}

/* the angle tables' values. h is the position in 16384ths of a turn, one table step is 16 of them. The displacement is
 * interpolated at the exact angle, as setBfree's is (its h1 is a float with the angle's fraction): f8 is the position
 * between two table entries in 256ths, h's low 4 bits and the 4 bits of the angle below them. The FIR is read at the
 * nearest 16384th (setBfree's x_iroundf) and interpolated between the table entries at that integer position. */
static inline int32_t tab_disp(const int32_t *d, uint32_t h, uint32_t f8)
{
    uint32_t k = h >> 4;
    int32_t a = d[k & (ROT_NA - 1u)], b = d[(k + 1u) & (ROT_NA - 1u)];
    return a + (((b - a) * (int32_t)f8) >> 8);
}
static inline void tab_fir(const int16_t f[ROT_NA][5], uint32_t h, int32_t c[5])
{
    uint32_t k = h >> 4, fr = h & 15u, j;
    uint32_t k1 = (k + 1u) & (ROT_NA - 1u);
    k &= ROT_NA - 1u;
    for (j = 0; j < 5; j++) {
        int32_t a = f[k][j], b = f[k1][j];
        c[j] = a + (((b - a) * (int32_t)fr) >> 4);
    }
}

/* a horn tap (setBfree's HN_MOTION): its read through the FIR of the four last inputs (hist, newest first), the
 * delay at the displacement, the write split linearly between the two samples around it (de-interpolating) */
static inline void horn_tap(rot_t *r, uint32_t ang, uint32_t P, const int32_t *disp, const int16_t fir[ROT_NA][5],
                            int32_t x, const int32_t *hist, int32_t *buf)
{
    uint32_t h = ((ang >> 18) + (uint32_t)ROT_PHASE[P]) & 16383u;
    int32_t c[5], xa, t, q, f;
    uint32_t n;
    tab_fir(fir, (h + ((ang >> 17) & 1u)) & 16383u, c);
    xa = mul14(x, c[0]) + mul14(hist[0], c[1]) + mul14(hist[1], c[2]) + mul14(hist[2], c[3]) + mul14(hist[3], c[4]);
    t = ROT_SPACE[P] + tab_disp(disp, h, ((h & 15u) << 4) | ((ang >> 14) & 15u)) + (int32_t)(r->outpos << 8);
    n = ((uint32_t)(t >> 8)) & ROT_MASK;
    f = t & 255;
    q = (xa * f + 128) >> 8;
    buf[n] += xa - q;
    buf[(n + 1u) & ROT_MASK] += q;
}

/* a drum tap (setBfree's DR_MOTION): no FIR, the same split write */
static inline void drum_tap(rot_t *r, uint32_t ang, uint32_t P, const int32_t *disp, int32_t x, int32_t *buf)
{
    uint32_t h = ((ang >> 18) + (uint32_t)ROT_PHASE[6u + P]) & 16383u;
    int32_t t, q, f;
    uint32_t n;
    t = ROT_SPACE[6u + P] + tab_disp(disp, h, ((h & 15u) << 4) | ((ang >> 14) & 15u)) + (int32_t)(r->outpos << 8);
    n = ((uint32_t)(t >> 8)) & ROT_MASK;
    f = t & 255;
    q = (x * f + 128) >> 8;
    buf[n] += x - q;
    buf[(n + 1u) & ROT_MASK] += q;
}

/* the speed's ramp for one rotor, once per block: setBfree's `inc += (1 - l) (target - inc)`, and the dead zone that
 * ends it (0.05 rpm, 81 phase units). The step's fraction is carried in *rem (Q30): a rounded or floored step stalls
 * where d * k < 0.5 (111 units below the horn's target, outside the dead zone: the angle drifted 10 table units in
 * 2 s), a carried one never stalls and has no bias. */
static void rot_ramp(uint32_t *inc, uint32_t tgt, int8_t *acdc, int32_t k, int32_t *rem)
{
    int32_t d, dd, step;
    int64_t acc;
    if (!*acdc)
        return;
    d = (int32_t)(tgt - *inc);
    acc = (int64_t)d * k + *rem;
    step = (int32_t)(acc >> 30);
    *rem = (int32_t)(acc - ((int64_t)step << 30));
    *inc += (uint32_t)step;
    dd = (int32_t)(tgt - *inc);
    if (dd < 81 && dd > -81) {
        *inc = tgt;
        *acdc = 0;
    }
}

/* 1 - exp(-CTL / (FS * T)) in Q30, T in microseconds: x - x^2/2 + x^3/6 (x < 0.1 for every time we use, error < x^4/24) */
static int32_t rot_k(uint32_t t_us)
{
    /* x = CTL / (FS * T), T = t_us * 1e-6 s, in Q30: CTL * 2^30 * 1e6 / (FS * t_us) (about 3e16: fits int64) */
    int64_t x30 = ((int64_t)ROT_CTL * ((int64_t)1 << 30) * 1000000LL) / ((int64_t)ROT_FS * (int64_t)t_us);
    int64_t x2 = (x30 * x30) >> 30;
    int64_t x3 = (x2 * x30) >> 30;
    return (int32_t)(x30 - (x2 >> 1) + x3 / 6);
}

/* a phase increment for a frequency in milli-hertz (2^32 per turn, per sample at FS) */
static uint32_t rot_inc(uint32_t mhz)
{
    return (uint32_t)((((uint64_t)mhz) << 32) / ((uint64_t)ROT_FS * 1000u));
}

static void rot_init(rot_t *r)
{
    uint32_t i;
    for (i = 0; i < sizeof(*r) / sizeof(int32_t); i++)
        ((int32_t *)r)[i] = 0;
    r->hl = 11469;                      /* 0.7 */
    r->leak = 1720;                     /* 0.15 * 0.7 */
    r->k_ha = rot_k(161000u);           /* setBfree's hornAcc 0.161 s */
    r->k_hd = rot_k(321000u);           /* hornDec 0.321 s */
    r->k_da = rot_k(4127000u);          /* drumAcc 4.127 s */
    r->k_dd = rot_k(1371000u);          /* drumDec 1.371 s */
}

/* the targets: the rotors ramp to them (setBfree's revControl sets hornAcDc / drumAcDc the same way) */
static void rot_target(rot_t *r, uint32_t horn_tgt, uint32_t drum_tgt)
{
    r->tgt_h = horn_tgt;
    r->tgt_d = drum_tgt;
    r->rem_h = 0;
    r->rem_d = 0;
    r->acdc_h = (int8_t)((int32_t)(horn_tgt - r->inc_h) > 0 ? 1 : (horn_tgt == r->inc_h ? 0 : -1));
    r->acdc_d = (int8_t)((int32_t)(drum_tgt - r->inc_d) > 0 ? 1 : (drum_tgt == r->inc_d ? 0 : -1));
}

/* one block of CTL samples (n <= ROT_CTL): in (Q15 mono) to outL and outR (Q15) */
static void rot_block(rot_t *r, const int32_t *in, int32_t *outL, int32_t *outR, uint32_t n)
{
    uint32_t i;

    /* the speeds for this block (setBfree: once per call of whirlProc2, before its sample loop) */
    rot_ramp(&r->inc_h, r->tgt_h, &r->acdc_h, r->acdc_h > 0 ? r->k_ha : r->k_hd, &r->rem_h);
    rot_ramp(&r->inc_d, r->tgt_d, &r->acdc_d, r->acdc_d > 0 ? r->k_da : r->k_dd, &r->rem_d);

    for (i = 0; i < n; i++) {
        int32_t xx = in[i], x, leak, y, lo, ro;
        uint32_t op = r->outpos;
        uint32_t ang_hf = r->ph_h + r->mic_fw, ang_hb = r->ph_h + r->mic_bw;
        uint32_t ang_d = r->ph_d;

        /* 1) the horn's filters A and B */
        x = rot_cpl(r->hA, ROT_CPL[0], xx);
        x = rot_cpl(r->hB, ROT_CPL[1], x);
        leak = mul14(x, r->leak);

        /* 2) the horn: taps 0 and 1 (history 0), reflection 1, taps 2 and 3 (history 1), reflection 2, taps 4 and 5 */
        horn_tap(r, ang_hf, 0, ROT_DISP[0], ROT_FIR[1], x, r->hh[0], r->HL);
        horn_tap(r, ang_hb, 1, ROT_DISP[1], ROT_FIR[0], x, r->hh[0], r->HR);
        r->hh[0][3] = r->hh[0][2]; r->hh[0][2] = r->hh[0][1]; r->hh[0][1] = r->hh[0][0]; r->hh[0][0] = x;
        x = rot_refl(&r->rf[0], x);

        horn_tap(r, ang_hf, 2, ROT_DISP[1], ROT_FIR[0], x, r->hh[1], r->HL);
        horn_tap(r, ang_hb, 3, ROT_DISP[0], ROT_FIR[1], x, r->hh[1], r->HR);
        r->hh[1][3] = r->hh[1][2]; r->hh[1][2] = r->hh[1][1]; r->hh[1][1] = r->hh[1][0]; r->hh[1][0] = x;
        x = rot_refl(&r->rf[1], x);

        horn_tap(r, ang_hf, 4, ROT_DISP[0], ROT_FIR[1], x, r->hh[2], r->HL);
        horn_tap(r, ang_hb, 5, ROT_DISP[1], ROT_FIR[0], x, r->hh[2], r->HR);
        r->hh[2][3] = r->hh[2][2]; r->hh[2][2] = r->hh[2][1]; r->hh[2][1] = r->hh[2][0]; r->hh[2][0] = x;

        /* 3) the drum: the unfiltered input, the same two reflections, then its high-shelf per mic */
        drum_tap(r, ang_d, 0, ROT_DISP[2], xx, r->DL);
        drum_tap(r, ang_d, 1, ROT_DISP[3], xx, r->DR);
        x = rot_refl(&r->rf[2], xx);
        drum_tap(r, ang_d, 2, ROT_DISP[3], x, r->DL);
        drum_tap(r, ang_d, 3, ROT_DISP[2], x, r->DR);
        x = rot_refl(&r->rf[3], x);
        drum_tap(r, ang_d, 4, ROT_DISP[2], x, r->DL);
        drum_tap(r, ang_d, 5, ROT_DISP[3], x, r->DR);

        /* 4) the outputs: the drum's shelf and the horn, and the leakage on both */
        y = rot_cpl(r->dL, ROT_CPL[2], r->DL[op]);
        lo = y + mul14(r->HL[op], r->hl) + leak;
        y = rot_cpl(r->dR, ROT_CPL[2], r->DR[op]);
        ro = y + mul14(r->HR[op], r->hl) + leak;
        outL[i] = lo;
        outR[i] = ro;
        r->HL[op] = 0; r->HR[op] = 0; r->DL[op] = 0; r->DR[op] = 0;

        /* 5) the rotors advance */
        r->outpos = (op + 1u) & ROT_MASK;
        r->ph_h += r->inc_h;
        r->ph_d += r->inc_d;
    }
}
