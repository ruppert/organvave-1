/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* ADVANCED EDIT (eng_organ.c, the tables built in the main loop: org_env_build, org_xt_build, org_tuning_build, org_adv_poll) on the
 * host, against setBfree's own tonegen.c (tests/organ_adv_ref.txt: tools/gen_organ_adv_ref.py).
 * 1. the defaults: the builders given a patch of zeros give the flash tables (the click envelopes within 2 LSB, the crosstalk
 *    table's levels within 2 LSB of Q18, the same entries).
 * 2. setBfree's settings: per key the contributions' count and sums, and the eight attack / release envelopes (within 3 LSB of Q15)
 *    for crosstalk (compartment, transformer, terminal strip, wiring, floor, min) and click settings (models, levels, lengths).
 * 3. a table too big for the RAM is built with a higher floor, not cut.
 * 4. in the engine: the RAM table is used by new keys and every key leaves nothing behind; it is rebuilt only while no key holds it.
 * 5. the tuning, the percussion and the scanner follow their bytes.
 * 6. the crosstalk settings are audible (the maximum far from the default) and the switch gates them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "felucca_tables.h"
#define NVOICE 1
#define NPART 1
static inline int32_t clamp_(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
#include "../firmware/engines/src/felucca_engine.h"
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
enum { P_E0, P_E1, P_E2, P_E3 };
struct track { int dummy; };
#include "../firmware/engines/src/dsp.c"
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
#include "../firmware/src/eng_organ.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("organ adv: %-110s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static void patch_zero(void)
{
    memset(org_patch, 0, sizeof org_patch);
    org_pgen++;
}

/* 1. the defaults */
static void defaults(void)
{
    uint8_t z[ORG_PSIZE];
    uint32_t b, i, bad_a = 0, bad_r = 0, worst = 0;
    memset(z, 0, sizeof z);
    org_env_build(z);
    for (b = 0; b < 8u; b++)
        for (i = 0; i < 128u; i++) {
            int da = abs((int)org_atk_ram[b][i] - (int)ORG_ATK[b][i]), dr = abs((int)org_rel_ram[b][i] - (int)ORG_REL[b][i]);
            bad_a += da > 2;
            bad_r += dr > 2;
            worst = (uint32_t)(da > (int)worst ? da : dr > (int)worst ? dr : (int)worst);
        }
    printf("organ adv: default envelopes: worst %u LSB\n", worst);
    check("the default attack envelopes (setBfree's rand() stream, click, 50 %, 16..80 samples) are the flash ones within 2 LSB", bad_a == 0);
    check("the default release envelopes (linear) are the flash ones", bad_r == 0);
    {
        uint32_t fl = org_xt_build(z), k, miss = 0, extra = 0, lv = 0, tot = 0;
        for (k = 0; k < 96u; k++) {
            uint32_t a = ORG_KC_OFF[k], ae = ORG_KC_OFF[k + 1], c = org_xt_off[k], ce = org_xt_off[k + 1];
            while (a < ae || c < ce) {
                uint32_t ka = a < ae ? ORG_KC[a] >> 20 : 0xFFFFFFFFu, kc = c < ce ? org_xt_tab[c] >> 20 : 0xFFFFFFFFu;
                if (ka == kc) {
                    int d = abs((int)(ORG_KC[a] & 0xFFFFF) - (int)(org_xt_tab[c] & 0xFFFFF));
                    lv += d > 2;
                    a++;
                    c++;
                    tot++;
                } else if (ka < kc) {
                    miss += (ORG_KC[a] & 0xFFFFF) > 8;
                    a++;
                } else {
                    extra += (org_xt_tab[c] & 0xFFFFF) > 8;
                    c++;
                }
            }
        }
        printf("organ adv: default crosstalk table: %u entries (flash %u), floor %u, levels off by more than 2 LSB: %u, missing %u, extra %u\n",
               org_xt_off[96], (unsigned)ORG_KC_OFF[96], fl, lv, miss, extra);
        check("the default crosstalk table has setBfree's entries (those above 8 / 2^18 all there, none more)", miss == 0 && extra == 0);
        check("  .. with their levels within 2 LSB of Q18", lv == 0 && tot > 10000u);
        check("  .. and nothing was cut for room", fl == ORG_XT_G[org_ci(OP_XT_FLOOR, 0)]);
    }
}

/* 2. setBfree's settings */
static int near_(int64_t a, int64_t b, int64_t tol_abs, double tol_rel)
{
    int64_t d = a > b ? a - b : b - a;
    int64_t m = b < 0 ? -b : b;
    return d <= tol_abs || (double)d <= tol_rel * (double)m;
}
static void reference(void)
{
    FILE *f = fopen("tests/organ_adv_ref.txt", "r");
    char line[8192], name[64] = "";
    uint8_t p[ORG_PSIZE];
    static int64_t rk[96][4];
    static uint8_t rkset[96];
    static int renv[2][8][128];
    int in_set = 0, nsets = 0;
    if (!f) {
        check("tests/organ_adv_ref.txt is there", 0);
        return;
    }
    memset(p, 0, sizeof p);
    for (;;) {
        char *s = fgets(line, sizeof line, f);
        int last = !s || !strncmp(line, "set ", 4);
        if (last && in_set) {   /* compare the set that was read */
            uint32_t k, b, i, kbad = 0, ebad = 0, nbad = 0, worst = 0;
            int full = 0;
            uint32_t fl;
            int has_xt = p[OP_XT_COMP] | p[OP_XT_TRANS] | p[OP_XT_TSTRIP] | p[OP_XT_WIRE] | p[OP_XT_FLOOR] | p[OP_XT_MIN];
            if (has_xt) {
                fl = org_xt_build(p);
                full = fl > ORG_XT_G[org_ci(OP_XT_FLOOR, p[OP_XT_FLOOR])];       /* the RAM is smaller than the model asks for */
                for (k = 0; k < 96u; k++) {
                    int64_t n = org_xt_off[k + 1] - org_xt_off[k], s0 = 0, s1 = 0, s2 = 0;
                    uint32_t j;
                    if (!rkset[k] && n == 0)
                        continue;
                    for (j = org_xt_off[k]; j < org_xt_off[k + 1u]; j++) {
                        int64_t q = org_xt_tab[j] & 0xFFFFF, w = org_xt_tab[j] >> 25, bu = (org_xt_tab[j] >> 20) & 31;
                        s0 += q;
                        s1 += w * q;
                        s2 += bu * q;
                    }
                    if (full) {     /* the RAM is smaller than the model asks for: the faintest entries are dropped (higher floor) */
                        if (fl < 40000u ? (!near_(s0, rk[k][1], 64, 0.03) || !near_(s1, rk[k][2], 64, 0.03))
                                        : (s0 > rk[k][1] + rk[k][1] / 500 + 64 || s0 * 2 < rk[k][1]))      /* (a high floor drops the faint entries: never more, at least half) */
                            kbad++;
                    } else {
                        if (!near_(n, rk[k][0], 30, 0.15) || !near_(s0, rk[k][1], 1500, 0.002) ||        /* (entries at the floor may fall either side) */
                            !near_(s1, rk[k][2], 1500 * 91, 0.002) || !near_(s2, rk[k][3], 1500 * 27, 0.002))
                            { if (!kbad) printf("  first bad key %u: n %lld/%lld s0 %lld/%lld s1 %lld/%lld s2 %lld/%lld\n", k, (long long)n, (long long)rk[k][0], (long long)s0, (long long)rk[k][1], (long long)s1, (long long)rk[k][2], (long long)s2, (long long)rk[k][3]); kbad++; }
                    }
                }
                if (full) {
                    char w[200];
                    snprintf(w, sizeof w, "%s: the table (%u entries) fits the RAM with the floor raised to %u / 2^24, the sums as setBfree's, or lower (%u keys differ)",
                             name, org_xt_off[96], fl, kbad);
                    check(w, org_xt_off[96] <= ORG_XT_CAP && org_xt_off[96] > ORG_XT_CAP / 2u && kbad == 0);
                }
                else {
                    char w[160];
                    snprintf(w, sizeof w, "%s: the crosstalk table's counts and sums are setBfree's (%u keys differ)", name, kbad);
                    check(w, kbad == 0);
                }
            }
            if (!has_xt) {
                org_env_build(p);
                for (b = 0; b < 8u; b++)
                    for (i = 0; i < 128u; i++) {
                        int da = abs((int)org_atk_ram[b][i] - renv[0][b][i]), dr = abs((int)org_rel_ram[b][i] - renv[1][b][i]);
                        ebad += da > 3;
                        ebad += dr > 3;
                        worst = (uint32_t)(da > (int)worst ? da : dr > (int)worst ? dr : (int)worst);
                        nbad++;
                    }
                {
                    char w[160];
                    snprintf(w, sizeof w, "%s: the eight attack and release envelopes are setBfree's within 3 LSB (worst %u)", name, worst);
                    check(w, ebad == 0 && nbad == 1024u);
                }
            }
            nsets++;
            in_set = 0;
        }
        if (!s)
            break;
        if (!strncmp(line, "set ", 4)) {
            char *q = line + 4;
            int n = 0, v;
            uint32_t i;
            sscanf(q, "%63s%n", name, &n);
            q += n;
            memset(p, 0, sizeof p);
            for (i = 58; i < 70u; i++) {
                if (sscanf(q, "%d%n", &v, &n) != 1)
                    break;
                p[i] = (uint8_t)v;
                q += n;
            }
            memset(rk, 0, sizeof rk);
            memset(rkset, 0, sizeof rkset);
            memset(renv, 0, sizeof renv);
            in_set = 1;
        } else if (!strncmp(line, "key ", 4)) {
            int k;
            long long a, b, c, d;
            if (sscanf(line + 4, "%d %lld %lld %lld %lld", &k, &a, &b, &c, &d) == 5) {
                rk[k][0] = a; rk[k][1] = b; rk[k][2] = c; rk[k][3] = d;
                rkset[k] = 1;
            }
        } else if (!strncmp(line, "attack ", 7) || !strncmp(line, "release ", 8)) {
            int isr = line[0] == 'r', b, n = 0, i = 0, v;
            char *q = line + (isr ? 8 : 7);
            sscanf(q, "%d%n", &b, &n);
            q += n;
            while (i < 128 && sscanf(q, "%d%n", &v, &n) == 1) {
                renv[isr][b][i++] = v;
                q += n;
            }
        }
    }
    fclose(f);
    check("all the reference sets were compared", nsets == 7);
}

/* 4. the engine */
static void org_rst(void)
{
    org_init();
    patch_zero();
    org_pgen_seen = 0xFF;
}
static void render(uint32_t blocks)
{
    int32_t b[CTL];
    while (blocks--) {
        memset(b, 0, sizeof b);
        org_render(b, CTL);
    }
}
static int quiet(void)
{
    uint32_t w, d, z = 1;
    for (w = 1; w < ORG_NWHEEL; w++) {
        if (org.w[w].refc)
            z = 0;
        for (d = 0; d < ORG_NBUS; d++)
            if (org.w[w].lvl[d])
                z = 0;
    }
    return z && org.nacl == 0 && org.ndown == 0 && org.nram == 0;
}
static void engine(void)
{
    uint32_t k;
    org_rst();
    org_patch[OP_XTALK] = 1;
    org_patch[OP_XT_WIRE] = (uint8_t)org_bi(OP_XT_WIRE, 40);
    org_block(0);
    org_adv_poll();
    check("a crosstalk setting off the default puts the RAM table in use for new keys", org.xt_use == 2 && org_xt_valid);
    for (k = 0; k < 10u; k++)
        org_key(10u + k, 1);
    check("  .. the keys that went down on it are counted", org.nram == 10u && org.keys[10] == 3u);
    render(8);
    org_patch[OP_XT_WIRE] = (uint8_t)org_bi(OP_XT_WIRE, 30);        /* another setting while they are held */
    org_pgen++;
    org_adv_poll();
    check("  .. the table is not rebuilt while a key holds it", memcmp(org_b_xt, org_patch + OP_XT_COMP, 6) != 0 && org.nram == 10u);
    for (k = 0; k < 10u; k++)
        org_key(10u + k, 0);
    render(8);
    check("  .. every key leaves nothing behind (levels, counts, the active list)", quiet());
    org_adv_poll();
    check("  .. and with the keys up the new setting is built", !memcmp(org_b_xt, org_patch + OP_XT_COMP, 6) && org.xt_use == 2);
    org_key(20, 1);
    org_patch[OP_XT_WIRE] = 0;
    org_pgen++;
    org_adv_poll();
    check("back to the defaults while a RAM key is held: new keys take the flash table at once", org.xt_use == 1);
    org_key(21, 1);
    render(8);
    org_key(20, 0);
    org_key(21, 0);
    render(8);
    check("  .. the held key and the new one release cleanly", quiet() && org.keys[20] == 0);
    /* the crosstalk switch off beats the RAM table */
    org_patch[OP_XT_WIRE] = (uint8_t)org_bi(OP_XT_WIRE, 40);
    org_patch[OP_XTALK] = 0;
    org_pgen++;
    org_block(0);
    org_adv_poll();
    org_key(30, 1);
    check("crosstalk off: the key takes no crosstalk table", org.keys[30] == 2u && org.nram == 0);
    org_key(30, 0);
    render(8);
    check("  .. and releases", quiet());
}

/* 5. the tuning, the percussion, the scanner */
static double freq_of(uint32_t key)           /* the sounding wheel's frequency, from the render (zero crossings of one key, no crosstalk) */
{
    static int32_t y[CTL * 700];
    uint32_t i, n = 0, z = 0;
    int32_t b[CTL];
    int last = 0;
    org_all_off();
    render(16);
    org.xtalk = 0;
    for (i = 0; i < 9u; i++)
        org.bars[i] = i == 2u ? 8u : 0u;
    org_key(key, 1);
    render(16);
    for (i = 0; i < 700u; i++) {
        memset(b, 0, sizeof b);
        org_render(b, CTL);
        memcpy(y + i * CTL, b, sizeof b);
    }
    for (i = 1; i < CTL * 700u; i++)
        if (y[i - 1] < 0 && y[i] >= 0) {
            if (!z)
                z = i;
            last = (int)i;
            n++;
        }
    org_key(key, 0);
    return n > 2 ? 44100.0 * (double)(n - 1) / (double)(last - (int)z) : 0.0;
}
static void params(void)
{
    double f0, f1;
    org_rst();
    org_block(0);
    org.xtalk = 0;
    f0 = freq_of(33);                        /* A4 on the upper manual's 8' */
    org_patch[OP_TUNING] = (uint8_t)org_bi(OP_TUNING, 64 + 20);             /* +10 Hz: 450 Hz */
    org_pgen++;
    org_adv_poll();
    org_block(0);
    org.xtalk = 0;
    f1 = freq_of(33);
    printf("organ adv: A4 %.3f Hz at 440, %.3f at 450\n", f0, f1);
    check("TUNING 450 Hz raises every wheel by 450 / 440", fabs(f1 / f0 - 450.0 / 440.0) < 0.002 && org.wtab == org_winc_ram);
    org_patch[OP_TUNING] = 0;
    org_pgen++;
    org_adv_poll();
    check("TUNING back to 440 Hz is the flash table again", org.wtab == ORG_WINC && org_b_tun == 0);
    org_patch[OP_TUNING] = (uint8_t)org_bi(OP_TUNING, 0);                    /* 408 Hz */
    org_pgen++;
    org_adv_poll();
    check("TUNING 408 Hz scales the increments by 408 / 440", fabs((double)org_winc_ram[0][40] / (double)ORG_WINC[0][40] - 408.0 / 440.0) < 1e-6);
    org_rst();
    org_patch[OP_PC_FAST] = (uint8_t)org_bi(OP_PC_FAST, 4);                 /* 0.5 s */
    org_patch[OP_PC_NORM] = (uint8_t)org_bi(OP_PC_NORM, 6);                 /* -6 dB */
    org_patch[OP_PC_GAIN] = (uint8_t)org_bi(OP_PC_GAIN, 44);                /* 22.0 */
    org_patch[OP_PC_BUSA] = (uint8_t)org_bi(OP_PC_BUSA, 5);
    org_patch[OP_PC_TRIG] = (uint8_t)org_bi(OP_PC_TRIG, 0);                 /* none */
    org_patch[OP_SC_HZ] = (uint8_t)org_bi(OP_SC_HZ, 13 + 4 * 4);            /* 11.25 Hz */
    org_patch[OP_SC_V2] = (uint8_t)org_bi(OP_SC_V2, 2);                     /* 1.0 */
    org_pgen++;
    org_block(0);
    check("PERC FAST 0.5 s: the decay per block is 60 dB in 0.5 s", org.pdec_fast == (uint16_t)lround(65536 * pow(10.0, -96.0 / (44100.0 * 0.5))));
    check("PERC NORMAL -6 dB and GAIN 22: the envelope starts at 22 x 0.501", fabs((double)org.pe_norm / 16777216.0 - 22.0 * pow(10.0, -0.3)) < 0.01);
    check("PERC SOFT at its default keeps setBfree's start (5.5132) only with the default gain", org.pe_soft != ORG_PERC_SOFT);
    check("PERC BUS A 5, TRIG none", org.pbusa == 5u && org.ptrig == -1);
    check("SCANNER 11.25 Hz and VIB2 depth 1.0", fabs((double)org.vinc / (double)ORG_VIB_INC - 11.25 / 7.25) < 1e-5 && org.vamp2[1] == 2u);
    org_rst();
    org_block(0);
    check("a patch of zeros: setBfree's defaults (perc 11.0 / 5.5132, 1 s / 4 s, buses 3 / 4, mutes 1', scanner 7.25 Hz, depths 3 / 6 / 9)",
          org.pe_norm == ORG_PERC_NORM && org.pe_soft == ORG_PERC_SOFT && org.pdec_fast == ORG_PERC_FAST && org.pdec_slow == ORG_PERC_SLOW &&
          org.pbusa == 3u && org.pbusb == 4u && org.ptrig == 8 && org.vinc == ORG_VIB_INC && org.vamp2[0] == 6u && org.vamp2[1] == 12u &&
          org.vamp2[2] == 18u);
    /* the bytes' ranges */
    {
        uint32_t i, ok = 1;
        uint8_t v[ORG_PSIZE];
        memset(v, 127, sizeof v);
        org_sanitize(v);
        for (i = OP_TUNING; i < OP_NUM; i++) {
            const org_adv_t *a = org_adv_of(i);
            ok &= a != 0 && v[i] == a->n - 1u && org_bi(i, org_ci(i, 5u % a->n)) == 5u % a->n && org_ci(i, 0) == a->d;
        }
        for (i = OP_NUM; i < ORG_PSIZE; i++)
            ok &= v[i] == 0;
        check("every ADVANCED EDIT byte: its range is N - 1, byte 0 is the default index, the coding round-trips; the reserved bytes are 0", ok);
    }
}

/* 6. 0.9.5, "the crosstalk sounds the same at the maximum and the minimum": the audio of a held chord against the default, per setting.
 * The engine follows the settings (the maximum of each is far from the default), the minimum is near it (the default is -40 dB, a
 * level that is already inaudible), and with the CROSSTALK switch off no setting reaches the sound */
static int32_t xt_out[400 * CTL], xt_ref[400 * CTL];
static void xt_run(int32_t *dst, uint32_t sw, const uint32_t *idx)
{
    uint32_t i, k;
    int32_t b[CTL];
    org_init();
    memset(org_patch, 0, sizeof org_patch);
    for (i = 0; i < 9u; i++)
        org_patch[OP_UB + i] = i == 2u || i == 3u ? 8u : 0u;
    org_patch[OP_XTALK] = (uint8_t)sw;
    for (i = 0; i < 6u; i++)
        org_patch[OP_XT_COMP + i] = (uint8_t)org_bi(OP_XT_COMP + i, idx[i]);
    org_pgen++;
    org_pgen_seen = 0xFF;
    org_block(0);
    org_adv_poll();
    org_key(10, 1); org_key(14, 1); org_key(17, 1); org_key(40, 1);
    for (k = 0; k < 400u; k++) {
        memset(b, 0, sizeof b);
        org_render(b, CTL);
        memcpy(dst + k * CTL, b, sizeof b);
    }
}
static double xt_diff(const int32_t *a, const int32_t *b)
{
    double e = 0, r = 0;
    uint32_t i;
    for (i = 100u * CTL; i < 400u * CTL; i++) {
        double d = (double)a[i] - (double)b[i];
        e += d * d;
        r += (double)b[i] * (double)b[i];
    }
    return sqrt(e / r);
}
static void audible(void)
{
    static const uint32_t DEF[6] = {31, 0, 31, 31, 3, 0};      /* the default steps (-40 dB x3, off, -96 dB, off) */
    static const char *const NM[6] = {"COMPARTMENT", "TRANSFORMER", "TERMINAL STRIP", "WIRING", "XT FLOOR", "XT MIN"};
    uint32_t p, ok_max = 1, ok_min = 1;
    char w[96];
    xt_run(xt_ref, 1, DEF);
    for (p = 0; p < 6u; p++) {
        uint32_t a[6], j;
        double dmax, dmin;
        for (j = 0; j < 6u; j++)
            a[j] = DEF[j];
        a[p] = 51;
        xt_run(xt_out, 1, a);
        dmax = xt_diff(xt_out, xt_ref);
        a[p] = 1;
        xt_run(xt_out, 1, a);
        dmin = xt_diff(xt_out, xt_ref);
        snprintf(w, sizeof w, "%s: the maximum is far from the default sound (%.3f), the minimum near it (%.3f)", NM[p], dmax, dmin);
        check(w, dmax > 0.3 && dmin < dmax / 10.0);
        ok_max &= dmax > 0.3;
        ok_min &= dmin < dmax / 10.0;
    }
    {
        uint32_t a[6] = {51, 51, 51, 51, 3, 0}, z[6] = {0, 0, 0, 0, 0, 0};
        xt_run(xt_out, 0, a);
        xt_run(xt_ref, 0, z);
        check("CROSSTALK off: the settings do not reach the sound (all at the maximum is the same as all at the minimum)", xt_diff(xt_out, xt_ref) < 1e-9);
        xt_run(xt_out, 1, a);
        check("  .. switched on, the same maximum is a different sound", xt_diff(xt_out, xt_ref) > 0.3);
    }
}

int main(void)
{
    org_init();
    defaults();
    reference();
    engine();
    params();
    audible();
    printf(bad ? "organ adv: %d FAILED\n" : "organ adv: all ok\n", bad);
    return bad != 0;
}
