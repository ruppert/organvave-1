/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* ORGAN's tone generator (src/eng_organ.c) on the host, against numbers measured on setBfree's own (the float
 * reference, tools/setbfree_ref: tools/setbfree_ref/compare.py renders both and prints them).
 *   build/host/organ_test                                    the checks
 *   build/host/organ_test render OUT.f32 BARS SECS EVENTS    float32 at 44.1 kHz in setBfree's units, the same
 *                                                            arguments and environment as sbf_ref render (TEMP, NOXT,
 *                                                            SWELL)
 * 1. tuning: the three temperaments' wheels are setBfree's (wheel 65, E6: 1318.209, 1320.000, 1318.510 Hz).
 * 2. drawbars: each footage alone on C4 gives setBfree's wheel; the level of 8' at 8, 4 and 1 is setBfree's
 *    (linear: 8 -> 4 = -6.02 dB).
 * 3. the cases of compare.py (one key, with and without crosstalk; chords at 888888888, equal and 50 Hz; ten keys
 *    under key compression; a run of overlapping keys; a key repeated fast; the swell at 0.25): the rms of the whole
 *    render within 0.01 dB of setBfree's, the first fragment's (the attack: the contact click envelope) within 0.05 dB.
 *    (Measured: within 0.002 dB and 0.04 dB; the waveforms agree at 56 to 68 dB SNR over the first 2048 samples.)
 * 4. bounds: every key of the three manuals at full drawbars never overflows; silence is exact 0. */
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
#include "../firmware/src/eng_organ.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("organ: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static void bars(const char *s)
{
    uint32_t i;
    for (i = 0; i < 9; i++)
        org.bars[i] = (uint8_t)(s[i] - '0');
}

/* render n samples (a multiple of CTL) into y (float, setBfree's units) */
static void run(float *y, uint32_t n)
{
    uint32_t i, j;
    int32_t b[CTL];
    for (i = 0; i < n; i += CTL) {
        memset(b, 0, sizeof b);
        org_render(b, CTL);
        for (j = 0; j < CTL; j++)
            if (y)
                y[i + j] = (float)b[j] / 32768.0f;
    }
}

static double rms(const float *y, uint32_t n)
{
    double s = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        s += (double)y[i] * y[i];
    return sqrt(s / n);
}

/* frequency of the strongest component: zero crossings of a pure tone over n samples */
static double freq_zc(const float *y, uint32_t n)
{
    uint32_t i, first = 0, last = 0, cnt = 0;
    for (i = 1; i < n; i++)
        if (y[i - 1] < 0 && y[i] >= 0) {
            double t = i - 1 + y[i - 1] / (y[i - 1] - y[i]);
            if (!cnt)
                first = i;
            last = i;
            cnt++;
            (void)t;
        }
    if (cnt < 2)
        return 0;
    {
        /* refine with the fractional crossings */
        double t0 = 0, t1 = 0;
        for (i = 1; i < n; i++)
            if (y[i - 1] < 0 && y[i] >= 0) {
                double t = i - 1 + y[i - 1] / (y[i - 1] - y[i]);
                if (i == first)
                    t0 = t;
                if (i == last)
                    t1 = t;
            }
        return (cnt - 1) * 44100.0 / (t1 - t0);
    }
}

/* render a case as sbf_ref does: key events at fragment starts (times in s); temp 1 gear 60, 2 gear 50, 0 equal
 * (sbf_ref's TEMP); returns the samples written to y (at most cap) */
static uint32_t render_case(const char *bar, double secs, const char *events, int temp, int xt, double swell, float *y,
                            uint32_t cap)
{
    uint32_t n, p, ei = 0, ne = 0;
    double et[256];
    int ek[256], eon[256];
    char *ev = strdup(events), *s;
    org_init();
    org.temp = (uint8_t)(temp == 1 ? 0 : temp == 2 ? 1 : 2);
    org.xtalk = (uint8_t)xt;
    org.swell = (uint16_t)(swell * 32768);
    org.sw = (int32_t)org.swell << 9;                               /* (the reference starts at its swell) */
    bars(bar);
    n = (uint32_t)(secs * 44100.0) / 128u * 128u;
    if (n > cap)
        n = cap / 128u * 128u;
    for (s = strtok(ev, ","); s && ne < 256; s = strtok(NULL, ",")) {
        char c;
        int key;
        if (sscanf(s, "%lf:%c%d", &et[ne], &c, &key) == 3) {
            ek[ne] = key;
            eon[ne] = c == '+';
            ne++;
        }
    }
    free(ev);
    for (p = 0; p < n; p += CTL) {
        if (p % 128u == 0)
            while (ei < ne && et[ei] * 44100.0 <= p) {
                org_key((uint32_t)ek[ei], (uint32_t)eon[ei]);
                ei++;
            }
        run(y + p, CTL);
    }
    return n;
}

static int render_main(int argc, char **argv)
{
    static float y[44100 * 30];
    uint32_t n;
    FILE *f;
    if (argc < 6)
        return 2;
    n = render_case(argv[3], atof(argv[4]), argv[5], getenv("TEMP") ? atoi(getenv("TEMP")) : 1, getenv("NOXT") ? 0 : 1,
                    getenv("SWELL") ? atof(getenv("SWELL")) : 1.0, y, NELEM(y));
    f = fopen(argv[2], "wb");
    if (!f)
        return 1;
    fwrite(y, sizeof(float), n, f);
    fclose(f);
    return 0;
}

#define N1 (44100u / 128u * 128u)
static float buf[N1];

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "render"))
        return render_main(argc, argv);

    /* 1. tuning: setBfree's wheel 65 (E6, C4's 1 3/5') differs in the three temperaments */
    {
        static const double want[3] = {1318.208955224, 1320.0, 1318.510227651};   /* setBfree's frequencies */
        uint32_t t;
        for (t = 0; t < 3; t++) {
            char what[110];
            double f;
            org_init();
            org.temp = (uint8_t)t;
            org.xtalk = 0;
            bars("000000800");
            org_key(24, 1);
            run(buf, N1);
            f = freq_zc(buf + 1024, N1 - 1024);
            snprintf(what, sizeof what, "C4 at 1 3/5' (%s): %.3f Hz, setBfree's %.3f", t == 0 ? "gear 60 Hz" : t == 1 ? "gear 50 Hz" : "equal", f, want[t]);
            check(what, fabs(f - want[t]) < 0.01);
        }
    }

    /* 2. each footage alone on C4 (key 24): setBfree's wheel (gear 60 Hz); the drawbar law */
    {
        static const char *bar[9] = {"800000000", "080000000", "008000000", "000800000", "000080000",
                                     "000008000", "000000800", "000000080", "000000008"};
        static const double want[9] = {130.769230769, 392.0, 261.538461538, 523.076923077, 784.0, 1046.153846154,
                                       1318.208955224, 1568.0, 2092.307692308};
        double r8 = 0, r4, r1, worst = 0;
        uint32_t b;
        for (b = 0; b < 9; b++) {
            double f;
            org_init();
            org.xtalk = 0;
            bars(bar[b]);
            org_key(24, 1);
            run(buf, N1);
            f = freq_zc(buf + 1024, N1 - 1024);
            if (b == 2)
                r8 = rms(buf + 1024, N1 - 1024);
            if (fabs(f - want[b]) > worst)
                worst = fabs(f - want[b]);
        }
        {
            char what[110];
            snprintf(what, sizeof what, "each footage alone on C4 sounds at setBfree's wheel (worst %.4f Hz)", worst);
            check(what, worst < 0.02);
        }
        org_init();
        org.xtalk = 0;
        bars("004000000");
        org_key(24, 1);
        run(buf, N1);
        r4 = rms(buf + 1024, N1 - 1024);
        org_init();
        org.xtalk = 0;
        bars("001000000");
        org_key(24, 1);
        run(buf, N1);
        r1 = rms(buf + 1024, N1 - 1024);
        {
            char what[120];
            snprintf(what, sizeof what, "drawbar law: 8' at 4 is %.2f dB, at 1 %.2f dB (linear: -6.02, -18.06); 8' at 8 rms %.4f (0.7071)",
                     20 * log10(r4 / r8), 20 * log10(r1 / r8), r8);
            check(what, fabs(20 * log10(r4 / r8) + 6.02) < 0.02 && fabs(20 * log10(r1 / r8) + 18.06) < 0.05 &&
                        fabs(r8 - 0.70711) < 0.001);
        }
    }

    /* 3. compare.py's cases against setBfree's numbers (sbf_ref render, the same arguments) */
    {
        static const struct {
            const char *name, *bars, *ev;
            double secs;
            int temp, xt;
            double swell, rms, atk;
        } C[] = {
            {"one key 888000000", "888000000", "0.0:+24,0.5:-24", 0.75, 1, 1, 1.0, 1.020910, 0.871000},
            {"one key, no crosstalk", "888000000", "0.0:+24,0.5:-24", 0.75, 1, 0, 1.0, 0.999517, 0.452909},
            {"C major 888888888", "888888888", "0.0:+24,0.0:+28,0.0:+31,0.0:+36,0.5:-24,0.5:-28,0.5:-31,0.5:-36", 0.75, 1, 1,
             1.0, 3.721132, 3.082493},
            {"C major 838000000 equal", "838000000", "0.0:+24,0.0:+28,0.0:+31,0.5:-24,0.5:-28,0.5:-31", 0.75, 0, 1, 1.0,
             1.240539, 1.528651},
            {"C major 008080400 gear 50", "008080400", "0.0:+24,0.0:+28,0.0:+31,0.5:-24,0.5:-28,0.5:-31", 0.75, 2, 1, 1.0,
             1.204695, 1.418005},
            {"ten keys (key compression)", "888000000",
             "0.0:+20,0.0:+22,0.0:+24,0.0:+26,0.0:+28,0.0:+30,0.0:+32,0.0:+34,0.0:+36,0.0:+38", 0.75, 1, 1, 1.0, 2.621400, 2.425436},
            {"a run of overlapping keys", "888800000", 0, 1.0, 1, 1, 1.0, 1.431978, 0.802287},
            {"a key repeated fast", "008000000", 0, 0.5, 1, 1, 1.0, 0.327192, 0.329700},
            {"swell 0.25", "888000000", "0.0:+24", 0.5, 1, 1, 0.25, 0.312210, 0.217750},
        };
        static char run_ev[2048], rep_ev[1024];
        static float y[44100];
        uint32_t i, n;
        char *p = run_ev;
        for (i = 0; i < 16; i++)
            p += sprintf(p, "%s%.3f:+%u,%.3f:-%u", i ? "," : "", 0.05 * i, 24 + i, 0.05 * i + 0.12, 24 + i);
        p = rep_ev;
        for (i = 0; i < 12; i++)
            p += sprintf(p, "%s%.3f:+33,%.3f:-33", i ? "," : "", 0.02 * i, 0.02 * i + 0.01);
        for (i = 0; i < NELEM(C); i++) {
            char what[140];
            double r, ra, dr, da;
            n = render_case(C[i].bars, C[i].secs, C[i].ev ? C[i].ev : i == 6 ? run_ev : rep_ev, C[i].temp, C[i].xt,
                            C[i].swell, y, NELEM(y));
            r = rms(y, n);
            ra = rms(y, 128);
            dr = 20 * log10(r / C[i].rms);
            da = 20 * log10(ra / C[i].atk);
            snprintf(what, sizeof what, "%-28s rms %+.4f dB, attack fragment %+.3f dB from setBfree's", C[i].name, dr, da);
            check(what, fabs(dr) < 0.01 && fabs(da) < 0.05);
        }
    }

    /* 6. silence is exact */
    {
        uint32_t i, z = 1;
        org_init();
        bars("888888888");
        run(buf, N1);
        for (i = 0; i < N1; i++)
            z &= buf[i] == 0.0f;
        org_key(24, 1);
        run(buf, 4096);
        org_key(24, 0);
        run(buf, 4096);
        run(buf, N1);
        for (i = 0; i < N1; i++)
            z &= buf[i] == 0.0f;
        check("no key: exact silence (before, and after a key's release)", z);
    }
    {
        uint32_t k, i;
        float mx = 0;
        int32_t b[CTL];
        org_init();
        for (k = 0; k < 27; k++)
            org.bars[k] = 8;
        for (k = 0; k < 61; k++) {
            org_key(k, 1);
            org_key(64 + k, 1);
        }
        for (k = 128; k < 160; k++)
            org_key(k, 1);
        for (i = 0; i < N1; i += CTL) {
            uint32_t j;
            memset(b, 0, sizeof b);
            org_render(b, CTL);
            for (j = 0; j < CTL; j++)
                if (fabsf((float)b[j]) > mx)
                    mx = fabsf((float)b[j]);
        }
        {
            char what[120];
            snprintf(what, sizeof what, "every key of the three manuals at full drawbars: bounded (peak %.1f of 1.0)", mx / 32768.0);
            check(what, mx < 2147483647.0f / 4 && mx > 0);
        }
    }
    /* 5. percussion (setBfree's HIPASS_PERCUSSION build). The envelope starts at 11.0 (normal) or 5.5132 (soft) and
     *    falls 60 dB in 1 s (fast) or 4 s (slow), per 32-sample block; the mix's gain steps to 0.60512 while it is on
     *    (normal; soft 1.0); the 1' is muted; the contact is the 4' (or 2 2/3') whatever the drawbars. */
    {
        uint32_t soft, fast, b;
        double ratio[2][2];
        for (soft = 0; soft < 2; soft++)
            for (fast = 0; fast < 2; fast++) {
                char what[120];
                double want = -60.0 * 1000.0 * 32.0 / ((fast ? 1.0 : 4.0) * 44100.0);
                double db;
                int32_t t[CTL];
                double p1;
                org_init();
                org.xtalk = 0;
                bars("000000000");
                org.perc = 1;
                org.psoft = (uint8_t)soft;
                org.pfast = (uint8_t)fast;
                org_key(24, 1);
                memset(t, 0, sizeof t);
                org_render(t, CTL);                                   /* the reset, then one block of decay */
                p1 = (double)org.pe;
                for (b = 0; b < 1000; b++) {
                    memset(t, 0, sizeof t);
                    org_render(t, CTL);
                }
                db = 20.0 * log10((double)org.pe / p1);
                ratio[soft][fast] = db;
                snprintf(what, sizeof what, "percussion %s, %s: the envelope falls %.2f dB in 1000 blocks (setBfree's %.2f)",
                         soft ? "soft" : "normal", fast ? "fast" : "slow", db, want);
                check(what, fabs(db - want) < 0.1);
            }
        {
            double rn = (double)0, rs;
            /* the start: 11.0 normal, 5.5132 soft (the same block, so their ratio is setBfree's 0.5012) */
            org_init();
            org.perc = 1;
            org.xtalk = 0;
            bars("000000000");
            org_key(24, 1);
            {
                int32_t t[CTL] = {0};
                org_render(t, CTL);
            }
            rn = (double)org.pe;
            org_init();
            org.perc = 1;
            org.psoft = 1;
            org.xtalk = 0;
            bars("000000000");
            org_key(24, 1);
            {
                int32_t t[CTL] = {0};
                org_render(t, CTL);
            }
            rs = (double)org.pe;
            check("percussion: the soft start is 0.5012 of the normal (setBfree's reset gains)",
                  fabs(rs / rn - 0.5012) < 0.0005);
        }
        /* the mix's gain (the swell target) */
        org_init();
        org.perc = 1;
        bars("000000000");
        org_key(24, 1);
        {
            int32_t t[CTL] = {0};
            org_render(t, CTL);
        }
        check("percussion normal: the mix's gain is 0.60512 (setBfree's percDrawbarGain)", org.pdg == 19829);
        org.psoft = 1;
        org_init();
        org.perc = 1;
        org.psoft = 1;
        bars("000000000");
        org_key(24, 1);
        {
            int32_t t[CTL] = {0};
            org_render(t, CTL);
        }
        check("percussion soft: the mix's gain stays 1.0", org.pdg == 32768);
        /* the 1' is muted while it is on: with only the 1' drawn, the wheels' gains are all 0 */
        {
            uint32_t i, sum_on = 0, sum_off = 0;
            int32_t t[CTL] = {0};
            org_init();
            org.xtalk = 0;
            bars("000000008");
            org.perc = 1;
            org_key(24, 1);
            org_render(t, CTL);
            for (i = 0; i < org.nacl; i++)
                sum_on += (uint32_t)org.w[org.acl[i]].sum;
            org_init();
            org.xtalk = 0;
            bars("000000008");
            org_key(24, 1);
            org_render(t, CTL);
            for (i = 0; i < org.nacl; i++)
                sum_off += (uint32_t)org.w[org.acl[i]].sum;
            check("percussion on: the 1' is muted (its wheels' gains 0); off, it sounds", sum_on == 0 && sum_off > 0);
        }
        /* the contact: with the drawbars at 0 the percussion sounds, at the 4' (2nd) or the 2 2/3' (3rd) harmonic */
        {
            static const double want[2] = {523.2511, 784.8767};          /* 2 x and 3 x C4 (261.6256 Hz) */
            uint32_t sel;
            for (sel = 0; sel < 2; sel++) {
                char what[120];
                double f, rr;
                org_init();
                org.xtalk = 0;
                bars("000000000");
                org.perc = 1;
                org.psel = (uint8_t)sel;
                org_key(24, 1);
                run(buf, N1);
                rr = rms(buf + 1024, N1 - 1024);
                f = freq_zc(buf + 1024, N1 - 1024);
                snprintf(what, sizeof what, "percussion %s with the drawbars at 0: %.1f Hz, rms %.4f (setBfree's %.1f Hz)",
                         sel ? "2 2/3'" : "4'", f, rr, want[sel]);
                check(what, rr > 0.01 && fabs(f - want[sel]) < 1.0);
            }
        }
    }

    /* 6. the scanner's vibrato and chorus against setBfree's vibratoProc (the float loop of vibrato.c, here): a
     *    1 kHz tone through VIB1..3 and CHO1..3 agrees with the integer port to -40 dB. */
    {
        static int32_t in[8192], out[8192];
        static float ref[8192];
        uint32_t v, n;
        for (n = 0; n < 8192; n++)
            in[n] = (int32_t)lround(8000.0 * sin(2.0 * 3.14159265358979 * 1000.0 * n / 44100.0));
        for (v = 1; v <= 6; v++) {
            char what[120];
            uint32_t amp = 3u * ((v - 1u) % 3u + 1u), mixed = v > 3u, i, nn;
            double pos_d = 0, stat = 0, err = 0, sig = 0;
            static double vib[1024];
            uint32_t pos = 0;
            memset(vib, 0, sizeof vib);
            for (nn = 0; nn < 8192; nn++) {
                double x = in[nn], j, f, g, wet;
                uint32_t h, k;
                j = (double)pos + 1.0 + amp + amp * sin(2.0 * 3.14159265358979 * stat);
                h = (uint32_t)floor(j) % 1024u;
                f = j - floor(j);
                k = (h + 1u) % 1024u;
                g = f * x;
                vib[h] += x - g;
                vib[k] += g;
                wet = vib[pos];
                vib[pos] = 0;
                ref[nn] = (float)(mixed ? (x + wet) * 0.70710678 : wet);
                pos = (pos + 1u) % 1024u;
                stat += 7.25 / 44100.0;
                (void)pos_d;
            }
            org_init();
            org.vibm_cur = (uint8_t)v;
            for (i = 0; i < 8192; i += CTL)
                org_vib(in + i, out + i, CTL);
            for (nn = 0; nn < 8192; nn++) {
                double d = (double)out[nn] - ref[nn];
                err += d * d;
                sig += (double)ref[nn] * ref[nn];
            }
            snprintf(what, sizeof what, "%s: the integer port against setBfree's float loop: %.1f dB",
                     mixed ? (v == 4 ? "CHO1" : v == 5 ? "CHO2" : "CHO3") : (v == 1 ? "VIB1" : v == 2 ? "VIB2" : "VIB3"),
                     10.0 * log10(err / (sig + 1e-9)));
            check(what, err < sig * 1e-4);                           /* -40 dB */
        }
    }

    printf("organ: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
