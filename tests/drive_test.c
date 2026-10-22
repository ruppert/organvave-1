/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The drive (src/eng_drive.c) on the host, against setBfree's overdrive.c (tools/setbfree_ref/drive_render.c, default
 * constants). The reference numbers below are its output for sines at 110 Hz and 1 kHz, 0.1 to 3.9 times 1.0 (odd harmonics 1, 3, 5, 7 in dB
 * re a full-scale sine, last two seconds of three, Hann window); the curve is odd, so the even ones stay below -85 dB.
 * 1. harmonics: the odd ones within 0.4 dB of the reference (1.0 dB below -60 dB), the even ones below -85 dB.
 * 2. blocks: the output does not depend on the block length (1, 7, 32, 64: bit-exact).
 * 3. bounds: full-scale square, noise and sines at the largest gain stay inside 16 bits and do not wrap; silence in,
 *    silence out; a DC step is gone within 6 s (the post-differentiator).
 * The largest gains (10 in and out) can put the output at 7 times full scale: the stage after the drive clamps. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../firmware/src/eng_drive.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("drive: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
#define FS_ 44100.0
#define N_ 132300

static void render(dr_t *d, const int32_t *x, int32_t *y, int n, int blk)
{
    int i;
    for (i = 0; i < n; i += blk)
        dr_run(d, x + i, y + i, (uint32_t)(n - i < blk ? n - i : blk));
}
static double level(const int32_t *y, double f)
{
    int n = (int)(FS_ * 2), n0 = N_ - n, i;
    double re = 0, im = 0, ws = 0;
    for (i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / n), ph = 2 * M_PI * f * (i + n0) / FS_;
        re += w * y[n0 + i] / 32768.0 * cos(ph);
        im -= w * y[n0 + i] / 32768.0 * sin(ph);
        ws += w;
    }
    return 20 * log10(2 * sqrt(re * re + im * im) / ws + 1e-9);
}

typedef struct { double f, amp; double h[4]; } hcase;
static const hcase HC[] = {
    {110, 0.1, {-23.1, -58.9, -75.5, -84.8}},
    {110, 0.3, {-20.2, -31.1, -41.2, -48.4}},
    {110, 1.0, {-11.9, -13.6, -20.7, -30.4}},
    {1000, 0.3, {-18.7, -28.7, -33.4, -42.4}},
    {1000, 1.0, {-12.9, -12.7, -28.0, -23.6}},
    {110, 3.0, {-13.4, -15.8, -22.9, -19.5}},      /* a loud chord is several times 1.0 */
    {110, 3.9, {-14.8, -17.3, -21.0, -20.2}},
    {1000, 3.0, {-14.4, -16.2, -20.5, -19.8}},
};

int main(void)
{
    static int32_t x[N_], y[N_], y2[N_];
    dr_t *d = calloc(1, sizeof *d);
    unsigned c, i, k;
    for (c = 0; c < sizeof HC / sizeof *HC; c++) {
        char what[160];
        int ok = 1, ok2 = 1;
        double worst = 0, ev = -200;
        for (i = 0; i < N_; i++)
            x[i] = (int32_t)lrint(HC[c].amp * 8192.0 * sin(2 * M_PI * HC[c].f * i / FS_));
        dr_init(d);
        render(d, x, y, N_, 32);
        for (k = 0; k < 4; k++) {
            double got = level(y, HC[c].f * (2 * k + 1)), dd = fabs(got - HC[c].h[k]);
            double tol = HC[c].h[k] > -60 ? 0.4 : 1.0;
            if (HC[c].f * (2 * k + 1) >= 21000) continue;
            if (dd > worst) worst = dd;
            if (dd > tol) ok = 0;
        }
        for (k = 1; k <= 3; k++) {
            double got = level(y, HC[c].f * 2 * k);
            if (got > ev) ev = got;
        }
        ok2 = ev < -85;
        snprintf(what, sizeof what, "%4.0f Hz at %.1f: odd harmonics within 0.4 dB of setBfree's (worst %.2f dB)", HC[c].f, HC[c].amp, worst);
        check(what, ok);
        snprintf(what, sizeof what, "%4.0f Hz at %.1f: even harmonics below -85 dB (%.1f)", HC[c].f, HC[c].amp, ev);
        check(what, ok2);
    }
    /* blocks */
    {
        static const int bl[] = {1, 7, 32, 64};
        int same = 1, b;
        for (i = 0; i < N_; i++)
            x[i] = (int32_t)lrint(0.6 * 8192.0 * sin(2 * M_PI * 233.0 * i / FS_) * (0.5 + 0.5 * sin(2 * M_PI * 0.7 * i / FS_)));
        dr_init(d);
        render(d, x, y, N_, 32);
        for (b = 0; b < 4; b++) {
            dr_init(d);
            render(d, x, y2, N_, bl[b]);
            if (memcmp(y, y2, sizeof y)) same = 0;
        }
        check("the output does not depend on the block length (1, 7, 32, 64: bit-exact)", same);
    }
    /* bounds */
    {
        int ok = 1, quiet = 1, gone = 1;
        unsigned seed = 12345;
        double pk = 0;
        for (i = 0; i < N_; i++) {
            seed = seed * 1664525u + 1013904223u;
            x[i] = ((i / 100) & 1) ? 32767 : -32768;   /* 4.0 in Q13 */
            y2[i] = (int32_t)(seed >> 16) - 32768;
        }
        dr_init(d);
        dr_set(d, 40960, 40960, 9766000, 16760438, 10425000, 35043, 157700, 16626000);
        render(d, x, y, N_, 32);
        for (i = 0; i < N_; i++) if (fabs((double)y[i]) > pk) pk = fabs((double)y[i]);
        ok &= pk < 2147483647.0 / 4096;
        render(d, y2, y, N_, 32);
        for (i = 0; i < N_; i++) if (fabs((double)y[i]) > pk) pk = fabs((double)y[i]);
        ok &= pk < 2147483647.0 / 4096;
        printf("drive: largest output at gains 10 / 10: %.0f (of 32768)\n", pk);
        check("full-scale square and noise at the largest gains: no overflow (the output stays below 2^19)", ok);
        dr_init(d);
        memset(x, 0, sizeof x);
        render(d, x, y, N_, 32);
        for (i = 0; i < N_; i++) if (y[i]) quiet = 0;
        check("silence in, silence out", quiet);
        dr_init(d);
        for (i = 0; i < N_; i++) x[i] = i < 8000 ? 0 : 1500;
        render(d, x, y, N_, 32);
        for (i = (unsigned)(FS_ * 2.9); i < N_; i++) if (abs(y[i]) > 40) gone = 0;
        printf("drive: the DC step of 1500 at the end: |y| < %d\n", 41);
        check("a DC step is blocked: the output is back near 0 (below 40 of 32768) at the end of 3 s", gone);
    }
    printf(bad ? "drive: FAILED\n" : "drive: all ok\n");
    free(d);
    return bad;
}
