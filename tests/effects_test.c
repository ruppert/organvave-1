/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The organ's effects section end to end on the host: the patch bytes (OP_ROT .. OP_DRV_FAT) through mix_block, with the
 * whole firmware (organ_app.c) and a chord held on the organ.
 * 1. the bytes: clamped by org_pmax; a patch of zeros leaves the mix as it was (no rotary, no drive).
 * 2. the rotary: FAST turns the horn (the left channel's level moves by more than 3 dB over a second, the channels differ);
 *    STOP holds still (the level varies under 1 dB); MODEL LIGHT runs (MIC 6 is printed: the mics' angle narrows the image).
 * 3. the fades: switching the rotary or the drive on or off with the chord held makes no step (largest sample-to-sample
 *    jump within 3 times the dry mix's).
 * 4. the drive: a single key at one drawbar gets its odd harmonics (the third more than 6 dB higher re the fundamental).
 * 5. silence: with everything on and no key, silence out.
 * 6. after the keys the output is quiet again (no limit cycle in the cabinet's filters), and MASTER 0 is silent. */
#include "host.h"
#include "../firmware/src/organ_app.c"
#include <stdio.h>

static int bad;
static void check(const char *what, int ok)
{
    printf("effects: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
#define FS_ 44100.0
#define MAXN (44100 * 40)
static int32_t oL[MAXN], oR[MAXN];
static uint32_t pos;

static void blocks(uint32_t n)           /* n samples (a multiple of CTL) from pos */
{
    int32_t o[2 * CTL];
    uint32_t i, t;
    for (t = 0; t < n; t += CTL) {
        mix_block(o, CTL);
        for (i = 0; i < CTL && pos < MAXN; i++, pos++) {
            oL[pos] = o[2u * i];
            oR[pos] = o[2u * i + 1u];
        }
    }
}
static void patch_set(const uint8_t *v) { org_set_patch(v); }
static uint8_t P[ORG_PSIZE];
static void boot(void)
{
    input_off(&trk[0], 48);              /* (a key of the last section) */
    input_off(&trk[0], 55);
    input_off(&trk[0], 60);
    host_tracks_init();
    memset(&fx, 0, sizeof fx);           /* (the last section's reverb tail) */
    rev_clear();
    org_init();
    rot_load_tables();
    rot_init(&rot_st);
    lt_init(&lt_st, &rot_st);
    dr_init(&dr_st);
    eq_init(&eq_st);
    eq_w = 0; eq_idle = 0; eq_on = 0;
    memset(&rot_mix, 0, sizeof rot_mix);
    rot_w = dr_w = 0;
    rot_idle = 0;
    rot_on = dr_on = 0;
    fxo_seen = 0xFF;
    pos = 0;
    host_preset(&trk[0], ENGI_ORGAN, 0);
    trk[0].p[P_AMODE] = 0;
    trk[0].p[P_DIST] = trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;     /* (no reverb tail: the effects alone) */
    memset(P, 0, sizeof P);
    P[OP_UB + 2] = 8;                    /* 8' */
    P[OP_UB + 3] = 6;                    /* 4' */
    P[OP_ROT_BAL] = 4;
    P[OP_DRV_GAIN] = 6;
    patch_set(P);
}
static double db(double x) { return 20 * log10(x > 1e-9 ? x : 1e-9); }
static double rms_win(const int32_t *y, uint32_t a, uint32_t n)
{
    double e = 0;
    uint32_t i;
    for (i = 0; i < n; i++) e += (double)y[a + i] * y[a + i];
    return sqrt(e / n);
}
/* the level of the signal in 50 ms windows over [a, b): the largest minus the smallest, dB */
static double swing(const int32_t *y, uint32_t a, uint32_t b)
{
    double lo = 1e30, hi = 0;
    uint32_t w = 2205, t;
    for (t = a; t + w <= b; t += w) {
        double r = rms_win(y, t, w);
        if (r < lo) lo = r;
        if (r > hi) hi = r;
    }
    return db(hi) - db(lo);
}
static int32_t maxjump(const int32_t *y, uint32_t a, uint32_t b)
{
    int32_t m = 0;
    uint32_t i;
    for (i = a + 1; i < b; i++) {
        int32_t d = y[i] - y[i - 1];
        if (d < 0) d = -d;
        if (d > m) m = d;
    }
    return m;
}
static double tone(const int32_t *y, uint32_t a, uint32_t n, double f)
{
    double re = 0, im = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / n), ph = 2 * M_PI * f * (a + i) / FS_;
        re += w * y[a + i] * cos(ph);
        im -= w * y[a + i] * sin(ph);
    }
    return 2 * sqrt(re * re + im * im) / (n * 0.5);
}
static void chord_on(void)
{
    static const uint8_t K[3] = {48, 55, 60};
    uint32_t i;
    for (i = 0; i < 3; i++) input_on(&trk[0], K[i], 100);
}
static void chord_off(void)
{
    static const uint8_t K[3] = {48, 55, 60};
    uint32_t i;
    for (i = 0; i < 3; i++) input_off(&trk[0], K[i]);
}

int main(void)
{
    uint32_t i;
    double d0, s;
    /* 1. the bytes */
    boot();
    org_set(OP_ROT_SPEED, 9);
    org_set(OP_ROT_MIC, 9);
    org_set(OP_DRV_GAIN, 99);
    org_set(OP_DRV_FAT, 99);
    org_set(OP_ROT_BAL, 99);
    org_set(OP_ROT, 5);
    check("the bytes are clamped (SPEED 2, MIC 6, GAIN 12, FAT 8, BAL 8, ROT 1)",
          org_patch[OP_ROT_SPEED] == 2 && org_patch[OP_ROT_MIC] == 6 && org_patch[OP_DRV_GAIN] == 12 &&
              org_patch[OP_DRV_FAT] == 8 && org_patch[OP_ROT_BAL] == 8 && org_patch[OP_ROT] == 1);
    /* a patch of zeros in the effects bytes leaves the mix dry */
    boot();
    chord_on();
    blocks(44100);
    check("with the effects bytes at 0 nothing is in the mix but the organ (rot_on, dr_on, both weights 0)",
          !rot_on && !dr_on && !rot_w && !dr_w && rms_win(oL, 22050, 22050) > 500);

    /* 2. the rotary */
    boot();
    chord_on();
    blocks(44100);
    P[OP_ROT] = 1; P[OP_ROT_SPEED] = 2;
    patch_set(P);
    blocks(44100 * 5);
    s = swing(oL, 44100 * 4, 44100 * 6);
    {
        double e = 0, l = 0;
        for (i = 44100 * 4; i < 44100 * 6; i++) { double d = (double)oL[i] - oR[i]; e += d * d; l += (double)oL[i] * oL[i]; }
        printf("effects: rotary FAST: left level swing %.1f dB, L-R / L %.1f dB\n", s, db(sqrt(e / l)));
        check("the rotary FAST: the level moves (more than 3 dB over 2 s in 50 ms windows)", s > 3.0);
        check("the rotary FAST: the channels differ (L - R above -20 dB re L)", db(sqrt(e / l)) > -20.0);
    }
    P[OP_ROT_SPEED] = 0;
    patch_set(P);
    blocks(44100 * 16);                   /* the drum coasts: setBfree has no brake, the speed decays e^-t/1.37 s to 0.05 rpm: about 12 s */
    s = swing(oL, pos - 44100, pos);
    printf("effects: rotary STOP: left level swing %.2f dB\n", s);
    check("the rotary STOP: the level holds (under 1 dB in the last second)", s < 1.0);
    P[OP_ROT_MIC] = 6; P[OP_ROT_SPEED] = 2;
    patch_set(P);
    blocks(44100 * 3);
    {
        double e = 0, l = 0;
        for (i = pos - 44100; i < pos; i++) { double d = (double)oL[i] - oR[i]; e += d * d; l += (double)oL[i] * oL[i]; }
        d0 = db(sqrt(e / l));
        printf("effects: rotary MIC 6 (mono): L-R / L %.1f dB\n", d0);
    }
    P[OP_ROT_MIC] = 0; P[OP_ROT_MODEL] = 1;
    patch_set(P);
    blocks(44100 * 2);
    check("the rotary LIGHT runs (the model in use is 1, the output is alive and bounded)",
          rot_mix.model == 1 && rms_win(oL, pos - 22050, 22050) > 300 && maxjump(oL, pos - 22050, pos) < 40000);

    /* 3. the fades */
    boot();
    chord_on();
    blocks(44100);
    d0 = maxjump(oL, 22050, 44100);
    P[OP_ROT] = 1; P[OP_ROT_SPEED] = 2;
    patch_set(P);
    blocks(44100 / 2);
    printf("effects: largest step dry %.0f, rotary coming in %.0f\n", d0, (double)maxjump(oL, 44100, pos));
    check("the rotary coming in: no step (largest jump within 3 times the dry mix's)", maxjump(oL, 44100, pos) < 3.0 * d0 + 50);
    blocks(44100 * 2);
    {
        uint32_t a = pos;
        double d1 = maxjump(oL, a - 22050, a);
        P[OP_ROT] = 0;
        patch_set(P);
        blocks(44100 / 2);
        printf("effects: largest step with the rotary %.0f, going out %.0f\n", d1, (double)maxjump(oL, a, pos));
        check("the rotary going out: no step (within 3 times the largest jump with it on)", maxjump(oL, a, pos) < 3.0 * d1 + 50);
    }
    boot();
    chord_on();
    blocks(44100);
    d0 = maxjump(oL, 22050, 44100);
    P[OP_DRV] = 1;
    patch_set(P);
    blocks(44100 / 2);
    {
        double dj = maxjump(oL, 44100, pos);
        printf("effects: largest step dry %.0f, drive coming in %.0f\n", d0, dj);
        check("the drive coming in: no step (within 4 times the dry mix's; it adds harmonics, so more is allowed)", dj < 4.0 * d0 + 50);
    }

    /* 4. the drive: a single key, one drawbar: the third harmonic */
    boot();
    memset(P, 0, sizeof P);
    P[OP_UB + 2] = 8;
    P[OP_DRV_GAIN] = 6;
    patch_set(P);
    input_on(&trk[0], 48, 100);          /* C3, 130.81 Hz at the equal temperament; the 60 Hz gear is near it */
    blocks(44100 * 2);
    {
        double f0 = 130.81, a1, a3, b1, b3;
        uint32_t a = 44100;
        a1 = tone(oL, a, 44100, f0);
        a3 = tone(oL, a, 44100, 3 * f0);
        P[OP_DRV] = 1;
        patch_set(P);
        blocks(44100 * 2);
        b1 = tone(oL, pos - 44100, 44100, f0);
        b3 = tone(oL, pos - 44100, 44100, 3 * f0);
        printf("effects: dry H3 %.1f dB re H1, driven H3 %.1f dB re H1\n", db(a3 / a1), db(b3 / b1));
        printf("effects: level dry %.0f, driven %.0f (rms)\n", rms_win(oL, 44100, 44100), rms_win(oL, pos - 44100, 44100));
        check("the drive: the third harmonic rises by more than 6 dB re the fundamental", db(b3 / b1) - db(a3 / a1) > 6.0);
    }

    /* 5. silence */
    {
        static const struct { uint8_t rot, model, drv; const char *n; } C[4] = {
            {0, 0, 1, "drive"}, {1, 0, 0, "rotary CLASSIC"}, {1, 1, 0, "rotary LIGHT"}, {1, 1, 1, "all"}};
        uint32_t c;
        int32_t worst = 0;
        for (c = 0; c < 4; c++) {
            int32_t pk = 0;
            boot();
            P[OP_ROT] = C[c].rot; P[OP_ROT_SPEED] = 2; P[OP_DRV] = C[c].drv; P[OP_ROT_MODEL] = C[c].model;
            patch_set(P);
            blocks(44100);
            for (i = 22050; i < pos; i++) {
                int32_t a = oL[i] < 0 ? -oL[i] : oL[i], b = oR[i] < 0 ? -oR[i] : oR[i];
                if (a > pk) pk = a;
                if (b > pk) pk = b;
            }
            printf("effects: no key, %s: peak %d\n", C[c].n, pk);
            if (pk > worst) worst = pk;
        }
        check("every effect on, no key: silence out (|y| < 4)", worst < 4);
    }
    /* 6. after the keys: the cabinet and the drive go quiet (no limit cycle: at most 1 LSB), and MASTER 0 is silent */
    {
        static const struct { uint8_t model, drv; const char *n; } C[3] = {{0, 0, "CLASSIC"}, {1, 0, "LIGHT"}, {0, 1, "CLASSIC and the drive"}};
        uint32_t c;
        for (c = 0; c < 3; c++) {
            int32_t pk = 0;
            boot();
            P[OP_ROT] = 1; P[OP_ROT_SPEED] = 2; P[OP_ROT_MODEL] = C[c].model; P[OP_DRV] = C[c].drv;
            patch_set(P);
            chord_on();
            blocks(44100);
            chord_off();
            blocks(44100 * 5);
            for (i = pos - 44100; i < pos; i++) {
                int32_t a = oL[i] < 0 ? -oL[i] : oL[i], b = oR[i] < 0 ? -oR[i] : oR[i];
                if (a > pk) pk = a;
                if (b > pk) pk = b;
            }
            printf("effects: %s, five seconds after the keys: peak %d\n", C[c].n, pk);
            check("  .. the output is quiet again (peak at most 1)", pk <= 1);
        }
        boot();
        P[OP_ROT] = 1; P[OP_ROT_SPEED] = 2; P[OP_DRV] = 1;
        patch_set(P);
        song.master_q12 = 0;
        chord_on();
        blocks(44100);
        {
            int32_t pk = 0;
            for (i = 0; i < pos; i++) {
                int32_t a = oL[i] < 0 ? -oL[i] : oL[i], b = oR[i] < 0 ? -oR[i] : oR[i];
                if (a > pk) pk = a;
                if (b > pk) pk = b;
            }
            check("MASTER 0 with the rotary and the drive on, a chord held: silent", pk == 0);
        }
        song.master_q12 = 4096;
    }
    /* 7. the equaliser (0.5.0): after MASTER, before the limiter */
    {
        uint32_t i, a;
        double lo0, lo1, hi0, hi1;
        boot();
        memset(P, 0, sizeof P);
        P[OP_EQ] = 5; P[OP_EQ_LOW] = 30; P[OP_EQ_MID] = 99; P[OP_EQ_FREQ] = 99; P[OP_EQ_HIGH] = 25;
        org_set_patch(P);
        check("the EQ bytes are clamped (ON 1, gains 24, FREQ 11)", org_patch[OP_EQ] == 1 && org_patch[OP_EQ_LOW] == 24 &&
              org_patch[OP_EQ_MID] == 24 && org_patch[OP_EQ_FREQ] == 11 && org_patch[OP_EQ_HIGH] == 24);
        /* flat: on with every byte at 0 is the dry mix within 3 */
        boot();
        P[OP_UB + 2] = 8; P[OP_UB + 3] = 6;
        patch_set(P);
        chord_on();
        blocks(44100);
        a = pos;
        blocks(22050);
        {
            int32_t ref[22050];
            int32_t worst = 0;
            memcpy(ref, oL + a, sizeof ref);
            boot();
            P[OP_EQ] = 1;
            patch_set(P);
            chord_on();
            blocks(44100);
            a = pos;
            blocks(22050);
            for (i = 0; i < 22050u; i++) {
                int32_t d = oL[a + i] - ref[i];
                d = d < 0 ? -d : d;
                worst = d > worst ? d : worst;
            }
            printf("effects: EQ on and flat against off: the largest difference %d\n", worst);
            check("the EQ on and flat: the output is the dry mix within 3", worst <= 3);
        }
        chord_off();
        /* LOW +12: a key at C2 (65.4 Hz, the 8' drawbar) gets louder by more than 6 dB */
        boot();
        P[OP_UB + 2] = 8;
        patch_set(P);
        input_on(&trk[0], 36, 100);
        blocks(44100);
        lo0 = tone(oL, 22050, 22050, 65.41);
        P[OP_EQ] = 1; P[OP_EQ_LOW] = 12;
        patch_set(P);
        blocks(44100 * 2);
        lo1 = tone(oL, pos - 22050, 22050, 65.41);
        input_off(&trk[0], 36);
        printf("effects: the EQ's LOW +12: the 65 Hz tone %.1f dB\n", db(lo1 / lo0));
        check("LOW +12 dB: the 65 Hz tone rises by 6 .. 14 dB", db(lo1 / lo0) > 6.0 && db(lo1 / lo0) < 14.0);
        /* HIGH -12: a key at C6 with the 1' drawbar (4186 Hz) goes down by more than 6 dB */
        boot();
        P[OP_UB + 8] = 8;
        patch_set(P);
        input_on(&trk[0], 84, 100);
        blocks(44100);
        hi0 = tone(oL, 22050, 22050, 4186.0);
        P[OP_EQ] = 1; P[OP_EQ_HIGH] = 13;
        patch_set(P);
        blocks(44100 * 2);
        hi1 = tone(oL, pos - 22050, 22050, 4186.0);
        input_off(&trk[0], 84);
        printf("effects: the EQ's HIGH -12: the 4.2 kHz tone %.1f dB\n", db(hi1 / hi0));
        check("HIGH -12 dB: the 4.2 kHz tone falls by 6 .. 14 dB", db(hi1 / hi0) < -6.0 && db(hi1 / hi0) > -14.0);
        /* the fade: the EQ coming in with a chord held makes no step */
        boot();
        P[OP_UB + 2] = 8; P[OP_UB + 3] = 6;
        patch_set(P);
        chord_on();
        blocks(44100);
        {
            int32_t d0 = maxjump(oL, 22050, pos), dj;
            a = pos;
            P[OP_EQ] = 1; P[OP_EQ_LOW] = 12; P[OP_EQ_MID] = 12; P[OP_EQ_HIGH] = 12;
            patch_set(P);
            blocks(22050);
            dj = maxjump(oL, a, pos);
            printf("effects: the EQ coming in: the largest jump %d against %d dry\n", dj, d0);
            check("the EQ coming in with a chord held: no step (within 4 times the dry mix's)", dj < 4 * d0 + 50);
        }
        chord_off();
        /* silence, and MASTER 0 */
        boot();
        P[OP_EQ] = 1; P[OP_EQ_LOW] = 12; P[OP_EQ_MID] = 12; P[OP_EQ_HIGH] = 12; P[OP_ROT] = 1; P[OP_DRV] = 1;
        patch_set(P);
        chord_on();
        blocks(22050);
        chord_off();
        blocks(44100 * 8);
        {
            int32_t pk = 0;
            for (i = pos - 44100; i < pos; i++) {
                int32_t x = oL[i] < 0 ? -oL[i] : oL[i], y = oR[i] < 0 ? -oR[i] : oR[i];
                pk = x > pk ? x : pk;
                pk = y > pk ? y : pk;
            }
            printf("effects: everything on, eight seconds after the keys: peak %d\n", pk);
            check("the EQ on with the rotary and the drive, long after the keys: silent (peak at most 1)", pk <= 1);
        }
        boot();
        P[OP_EQ] = 1; P[OP_EQ_LOW] = 12; P[OP_EQ_MID] = 12; P[OP_EQ_HIGH] = 12;
        patch_set(P);
        song.master_q12 = 0;
        chord_on();
        blocks(44100);
        {
            int32_t pk = 0;
            for (i = 0; i < pos; i++) {
                int32_t x = oL[i] < 0 ? -oL[i] : oL[i];
                pk = x > pk ? x : pk;
            }
            check("MASTER 0 with the EQ on, a chord held: silent", pk == 0);
        }
        song.master_q12 = 4096;
    }
    /* 8. CPU overload: the CLASSIC rotary gives way to LIGHT, then the drive goes off (fx_degrade, which audio.c's shed_voice calls
     * when it has no voice to take; tests/audio_test.c checks that call) */
    {
        boot();
        memset(P, 0, sizeof P);
        P[OP_UB + 2] = 8; P[OP_ROT] = 1; P[OP_ROT_MODEL] = 0; P[OP_ROT_SPEED] = 2; P[OP_ROT_BAL] = 4; P[OP_DRV] = 1; P[OP_DRV_GAIN] = 6;
        patch_set(P);
        chord_on();
        blocks(8820);
        check("overload: before, CLASSIC and the drive run", rot_mix.model == 0 && rot_on && dr_on);
        check("  .. the first call lowers something", fx_degrade() == 1);
        blocks(8820);
        check("  .. the first shed: LIGHT plays, the patch still says CLASSIC", rot_mix.model == 1 && org_patch[OP_ROT_MODEL] == 0 && dr_on == 1 && cpu_degrade == 1);
        check("  .. the second lowers something", fx_degrade() == 1);
        blocks(8820);
        check("  .. the second: the drive is off", dr_on == 0 && org_patch[OP_DRV] == 1 && cpu_degrade == 2);
        check("  .. a third finds nothing more to lower", fx_degrade() == 0 && cpu_degrade == 2);
        {
            int32_t pk = 0;
            uint32_t i;
            for (i = pos - 4410; i < pos; i++)
                pk = oL[i] > pk ? oL[i] : -oL[i] > pk ? -oL[i] : pk;
            check("  .. and the organ still sounds", pk > 500);
        }
        patch_set(P);
        blocks(8820);
        check("  .. a new patch load brings the full effects back", cpu_degrade == 0 && rot_mix.model == 0 && dr_on == 1);
        chord_off();
    }
    /* 9. every key of the three manuals at once, everything on: bounded and alive for ten seconds, then quiet */
    {
        uint32_t k, i, n = 0;
        int32_t pk = 0, pk_end = 0;
        double e = 0;
        boot();
        memset(P, 0, sizeof P);
        for (i = 0; i < 27u; i++)
            P[i] = 8;                                    /* every drawbar of every manual at 8 */
        P[OP_PERC] = 1; P[OP_VIB] = 5; P[OP_ROT] = 1; P[OP_ROT_SPEED] = 2; P[OP_ROT_BAL] = 4; P[OP_DRV] = 1; P[OP_DRV_GAIN] = 12;
        P[OP_EQ] = 1; P[OP_EQ_LOW] = 12; P[OP_EQ_MID] = 12; P[OP_EQ_HIGH] = 12;
        patch_set(P);
        trk[0].p[P_REV] = 127; trk[0].p[P_DLY] = 127;
        for (k = 0; k < ORG_NKEY; k++) {                 /* (the key queue holds 64 messages a fragment: a block between each 32) */
            org_key(k, 1);
            if (k % 32u == 31u)
                blocks(CTL);
        }
        blocks(44100 * 10);
        for (i = 44100; i < pos; i++) {
            int32_t a = oL[i] < 0 ? -oL[i] : oL[i], b = oR[i] < 0 ? -oR[i] : oR[i];
            pk = a > pk ? a : pk;
            pk = b > pk ? b : pk;
            e += (double)oL[i] * oL[i];
            n++;
        }
        printf("effects: all %u keys down, everything on, 10 s: peak %d, rms %.0f\n", (unsigned)org.ndown, pk, sqrt(e / n));
        check("all keys of the three manuals with everything on: bounded (peak under 2^17) and alive (rms above 1000)", pk < 131072 && sqrt(e / n) > 1000.0);
        for (k = 0; k < ORG_NKEY; k++) {
            org_key(k, 0);
            if (k % 32u == 31u)
                blocks(CTL);
        }
        trk[0].p[P_REV] = 0; trk[0].p[P_DLY] = 0;
        blocks(44100 * 14);
        for (i = pos - 44100; i < pos; i++) {
            int32_t a = oL[i] < 0 ? -oL[i] : oL[i], b = oR[i] < 0 ? -oR[i] : oR[i];
            pk_end = a > pk_end ? a : pk_end;
            pk_end = b > pk_end ? b : pk_end;
        }
        printf("effects: 14 s after letting go: peak %d\n", pk_end);
        check("  .. and after letting go it falls quiet (peak at most 2 after 14 s)", pk_end <= 2);
    }
    chord_off();
    printf(bad ? "effects: FAILED\n" : "effects: all ok\n");
    return bad;
}
