/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The drive (src/eng_drive.c, 0.9.24) on the host: the four types at GAIN 1 .. 10.
 * 1. level: a -20 dBFS chord comes out between the dry level -0.5 dB and +3 dB at every GAIN step (the
 *    compensation: the same level, rising a little over the knob, +2.5 dB at the top).
 * 2. saturation: the third harmonic of a -20 dBFS sine (against the fundamental) rises from GAIN 1 to GAIN 10, every type, by 3 dB or more.
 * 3. bounds: full-scale sines, a square and noise stay inside 16 bits at every gain; silence in, silence out.
 * 4. blocks: the output does not depend on the block length (1, 7, 64: bit-exact).
 * Levels are in Q15 (1.0 = 32768), the input in Q13 (1.0 = 8192). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../firmware/src/eng_drive.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("drive: %-96s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        bad++;
}

#define N 32768
static int32_t in[N], out[N], ref[N], sine[N];

/* the amplitude (dB re 1.0) of frequency f over the last half of x (a correlation at f, Hann) */
static double amp_db(const int32_t *x, double f, double fs)
{
    double re = 0, im = 0, w = 0, t;
    size_t i, s = N / 2;
    for (i = s; i < N; i++) {
        t = (double)(i - s) / (double)(N - s);
        double h = 0.5 * (1 - cos(2 * M_PI * t));
        re += h * (x[i] / 32768.0) * cos(2 * M_PI * f * (double)i / fs);
        im += h * (x[i] / 32768.0) * sin(2 * M_PI * f * (double)i / fs);
        w += h;
    }
    return 20 * log10(2 * sqrt(re * re + im * im) / w + 1e-12);
}
static double rms_db(const int32_t *x)
{
    double s = 0;
    size_t i;
    for (i = N / 2; i < N; i++)
        s += (x[i] / 32768.0) * (x[i] / 32768.0);
    return 10 * log10(s / (N / 2) + 1e-30);
}
static void run_drive(dr_t *d, const int32_t *x, int32_t *y, size_t blk)
{
    size_t i = 0;
    while (i < N) {
        size_t k = N - i < blk ? N - i : blk;
        dr_run_ex(d, x + i, y + i, (uint32_t)k);
        i += k;
    }
}
static dr_t D;

int main(void)
{
    static const char *NAME[4] = {"TUBE SCREAM", "TUBE PREAMP", "TUBE DRIVE", "DISTORTION"};
    size_t i;
    int t, g;
    /* the level input: a three-note chord at -20 dBFS RMS (as the tables are calibrated: tools/gen_drive_tables.py), in
     * Q13 (1.0 = 8192); the saturation input: a sine at -20 dBFS, 1 kHz */
    const double freqs[7] = {110, 165, 220, 330, 440, 660, 880};
    const double a = pow(10.0, -18.0 / 20) * sqrt(2.0) / sqrt(7.0);
    for (i = 0; i < N; i++) {
        double t = (double)i / 44100.0, c = 0;
        size_t k;
        for (k = 0; k < 7; k++)
            c += sin(2 * M_PI * freqs[k] * t);
        in[i] = (int32_t)lrint(8192 * a * c);
        sine[i] = (int32_t)lrint(0.1 * 8192 * sin(2 * M_PI * 1000 * t));
    }
    /* the dry level: the input in the drive's unit (1.0 = 8192 in Q13): 0.1 RMS */
    const double dry = -18.0;
    for (t = 0; t < 4; t++) {
        double h3[11], lev[11];
        int ok_level = 1, ok_top = 1;
        for (g = 1; g <= 10; g++) {
            dr_init(&D);
            dr_user(&D, (uint32_t)t, (uint32_t)(g * 10), 4u, 4u);
            run_drive(&D, in, out, 64);
            lev[g] = rms_db(out);
            dr_init(&D);
            dr_user(&D, (uint32_t)t, (uint32_t)(g * 10), 4u, 4u);
            run_drive(&D, sine, out, 64);
            h3[g] = amp_db(out, 3000.0, 44100.0);
            if (lev[g] - dry < -0.5 || lev[g] - dry > 3.0)
                ok_level = 0;
        }
        {
            char what[120];
            snprintf(what, sizeof what, "level: %s: every GAIN step between the dry level -0.5 dB and +3 dB", NAME[t]);
            check(what, ok_level);
        }
        if (lev[10] - dry < 1.0 || lev[10] - dry > 3.0)
            ok_top = 0;
        {
            char what[120];
            snprintf(what, sizeof what, "level: %s rises a little over the knob (GAIN 10: %+.2f dB against GAIN 1: %+.2f dB)",
                     NAME[t], lev[10] - dry, lev[1] - dry);
            check(what, ok_top && lev[10] >= lev[1] - 0.2);
        }
        {
            char what[120];
            snprintf(what, sizeof what, "saturation: %s: the third harmonic rises from GAIN 1 (%.1f dB) to GAIN 10 (%.1f dB)",
                     NAME[t], h3[1], h3[10]);
            check(what, h3[10] > h3[1] + 3.0);
        }
    }
    /* bounds: full scale (the input's clamp is 32767, 4.0 in Q13), a square and noise, at every gain and type */
    {
        int ok = 1;
        for (t = 0; t < 4; t++)
            for (g = 1; g <= 10; g++) {
                size_t k;
                dr_init(&D);
                dr_user(&D, (uint32_t)t, (uint32_t)(g * 10), 8u, 8u);
                for (k = 0; k < N; k++)
                    in[k] = (k / 50) % 2 ? 32767 : -32768;
                run_drive(&D, in, out, 64);
                for (k = 0; k < N; k++)
                    if (out[k] > 32767 || out[k] < -32768)
                        ok = 0;
                for (k = 0; k < N; k++)
                    in[k] = (int32_t)(rand() % 65536) - 32768;
                dr_init(&D);
                dr_user(&D, (uint32_t)t, (uint32_t)(g * 10), 8u, 8u);
                run_drive(&D, in, out, 64);
                for (k = 0; k < N; k++)
                    if (out[k] > 32767 || out[k] < -32768)
                        ok = 0;
            }
        check("bounds: square and noise at full scale stay inside 16 bits, every type and GAIN, with the output at +8", ok);
    }
    /* silence in, silence out */
    {
        int ok = 1;
        for (i = 0; i < N; i++)
            in[i] = 0;
        for (t = 0; t < 4; t++) {
            dr_init(&D);
            dr_user(&D, (uint32_t)t, 100u, 4u, 4u);
            run_drive(&D, in, out, 64);
            for (i = 0; i < N; i++)
                if (out[i] != 0)
                    ok = 0;
        }
        check("silence in, silence out (no DC from the high-pass or the tone)", ok);
    }
    /* block independence */
    {
        int ok = 1;
        for (i = 0; i < N; i++)
            in[i] = (int32_t)lrint(0.3 * 8192 * sin(2 * M_PI * 220.0 * (double)i / 44100.0));
        for (t = 0; t < 4; t++) {
            dr_init(&D);
            dr_user(&D, (uint32_t)t, 70u, 2u, 6u);
            run_drive(&D, in, ref, 64);
            dr_init(&D);
            dr_user(&D, (uint32_t)t, 70u, 2u, 6u);
            run_drive(&D, in, out, 1);
            for (i = 0; i < N; i++)
                if (out[i] != ref[i])
                    ok = 0;
            dr_init(&D);
            dr_user(&D, (uint32_t)t, 70u, 2u, 6u);
            run_drive(&D, in, out, 7);
            for (i = 0; i < N; i++)
                if (out[i] != ref[i])
                    ok = 0;
        }
        check("blocks: the output is the same for block lengths 1, 7 and 64 (bit-exact, every type)", ok);
    }
    printf("drive: %s\n", bad ? "FAILED" : "all ok");
    return bad ? 1 : 0;
}
