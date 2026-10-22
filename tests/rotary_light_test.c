/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The rotary LIGHT (src/eng_rotary_light.c) on the host, against the rotary CLASSIC (src/eng_rotary.c), which
 * rotary_test.c checks against setBfree. LIGHT has no reference of its own: it is a cheaper approximation of CLASSIC, fitted
 * to its sidebands (tools/rotary_light), so the checks are about how close the two are and about LIGHT's own safety.
 * 1. sidebands: tones at 700 .. 5000 Hz (horn) and 100 .. 500 Hz (drum) at constant speed, the 9 sidebands (k = -4 .. 4
 *    times the rotor's rate) in dB: the rms difference to CLASSIC's, every case below 12 dB and the mean below 7.5 dB
 *    (measured 5 to 10, mean 6.4).
 * 2. levels: the horn's and the drum's total output within 7 dB of CLASSIC's (measured 1.4 to 4.1 dB, except
 *    500 Hz on the drum, 6.7: the crossover's region).
 * 3. bounds: a loud chord and full-scale sines with both rotors turning stay bounded; silence in, silence out.
 * 4. the model switch: CLASSIC -> LIGHT -> CLASSIC while a chord plays: no jump (the output stays within 4x the
 *    steady rms), and once the fade is over the output is LIGHT's / CLASSIC's alone (above 45 dB SNR). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "felucca_tables.h"
static inline int32_t sine_i(uint32_t ph)
{
    uint32_t i = ph >> 22;
    int32_t a = SINE[i], b = SINE[(i + 1u) & 1023u];
    return a + (((b - a) * (int32_t)((ph >> 7) & 0x7FFFu)) >> 15);
}
#include "../firmware/src/eng_rotary.c"
#include "../firmware/src/eng_rotary_light.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("rotary light: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
#define FS_ 44100.0
static uint32_t turn(double rpm) { return (uint32_t)(rpm / 60.0 / FS_ * 4294967296.0 + 0.5); }

static void sbands(const float *y, double f, double fr, int n0, int W, double *o)
{
    int k, n;
    double ws = 0;
    for (n = 0; n < W; n++) ws += 0.5 - 0.5 * cos(2 * M_PI * n / W);
    for (k = -4; k <= 4; k++) {
        double re = 0, im = 0, w = 2 * M_PI * (f + k * fr) / FS_, a;
        for (n = 0; n < W; n++) {
            double wn = 0.5 - 0.5 * cos(2 * M_PI * n / W), ph = w * (n + n0);
            re += wn * y[n0 + n] * cos(ph);
            im -= wn * y[n0 + n] * sin(ph);
        }
        a = 2 * sqrt(re * re + im * im) / ws / 0.5;
        o[k + 4] = 20 * log10(a > 1e-3 ? a : 1e-3);
    }
}
static double tot(const double *v) { double s = 0; int k; for (k = 0; k < 9; k++) s += pow(10, v[k] / 10); return 10 * log10(s); }
static double peak(const float *y, size_t n) { double m = 0; size_t i; for (i = 0; i < n; i++) if (fabs(y[i]) > m) m = fabs(y[i]); return m; }
static double rms(const float *y, size_t n) { double e = 0; size_t i; for (i = 0; i < n; i++) e += (double)y[i] * y[i]; return sqrt(e / n); }

/* model 0 CLASSIC, 1 LIGHT; hrpm / drpm constant (the rotors start at full speed: no ramp) */
static void render(int model, const float *in, float *L, float *R, size_t n, uint32_t hinc, uint32_t dinc)
{
    rot_t *r = calloc(1, sizeof *r);
    lt_t *t = calloc(1, sizeof *t);
    int32_t x[ROT_CTL], l[ROT_CTL], rr[ROT_CTL];
    size_t i, j;
    rot_init(r);
    r->inc_h = hinc; r->inc_d = dinc; r->tgt_h = hinc; r->tgt_d = dinc;
    lt_init(t, r);
    for (i = 0; i < n; i += ROT_CTL) {
        size_t k = n - i < ROT_CTL ? n - i : ROT_CTL;
        for (j = 0; j < k; j++) x[j] = (int32_t)lrintf(in[i + j] * 32768.0f);
        if (model) lt_samples(r, t, x, l, rr, (uint32_t)k); else rot_samples(r, x, l, rr, (uint32_t)k);
        for (j = 0; j < k; j++) { L[i + j] = l[j] / 32768.0f; R[i + j] = rr[j] / 32768.0f; }
    }
    free(r); free(t);
}

int main(void)
{
    static const struct { int kind; int hz; } cs[9] = {{0, 700}, {0, 1000}, {0, 2000}, {0, 3500}, {0, 5000}, {1, 100}, {1, 200}, {1, 300}, {1, 500}};
    enum { N = 176400 };
    static float in[N], L[N], R[N], L2[N], R2[N], L3[N], R3[N];
    double sum = 0, worst = 0, lev = 0;
    int c, ok_lev = 1;
    size_t i;

    for (c = 0; c < 9; c++) {
        double fr = cs[c].kind ? 340.0 / 60 : 400.0 / 60, a[18], b[18], e = 0, d;
        int k, n0, W;
        double T = FS_ / fr;
        n0 = (int)(2 * T + 0.5); W = (int)(6 * T + 0.5);
        for (i = 0; i < (size_t)(n0 + W); i++) in[i] = 0.5f * (float)sin(2 * M_PI * cs[c].hz * i / FS_);
        render(0, in, L, R, n0 + W, cs[c].kind ? 0 : turn(400), cs[c].kind ? turn(340) : 0);
        render(1, in, L2, R2, n0 + W, cs[c].kind ? 0 : turn(400), cs[c].kind ? turn(340) : 0);
        sbands(L, cs[c].hz, fr, n0, W, a); sbands(R, cs[c].hz, fr, n0, W, a + 9);
        sbands(L2, cs[c].hz, fr, n0, W, b); sbands(R2, cs[c].hz, fr, n0, W, b + 9);
        for (k = 0; k < 18; k++) { double x = a[k] - b[k]; e += x * x; }
        d = sqrt(e / 18);
        sum += d;
        if (d > worst) worst = d;
        d = (tot(a) + tot(a + 9)) / 2 - (tot(b) + tot(b + 9)) / 2;
        if (fabs(d) > 7) ok_lev = 0;
        lev += d;
        printf("rotary light: %s %4d Hz: sidebands rms %.1f dB, level (CLASSIC - LIGHT) %.1f dB\n", cs[c].kind ? "drum" : "horn", cs[c].hz, sqrt(e / 18), d);
    }
    check("sidebands: every case within 12 dB (rms) of CLASSIC's", worst < 12);
    check("sidebands: the mean within 7.5 dB of CLASSIC's", sum / 9 < 7.5);
    check("levels: every case within 7 dB of CLASSIC's", ok_lev);

    for (i = 0; i < N; i++) {
        double t = i / FS_;
        in[i] = (float)(0.55 * sin(2 * M_PI * 55 * t) + 0.3 * sin(2 * M_PI * 110 * t) + 0.15 * sin(2 * M_PI * 1760 * t));
    }
    render(1, in, L, R, N, turn(400), turn(340));
    check("a loud chord with both rotors turning stays within 4x full scale", peak(L, N) < 4 && peak(R, N) < 4);
    {
        static const int hz[4] = {263, 668, 1182, 4486};
        int f, ok = 1;
        for (f = 0; f < 4; f++) {
            for (i = 0; i < N; i++) in[i] = 0.99f * (float)sin(2 * M_PI * hz[f] * i / FS_);
            render(1, in, L, R, N, turn(400), turn(340));
            ok &= peak(L, N) < 6 && peak(R, N) < 6;
        }
        check("full-scale sines at the filters' resonances stay within 6x full scale", ok);
    }
    memset(in, 0, sizeof in);
    render(1, in, L, R, N, turn(400), turn(340));
    check("silence in, silence out", peak(L, N) == 0 && peak(R, N) == 0);

    /* the switch: CLASSIC until 1 s, LIGHT from 1 s to 2.5 s, CLASSIC again from 2.5 s */
    for (i = 0; i < N; i++) {
        double t = i / FS_;
        in[i] = (float)(0.3 * sin(2 * M_PI * 220 * t) + 0.2 * sin(2 * M_PI * 880 * t) + 0.1 * sin(2 * M_PI * 2200 * t));
    }
    render(0, in, L2, R2, N, turn(400), turn(340));
    render(1, in, L3, R3, N, turn(400), turn(340));
    {
        rot_t *r = calloc(1, sizeof *r);
        lt_t *t = calloc(1, sizeof *t);
        rotmix_t m = {0, 0};
        int32_t x[ROT_CTL], l[ROT_CTL], rr[ROT_CTL];
        size_t j, s1 = 44100, s2 = 110250;
        double e1 = 0, e2 = 0, e3 = 0, d1 = 0, d2 = 0, e1s = 0, mx = 0;
        rot_init(r);
        r->inc_h = turn(400); r->inc_d = turn(340); r->tgt_h = r->inc_h; r->tgt_d = r->inc_d;
        lt_init(t, r);
        for (i = 0; i < N; i += ROT_CTL) {
            size_t k = N - i < ROT_CTL ? N - i : ROT_CTL;
            if (i >= s1 && i < s1 + ROT_CTL) rotary_model(&m, r, t, 1);
            if (i >= s2 && i < s2 + ROT_CTL) rotary_model(&m, r, t, 0);
            for (j = 0; j < k; j++) x[j] = (int32_t)lrintf(in[i + j] * 32768.0f);
            rotary_run(&m, r, t, x, l, rr, (uint32_t)k);
            for (j = 0; j < k; j++) {
                float yl = l[j] / 32768.0f;
                size_t n = i + j;
                if (n < s1 - 4000) { double dd = yl - L2[n]; d1 += dd * dd; e1 += L2[n] * L2[n]; }
                if (n >= s1 + 8000 && n < s2 - 100) { double dd = yl - L3[n]; d2 += dd * dd; e2 += L3[n] * L3[n]; }
                if (n >= s2 + 8000) { double dd = yl - L2[n]; e3 += dd * dd; e1s += L2[n] * L2[n]; }
                if (n >= s1 && n < s1 + 1500 && fabs(yl) > mx) mx = fabs(yl);
                if (n >= s2 && n < s2 + 1500 && fabs(yl) > mx) mx = fabs(yl);
            }
        }
        printf("rotary light: switch: CLASSIC before %.1f dB, LIGHT during %.1f dB, CLASSIC after %.1f dB SNR\n",
               10 * log10(e1 / (d1 + 1e-12)), 10 * log10(e2 / (d2 + 1e-12)), 10 * log10(e1s / (e3 + 1e-12)));
        check("the switch: before it the output is CLASSIC's (bit exact)", d1 == 0 && e1 > 0);
        check("the switch: no jump in either fade (the output stays within 4x the steady rms)", mx < 4 * rms(L2, N));
        check("the switch: LIGHT's output once the fade is over (above 45 dB SNR with LIGHT alone)", 10 * log10(e2 / (d2 + 1e-12)) > 45);
        check("the switch back: CLASSIC's output once the fade is over (above 45 dB SNR with CLASSIC alone)", 10 * log10(e1s / (e3 + 1e-12)) > 45);
        free(r); free(t);
    }
    return bad ? 1 : 0;
}
