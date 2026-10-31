/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The rotary CLASSIC (src/eng_rotary.c) on the host, against numbers measured on setBfree's own whirl.c
 * (tools/setbfree_ref/whirl_render; DEVELOPING.md).
 * 1. sidebands: 440 Hz at 0.5, horn 400 rpm, drum stopped, last second of 4 s: the carrier's sidebands at -/+ 6.67 Hz and
 *    -13.33 Hz are setBfree's -16.26 / -26.82 / -30.48 dB (Hann window; within 0.5 dB, measured within 0.05).
 *    The same with the mic angle at 90 degrees (right channel: -5.85 / -13.09 / -15.12 dB).
 * 2. stopped rotors: no sidebands (rounding noise only), and the same render twice is identical.
 * 3. the speed ramp: reaches its target exactly (no stall short of it, 400 rpm horn, 340 rpm drum) and holds.
 * 4. bounds: a loud chord and full-scale sines at the filters' resonances never overflow (the output stays within 8x full scale, the rms in setBfree's range) with both rotors turning; silence in, silence out. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../firmware/src/eng_rotary.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("rotary: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
static uint32_t mic_deg = 180;                       /* render(): the horn mic angle */
static uint8_t var_req = 0;                         /* render(): the classic variant (ROT_V_*, rotmix's table) */
static int32_t vg_req = 1 << 24;                    /* render(): its level gain, Q24 */
static uint32_t turn(double rpm) { return (uint32_t)(rpm / 60.0 / 44100.0 * 4294967296.0 + 0.5); }

/* renders n samples of in[] (float, 1.0 = full scale) into L, R (float) */
static void render(const float *in, float *L, float *R, size_t n, double hrpm, double drpm)
{
    rot_t *r = calloc(1, sizeof *r);
    int32_t x[ROT_CTL], l[ROT_CTL], rr[ROT_CTL];
    size_t i, j;
    rot_init(r); r->k_ha = rot_k(161000u); r->k_hd = rot_k(321000u);   /* (setBfree's horn ramps: the reference's; the firmware's defaults are slower since 0.9.3) */
    r->var = var_req; r->vg = vg_req;
    rot_mic(r, mic_deg);
    rot_target(r, turn(hrpm), turn(drpm));
    for (i = 0; i < n; i += ROT_CTL) {
        size_t k = n - i < ROT_CTL ? n - i : ROT_CTL;
        for (j = 0; j < k; j++) x[j] = (int32_t)lrintf(in[i + j] * 32768.0f);
        rot_block(r, x, l, rr, (uint32_t)k);
        for (j = 0; j < k; j++) { L[i + j] = l[j] / 32768.0f; R[i + j] = rr[j] / 32768.0f; }
    }
    free(r);
}
static double amp(const float *y, size_t n, double f)
{
    double re = 0, im = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        double a = 2 * M_PI * f * i / 44100.0, w = 0.5 - 0.5 * cos(2 * M_PI * i / n);
        re += w * y[i] * cos(a);
        im -= w * y[i] * sin(a);
    }
    return 2 * sqrt(re * re + im * im) / n;
}
static double peak(const float *y, size_t n) { double m = 0; size_t i; for (i = 0; i < n; i++) if (fabs(y[i]) > m) m = fabs(y[i]); return m; }

int main(void)
{
    rot_load_tables();                          /* (main.c's boot: the tables to SRAM and the 256-angle copies) */
    enum { N = 176400 };
    static float in[N], L[N], R[N], L2[N], R2[N];
    double fr = 400.0 / 60.0, c, s1, s2, s3;
    size_t i;
    int f;

    for (i = 0; i < N; i++) in[i] = 0.5f * (float)sin(2 * M_PI * 440.0 * i / 44100.0);
    render(in, L, R, N, 400, 0);
    c = amp(L + 3 * 44100, 44100, 440.0);
    s1 = 20 * log10(amp(L + 3 * 44100, 44100, 440.0 - fr) / c);
    s2 = 20 * log10(amp(L + 3 * 44100, 44100, 440.0 + fr) / c);
    s3 = 20 * log10(amp(L + 3 * 44100, 44100, 440.0 - 2 * fr) / c);
    printf("rotary: sidebands %.2f %.2f %.2f dB\n", s1, s2, s3);
    check("sideband at 440 - 6.67 Hz is setBfree's -16.26 dB", fabs(s1 + 16.26) < 0.5);
    check("sideband at 440 + 6.67 Hz is setBfree's -26.82 dB", fabs(s2 + 26.82) < 0.5);
    check("sideband at 440 - 13.33 Hz is setBfree's -30.48 dB", fabs(s3 + 30.48) < 0.5);

    mic_deg = 90;
    render(in, L, R, N, 400, 0);
    s1 = 20 * log10(amp(R + 3 * 44100, 44100, 440.0 - fr) / amp(R + 3 * 44100, 44100, 440.0));
    s2 = 20 * log10(amp(R + 3 * 44100, 44100, 440.0 + fr) / amp(R + 3 * 44100, 44100, 440.0));
    s3 = 20 * log10(amp(R + 3 * 44100, 44100, 440.0 - 2 * fr) / amp(R + 3 * 44100, 44100, 440.0));
    printf("rotary: mic 90 degrees, right channel sidebands %.2f %.2f %.2f dB\n", s1, s2, s3);
    check("mic angle 90: the right channel's sidebands are setBfree's (-5.85 / -13.09 / -15.12 dB)",
          fabs(s1 + 5.85) < 0.5 && fabs(s2 + 13.09) < 0.5 && fabs(s3 + 15.12) < 0.5);
    mic_deg = 180;

    render(in, L, R, N, 0, 0);
    render(in, L2, R2, N, 0, 0);
    s1 = 20 * log10(amp(L + 3 * 44100, 44100, 440.0 - fr) / amp(L + 3 * 44100, 44100, 440.0) + 1e-12);
    printf("rotary: stopped sideband %.1f dB\n", s1);
    check("stopped rotors: no sidebands (below -55 dB: the window's leakage; rotating: -16 dB)", s1 < -55);
    check("stopped rotors: deterministic", !memcmp(L, L2, sizeof L) && !memcmp(R, R2, sizeof R));

    {
        rot_t *r = calloc(1, sizeof *r);
        int32_t x[ROT_CTL] = {0}, l[ROT_CTL], rr[ROT_CTL];
        int k;
        rot_init(r); r->k_ha = rot_k(161000u); r->k_hd = rot_k(321000u);   /* (setBfree's horn ramps: the reference's; the firmware's defaults are slower since 0.9.3) */
        rot_target(r, turn(400), turn(340));
        for (k = 0; k < 44100 * 45 / ROT_CTL; k++) rot_block(r, x, l, rr, ROT_CTL);
        check("speed ramp: the horn reaches 400 rpm exactly", r->inc_h == turn(400) && !r->acdc_h);
        check("speed ramp: the drum reaches 340 rpm exactly", r->inc_d == turn(340) && !r->acdc_d);
        rot_target(r, 0, 0);
        for (k = 0; k < 44100 * 30 / ROT_CTL; k++) rot_block(r, x, l, rr, ROT_CTL);
        check("speed ramp: both rotors come to rest exactly", r->inc_h == 0 && r->inc_d == 0);
        free(r);
    }

    for (i = 0; i < N; i++) {
        double t = i / 44100.0;
        in[i] = (float)(0.55 * sin(2 * M_PI * 55 * t) + 0.3 * sin(2 * M_PI * 110 * t) + 0.15 * sin(2 * M_PI * 1760 * t));
    }
    render(in, L, R, N, 400, 340);
    check("a loud chord with both rotors turning stays within 4x full scale", peak(L, N) < 4 && peak(R, N) < 4);
    {
        static const int hz[4] = {263, 668, 1182, 4486};
        int ok = 1;
        for (f = 0; f < 4; f++) {
            for (i = 0; i < N; i++) in[i] = 0.99f * (float)sin(2 * M_PI * hz[f] * i / 44100.0);
            render(in, L, R, N, 400, 340);
            ok &= peak(L, N) < 8 && peak(R, N) < 8;
            /* an overflow shows as a burst: the rms stays in setBfree's range (0.25 to 1.5 for these sines) */
            {
                double e = 0;
                for (i = 0; i < N; i++) e += (double)L[i] * L[i] + (double)R[i] * R[i];
                e = sqrt(e / N);
                ok &= e > 0.15 && e < 2.5;
            }
        }
        check("full-scale sines at the filters' resonances (263, 668, 1182, 4486 Hz) never overflow", ok);
    }
    memset(in, 0, sizeof in);
    render(in, L, R, N, 400, 340);
    check("silence in, silence out", peak(L, N) == 0 && peak(R, N) == 0);
    /* 5. the variants of the CLASSIC engine (the models CLASSIC 2 and 3): the same chord through each, the level in dB and the
     *    difference to CLASSIC. CLASSIC 2 (the nearest FIR and the drum's displacement every 4 samples) sounds the same
     *    (above 50 dB SNR); CLASSIC 3 (four horn taps) is another colour (about 17 dB SNR to CLASSIC), with the same level
     *    in dB (within 0.05 dB, its gain ROT_VG[3]). */
    {
        static float V0[N], V1[N], V2[N], V3[N], V4[N];
        double l0 = 0, l1 = 0, l2 = 0, l3 = 0, l4 = 0, s1v = 0, n1v = 0, n2v = 0, n3v = 0, n4v = 0;
        size_t k;
        for (i = 0; i < N; i++) in[i] = 0.25f * (float)(sin(2 * M_PI * 110 * i / 44100.0) + sin(2 * M_PI * 440 * i / 44100.0) + sin(2 * M_PI * 880 * i / 44100.0)) + 0.2f * (float)sin(2 * M_PI * 1760 * i / 44100.0);
        var_req = 0; vg_req = 1 << 24; render(in, L, R, N, 400, 340); memcpy(V0, L, sizeof V0);
        var_req = 3; vg_req = 1 << 24; render(in, L, R, N, 400, 340); memcpy(V1, L, sizeof V1);
        var_req = 4; vg_req = 16944154; render(in, L, R, N, 400, 340); memcpy(V2, L, sizeof V2);
        var_req = 9; vg_req = 1 << 24; render(in, L, R, N, 400, 340); memcpy(V3, L, sizeof V3);
        var_req = 16; vg_req = 1 << 24; render(in, L, R, N, 400, 340); memcpy(V4, L, sizeof V4);
        for (k = N / 2; k < N; k++) {
            l0 += (double)V0[k] * V0[k];
            l1 += (double)V1[k] * V1[k];
            l2 += (double)V2[k] * V2[k];
            l3 += (double)V3[k] * V3[k];
            l4 += (double)V4[k] * V4[k];
            s1v += (double)V0[k] * V0[k];
            n1v += ((double)V0[k] - V1[k]) * ((double)V0[k] - V1[k]);
            n2v += ((double)V0[k] - V2[k]) * ((double)V0[k] - V2[k]);
            n3v += ((double)V0[k] - V3[k]) * ((double)V0[k] - V3[k]);
            n4v += ((double)V0[k] - V4[k]) * ((double)V0[k] - V4[k]);
        }
        printf("rotary: variants: level CLASSIC 2 %.3f dB, CLASSIC 3 %.3f dB against CLASSIC; SNR CLASSIC 2 %.1f dB, CLASSIC 3 %.1f dB\n",
               10 * log10(l1 / l0), 10 * log10(l2 / l0), 10 * log10(s1v / n1v), 10 * log10(s1v / n2v));
        check("variants: CLASSIC 2 at CLASSIC's level (within 0.05 dB)", fabs(10 * log10(l1 / l0)) < 0.05);
        check("variants: CLASSIC 3 at CLASSIC's level (within 0.05 dB)", fabs(10 * log10(l2 / l0)) < 0.05);
        check("variants: CLASSIC 2 sounds as CLASSIC (above 50 dB SNR)", 10 * log10(s1v / n1v) > 50);
        check("variants: CLASSIC 3 is another colour (SNR to CLASSIC between 10 and 25 dB)", 10 * log10(s1v / n2v) > 10 && 10 * log10(s1v / n2v) < 25);
        check("variants: CLASSIC 4 at CLASSIC's level (within 0.05 dB) and above 40 dB SNR (256 angles)", fabs(10 * log10(l3 / l0)) < 0.05 && 10 * log10(s1v / n3v) > 40);
        check("variants: CLASSIC 5 at CLASSIC's level (within 0.05 dB) and above 30 dB SNR (horn geometry every 8 samples)", fabs(10 * log10(l4 / l0)) < 0.05 && 10 * log10(s1v / n4v) > 30);
    }
    return bad ? 1 : 0;
}
