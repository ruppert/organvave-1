/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The organ's equaliser (firmware/src/eng_eq.c) against its design (tools/gen_eq_tables.py): sines through eq_run1 and the gain
 * measured at the end. 1. the byte codings (zero is flat). 2. flat is flat (every frequency within 0.1 dB). 3. LOW: +12 dB
 * at 40 Hz within 2 dB of 12, none at 2 kHz (within 0.5), -12 likewise; HIGH the same at 14 kHz and 100 Hz; MID: the set gain
 * at its own frequency within 0.5 dB for each of the twelve, little at two octaves away; the steps are 1 dB (within 0.15).
 * 4. bounded: loud noise through everything at +12 stays inside the clamp, and no output at the end of a silence (the
 * states settle). 5. both channels alike. */
#include "host.h"
#include "../firmware/src/organ_app.c"
#include <stdio.h>

static int bad;
static void check(const char *what, int ok)
{
    printf("eq: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
static eq_t e;
static void eq_run1(eq_t *q, int32_t *l, int32_t *r) { eq_block(q, l, r, 1); }       /* a sample at a time (the firmware runs blocks) */
/* the gain in dB of a sine of f Hz (amplitude 8000) after a second */
static double gain(double f)
{
    double sa = 0, sy = 0;
    int32_t n, N = 44100;
    eq_clear(&e);
    for (n = 0; n < N; n++) {
        int32_t x = (int32_t)lrint(8000.0 * sin(2 * M_PI * f * n / 44100.0)), l = x, r = x;
        eq_run1(&e, &l, &r);
        if (n >= N / 2) {
            sa += (double)x * x;
            sy += (double)l * l;
        }
    }
    return 10 * log10(sy / sa);
}
static void set_db(int32_t lo, int32_t mid, uint32_t fidx, int32_t hi)
{
    eq_set(&e, eq_byte(lo), eq_byte(mid), eq_fbyte(fidx), eq_byte(hi));
}

int main(void)
{
    uint32_t i;
    int ok;
    double g, f;
    ok = 1;
    for (i = 0; i < 25u; i++) {
        int32_t db = (int32_t)i - 12;
        ok &= eq_db(eq_byte(db)) == db && eq_byte(db) <= 24u;
    }
    for (i = 0; i < 12u; i++)
        ok &= eq_fidx(eq_fbyte(i)) == i;
    check("the codings: zero is flat (0 dB, 900 Hz), 13 is -12, 24 is -1; the frequency index round-trips",
          ok && eq_db(0) == 0 && eq_db(13) == -12 && eq_db(24) == -1 && eq_fidx(0) == 5u && EQ_FREQ_HZ[eq_fidx(0)] == 900u);

    eq_init(&e);
    ok = 1;
    for (f = 30; f < 18000; f *= 1.6)
        ok &= fabs(gain(f)) < 0.1;
    check("flat: 0 dB within 0.1 at every frequency from 30 Hz to 18 kHz", ok);

    set_db(12, 0, 5, 0);
    g = gain(40);
    printf("eq: LOW +12 at 40 Hz %.2f dB, at 2 kHz %.2f dB\n", g, gain(2000));
    check("LOW +12: about +12 dB at 40 Hz (within 2), flat at 2 kHz (within 0.5)", fabs(g - 12) < 2 && fabs(gain(2000)) < 0.5);
    set_db(-12, 0, 5, 0);
    g = gain(40);
    check("LOW -12: about -12 dB at 40 Hz (within 2), flat at 2 kHz (within 0.5)", fabs(g + 12) < 2 && fabs(gain(2000)) < 0.5);
    set_db(0, 0, 5, 12);
    g = gain(14000);
    printf("eq: HIGH +12 at 14 kHz %.2f dB, at 100 Hz %.2f dB\n", g, gain(100));
    check("HIGH +12: about +12 dB at 14 kHz (within 2), flat at 100 Hz (within 0.5)", fabs(g - 12) < 2 && fabs(gain(100)) < 0.5);
    set_db(0, 0, 5, -12);
    g = gain(14000);
    check("HIGH -12: about -12 dB at 14 kHz (within 2), flat at 100 Hz (within 0.5)", fabs(g + 12) < 2 && fabs(gain(100)) < 0.5);

    ok = 1;
    for (i = 0; i < 12u; i++) {
        double hz = EQ_FREQ_HZ[i], up, dn, far;
        set_db(0, 9, i, 0);
        up = gain(hz);
        far = gain(hz * 4 < 16000 ? hz * 4 : hz / 4);
        set_db(0, -9, i, 0);
        dn = gain(hz);
        if (fabs(up - 9) > 0.5 || fabs(dn + 9) > 0.5 || fabs(far) > 1.5)
            printf("eq: MID %g Hz: +9 gives %.2f, -9 gives %.2f, two octaves away %.2f\n", hz, up, dn, far);
        ok &= fabs(up - 9) <= 0.5 && fabs(dn + 9) <= 0.5 && fabs(far) <= 1.5;
    }
    check("MID: the set gain (+9 and -9 dB) at each of the twelve frequencies within 0.5, little two octaves away (1.5)", ok);

    ok = 1;
    for (i = 1; i <= 12u; i++) {
        double a, b;
        set_db(0, (int32_t)i - 1, 5, 0);
        a = gain(900);
        set_db(0, (int32_t)i, 5, 0);
        b = gain(900);
        ok &= fabs((b - a) - 1.0) < 0.15;
    }
    check("the MID steps are 1 dB (within 0.15), 0 to +12", ok);

    {   /* bounded, settles */
        uint32_t seed = 12345, n;
        int32_t peak = 0, l, r, tail = 0, tmin = 1 << 30, tmax = -(1 << 30);
        set_db(12, 12, 3, 12);
        eq_clear(&e);
        for (n = 0; n < 88200u; n++) {
            seed = seed * 1664525u + 1013904223u;
            l = (int32_t)(seed >> 16) - 32768;
            r = -l;
            eq_run1(&e, &l, &r);
            peak = l > peak ? l : -l > peak ? -l : peak;
        }
        for (n = 0; n < 44100u * 5u; n++) {
            l = r = 0;
            eq_run1(&e, &l, &r);
            if (n > 44100u * 4u) {
                tail = l > tail ? l : -l > tail ? -l : tail;
                tmin = l < tmin ? l : tmin;
                tmax = l > tmax ? l : tmax;
            }
        }
        printf("eq: loud noise at +12 / +12 / +12: peak %d; after 4 s of silence the output is %d (spread %d)\n", peak, tail, tmax - tmin);
        check("bounded: loud noise at +12 stays inside the clamp (a sample of 300,000 in, at most about twice out)", peak < 300000*2);
        /* a filter this low rounds to a small constant or cycle (tens of 32768, at most; the DC blocker in master_out takes the
         * constant) and fx.c empties the states after 1.1 s of silence anyway */
        check("  .. and after seconds of silence the output is small: under 100, a spread under 64", tail < 100 && tmax - tmin < 64);
    }
    {   /* both channels alike */
        int32_t l = 5000, r = 5000, k;
        set_db(5, -4, 7, 3);
        eq_clear(&e);
        for (k = 0; k < 200; k++) {
            l = r = (k & 1) ? 9000 : -3000;
            eq_run1(&e, &l, &r);
        }
        check("both channels run the same filter", l == r);
    }
    printf("%s\n", bad ? "EQ TEST FAILED" : "eq test passed");
    return bad != 0;
}
