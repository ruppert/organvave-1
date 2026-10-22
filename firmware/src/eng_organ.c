/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1)
 * Ported from setBfree's tone generator (src/tonegen.c, src/vibrato.c, GPL-2.0-or-later; Copyright (C) 2003-2004
 * Fredrik Kilander, 2003-2004 Robin Gareus, 2003-2004 Ken Restivo, 2008-2015 Robin Gareus, 2010 Will Panther). */
/* ORGAN: the tone generator (ORGAN.md). setBfree's oscGenerateFragment rewritten in integer C, with its data
 * (eng_organ_tables.h, tools/gen_organ_tables.py):
 *   - 91 wheels, each a 32-bit DDS on the shared sine (setBfree: looped wavetables; the same frequencies). A wheel
 *     turns while a key holds it and keeps its phase while it rests, as setBfree's wavetable position does.
 *   - the keys' contributions (wheel, bus, level: tapering and, if on, crosstalk) summed per wheel and bus;
 *     the drawbars weigh the buses (setting / 8, linear).
 *   - setBfree's fragment of 128 samples: key events and drawbar changes act at a fragment's start; a wheel that
 *     comes on runs the attack envelope (the contact click), one that goes off the release, over the fragment; a
 *     wheel already sounding steps to its new level at the next fragment. The firmware's blocks are CTL (32)
 *     samples: a fragment is four of them.
 *   - key compression (the level per keys down, ramped over a fragment) and the swell (a one-pole, 25 Hz).
 *   - percussion (setBfree's HIPASS_PERCUSSION build): the wheels' contacts on the 4' (or 2 2/3') bus, whatever the
 *     drawbar, as a first difference, with an envelope that falls 60 dB in 1 s (fast) or 4 s (slow) from the
 *     starting level (normal 11.0, soft 5.5132) and resets when no key is down. On: the whole mix steps to the
 *     swell x 0.60512 (normal; soft 1.0) and the 1' is muted, as setBfree does.
 *   - the scanner vibrato / chorus (setBfree's vibrato.c): a 1024-sample delay line read at 1 + amp + amp * sin
 *     samples, amp 3 / 6 / 9, the scanner at 7.25 Hz; the chorus mixes it with the dry at 0.7071. It is applied to
 *     the whole organ: setBfree applies it to the upper manual only, and the upper manual is the only one played
 *     until 0.4.0 (then the lower and the pedals will be routed past it, as in setBfree).
 * Keys use setBfree's numbers: 0..60 the upper manual (C..C), 64..124 the lower, 128..159 the pedals; buses 0..8
 * upper (16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1'), 9..17 lower, 18..26 pedals.
 * The output is in setBfree's units: 1.0 (one wheel at full level) = 32768.
 * Formats: wheel x Q15, gains Q12 (up to 16.0 per wheel: about 61 keys on one wheel at full drawbars), levels Q18,
 * the percussion's envelope Q24 (the decay per block in Q16, the one 64-bit multiply per block). */
#include "eng_organ_tables.h"

#define ORG_NWHEEL 92u                  /* 1..91 (0 unused, as setBfree's) */
#define ORG_NBUS 27u
#define ORG_NKEY 160u
#define ORG_FRAG 128u
#define ORG_SUB (ORG_FRAG / CTL)        /* firmware blocks per fragment */
#define ORG_QLEN 64u                    /* key messages per fragment (more are dropped, as a full queue) */
#define ORG_GMAX 65535                  /* gain ceiling, Q12 */
#define ORG_PERC_NORM (11u << 24)       /* the percussion envelope's start, Q24: setBfree's 11.0 x 1.0 */
#define ORG_PERC_SOFT (22582u << 12)    /* 11.0 x 0.5012 = 5.5132, Q24 */
#define ORG_PERC_FAST 65208u            /* the decay per block (32 samples), Q16: 60 dB in 1 s (10^(-96/44100)) */
#define ORG_PERC_SLOW 65454u            /* the same, 60 dB in 4 s (10^(-24/44100)) */
#define ORG_PERC_GAIN 19829u            /* the mix's gain while the percussion is on, normal: 0.60512, Q15 */
#define ORG_VIB_INC 706122u             /* the scanner's phase step: 7.25 Hz at 44.1 kHz, 2^32 * 7.25 / 44100 */
#define ORG_VIB_N 1024u                 /* the delay line: 1024 samples (setBfree's BUF_SIZE) */
#define ORG_VIB_CLAMP 65535             /* the delay line's input: x >> 3, clamped */

enum { ORG_ADD = 1, ORG_MOD = 2, ORG_REM = 4 };   /* setBfree's rflags: OR_ADD / ORF_MODIFIED / OR_REM */

typedef struct {
    int32_t lvl[ORG_NBUS];              /* the bus levels (aot busLevel), Q18 */
    int32_t sum;                        /* the swell mix with the drawbars (aot sumSwell), Q12 */
    int32_t pn;                         /* the percussion's contact gain: the perc bus's level, Q12, 0 while off */
    int32_t gs, gd;                     /* this fragment: the start gain and the step the envelope scales, Q12 */
    int32_t pgs, pgd;                   /* the same for the percussion's contact gain */
    const uint16_t *env;                /* this fragment's envelope (attack or release), or 0 */
    uint32_t ph;
    int16_t refc;                       /* contributions holding it (aot refCount) */
    int8_t acl;                         /* its place on the active list, -1 off */
    uint8_t rflags;
} org_wheel_t;

typedef struct {
    /* settings (any context; read at a fragment's start) */
    uint8_t bars[ORG_NBUS];             /* drawbars 0..8 */
    uint8_t temp;                       /* 0 gear 60 Hz, 1 gear 50 Hz, 2 equal */
    uint8_t xtalk;                      /* 1 setBfree's crosstalk model, 0 none */
    uint8_t perc, psoft, pfast, psel;   /* percussion: on, soft, fast, the 3rd harmonic (2 2/3') */
    uint8_t vibm;                       /* vibrato: 0 off, 1..3 VIB1..3, 4..6 CHO1..3 */
    uint16_t swell;                     /* the swell pedal, Q15 (32768 = 1.0) */
    /* state (the audio path) */
    org_wheel_t w[ORG_NWHEEL];
    uint8_t acl[ORG_NWHEEL];            /* the active list (activeOscList) */
    uint8_t nacl;
    uint8_t ins[ORG_NWHEEL];            /* the fragment's wheels, in active-list order (the core program) */
    uint8_t nins;
    uint8_t sub;                        /* the block within the fragment, 0..3 */
    uint8_t keys[ORG_NKEY];             /* 0 up; 1 down with crosstalk, 2 down without (the table it was added with) */
    uint8_t ndown;                      /* keys down (keyDownCount; at most 61 + 61 + 32) */
    uint8_t adown;                      /* keys down on the audio side (the fragment's messages applied) */
    uint8_t bars_cur[ORG_NBUS], temp_cur;
    uint8_t perc_cur, psoft_cur, pfast_cur, psel_cur, vibm_cur;
    uint8_t q[ORG_QLEN], nq;            /* key messages: bit 0 on, bit 1 the crosstalk table (the key's own) */
    uint8_t qk[ORG_QLEN];
    int32_t kc, kcd;                    /* key compression level and its step per sample, Q23 */
    int32_t sw;                         /* the swell's current gain, Q24 */
    int32_t pdg;                        /* the mix's gain for the percussion, Q15 (ORG_PERC_GAIN normal, else 32768) */
    uint32_t pe;                        /* the percussion envelope, Q24 */
    int32_t pz;                         /* the percussion's previous contact sample (the first difference) */
    uint32_t vpos, vst;                 /* the vibrato's delay line position and scanner phase */
    int32_t vb[ORG_VIB_N];              /* the delay line, x >> 3 */
} org_t;

static org_t org;

static void org_init(void)
{
    uint32_t i;
    memset(&org, 0, sizeof org);
    for (i = 0; i < ORG_NWHEEL; i++)
        org.w[i].acl = -1;
    org.xtalk = 1;
    org.swell = 32768;
    org.kc = (int32_t)ORG_KEYCOMP[0] << 8;
    org.sw = 32768 << 9;
    org.pdg = 32768;
    org.pe = ORG_PERC_NORM;
}

/* a key goes down (on) or up: queued for the next fragment, as setBfree's oscKeyOn / oscKeyOff */
static void org_key(uint32_t k, uint32_t on)
{
    if (k >= ORG_NKEY || (k > 60u && k < 64u) || (k > 124u && k < 128u))
        return;
    if (on && org.keys[k])
        org_key(k, 0);                  /* down again: up first (oscKeyOn) */
    if (!on && !org.keys[k])
        return;
    if (org.nq >= ORG_QLEN)
        return;
    if (on) {
        org.keys[k] = (uint8_t)(org.xtalk ? 1 : 2);
        org.ndown++;
        org.q[org.nq] = (uint8_t)(1u | (org.xtalk ? 2u : 0u));
    } else {
        org.q[org.nq] = (uint8_t)(org.keys[k] == 1u ? 2u : 0u);     /* up with the table it went down with */
        org.keys[k] = 0;
        org.ndown--;
    }
    org.qk[org.nq++] = (uint8_t)k;
}

/* the key's contributions: *n table entries and the bus offset (the lower manual: + 9) */
static const uint32_t *org_contrib(uint32_t k, uint32_t xt, uint32_t *n, uint32_t *boff)
{
    const uint16_t *off = xt ? ORG_KC_OFF : ORG_KCX_OFF;
    const uint32_t *tab = xt ? ORG_KC : ORG_KCX;
    uint32_t tk = k < 128u ? k & 63u : k - 64u;               /* the lower manual reads the upper's entries */
    *boff = (k >= 64u && k < 128u) ? 9u : 0u;
    *n = off[tk + 1u] - off[tk];
    return tab + off[tk];
}

static __attribute__((noinline)) void org_msg(uint32_t k, uint32_t on, uint32_t xt)
{
    uint32_t n, boff, i;
    const uint32_t *c = org_contrib(k, xt, &n, &boff);
    for (i = 0; i < n; i++) {
        uint32_t e = c[i], wn = e >> 25, b = ((e >> 20) & 31u) + boff;
        int32_t lv = (int32_t)(e & 0xFFFFFu);
        org_wheel_t *w = &org.w[wn];
        if (on) {
            if (w->refc == 0) {
                w->rflags = ORG_ADD;
                if (w->acl < 0) {
                    w->acl = (int8_t)org.nacl;
                    org.acl[org.nacl++] = (uint8_t)wn;
                }
            } else {
                w->rflags |= ORG_MOD;
            }
            w->lvl[b] += lv;
            w->refc++;
        } else {
            w->lvl[b] -= lv;
            w->refc--;
            w->rflags = w->refc == 0 ? ORG_REM : (uint8_t)(w->rflags | ORG_MOD);
        }
    }
}

/* the start of a fragment: the key messages, the drawbars, the percussion and the vibrato, then each active wheel's
 * gains and envelope for the fragment (setBfree's message queue, activated list and removal list) */
static __attribute__((noinline)) void org_fragment(void)
{
    uint32_t i, d, chg = 0, nrem = 0;
    uint8_t rem[ORG_NWHEEL];
    int32_t kct;
    uint8_t pc = org.perc ? 1u : 0u, psel = org.psel ? 1u : 0u, vm = org.vibm <= 6u ? org.vibm : 0u;
    if (org.adown == 0u)                                             /* setBfree: no key down, the envelope starts over */
        org.pe = org.psoft ? ORG_PERC_SOFT : ORG_PERC_NORM;
    for (i = 0; i < org.nq; i++) {
        uint32_t on = org.q[i] & 1u;
        org_msg(org.qk[i], on, org.q[i] >> 1);
        if (on)
            org.adown++;
        else if (org.adown)
            org.adown--;
    }
    org.nq = 0;
    for (d = 0; d < ORG_NBUS; d++)
        if (org.bars_cur[d] != org.bars[d]) {
            org.bars_cur[d] = org.bars[d] > 8u ? 8u : org.bars[d];
            chg = 1;
        }
    if (org.perc_cur != pc || org.psel_cur != psel)
        chg = 1;
    if (pc && !org.perc_cur)
        org.pz = 0;                                                  /* the first difference starts at the onset */
    org.perc_cur = pc;
    org.psel_cur = psel;
    org.psoft_cur = org.psoft ? 1u : 0u;
    org.pfast_cur = org.pfast ? 1u : 0u;
    org.pdg = (pc && !org.psoft_cur) ? ORG_PERC_GAIN : 32768;
    if (vm != org.vibm_cur && !vm)
        memset(org.vb, 0, sizeof org.vb);                            /* off: the delay line starts clean */
    org.vibm_cur = vm;
    org.temp_cur = org.temp < 3u ? org.temp : 0u;
    org.nins = 0;
    for (i = 0; i < org.nacl; i++) {
        uint32_t wn = org.acl[i];
        org_wheel_t *w = &org.w[wn];
        org.ins[org.nins++] = (uint8_t)wn;
        if (w->rflags & ORG_REM) {
            rem[nrem++] = (uint8_t)wn;
            w->env = ORG_REL[i & 7u];
            w->gs = w->sum;
            w->gd = -w->sum;
            w->pgs = w->pn;
            w->pgd = -w->pn;
        } else {
            int32_t old = w->rflags & ORG_ADD ? 0 : w->sum;
            int32_t oldp = w->rflags & ORG_ADD ? 0 : w->pn;
            if ((w->rflags & ORG_MOD) || chg || (w->rflags & ORG_ADD)) {
                int32_t s = 0, pn;
                for (d = 0; d < ORG_NBUS; d++) {
                    int32_t bb = org.bars_cur[d];
                    if (d == 8u && org.perc_cur)
                        bb = 0;                                       /* the 1' is muted while the percussion is on */
                    s += w->lvl[d] * bb;                              /* Q18 * 0..8: under 2^31 for any 3 manuals */
                }
                s = (s + (1 << 8)) >> 9;                              /* / 8 (the drawbar law), Q18 -> Q12 */
                w->sum = s < 0 ? 0 : s > ORG_GMAX ? ORG_GMAX : s;
                pn = org.perc_cur ? w->lvl[org.psel_cur ? 4 : 3] >> 6 : 0;   /* the perc bus, whatever its drawbar */
                w->pn = pn < 0 ? 0 : pn > ORG_GMAX ? ORG_GMAX : pn;
            }
            w->gs = old;
            w->pgs = oldp;
            if (w->rflags & ORG_ADD) {
                w->env = ORG_ATK[i & 7u];
                w->gd = w->sum;
                w->pgd = w->pn;
            } else {
                w->env = 0;
                w->gd = 0;
                w->pgd = 0;
            }
        }
        w->rflags = 0;
    }
    for (i = 0; i < nrem; i++) {                                    /* the removal list: the last fills the hole */
        org_wheel_t *v = &org.w[rem[i]];
        uint32_t pos = (uint32_t)v->acl;
        v->acl = -1;
        org.nacl--;
        if (org.nacl) {
            uint32_t mv = org.acl[org.nacl];
            if (mv != rem[i]) {
                org.acl[pos] = (uint8_t)mv;
                org.w[mv].acl = (int8_t)pos;
            }
        }
    }
    kct = (int32_t)ORG_KEYCOMP[org.ndown < 128u ? org.ndown : 127u] << 8;
    org.kcd = (kct - org.kc) / (int32_t)ORG_FRAG;
}

/* the scanner's vibrato / chorus (setBfree's vibratoProc, in integer): the delay line holds x >> 3; a sample is
 * written at 1 + amp + amp * sin ahead of the read point, linear between the two cells, and read out when the
 * point reaches it. vibm 1..3 the vibrato (the wet alone), 4..6 the chorus (x + wet, at 0.7071). */
static __attribute__((noinline)) void org_vib(const int32_t *x, int32_t *o, uint32_t n)
{
    uint32_t i, amp = 3u * (((uint32_t)org.vibm_cur - 1u) % 3u + 1u);
    uint32_t mixed = org.vibm_cur > 3u;
    uint32_t pos = org.vpos, st = org.vst;
    for (i = 0; i < n; i++) {
        int32_t xs = clamp(x[i] >> 3, -ORG_VIB_CLAMP, ORG_VIB_CLAMP);
        int32_t m = sine_i(st);                                       /* the scanner, Q15 */
        uint32_t j = ((pos << 16) + ((1u + amp) << 16) + (uint32_t)((m * (int32_t)amp) << 1)) & 0x03FFFFFFu;
        uint32_t h = j >> 16, k = (h + 1u) & 1023u, f = (j & 0xFFFFu) >> 1;   /* the cell, the next, the fraction Q15 */
        int32_t g = (xs * (int32_t)f) >> 15;
        int32_t wet;
        org.vb[h] += xs - g;
        org.vb[k] += g;
        wet = org.vb[pos] * 8;
        org.vb[pos] = 0;
        o[i] = mixed ? ((((x[i] + wet) >> 5) * 23170) >> 10) : wet;  /* (x + wet) * 0.7071, Q15 */
        pos = (pos + 1u) & 1023u;
        st += ORG_VIB_INC;
    }
    org.vpos = pos;
    org.vst = st;
}

/* a wheel on the percussion's bus: its sine once per sample, into the organ (y) and the contact (p) with their own gains
 * (the same ramps as the organ's), and the phase advanced. Not inlined (tests/target_budget.py counts it) */
static __attribute__((noinline)) void org_wheel_pc(org_wheel_t *w, uint32_t inc, uint32_t off, uint32_t n, int32_t *y, int32_t *p)
{
    uint32_t ph = w->ph, i;
    int32_t g = w->gs, pg = w->pgs;
    if (w->env) {
        const uint16_t *e = w->env + off;
        int32_t gd = w->gd, pgd = w->pgd;
        for (i = 0; i < n; i++) {
            int32_t s = sine_i(ph), ee = (int32_t)e[i];
            y[i] += (s * (g + ((ee * gd) >> 15))) >> 12;
            p[i] += (s * (pg + ((ee * pgd) >> 15))) >> 12;
            ph += inc;
        }
    } else {
        for (i = 0; i < n; i++) {
            int32_t s = sine_i(ph);
            y[i] += (s * g) >> 12;
            p[i] += (s * pg) >> 12;
            ph += inc;
        }
    }
    w->ph = ph;
}

/* add n (= CTL) samples to out[] (not inlined: tests/target_budget.py counts it) */
static __attribute__((noinline)) void org_render(int32_t *out, uint32_t n)
{
    int32_t y[CTL], p[CTL], q[CTL], v[CTL];
    const int32_t *vv = y;
    uint32_t i, k, off;
    const uint32_t *winc;
    int32_t swt;
    if (org.sub == 0u)
        org_fragment();
    off = org.sub * CTL;
    org.sub = (uint8_t)((org.sub + 1u) % ORG_SUB);
    winc = ORG_WINC[org.temp_cur];
    for (i = 0; i < n; i++) {
        y[i] = 0;
        p[i] = 0;
    }
    for (k = 0; k < org.nins; k++) {
        uint32_t wn = org.ins[k];
        org_wheel_t *w = &org.w[wn];
        uint32_t ph = w->ph, inc = winc[wn - 1u];
        int32_t g = w->gs;
        if (w->pgs | w->pgd) {                                        /* on the percussion's bus: the sine once, both sums */
            org_wheel_pc(w, inc, off, n, y, p);
            continue;
        }
        if (w->env) {
            const uint16_t *e = w->env + off;
            int32_t gd = w->gd, *yp = y;
            for (i = n; i; i--) {
                int32_t gg = g + (((int32_t)*e++ * gd) >> 15);
                *yp++ += (sine_i(ph) * gg) >> 12;
                ph += inc;
            }
        } else if (g) {
            int32_t *yp = y;
            for (i = n; i; i--) {
                *yp++ += (sine_i(ph) * g) >> 12;
                ph += inc;
            }
        } else {
            ph += inc * n;
        }
        w->ph = ph;
    }
    if (org.vibm_cur) {                                               /* the scanner: the organ's sound, replaced by its wet */
        org_vib(y, v, n);
        vv = v;
    }
    if (org.perc_cur) {                                               /* setBfree's first difference, then the envelope */
        int32_t pz = org.pz, pe = (int32_t)(org.pe >> 12);            /* pe Q12 */
        for (i = 0; i < n; i++) {
            int32_t dd = pz - p[i];                                   /* p[i - 1] - p[i] */
            dd = clamp(dd, -(1 << 20), (1 << 20) - 1);
            pz = p[i];
            q[i] = ((dd >> 5) * pe) >> 7;                             /* d * pe, Q12 * Q12 scaled back */
        }
        org.pz = pz;
        org.pe = (uint32_t)(((uint64_t)org.pe * (org.pfast_cur ? ORG_PERC_FAST : ORG_PERC_SLOW)) >> 16);
    } else {
        for (i = 0; i < n; i++)
            q[i] = 0;
    }
    swt = (int32_t)((((uint32_t)org.swell * (uint32_t)org.pdg) >> 15) << 9);
    for (i = 0; i < n; i++) {
        int32_t m;
        org.sw += mulq16(swt - org.sw, ORG_SWELL_TC);
        org.kc += org.kcd;
        m = ((org.sw >> 9) * (org.kc >> 8)) >> 14;                  /* Q15 * Q15 -> Q16 */
        out[i] += mulq16(vv[i] + q[i], (uint32_t)m);
    }
}

/* ------------------------------------------------------------------ the patch --- */
/* The organ's own settings, a record of ORG_PSIZE 7-bit values (stored in the autosave and in user presets: their
 * meaning is fixed, web/EDITOR_PROTOCOL.md). The main loop writes it (org_set_patch), the audio path takes it at its
 * next block (org_block). */
#define ORG_PSIZE 64u
enum {
    OP_UB = 0,                  /* upper drawbars 16' .. 1', 0..8 (9 bytes) */
    OP_LB = 9,                  /* lower drawbars (9) */
    OP_PB = 18,                 /* pedal drawbars (9: setBfree's pedal buses) */
    OP_TEMP = 27,               /* tuning: 0 gear 60 Hz, 1 gear 50 Hz, 2 equal */
    OP_XTALK = 28,              /* crosstalk: 0 off, 1 setBfree's model */
    OP_PERC = 29,               /* percussion: 0 off, 1 on (version 0.2.0; 0 in older patches) */
    OP_PERC_SOFT = 30,          /* percussion: 0 normal (the swell x 0.60512), 1 soft */
    OP_PERC_FAST = 31,          /* percussion decay: 0 slow (4 s to -60 dB), 1 fast (1 s) */
    OP_PERC_SEL = 32,           /* the harmonic: 0 the 4' (second), 1 the 2 2/3' (third) */
    OP_VIB = 33,                /* scanner: 0 off, 1..3 VIB1..3, 4..6 CHO1..3 */
    OP_ROT = 34,                /* rotary speaker (version 0.3.0; 0 in older patches): 0 off, 1 on */
    OP_ROT_MODEL = 35,          /* 0 CLASSIC (setBfree's whirl.c), 1 LIGHT (the cheaper model) */
    OP_ROT_SPEED = 36,          /* 0 STOP, 1 SLOW (chorale), 2 FAST (tremolo) */
    OP_ROT_HORN = 37,           /* the horn's character: 0 normal, 1 sharp */
    OP_ROT_MIC = 38,            /* the mics' angle (the stereo width): 0 wide (180 degrees) .. 6 mono (0), 30 degrees a step */
    OP_ROT_BAL = 39,            /* horn level against the drum: 0..8, 4 even (1.5 dB a step, -6 .. +6 dB) */
    OP_DRV = 40,                /* drive (the preamp): 0 off, 1 on */
    OP_DRV_GAIN = 41,           /* its input gain: 0..12, 2 dB a step, 6 setBfree's default (3.5675) */
    OP_DRV_FAT = 42,            /* its character: 0..8, 0 setBfree's default; higher: more bass into, then out of, the curve */
    OP_NUM                      /* (43..63: reserved: 0 until then) */
};
static uint8_t org_patch[ORG_PSIZE];             /* main loop writes, then org_pgen */
static volatile uint8_t org_pgen;
static uint8_t org_pgen_seen = 0xFF;

static uint32_t org_pmax(uint32_t i)
{
    if (i < OP_TEMP)
        return 8u;
    if (i == OP_TEMP)
        return 2u;
    if (i == OP_XTALK || (i >= OP_PERC && i <= OP_PERC_SEL))
        return 1u;
    if (i == OP_VIB || i == OP_ROT_MIC)
        return 6u;
    if (i == OP_ROT || i == OP_ROT_MODEL || i == OP_ROT_HORN || i == OP_DRV)
        return 1u;
    if (i == OP_ROT_SPEED)
        return 2u;
    if (i == OP_ROT_BAL || i == OP_DRV_FAT)
        return 8u;
    if (i == OP_DRV_GAIN)
        return 12u;
    return 0u;
}
static void org_sanitize(uint8_t *v)
{
    uint32_t i;
    for (i = 0; i < ORG_PSIZE; i++)
        v[i] = (uint8_t)(v[i] > org_pmax(i) ? org_pmax(i) : v[i]);
}

/* the patch = v (ORG_PSIZE bytes, sanitized). Main loop */
static void org_set_patch(const uint8_t *v)
{
    uint8_t s[ORG_PSIZE];
    memcpy(s, v, ORG_PSIZE);
    org_sanitize(s);
    memcpy(org_patch, s, ORG_PSIZE);
    RING_PUBLISH();
    org_pgen++;
}
/* one byte of the patch (the UI's controls). Main loop */
static void org_set(uint32_t i, uint32_t v)
{
    if (i >= ORG_PSIZE)
        return;
    org_patch[i] = (uint8_t)(v > org_pmax(i) ? org_pmax(i) : v);
    RING_PUBLISH();
    org_pgen++;
}

#include "eng_organ_presets.h"          /* ORG_FACTORY, ORG_LEVEL, ORG_PRESETS: tools/gen_organ_presets.py */

/* ----------------------------------------------------------------- the engine --- */
/* The organ is not a voice engine: voice.c sends its keys to org_key and its render to org_track_render. The key
 * numbers: MIDI notes 36..96 (C2..C7) are the upper manual's 61 keys, as setBfree's default map */
static void org_block(struct track *t)          /* audio path, before the render: a new patch */
{
    uint32_t i;
    (void)t;
    if (org_pgen == org_pgen_seen)
        return;
    org_pgen_seen = org_pgen;
    for (i = 0; i < ORG_NBUS; i++)
        org.bars[i] = org_patch[OP_UB + i];
    org.temp = org_patch[OP_TEMP];
    org.xtalk = org_patch[OP_XTALK];
    org.perc = org_patch[OP_PERC];
    org.psoft = org_patch[OP_PERC_SOFT];
    org.pfast = org_patch[OP_PERC_FAST];
    org.psel = org_patch[OP_PERC_SEL];
    org.vibm = org_patch[OP_VIB];
}
static void org_note(uint32_t note, uint32_t on)
{
    if (note >= 36u && note <= 96u)
        org_key(note - 36u, on);
}
static void org_all_off(void)
{
    uint32_t k;
    for (k = 0; k < ORG_NKEY; k++)
        if (org.keys[k])
            org_key(k, 0);
}
/* the part's block: out[] (cleared by the caller) gets the organ, half setBfree's scale (one key at 888000000 peaks
 * near 46000, about one FMUG voice); returns 1 while anything sounds */
static uint32_t org_track_render(struct track *t, int32_t *out, uint32_t n)
{
    uint32_t i;
    org_block(t);
    if (!org.nins && !org.nq && !org.nacl)
        return 0;
    org_render(out, n);
    for (i = 0; i < n; i++)
        out[i] >>= 1;
    return 1;
}
static void org_note_on_unused(struct track *t, voice_t *v) { (void)t; (void)v; }
static void org_render_unused(struct track *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    (void)t; (void)v; (void)out; (void)n; (void)m;
}
#define ORG_NODESC {"-", F_INT, 0, 0, 0, 0, 0}
static const engine_t ENG_ORGAN = {
    .name = "ORGAN",
    .page_title = {"-", "-"},
    .edit = {ORG_NODESC, ORG_NODESC, ORG_NODESC, ORG_NODESC, ORG_NODESC, ORG_NODESC, ORG_NODESC, ORG_NODESC},
    .presets = ORG_PRESETS,
    .npresets = ORG_NFACTORY,
    .note_on = org_note_on_unused,
    .render = org_render_unused,
    .knob = {P_E0, P_E1, P_E2, P_E3},
};
