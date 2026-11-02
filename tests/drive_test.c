/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The drive (src/eng_drive.c; two types since 0.9.30: TUBE DRIVE, DISTORTION) on the host.
 * 1. TUBE DRIVE's law (owner, 0.9.30): the harmonic distortion of a 441 Hz sine at the organ's level (RMS 0.73 at the drive)
 *    grows on an exponential of the GAIN knob: nearly none at 0.1 (< 0.5 %), 0.5 just audible (0.4 .. 1.5 %), about 3 % at 2.0,
 *    10 % at 5.0, 27 % at 10.0, and it never falls from one tenth to the next. Measured on the firmware's integer chain.
 * 2. level: the organ-like chord at that level comes out between the dry level -1.5 dB and +1.5 dB at every GAIN step, both types.
 * 3. bounds: a full-scale square and noise stay inside the output's clamp (4.0 in Q15) at every gain; silence in, silence out.
 * 5. DC: no type leaves a DC (the shifts round; a DC blocker after the shaper): under 1 % of the signal's RMS.
 * 4. blocks: the output does not depend on the block length (1, 7, 64: bit-exact).
 * The input is Q13 (1.0 = 8192), the output Q15 up to 4.0 (+-131071). The type is the patch byte: 2 TUBE DRIVE, 3 DISTORTION (0, 1: TUBE DRIVE). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../firmware/src/eng_drive.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("drive: %-112s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        bad++;
}

#define N 32768
static int32_t in[N], out[N], ref[N];
static dr_t D;

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
static double thd(uint32_t g)                                  /* TUBE DRIVE at GAIN g (tenths), THD in % */
{
    double f1, h = 0;
    int k;
    dr_init(&D);
    dr_user(&D, 2u, g, 4u, 4u);
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
    static const char *NAME[2] = {"TUBE DRIVE", "DISTORTION"};
    size_t i;
    int t, g;
    const double op = 0.73;                                    /* the organ's RMS at the drive (tools/gen_drive_tables.py) */
    /* 1. the law */
    {
        static const struct { int g; double lo, hi; } P[] = {{1, 0.0, 0.5}, {5, 0.4, 1.5}, {20, 2.5, 4.5}, {50, 8.0, 12.0}, {100, 23.0, 31.0}};
        double prev = 0, d[101];
        int mono = 1;
        size_t k;
        for (i = 0; i < N; i++)
            in[i] = (int32_t)lrint(op * sqrt(2.0) * 8192 * sin(2 * M_PI * 441.0 * (double)i / 44100.0));
        for (g = 1; g <= 100; g++) {
            d[g] = thd((uint32_t)g);
            if (d[g] < prev - 0.05)
                mono = 0;
            prev = d[g];
        }
        for (k = 0; k < sizeof P / sizeof P[0]; k++) {
            char what[128];
            snprintf(what, sizeof what, "TUBE DRIVE law: GAIN %.1f gives %.2f %% THD (between %.1f and %.1f %%)", P[k].g / 10.0, d[P[k].g], P[k].lo, P[k].hi);
            check(what, d[P[k].g] >= P[k].lo && d[P[k].g] <= P[k].hi);
        }
        check("TUBE DRIVE law: the distortion never falls from one tenth to the next (0.1 .. 10.0)", mono);
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
        for (t = 0; t < 2; t++) {
            double lo = 99, hi = -99, top = 0;
            char what[128];
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
        for (t = 0; t < 2; t++)
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
        check("bounds: square and noise at full scale stay inside the output's 4.0 (Q15), both types, every 0.3 of GAIN, the output at +8", ok);
    }
    /* silence in, silence out */
    {
        int ok = 1;
        for (i = 0; i < N; i++)
            in[i] = 0;
        for (t = 0; t < 2; t++)
            for (g = 1; g <= 100; g += 33) {
                dr_init(&D);
                dr_user(&D, (uint32_t)(2 + t), (uint32_t)g, 4u, 4u);
                run_drive(&D, in, out, 64);
                for (i = 0; i < N; i++)
                    if (out[i] != 0)
                        ok = 0;
            }
        check("silence in, silence out (no DC from the high-pass or the tone)", ok);
    }
    /* 5. DC: a sine at the organ's level and a quiet one, every type, a few gains */
    {
        double worst = 0;
        int a;
        for (a = 0; a < 2; a++) {
            for (i = 0; i < N; i++)
                in[i] = (int32_t)lrint((a ? op : 0.2) * sqrt(2.0) * 8192 * sin(2 * M_PI * 441.0 * (double)i / 44100.0));
            for (t = 0; t < 2; t++)
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
                    worst = q > worst ? q : worst;
                }
        }
        {
            char what[128];
            snprintf(what, sizeof what, "DC: under 1 %% of the RMS, both types, quiet and at the organ's level (worst %.2f %%)", 100 * worst);
            check(what, worst < 0.01);
        }
    }
    /* the removed types: bytes 0 and 1 play as TUBE DRIVE */
    {
        int ok = 1;
        for (i = 0; i < N; i++)
            in[i] = (int32_t)lrint(0.5 * 8192 * sin(2 * M_PI * 220.0 * (double)i / 44100.0));
        dr_init(&D);
        dr_user(&D, 2u, 40u, 4u, 4u);
        run_drive(&D, in, ref, 64);
        for (t = 0; t < 2; t++) {
            dr_init(&D);
            dr_user(&D, (uint32_t)t, 40u, 4u, 4u);
            run_drive(&D, in, out, 64);
            if (memcmp(out, ref, sizeof out))
                ok = 0;
        }
        check("the type bytes 0 and 1 (TUBE SCREAM, TUBE PREAMP before 0.9.30) play as TUBE DRIVE", ok);
    }
    /* block independence */
    {
        int ok = 1;
        for (i = 0; i < N; i++)
            in[i] = (int32_t)lrint(0.3 * 8192 * sin(2 * M_PI * 220.0 * (double)i / 44100.0));
        for (t = 0; t < 2; t++) {
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
        check("blocks: the output is the same for block lengths 1, 7 and 64 (bit-exact, both types)", ok);
    }
    printf("drive: %s\n", bad ? "FAILED" : "all ok");
    return bad ? 1 : 0;
}
