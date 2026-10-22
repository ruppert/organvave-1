/* float prototype of the LIGHT rotary, for fitting against CLASSIC's sideband table (ref.txt) */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#define FS 44100.0
#define NP 24
enum { P_FC, P_HP, P_HQ, P_HS1, P_HPH1, P_HA1, P_HB1, P_HGPH1, P_HS2, P_HPH2, P_HA2, P_HB2, P_HGPH2, P_PSI,
       P_DP, P_DQ, P_DA, P_DB, P_DGPH, P_DPSI, P_HL, P_DL, P_LK, P_HSLOPE };
typedef struct { int kind; double tone, fr; int n0, W; double ref[18]; } kase;
static kase K[64]; static int NK;
static void load(void)
{
    FILE *f = fopen("ref.txt", "r"); double v[23]; int i;
    while (NK < 64 && fscanf(f, "%lf", &v[0]) == 1) {
        for (i = 1; i < 23; i++) fscanf(f, "%lf", &v[i]);
        K[NK].kind = (int)v[0]; K[NK].tone = v[1]; K[NK].fr = v[2]; K[NK].n0 = (int)v[3]; K[NK].W = (int)v[4];
        for (i = 0; i < 18; i++) K[NK].ref[i] = v[5 + i];
        NK++;
    }
    fclose(f);
}
static double tw(double x) { return x - floor(x); }
static double geo(double P, double Q, double th) { double d2 = P + Q * cos(2 * M_PI * th); return sqrt(d2 > 1 ? d2 : 1); }
static double gain(double a, double b, double th) { double g = a + b * cos(2 * M_PI * th); return g > 0 ? g : 0; }
/* renders N samples of a tone; kind 0: horn rotates at fr, 1: drum rotates */
static void render(const double *p, int kind, double tone, double fr, int N, double *L, double *R)
{
    enum { LN = 2048, LM = LN - 1 };
    static double hl[LN], dl[LN]; double s1 = 0, s2 = 0, a1, a2, b0, b1, b2, w0, al, c;
    int n, i;
    memset(hl, 0, sizeof hl); memset(dl, 0, sizeof dl);
    w0 = 2 * M_PI * p[P_FC] / FS; c = cos(w0); al = sin(w0) / (2 * 0.7071);
    b0 = (1 - c) / 2; b1 = 1 - c; b2 = b0; a1 = -2 * c; a2 = 1 - al; { double a0 = 1 + al; b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0; }
    double lx1 = 0, lx2 = 0, ly1 = 0, ly2 = 0, tx1 = 0, tx2 = 0, ty1 = 0, ty2 = 0;
    (void)s1; (void)s2;
    for (n = 0; n < N; n++) {
        double x = 0.5 * sin(2 * M_PI * tone * n / FS), lo, hi, th = tw(fr * n / FS);
        double thh = kind == 0 ? th : 0, thd = kind == 1 ? th : 0, out[2];
        lo = b0 * x + b1 * lx1 + b2 * lx2 - a1 * ly1 - a2 * ly2; lx2 = lx1; lx1 = x; ly2 = ly1; ly1 = lo;
        hi = x - lo;
        { double y = p[24] * hi + p[25] * tx1 + p[26] * tx2 - p[27] * ty1 - p[28] * ty2; tx2 = tx1; tx1 = hi; ty2 = ty1; ty1 = y; hi = y; }
        hl[n & LM] = hi; dl[n & LM] = lo;
        for (i = 0; i < 2; i++) {
            double tm = tw(thh + i * p[P_HP + 0] * 0 + i * p[P_PSI]), y = 0, dd, d, fi, g; int k, j;
            for (k = 0; k < 2; k++) {
                double s = k ? p[P_HS2] : p[P_HS1], ph = k ? p[P_HPH2] : p[P_HPH1], a = k ? p[P_HA2] : p[P_HA1], b = k ? p[P_HB2] : p[P_HB1], gp = k ? p[P_HGPH2] : p[P_HGPH1];
                d = geo(p[P_HP], p[P_HQ], tm + ph) + s;
                g = gain(a, b, tm + gp);
                dd = n - d; j = (int)floor(dd); fi = dd - j;
                y += g * (hl[j & LM] * (1 - fi) + hl[(j + 1) & LM] * fi);
            }
            out[i] = p[P_HL] * y;
        }
        for (i = 0; i < 2; i++) {
            double tm = tw(thd + i * p[P_DPSI]), d = geo(p[P_DP], p[P_DQ], tm), g = gain(p[P_DA], p[P_DB], tm + p[P_DGPH]), dd = n - d, fi; int j = (int)floor(dd);
            fi = dd - j;
            out[i] += p[P_DL] * g * (dl[j & LM] * (1 - fi) + dl[(j + 1) & LM] * fi) + p[P_LK] * x;
        }
        L[n] = out[0]; R[n] = out[1];
    }
}
static void sbands(const double *y, double f, double fr, int n0, int W, double *o)
{
    int k, n; double ws = 0; static double *win; static int wW;
    if (wW != W) { free(win); win = malloc(sizeof(double) * W); wW = W; for (n = 0; n < W; n++) win[n] = 0.5 - 0.5 * cos(2 * M_PI * n / W); }
    for (n = 0; n < W; n++) ws += win[n];
    for (k = -4; k <= 4; k++) {
        double w = 2 * M_PI * (f + k * fr) / FS, re = 0, im = 0, cr = cos(w * n0), ci = -sin(w * n0), dr = cos(w), di = -sin(w), a;
        for (n = 0; n < W; n++) {
            double v = win[n] * y[n0 + n]; re += v * cr; im += v * ci;
            { double t = cr * dr - ci * di; ci = cr * di + ci * dr; cr = t; }
            if ((n & 1023) == 1023) { double m = 1.0 / sqrt(cr * cr + ci * ci); cr *= m; ci *= m; }
        }
        a = 2 * sqrt(re * re + im * im) / ws / 0.5;
        o[k + 4] = 20 * log10(a > 1e-3 ? a : 1e-3);
    }
}
static double *LB, *RB;
static int MASK = 3;
void setmask(int m) { MASK = m; }
double cost(const double *p, int verbose)
{
    double tot = 0; int c, k;
    if (!NK) { load(); LB = malloc(sizeof(double) * 400000); RB = malloc(sizeof(double) * 400000); }
    for (c = 0; c < NK; c++) {
        kase *q = &K[c]; double sl[9], sr[9], e = 0;
        if (q->fr < 2 && !verbose) continue;
        if (!verbose && !((MASK >> q->kind) & 1)) continue;
        render(p, q->kind, q->tone, q->fr, q->n0 + q->W, LB, RB);
        sbands(LB, q->tone, q->fr, q->n0, q->W, sl); sbands(RB, q->tone, q->fr, q->n0, q->W, sr);
        for (k = 0; k < 9; k++) {
            double dl_ = sl[k] - q->ref[k], dr_ = sr[k] - q->ref[9 + k];
            double wl = q->ref[k] > -45 ? 1 : 0.3, wr = q->ref[9 + k] > -45 ? 1 : 0.3;
            e += wl * dl_ * dl_ + wr * dr_ * dr_;
        }
        if (verbose) { printf("%d %5.0f Hz fr %.2f: rms dB err %.1f\n", q->kind, q->tone, q->fr, sqrt(e / 18)); }
        if (q->fr > 2) tot += e; /* the fast cases only */
    }
    return tot;
}
int ncases(void) { if (!NK) { load(); LB = malloc(sizeof(double) * 400000); RB = malloc(sizeof(double) * 400000); } return NK; }
void evalcase(const double *p, int c, double *o)
{
    kase *q = &K[c]; ncases();
    render(p, q->kind, q->tone, q->fr, q->n0 + q->W, LB, RB);
    sbands(LB, q->tone, q->fr, q->n0, q->W, o); sbands(RB, q->tone, q->fr, q->n0, q->W, o + 9);
}
