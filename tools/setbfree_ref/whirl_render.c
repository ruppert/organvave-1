/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The rotary's floating-point reference: setBfree's own whirl.c (b_whirl/whirl.c, GPL-2.0-or-later), at the default
 * geometry and the default horn / drum filters, run on a mono input file.
 *
 *   whirl_render IN.f32 OUT.f32 HORN_RPM DRUM_RPM [ACC_HORN ACC_DRUM]
 *
 * IN.f32: mono float32 at 44.1 kHz (samples in setBfree's units, 1.0 = one wheel at full level). OUT.f32: stereo,
 * interleaved L R. The rotors start at rest and their targets are HORN_RPM / DRUM_RPM (0 = stopped); the speeds
 * follow setBfree's exponential law, blocks of 32 samples (the firmware's CTL). ACC_* overrides the acceleration
 * time constants in seconds (default: setBfree's 0.161 s horn, 4.127 s drum; the deceleration is left at its default).
 * Built with tools/setbfree_ref/build.sh (the same stubs as whirl_tables.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "whirl.c"

#define BLK 32

int getConfigParameter_d (const char *par, ConfigContext *cfg, double *dp) { (void)par; (void)cfg; (void)dp; return 0; }
int getConfigParameter_i (const char *par, ConfigContext *cfg, int *dp) { (void)par; (void)cfg; (void)dp; return 0; }
int getConfigParameter_f (const char *par, ConfigContext *cfg, float *dp) { (void)par; (void)cfg; (void)dp; return 0; }
int getConfigParameter_ir (const char *par, ConfigContext *cfg, int *ip, int lo, int hi) { (void)par; (void)cfg; (void)ip; (void)lo; (void)hi; return 0; }
int getConfigParameter_dr (const char *par, ConfigContext *cfg, double *dp, double lo, double hi) { (void)par; (void)cfg; (void)dp; (void)lo; (void)hi; return 0; }
void useMIDIControlFunction (void *m, const char *cfname, void (*f) (void *, unsigned char), void *d) { (void)m; (void)cfname; (void)f; (void)d; }
void notifyControlChangeByName (void *mcfg, const char *cfname, unsigned char val) { (void)mcfg; (void)cfname; (void)val; }

int main(int argc, char **argv)
{
    struct b_whirl *w;
    FILE *fi, *fo;
    float *in, *lo, *ro;
    size_t n, i, done;
    double hrpm, drpm;

    if (argc != 5 && argc != 7) {
        fprintf(stderr, "usage: whirl_render IN.f32 OUT.f32 HORN_RPM DRUM_RPM [ACC_HORN ACC_DRUM]\n");
        return 2;
    }
    fi = fopen(argv[1], "rb");
    if (!fi) {
        perror(argv[1]);
        return 1;
    }
    fseek(fi, 0, SEEK_END);
    n = (size_t)ftell(fi) / sizeof(float);
    fseek(fi, 0, SEEK_SET);
    in = malloc(n * sizeof(float));
    lo = malloc(n * sizeof(float));
    ro = malloc(n * sizeof(float));
    if (fread(in, sizeof(float), n, fi) != n) {
        fprintf(stderr, "short read\n");
        return 1;
    }
    fclose(fi);

    hrpm = atof(argv[3]);
    drpm = atof(argv[4]);
    w = allocWhirl();
    initValues(w);
    w->SampleRateD = 44100.0;
    if (argc == 7) {
        w->hornAcc = atof(argv[5]);
        w->drumAcc = atof(argv[6]);
    }
    initialize(w);
    /* whirlProc2 ramps the speed only while hornAcDc / drumAcDc is set (revControl sets them as it sets the
     * targets): +1 accelerating (hornAcc), -1 decelerating (hornDec); it clears itself on reaching the target. */
    w->hornTarget = hrpm / (60.0 * w->SampleRateD);
    w->drumTarget = drpm / (60.0 * w->SampleRateD);
    w->hornAcDc = (w->hornTarget > w->hornIncr) ? 1 : -1;
    w->drumAcDc = (w->drumTarget > w->drumIncr) ? 1 : -1;

    for (done = 0; done < n; done += BLK) {
        size_t k = (n - done < BLK) ? n - done : BLK;
        whirlProc2(w, in + done, lo + done, ro + done, NULL, NULL, NULL, NULL, k);
    }

    fo = fopen(argv[2], "wb");
    if (!fo) {
        perror(argv[2]);
        return 1;
    }
    for (i = 0; i < n; i++) {
        float s[2] = {lo[i], ro[i]};
        fwrite(s, sizeof(float), 2, fo);
    }
    fclose(fo);
    free(in);
    free(lo);
    free(ro);
    return 0;
}
