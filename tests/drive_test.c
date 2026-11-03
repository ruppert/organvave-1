/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The drive (src/eng_drive.c; two types since 0.9.30, six since 0.9.41) on the host, measured on the firmware's integer chains.
 * 1. the laws: the harmonic distortion of a 441 Hz sine at the organ's level (RMS 0.73 at the drive) follows the GAIN knob on an
 *    exponential. TUBE DRIVE (0.9.31): THD = 25.2 (e^(0.085 g) - 1) %: under 0.5 % at 0.1, 0.6 .. 1.8 % at 0.5, about 4.7 % at
 *    2.0, 13.4 % at 5.0, 33.75 % at 10.0. The four of 0.9.41: the same exponential scaled to each type's top (SOFT TUBE 15 %,
 *    MID DRIVE 30 %, HARD CLIP 40 %, TIGHT TUBE 25 %), within 15 % at 2.0, 5.0 and 10.0. Never falling from one tenth to the next.
 * 2. level: the organ-like chord at that level comes out between the dry level -1.5 dB and +1.5 dB at every GAIN step, every type.
 * 3. bounds: a full-scale square and noise stay inside the output's clamp (4.0 in Q15) at every gain; silence in, silence out.
 * 4. blocks: the output does not depend on the block length (1, 7, 64: bit-exact).
 * 5. DC: no type leaves a DC (the shifts round; a DC blocker after the shaper): under 1 % of the signal's RMS.
 * 6. the bass (owner, 0.9.41): on a C2 + C3 triad organ chord, TIGHT TUBE leaves at least 6 dB less new low content (20 .. 250 Hz,
 *    away from the chord's partials: the intermodulation's difference tones, the rumble) than TUBE DRIVE at GAIN 5.0 and 10.0.
 * The input is Q13 (1.0 = 8192), the output Q15 up to 4.0 (+-131071). The type is the patch byte: 2 TUBE DRIVE, 3 DISTORTION,
 * 4 SOFT TUBE, 5 MID DRIVE, 6 HARD CLIP, 7 TIGHT TUBE (0, 1 and others: TUBE DRIVE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../firmware/src/eng_drive.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("drive: %-118s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        bad++;
}

#define N 44100
#define NT 6
static int32_t in[N], out[N], ref[N];
static dr_t D;
static const char *NAME[NT] = {"TUBE DRIVE", "DISTORTION", "SOFT TUBE", "MID DRIVE", "HARD CLIP", "TIGHT TUBE"};
static const double TOP[NT] = {33.75, 0, 15.0, 30.0, 40.0, 25.0};

static void run_drive(dr_t *d, const int32_t *x, int32_t *y, size_t blk)
{
    size_t i = 0;
    while (i < N) {
        size_t k = N - i < blk ? N - i : blk;
        dr_run_ex(d, x + i, y + i, (uint32_t)k);
        i += k;
    }
}
static double harm(const int32_t *y, size_t n, int k)          /* amplitude of harmonic k of 441 Hz over y[0..n) */
{
    double re = 0, im = 0, w = 2 * M_PI * 441.0 * k / 44100.0;
    size_t i;
    for (i = 0; i < n; i++) {
        re += y[i] * cos(w * (double)i);
        im += y[i] * sin(w * (double)i);
    }
    return 2 * sqrt(re * re + im * im) / (double)n;
}
static double thd(int t, uint32_t g)                           /* type t at GAIN g (tenths), THD in % */
{
    double f1, h = 0;
    int k;
    dr_init(&D);
    dr_user(&D, (uint32_t)(t + 2), g, 4u, 4u);
    run_drive(&D, in, out, 64);
    f1 = harm(out + 8000, 4000, 1);
    for (k = 2; k < 10; k++) {
        double a = harm(out + 8000, 4000, k);
        h += a * a;
    }
    return 100.0 * sqrt(h) / f1;
}
static double rms_db(const int32_t *y)
{
    double s = 0;
    size_t i;
    for (i = N / 2; i < N; i++)
        s += (double)y[i] * y[i];
    return 20 * log10(sqrt(s / (N / 2)) / 32768.0);
}

int main(void)
{
    size_t i;
    int t, g;
    const double op = 0.73;                                    /* the organ's RMS at the drive (tools/gen_drive_tables.py) */
    /* 1. the laws */
    for (i = 0; i < N; i++)
        in[i] = (int32_t)lrint(op * sqrt(2.0) * 8192 * sin(2 * M_PI * 441.0 * (double)i / 44100.0));
    for (t = 0; t < NT; t++) {
        double prev = 0, d[101];
        int mono = 1;
        size_t k;
        char what[160];
        if (t == 1)
            continue;                                          /* (DISTORTION: 3 dB of pre-gain a unit, no THD law) */
        for (g = 1; g <= 100; g++) {
            d[g] = thd(t, (uint32_t)g);
            if (d[g] < prev - 0.05)
                mono = 0;
            prev = d[g];
        }
        if (t == 0) {
            static const struct { int g; double lo, hi; } P[] = {{1, 0.0, 0.5}, {5, 0.6, 1.8}, {20, 3.7, 5.7}, {50, 11.4, 15.4}, {100, 30.0, 37.5}};
            for (k = 0; k < sizeof P / sizeof P[0]; k++) {
                snprintf(what, sizeof what, "TUBE DRIVE law: GAIN %.1f gives %.2f %% THD (between %.1f and %.1f %%)", P[k].g / 10.0, d[P[k].g], P[k].lo, P[k].hi);
                check(what, d[P[k].g] >= P[k].lo && d[P[k].g] <= P[k].hi);
            }
        } else {
            static const int G3[3] = {20, 50, 100};
            for (k = 0; k < 3; k++) {
                double want = TOP[t] * (exp(0.085 * G3[k] / 10.0) - 1.0) / (exp(0.85) - 1.0);
                snprintf(what, sizeof what, "%s law: GAIN %.1f gives %.2f %% THD (the law %.2f %%, within 15 %%)", NAME[t], G3[k] / 10.0, d[G3[k]], want);
                check(what, fabs(d[G3[k]] / want - 1.0) < 0.15);
            }
        }
        snprintf(what, sizeof what, "%s law: the distortion never falls from one tenth to the next (0.1 .. 10.0; %.2f %% at 0.1)", NAME[t], d[1]);
        check(what, mono);
    }
    /* 2. the level: an organ-like chord at the organ's level */
    {
        const double freqs[7] = {110, 165, 220, 330, 440, 660, 880};
        const double a = op * sqrt(2.0) / sqrt(7.0), dry = 20 * log10(op);
        for (i = 0; i < N; i++) {
            double tt = (double)i / 44100.0, c = 0;
            size_t k;
            for (k = 0; k < 7; k++)
                c += sin(2 * M_PI * freqs[k] * tt);
            in[i] = (int32_t)lrint(8192 * a * c);
        }
        for (t = 0; t < NT; t++) {
            double lo = 99, hi = -99, top = 0;
            char what[160];
            for (g = 1; g <= 100; g++) {
                double l;
                dr_init(&D);
                dr_user(&D, (uint32_t)(2 + t), (uint32_t)g, 4u, 4u);
                run_drive(&D, in, out, 64);
                l = rms_db(out) - dry;
                lo = l < lo ? l : lo;
                hi = l > hi ? l : hi;
                if (g == 100)
                    top = l;
            }
            snprintf(what, sizeof what, "level: %s: every GAIN step within the dry level +-1.5 dB (%+.2f .. %+.2f, at 10.0 %+.2f dB)", NAME[t], lo, hi, top);
            check(what, lo >= -1.5 && hi <= 1.5);
        }
    }
    /* 3. bounds: full scale (the input's clamp is 32767, 4.0 in Q13), a square and noise, at every gain and type */
    {
        int ok = 1;
        for (t = 0; t < NT; t++)
            for (g = 1; g <= 100; g += 3) {
                size_t k;
                dr_init(&D);
                dr_user(&D, (uint32_t)(2 + t), (uint32_t)g, 8u, 8u);
                for (k = 0; k < N; k++)
                    in[k] = (k / 50) % 2 ? 32767 : -32768;
                run_drive(&D, in, out, 64);
                for (k = 0; k < N; k++)
                    if (out[k] > 131071 || out[k] < -131072)
                        ok = 0;
                for (k = 0; k < N; k++)
                    in[k] = (int32_t)(rand() % 65536) - 32768;
                dr_init(&D);
                dr_user(&D, (uint32_t)(2 + t), (uint32_t)g, 8u, 8u);
                run_drive(&D, in, out, 64);
                for (k = 0; k < N; k++)
                    if (out[k] > 131071 || out[k] < -131072)
                        ok = 0;
            }
        check("bounds: square and noise at full scale stay inside the output's 4.0 (Q15), all six types, every 0.3 of GAIN, the output at +8", ok);
    }
    /* silence in, silence out */
    {
        int ok = 1;
        for (i = 0; i < N; i++)
            in[i] = 0;
        for (t = 0; t < NT; t++)
            for (g = 1; g <= 100; g += 33) {
                dr_init(&D);
                dr_user(&D, (uint32_t)(2 + t), (uint32_t)g, 4u, 4u);
                run_drive(&D, in, out, 64);
                for (i = 0; i < N; i++)
                    if (out[i] != 0)
                        ok = 0;
            }
        check("silence in, silence out (no DC from the high-pass, the bias shift or the tone), all six types", ok);
    }
    /* after a loud chord, silence again: the bias shift leaks back and the chain empties to exactly 0 */
    {
        int ok = 1;
        for (t = 0; t < NT; t++) {
            dr_init(&D);
            dr_user(&D, (uint32_t)(2 + t), 100u, 4u, 4u);
            for (i = 0; i < N; i++)
                in[i] = i < N / 4 ? (int32_t)lrint(2.0 * 8192 * sin(2 * M_PI * 220.0 * (double)i / 44100.0)) : 0;
            run_drive(&D, in, out, 64);                        /* a loud quarter second, then silence */
            for (i = 0; i < N; i++)
                in[i] = 0;
            run_drive(&D, in, out, 64);                        /* (another second of silence: the 10 Hz blockers' tails) */
            for (i = N / 2; i < N; i++)
                if (out[i] != 0)
                    ok = 0;
        }
        check("after a loud note, silence returns to exactly 0 within 1.5 s (the bias shift leaks back), all six types", ok);
    }
    /* 5. DC: a sine at the organ's level and a quiet one, every type, a few gains */
    {
        double worst = 0;
        int a, wt = 0;
        for (a = 0; a < 2; a++) {
            for (i = 0; i < N; i++)
                in[i] = (int32_t)lrint((a ? op : 0.2) * sqrt(2.0) * 8192 * sin(2 * M_PI * 441.0 * (double)i / 44100.0));
            for (t = 0; t < NT; t++)
                for (g = 1; g <= 100; g += 11) {
                    double m = 0, r = 0, q;
                    dr_init(&D);
                    dr_user(&D, (uint32_t)(2 + t), (uint32_t)g, 4u, 4u);
                    run_drive(&D, in, out, 64);
                    for (i = N / 2; i < N; i++) {
                        m += out[i];
                        r += (double)out[i] * out[i];
                    }
                    q = fabs(m / (N / 2)) / sqrt(r / (N / 2));
                    if (q > worst) {
                        worst = q;
                        wt = t;
                    }
                }
        }
        {
            char what[160];
            snprintf(what, sizeof what, "DC: under 1 %% of the RMS, all six types, quiet and at the organ's level (worst %.2f %%, %s)", 100 * worst, NAME[wt]);
            check(what, worst < 0.01);
        }
    }
    /* the removed types and the bytes out of range play as TUBE DRIVE */
    {
        static const uint32_t B[4] = {0, 1, 8, 255};
        int ok = 1;
        for (i = 0; i < N; i++)
            in[i] = (int32_t)lrint(0.5 * 8192 * sin(2 * M_PI * 220.0 * (double)i / 44100.0));
        dr_init(&D);
        dr_user(&D, 2u, 40u, 4u, 4u);
        run_drive(&D, in, ref, 64);
        for (t = 0; t < 4; t++) {
            dr_init(&D);
            dr_user(&D, B[t], 40u, 4u, 4u);
            run_drive(&D, in, out, 64);
            if (memcmp(out, ref, sizeof out))
                ok = 0;
        }
        check("the type bytes 0 and 1 (TUBE SCREAM, TUBE PREAMP before 0.9.30), 8 and 255 play as TUBE DRIVE", ok);
    }
    /* 4. block independence */
    {
        int ok = 1;
        for (i = 0; i < N; i++)
            in[i] = (int32_t)lrint(0.9 * 8192 * sin(2 * M_PI * 220.0 * (double)i / 44100.0));
        for (t = 0; t < NT; t++) {
            dr_init(&D);
            dr_user(&D, (uint32_t)(2 + t), 70u, 2u, 6u);
            run_drive(&D, in, ref, 64);
            dr_init(&D);
            dr_user(&D, (uint32_t)(2 + t), 70u, 2u, 6u);
            run_drive(&D, in, out, 1);
            if (memcmp(out, ref, sizeof out))
                ok = 0;
            dr_init(&D);
            dr_user(&D, (uint32_t)(2 + t), 70u, 2u, 6u);
            run_drive(&D, in, out, 7);
            if (memcmp(out, ref, sizeof out))
                ok = 0;
        }
        check("blocks: the output is the same for block lengths 1, 7 and 64 (bit-exact, all six types)", ok);
    }
    /* 6. the bass: new low content on a C2 + C3 E3 G3 organ chord (8', 4', 2 2/3' each) at the organ's level */
    {
        static const double F0[4] = {65.41, 130.81, 164.81, 196.0};
        double fr[12], mud[NT][2];
        static double w[N];
        int k, gi;
        double s2 = 0;
        for (k = 0; k < 12; k++)
            fr[k] = F0[k / 3] * (k % 3 + 1);
        for (i = 0; i < N; i++) {
            double c = 0, tt = (double)i / 44100.0;
            for (k = 0; k < 12; k++)
                c += sin(2 * M_PI * fr[k] * tt);
            w[i] = c;
            s2 += c * c;
        }
        for (i = 0; i < N; i++)
            in[i] = (int32_t)lrint(8192 * op * w[i] / sqrt(s2 / N));
        for (gi = 0; gi < 2; gi++)
            for (t = 0; t < NT; t++) {
                double tot = 0, nl = 0;
                const size_t n0 = N / 4, nn = N - N / 4;
                int f;
                dr_init(&D);
                dr_user(&D, (uint32_t)(2 + t), gi ? 100u : 50u, 4u, 4u);
                run_drive(&D, in, out, 64);
                for (i = 0; i < nn; i++) {
                    double hw = 0.5 - 0.5 * cos(2 * M_PI * (double)i / (double)(nn - 1)), v = out[n0 + i] * hw;
                    tot += v * v;
                }
                tot *= (double)nn / 2.0;                       /* (Parseval, one-sided) */
                for (f = 21; f < 250; f++) {
                    double re = 0, im = 0, wf = 2 * M_PI * f / 44100.0;
                    int near = 0;
                    for (k = 0; k < 12; k++)
                        if (fabs(f - fr[k]) < 3.0)
                            near = 1;
                    if (near)
                        continue;
                    for (i = 0; i < nn; i++) {
                        double hw = 0.5 - 0.5 * cos(2 * M_PI * (double)i / (double)(nn - 1)), v = out[n0 + i] * hw;
                        re += v * cos(wf * (double)i);
                        im += v * sin(wf * (double)i);
                    }
                    nl += (re * re + im * im) * (44100.0 / (double)nn);   /* (the bins are 44100 / nn Hz: per 1 Hz step) */
                }
                mud[t][gi] = 10 * log10(nl / tot);
            }
        for (gi = 0; gi < 2; gi++) {
            char what[200];
            printf("drive: new low content at GAIN %s (dB re the output):", gi ? "10.0" : "5.0");
            for (t = 0; t < NT; t++)
                printf(" %s %.1f;", NAME[t], mud[t][gi]);
            printf("\n");
            snprintf(what, sizeof what, "the bass: TIGHT TUBE at least 6 dB less new low content than TUBE DRIVE at GAIN %s (%.1f against %.1f dB)",
                     gi ? "10.0" : "5.0", mud[5][gi], mud[0][gi]);
            check(what, mud[5][gi] <= mud[0][gi] - 6.0);
        }
    }
    printf("drive: %s\n", bad ? "FAILED" : "all ok");
    return bad ? 1 : 0;
}
