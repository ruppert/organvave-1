/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The rotary's angle tables, from setBfree's own code (b_whirl/whirl.c, GPL-2.0-or-later), at the default geometry
 * (initValues: horn 19.2 cm, drum 22 cm, mic 42 cm, 44.1 kHz). whirl.c is included as it is: the tables are the
 * ones setBfree builds (computeOffsets, initTables), not a re-derivation.
 *
 *   sh tools/setbfree_ref/build.sh OUT        (builds OUT/whirl_tables with the reference)
 *   OUT/whirl_tables FILE.bin                 writes the sampled tables (see below) and the tap phases
 *
 * The tables have WHIRL_DISPLC_SIZE (16384) angles in setBfree. The firmware keeps every 16th (1024 angles, the
 * step is 16 / 16384 of a turn); the tap phases are not on that grid, so the firmware interpolates both the
 * displacement and the FIR coefficients between the sampled angles. The file:
 *   magic "WHL1", then 1024 records of: int32 horn forward, horn backward, drum forward, drum backward (Q8 samples),
 *   and 5 + 5 int16 (the horn's forward and backward angle FIR, Q14); then the 12 tap phases (int32: horn 0..5, drum
 * 6..11) and the 12 spacings (float32: the same order, samples); then 25 float32 biquads (a1 a2 b0 b1 b2 each):
 * default horn A, default horn B, drum (high-shelf), sharp horn A, sharp horn B. Little endian. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "whirl.c"

/* the configuration and MIDI hooks whirl.c calls: the defaults stand (no config file, no MIDI map here) */
int getConfigParameter_d (const char *par, ConfigContext *cfg, double *dp) { (void)par; (void)cfg; (void)dp; return 0; }
int getConfigParameter_i (const char *par, ConfigContext *cfg, int *dp) { (void)par; (void)cfg; (void)dp; return 0; }
int getConfigParameter_f (const char *par, ConfigContext *cfg, float *dp) { (void)par; (void)cfg; (void)dp; return 0; }
int getConfigParameter_ir (const char *par, ConfigContext *cfg, int *ip, int lo, int hi) { (void)par; (void)cfg; (void)ip; (void)lo; (void)hi; return 0; }
int getConfigParameter_dr (const char *par, ConfigContext *cfg, double *dp, double lo, double hi) { (void)par; (void)cfg; (void)dp; (void)lo; (void)hi; return 0; }
void useMIDIControlFunction (void *m, const char *cfname, void (*f) (void *, unsigned char), void *d) { (void)m; (void)cfname; (void)f; (void)d; }
void notifyControlChangeByName (void *mcfg, const char *cfname, unsigned char val) { (void)mcfg; (void)cfname; (void)val; }

#define SAMPLED 1024
#define STEP (WHIRL_DISPLC_SIZE / SAMPLED)

static int32_t q8(double v) { return (int32_t)lround(v * 256.0); }
static int16_t q14(double v)
{
    double r = lround(v * 16384.0);
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    return (int16_t)r;
}

int main(int argc, char **argv)
{
    struct b_whirl *w;
    unsigned int i, j;
    FILE *f;

    if (argc != 2) {
        fprintf(stderr, "usage: whirl_tables FILE.bin\n");
        return 2;
    }
    w = allocWhirl();
    initValues(w);
    w->SampleRateD = 44100.0;
    computeOffsets(w);
    initTables(w);

    f = fopen(argv[1], "wb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fwrite("WHL1", 1, 4, f);
    for (i = 0; i < SAMPLED; i++) {
        unsigned int a = i * STEP;
        int32_t v[4];
        int16_t c[10];
        v[0] = q8(w->hnFwdDispl[a]);
        v[1] = q8(w->hnBwdDispl[a]);
        v[2] = q8(w->drFwdDispl[a]);
        v[3] = q8(w->drBwdDispl[a]);
        fwrite(v, sizeof v[0], 4, f);
        for (j = 0; j < 5; j++) {
            c[j] = q14(w->bfw[a].b[j]);
            c[5 + j] = q14(w->bbw[a].b[j]);
        }
        fwrite(c, sizeof c[0], 10, f);
    }
    /* the tap phases (horn 6, drum 6, in 16384ths of a turn) and the spacings (horn 6, drum 6, samples): exact */
    {
        int32_t ph[12];
        float sp[12];
        for (i = 0; i < 6; i++) {
            ph[i] = (int32_t)w->hornPhase[i];
            ph[6 + i] = (int32_t)w->drumPhase[i];
            sp[i] = (float)w->hornSpacing[i];
            sp[6 + i] = (float)w->drumSpacing[i];
        }
        fwrite(ph, sizeof ph[0], 12, f);
        fwrite(sp, sizeof sp[0], 12, f);
    }
    /* the biquads (setIIRFilter, setBfree's own): a1 a2 b0 b1 b2 of each, as float32. The default horn set (horn A
     * low-pass 4500 Hz, horn B low-shelf 300 Hz, the drum's high-shelf 811.97 Hz), then the sharp horn set (cfg/
     * whirl-sharp.cfg: A 3900 Hz Q 1.4 gain 0; B 300 Hz Q 2 gain -60). The mellow set is the default (its cfg says so). */
    {
        float bq[25];
        int n = 0, k;
        double ha_f[2] = {4500.0, 300.0}, ha_q[2] = {2.7456, 1.0}, ha_g[2] = {-30.0, -30.0};
        /* sharp B's gain: the cfg says -60 dB, but setIIRFilter refuses a gain of -48 or less (it leaves the filter at zero:
     * setBfree's horn B would be silent). -48 dB is the limit it accepts; ORGAN.md notes it. */
        double sh_f[2] = {3900.0, 300.0}, sh_q[2] = {1.4, 2.0}, sh_g[2] = {0.0, -47.9};
        int T[2] = {EQC_LPF, EQC_LOW};
        for (k = 0; k < 2; k++) {
            float t[8] = {0};
            setIIRFilter(t, T[k], ha_f[k], ha_q[k], ha_g[k], 44100.0);
            memcpy(bq + n, t + 1, 5 * sizeof(float)); n += 5;
        }
        {
            float t[8] = {0};
            setIIRFilter(t, EQC_HIGH, 811.9695, 1.6016, -38.9291, 44100.0);
            memcpy(bq + n, t + 1, 5 * sizeof(float)); n += 5;
        }
        for (k = 0; k < 2; k++) {
            float t[8] = {0};
            setIIRFilter(t, T[k], sh_f[k], sh_q[k], sh_g[k], 44100.0);
            memcpy(bq + n, t + 1, 5 * sizeof(float)); n += 5;
        }
        fwrite(bq, sizeof bq[0], 25, f);
    }
    fclose(f);
    return 0;
}
