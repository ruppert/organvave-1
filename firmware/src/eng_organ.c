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
 *     samples, amp 3 / 6 / 9, the scanner at 7.25 Hz; the chorus mixes it with the dry at 0.7071. As setBfree
 *     (0.10.2): one scanner, fed by the upper and / or the lower manual by their own switches (OP_VIB_UP, OP_VIB_LO);
 *     the pedals and the percussion stay dry. The rest of the organ is dry; a patch with OP_VIB_ROUTE 0 routes both.
 * Keys use setBfree's numbers: 0..60 the upper manual (C..C), 64..124 the lower, 128..159 the pedals; buses 0..8
 * upper (16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1'), 9..17 lower, 18..26 pedals.
 * The output is in setBfree's units: 1.0 (one wheel at full level) = 32768.
 * ADVANCED EDIT (0.9.4): setBfree's other tone generator settings, from patch bytes 57..84 (the "zero is the default" coding):
 * the tuning, the attack and release click envelopes (rebuilt by org_env_build: setBfree's initEnvelopes with glibc's rand()
 * drawn from the same point), the crosstalk model (org_xt_build: setBfree's applyDefaultConfiguration + compilePlayMatrix in
 * integer, into a RAM table), the percussion's times, levels and buses, the scanner's rate and depths. The defaults are the
 * tables of eng_organ_tables.h as they were, so a patch of zeros sounds as before. Built in the main loop (org_adv_poll).
 * Formats: wheel x Q15, the click envelopes Q14 (0.9.5), gains Q12 (up to 16.0 per wheel: about 61 keys on one wheel at full drawbars), levels Q18,
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
#define ORG_XT_CAP 10752u               /* the RAM crosstalk table's entries (setBfree's default needs 10687) */
#define ORG_VIB_N 1024u                 /* the delay line: 1024 samples (setBfree's BUF_SIZE) */
#define ORG_VIB_CLAMP 65535             /* the delay line's input: x >> 3, clamped */

enum { ORG_ADD = 1, ORG_MOD = 2, ORG_REM = 4 };   /* setBfree's rflags: OR_ADD / ORF_MODIFIED / OR_REM */

typedef struct {
    int32_t lvl[ORG_NBUS];              /* the bus levels (aot busLevel), Q18 */
    int32_t sum;                        /* the swell mix with the drawbars (aot sumSwell), Q12 */
    int32_t pn;                         /* the percussion's contact gain: the perc bus's level, Q12, 0 while off */
    int32_t gs, gd;                     /* this fragment: the start gain and the step the envelope scales, Q12 */
    int32_t pgs, pgd;                   /* the same for the percussion's contact gain */
    int32_t ssum, sgs, sgd;             /* the part of sum that goes to the scanner (the manuals routed through it), and its start / step */
    const int16_t *env;                 /* this fragment's envelope (attack or release), or 0 */
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
    uint8_t vup, vlo;                   /* the upper / the lower manual routed through the scanner (setBfree's RT_UPPRVIB / RT_LOWRVIB); the pedals never */
    uint8_t xt_use;                     /* the table a new key takes when the crosstalk is on: 1 setBfree's default (flash), 2 the RAM one */
    uint8_t nram;                       /* keys down (or with a release not yet applied) on the RAM table: it is rebuilt only at 0 */
    int8_t ptrig;                       /* the drawbar the percussion mutes (setBfree's osc.perc.bus.trig; -1 none) */
    uint8_t pbusa, pbusb;               /* the percussion's buses: the 4' / 2 2/3' choice (osc.perc.bus.a / b) */
    uint8_t vamp2[3];                   /* the scanner's depth for VIB/CHO 1..3, in half samples (scanner.modulation.v1..v3 x 2) */
    uint32_t vinc;                      /* the scanner's phase step */
    uint32_t pe_norm, pe_soft;          /* the percussion envelope's start, Q24 (normal / soft) */
    uint16_t pdec_fast, pdec_slow;      /* its decay per block, Q16 */
    const int16_t (*atk)[128];          /* the attack / release envelopes in use (flash: setBfree's, or the RAM ones) */
    const int16_t (*rel)[128];
    const uint32_t (*wtab)[91];         /* the wheels' increments by temperament (flash, or scaled by the tuning) */
    uint16_t swell;                     /* the swell pedal, Q15 (32768 = 1.0) */
    uint32_t mg;                        /* the output volume (OP_EXPR), Q15: 10^(-i/20) (0.10.3) */
    /* state (the audio path) */
    org_wheel_t w[ORG_NWHEEL];
    uint8_t acl[ORG_NWHEEL];            /* the active list (activeOscList) */
    uint8_t nacl;
    uint8_t ins[ORG_NWHEEL];            /* the fragment's wheels, in active-list order (the core program) */
    uint8_t nins;
    uint8_t sub;                        /* the block within the fragment, 0..3 */
    uint8_t keys[ORG_NKEY];             /* 0 up; 1 down with crosstalk, 2 down without, 3 down with the RAM crosstalk (the table it was added with) */
    uint32_t kage[ORG_NKEY], kseq;      /* when each key went down (a counter): the overload releases the oldest */
    uint8_t ndown;                      /* keys down (keyDownCount; at most 61 + 61 + 32) */
    uint8_t adown;                      /* keys down on the audio side (the fragment's messages applied) */
    uint8_t bars_cur[ORG_NBUS], temp_cur;
    uint8_t perc_cur, psoft_cur, pfast_cur, psel_cur, vibm_cur;
    uint8_t vup_cur, vlo_cur, vrt_cur;  /* the routing as the fragment saw it; vrt: the scanner is in use (a type on and a manual routed) */
    uint16_t pbus_cur;                  /* the percussion's buses as the fragment saw them */
    uint8_t q[ORG_QLEN], nq;            /* key messages: bit 0 on, bits 1..2 the table (0 none, 1 default crosstalk, 2 RAM crosstalk; the key's own) */
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
/* ADVANCED EDIT's RAM: the crosstalk table (the layout of ORG_KC: ORG_XT_OFF[k] .. [k + 1]), the click envelopes, the scaled increments */
static uint32_t org_xt_tab[ORG_XT_CAP] __attribute__((section(".pool")));
static uint16_t org_xt_off[97] __attribute__((section(".pool")));
static int16_t org_atk_ram[8][128] __attribute__((section(".pool")));
static int16_t org_rel_ram[8][128] __attribute__((section(".pool")));
/* what the tables were built from (0: setBfree's defaults, the flash tables) */
static uint8_t org_b_env[8], org_b_xt[6], org_xt_valid;     /* org_b_env: the six click bytes 58..63, the attack gain (84), the release gain (85) */
static volatile uint16_t cpu_high;      /* overload: blocks left of "CPU HIGH" (the delay is off meanwhile; audio.c shed_voice arms it) */
static volatile uint8_t org_shed_req;   /* overload: release the oldest key at the next fragment (org_fragment) */

static void org_init(void)
{
    uint32_t i;
    memset(&org, 0, sizeof org);
    memset(org_b_env, 0, sizeof org_b_env);
    memset(org_b_xt, 0, sizeof org_b_xt);
    org_xt_valid = 0;
    for (i = 0; i < ORG_NWHEEL; i++)
        org.w[i].acl = -1;
    org.xtalk = 1;
    org.xt_use = 1;
    org.ptrig = 8;
    org.pbusa = 3;
    org.pbusb = 4;
    org.vamp2[0] = 6;
    org.vamp2[1] = 12;
    org.vamp2[2] = 18;
    org.vinc = ORG_VIB_INC;
    org.pe_norm = ORG_PERC_NORM;
    org.pe_soft = ORG_PERC_SOFT;
    org.pdec_fast = ORG_PERC_FAST;
    org.pdec_slow = ORG_PERC_SLOW;
    org.atk = ORG_ATK;
    org.rel = ORG_REL;
    org.wtab = ORG_WINC;
    org.pbus_cur = 3u | (4u << 4) | (9u << 8);
    org.swell = 32768;
    org.mg = 32768;                                         /* the output volume: full (OP_EXPR 0) */
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
        uint32_t tid = org.xtalk ? org.xt_use : 0u;                 /* 0 none, 1 default, 2 RAM */
        org.keys[k] = (uint8_t)(tid == 0u ? 2u : tid == 1u ? 1u : 3u);
        if (tid == 2u)
            org.nram++;
        org.ndown++;
        org.kage[k] = ++org.kseq;
        org.q[org.nq] = (uint8_t)(1u | (tid << 1));
    } else {
        uint32_t kv = org.keys[k];
        org.q[org.nq] = (uint8_t)(kv == 1u ? 2u : kv == 3u ? 4u : 0u);     /* up with the table it went down with */
        org.keys[k] = 0;
        org.ndown--;
    }
    org.qk[org.nq++] = (uint8_t)k;
}

/* the key's contributions: *n table entries and the bus offset (the lower manual: + 9) */
static const uint32_t *org_contrib(uint32_t k, uint32_t xt, uint32_t *n, uint32_t *boff)
{
    const uint16_t *off = xt == 2u ? org_xt_off : xt ? ORG_KC_OFF : ORG_KCX_OFF;
    const uint32_t *tab = xt == 2u ? org_xt_tab : xt ? ORG_KC : ORG_KCX;
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
        org.pe = org.psoft ? org.pe_soft : org.pe_norm;
    if (org_shed_req) {                                              /* CPU overload: the oldest key held lets go (never the last one) */
        uint32_t k, old = ORG_NKEY;
        org_shed_req = 0;
        if (org.ndown > 1u) {
            for (k = 0; k < ORG_NKEY; k++)
                if (org.keys[k] && (old == ORG_NKEY || org.kage[k] < org.kage[old]))
                    old = k;
            if (old < ORG_NKEY)
                org_key(old, 0);
        }
    }
    for (i = 0; i < org.nq; i++) {
        uint32_t on = org.q[i] & 1u;
        org_msg(org.qk[i], on, org.q[i] >> 1);
        if (on)
            org.adown++;
        else if (org.adown)
            org.adown--;
        if (!on && (org.q[i] >> 1) == 2u && org.nram)               /* the RAM table's key is let go: the table may be rebuilt */
            org.nram--;
    }
    org.nq = 0;
    for (d = 0; d < ORG_NBUS; d++)
        if (org.bars_cur[d] != org.bars[d]) {
            org.bars_cur[d] = org.bars[d] > 8u ? 8u : org.bars[d];
            chg = 1;
        }
    if (org.perc_cur != pc || org.psel_cur != psel)
        chg = 1;
    {
        uint16_t pb = (uint16_t)(org.pbusa | (org.pbusb << 4) | ((uint32_t)(org.ptrig + 1) << 8));
        if (pb != org.pbus_cur) {
            org.pbus_cur = pb;
            chg = 1;
        }
    }
    if (pc && !org.perc_cur)
        org.pz = 0;                                                  /* the first difference starts at the onset */
    org.perc_cur = pc;
    org.psel_cur = psel;
    org.psoft_cur = org.psoft ? 1u : 0u;
    org.pfast_cur = org.pfast ? 1u : 0u;
    org.pdg = (pc && !org.psoft_cur) ? ORG_PERC_GAIN : 32768;
    {
        uint8_t vu = org.vup ? 1u : 0u, vl = org.vlo ? 1u : 0u, vr = (vm && (vu || vl)) ? 1u : 0u;
        if (vu != org.vup_cur || vl != org.vlo_cur)
            chg = 1;                                                 /* the routing moved: every wheel's scanner share is new */
        if (vr != org.vrt_cur && !vr)
            memset(org.vb, 0, sizeof org.vb);                        /* the scanner is off: the delay line starts clean */
        org.vup_cur = vu;
        org.vlo_cur = vl;
        org.vrt_cur = vr;
    }
    org.vibm_cur = vm;
    org.temp_cur = org.temp < 3u ? org.temp : 0u;
    org.nins = 0;
    for (i = 0; i < org.nacl; i++) {
        uint32_t wn = org.acl[i];
        org_wheel_t *w = &org.w[wn];
        org.ins[org.nins++] = (uint8_t)wn;
        if (w->rflags & ORG_REM) {
            rem[nrem++] = (uint8_t)wn;
            w->env = org.rel[i & 7u];
            w->gs = w->sum;
            w->gd = -w->sum;
            w->pgs = w->pn;
            w->pgd = -w->pn;
            w->sgs = w->ssum;
            w->sgd = -w->ssum;
        } else {
            int32_t old = w->rflags & ORG_ADD ? 0 : w->sum;
            int32_t oldp = w->rflags & ORG_ADD ? 0 : w->pn;
            int32_t olds = w->rflags & ORG_ADD ? 0 : w->ssum;
            if ((w->rflags & ORG_MOD) || chg || (w->rflags & ORG_ADD)) {
                int32_t s = 0, sc = 0, pn;
                for (d = 0; d < ORG_NBUS; d++) {
                    int32_t bb = org.bars_cur[d];
                    if (org.perc_cur && (int32_t)d == org.ptrig)
                        bb = 0;                                       /* the 1' (setBfree's trigger bus) is muted while the percussion is on */
                    s += w->lvl[d] * bb;                              /* Q18 * 0..8: under 2^31 for any 3 manuals */
                    if ((d < 9u && org.vup_cur) || (d >= 9u && d < 18u && org.vlo_cur))
                        sc += w->lvl[d] * bb;                         /* the upper (0..8) or the lower (9..17) manual, if routed through the scanner */
                }
                s = (s + (1 << 8)) >> 9;                              /* / 8 (the drawbar law), Q18 -> Q12 */
                w->sum = s < 0 ? 0 : s > ORG_GMAX ? ORG_GMAX : s;
                sc = (sc + (1 << 8)) >> 9;
                w->ssum = sc < 0 ? 0 : sc > w->sum ? w->sum : sc;     /* the same law: with both manuals routed, ssum == sum */
                pn = org.perc_cur ? w->lvl[org.psel_cur ? org.pbusb : org.pbusa] >> 6 : 0;   /* the perc bus, whatever its drawbar */
                w->pn = pn < 0 ? 0 : pn > ORG_GMAX ? ORG_GMAX : pn;
            }
            w->gs = old;
            w->pgs = oldp;
            w->sgs = olds;
            if (w->rflags & ORG_ADD) {
                w->env = org.atk[i & 7u];
                w->gd = w->sum;
                w->pgd = w->pn;
                w->sgd = w->ssum;
            } else {
                w->env = 0;
                w->gd = 0;
                w->pgd = 0;
                w->sgd = 0;
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
    uint32_t i, amp2 = org.vamp2[((uint32_t)org.vibm_cur - 1u) % 3u], vinc = org.vinc;      /* the depth in half samples */
    uint32_t mixed = org.vibm_cur > 3u;
    uint32_t pos = org.vpos, st = org.vst;
    for (i = 0; i < n; i++) {
        int32_t xs = clamp(x[i] >> 3, -ORG_VIB_CLAMP, ORG_VIB_CLAMP);
        int32_t m = sine_i(st);                                       /* the scanner, Q15 */
        uint32_t j = ((pos << 16) + ((2u + amp2) << 15) + (uint32_t)(m * (int32_t)amp2)) & 0x03FFFFFFu;
        uint32_t h = j >> 16, k = (h + 1u) & 1023u, f = (j & 0xFFFFu) >> 1;   /* the cell, the next, the fraction Q15 */
        int32_t g = (xs * (int32_t)f) >> 15;
        int32_t wet;
        org.vb[h] += xs - g;
        org.vb[k] += g;
        wet = org.vb[pos] * 8;
        org.vb[pos] = 0;
        o[i] = mixed ? ((((x[i] + wet) >> 5) * 23170) >> 10) : wet;  /* (x + wet) * 0.7071, Q15 */
        pos = (pos + 1u) & 1023u;
        st += vinc;
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
        const int16_t *e = w->env + off;
        int32_t gd = w->gd, pgd = w->pgd;
        for (i = 0; i < n; i++) {
            int32_t s = sine_i(ph), ee = (int32_t)e[i];
            y[i] += (s * (g + ((ee * gd) >> 14))) >> 12;
            p[i] += (s * (pg + ((ee * pgd) >> 14))) >> 12;
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

/* a wheel's share of the scanner's input (the manuals routed through it): the same sine and gains as its share of y,
 * added to ys; ph is the wheel's phase at the block's start (the caller advances the wheel). Not inlined */
static __attribute__((noinline)) void org_wheel_scan(const org_wheel_t *w, uint32_t ph, uint32_t inc, uint32_t off, uint32_t n, int32_t *ys)
{
    uint32_t i;
    int32_t g = w->sgs;
    if (w->env) {
        const int16_t *e = w->env + off;
        int32_t gd = w->sgd;
        for (i = 0; i < n; i++) {
            int32_t gg = g + (((int32_t)e[i] * gd) >> 14);
            ys[i] += (sine_i(ph) * gg) >> 12;
            ph += inc;
        }
    } else {
        for (i = 0; i < n; i++) {
            ys[i] += (sine_i(ph) * g) >> 12;
            ph += inc;
        }
    }
}

/* add n (= CTL) samples to out[] (not inlined: tests/target_budget.py counts it) */
static __attribute__((noinline)) void org_render(int32_t *out, uint32_t n)
{
    int32_t y[CTL], p[CTL], q[CTL], v[CTL], ys[CTL];
    const int32_t *vv = y;
    uint32_t i, k, off;
    const uint32_t *winc;
    int32_t swt;
    uint8_t vr = org.vrt_cur;                                         /* the scanner is in use: ys takes the manuals routed through it */
    if (org.sub == 0u)
        org_fragment();
    off = org.sub * CTL;
    org.sub = (uint8_t)((org.sub + 1u) % ORG_SUB);
    winc = org.wtab[org.temp_cur];
    for (i = 0; i < n; i++) {
        y[i] = 0;
        p[i] = 0;
    }
    if (vr)
        for (i = 0; i < n; i++)
            ys[i] = 0;
    for (k = 0; k < org.nins; k++) {
        uint32_t wn = org.ins[k];
        org_wheel_t *w = &org.w[wn];
        uint32_t ph = w->ph, inc = winc[wn - 1u];
        int32_t g = w->gs;
        if (vr && (w->sgs | w->sgd))                                  /* the scanner's share, from the same phase */
            org_wheel_scan(w, ph, inc, off, n, ys);
        if (w->pgs | w->pgd) {                                        /* on the percussion's bus: the sine once, both sums */
            org_wheel_pc(w, inc, off, n, y, p);
            continue;
        }
        if (w->env) {
            const int16_t *e = w->env + off;
            int32_t gd = w->gd, *yp = y;
            for (i = n; i; i--) {
                int32_t gg = g + (((int32_t)*e++ * gd) >> 14);
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
    if (vr) {                                                         /* the scanner: its manuals' sound, replaced by its wet; the rest stays dry */
        org_vib(ys, v, n);
        for (i = 0; i < n; i++)
            v[i] += y[i] - ys[i];
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
        org.pe = (uint32_t)(((uint64_t)org.pe * (org.pfast_cur ? org.pdec_fast : org.pdec_slow)) >> 16);
    } else {
        for (i = 0; i < n; i++)
            q[i] = 0;
    }
    swt = (int32_t)(((((uint32_t)org.swell * (uint32_t)org.pdg) >> 15) * org.mg >> 15) << 9);   /* the output volume (EXPR) in the swell's target */
    for (i = 0; i < n; i++) {
        int32_t m;
        org.sw += mulq16(swt - org.sw, ORG_SWELL_TC);
        org.kc += org.kcd;
        m = ((org.sw >> 9) * (org.kc >> 8)) >> 14;                  /* Q15 * Q15 -> Q16 */
        out[i] += mulq16(vv[i] + q[i], (uint32_t)m);
    }
}

/* the output volume (OP_EXPR): 10^(-i/20) in Q15, i = 0..60 dB down (0.10.3) */
static const uint32_t ORG_EXPR_Q15[61] = {32768, 29205, 26029, 23198, 20675, 18427, 16423, 14637, 13045, 11627, 10362, 9235, 8231, 7336, 6538, 5827, 5193, 4629, 4125, 3677, 3277, 2920, 2603, 2320, 2068, 1843, 1642, 1464, 1305, 1163, 1036, 924, 823, 734, 654, 583, 519, 463, 413, 368, 328, 292, 260, 232, 207, 184, 164, 146, 130, 116, 104, 92, 82, 73, 65, 58, 52, 46, 41, 37, 33};

/* ------------------------------------------------------------------ the patch --- */
/* The organ's own settings, a record of ORG_PSIZE 7-bit values (stored in the autosave and in user presets: their
 * meaning is fixed, web/EDITOR_PROTOCOL.md). The main loop writes it (org_set_patch), the audio path takes it at its
 * next block (org_block). */
#define ORG_PSIZE 96u
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
    OP_ROT_MODEL = 35,          /* 0 CLASSIC (setBfree's whirl.c), 1 LIGHT (the cheaper model), 2 CLASSIC 2 (0.10.6: A+B), 3 CLASSIC 3 (D) */
    OP_ROT_SPEED = 36,          /* 0 STOP, 1 SLOW (chorale), 2 FAST (tremolo) */
    OP_ROT_HORN = 37,           /* the horn's character: 0 normal, 1 sharp */
    OP_ROT_MIC = 38,            /* the mics' angle (the stereo width): 0 wide (180 degrees) .. 6 mono (0), 30 degrees a step */
    OP_ROT_BAL = 39,            /* horn level against the drum: 0..8, 4 even (1.5 dB a step, -6 .. +6 dB) */
    OP_DRV = 40,                /* drive (the preamp): 0 off, 1 on */
    OP_DRV_GAIN = 41,           /* its input gain: 0..12, 2 dB a step, 6 setBfree's default (3.5675) */
    OP_DRV_FAT = 42,            /* unused since 0.9.2 (the character went); stored 0..8 in older patches, ignored */
    OP_EQ = 43,                 /* the equaliser (version 0.5.0; 0 in older patches): 0 off, 1 on */
    OP_EQ_LOW = 44,             /* LOW (120 Hz shelf), -12 .. +12 dB: 0 flat, 1..12 dB up, 13..24 dB down (s - 25) */
    OP_EQ_MID = 45,             /* MID (the peak), the same coding */
    OP_EQ_FREQ = 46,            /* the peak's frequency, 0..11: 0 900 Hz, then 1280 Hz .. 7.2 kHz, 7 is 160 Hz .. 11 is 640 Hz */
    OP_EQ_HIGH = 47,            /* HIGH (4 kHz shelf), the same coding as LOW */
    OP_DRV_TYPE = 48,           /* the drive's type (version 0.9.2; 0 in older patches): 0 DISTORTION (setBfree's preamp), 1 TUBE (a valve's soft saturation) */
    OP_DRV_TONE = 49,           /* its tone: 0..8 "zero is the default" (c9: index (s + 4) % 9, 4 flat, 0 dark .. 8 bright) */
    OP_DRV_OUT = 50,            /* its output level, the same coding: index 4 is 0 dB, 2 dB a step (-8 .. +8 dB) */
    OP_ROT_HSLOW = 51,          /* the rotary's horn SLOW speed (version 0.9.2; 0 in older patches), c9: index 4 is 40 rpm, 12 % a step */
    OP_ROT_HFAST = 52,          /* horn FAST speed, c9: index 4 is 390 rpm (since 0.9.3; setBfree's 423) */
    OP_ROT_DSLOW = 53,          /* drum SLOW speed, c9: index 4 is 36 rpm */
    OP_ROT_DFAST = 54,          /* drum FAST speed, c9: index 4 is 357 rpm */
    OP_ROT_HRAMP = 55,          /* horn acceleration time (and deceleration, twice it), c9: index 4 is 1.5 s (since 0.9.3; setBfree's 0.161 s), x1.4 a step (higher: slower) */
    OP_ROT_DRAMP = 56,          /* drum acceleration time, c9: index 4 is 4.1 s, x1.4 a step */
    /* ADVANCED EDIT (version 0.9.4; 0 in older patches = setBfree's defaults). Every byte is a step count N and a default index D
     * (ORG_ADV below): the index is (byte + D) % N, so a patch of zeros is the default. */
    OP_TUNING = 57,             /* osc.tuning: 408.0 .. 471.5 Hz in 0.5 Hz steps, 440 the default */
    OP_ATK_MODEL = 58,          /* osc.attack.model: 0 CLICK (default), 1 COSINE, 2 LINEAR, 3 SHELF */
    OP_ATK_LEVEL = 59,          /* osc.attack.click.level: 0 .. 200 % in 5 % steps, 100 % the default (0.9.5: twice setBfree's 50 %) */
    OP_ATK_MIN = 60,            /* osc.attack.click.minlength: the shortest click burst, 0 .. 100 % of a fragment (128 samples), 12.5 % */
    OP_ATK_MAX = 61,            /* osc.attack.click.maxlength: the longest, 62.5 % */
    OP_REL_MODEL = 62,          /* osc.release.model: 0 CLICK (default since 0.9.5; setBfree's is LINEAR), 1 COSINE, 2 LINEAR, 3 SHELF */
    OP_REL_LEVEL = 63,          /* osc.release.click.level: 0 .. 100 % in 2.5 % steps, 50 % the default (0.9.5: twice setBfree's 25 %; acts with the CLICK model) */
    OP_XT_COMP = 64,            /* osc.compartment-crosstalk: 0 off, then -100 .. 0 dB in 2 dB steps, -40 dB (0.01) the default */
    OP_XT_TRANS = 65,           /* osc.transformer-crosstalk: the same ladder, off the default */
    OP_XT_TSTRIP = 66,          /* osc.terminalstrip-crosstalk: the same ladder, -40 dB */
    OP_XT_WIRE = 67,            /* osc.wiring-crosstalk: the same ladder, -40 dB */
    OP_XT_FLOOR = 68,           /* osc.contribution-floor: the same ladder, -96 dB (0.0000158) the default */
    OP_XT_MIN = 69,             /* osc.contribution-min: the same ladder, off the default */
    OP_PC_FAST = 70,            /* osc.perc.fast: the fast decay, 0.1 .. 10.0 s (60 dB) in 0.1 s steps, 1.0 s the default */
    OP_PC_SLOW = 71,            /* osc.perc.slow: the slow decay, 4.0 s the default */
    OP_PC_NORM = 72,            /* osc.perc.normal: the starting gain, 0 .. -12 dB in 1 dB steps, 0 dB the default */
    OP_PC_SOFT = 73,            /* osc.perc.soft: the soft one, -6 dB (0.5012) the default */
    OP_PC_GAIN = 74,            /* osc.perc.gain: the basic volume, 0 .. 22 in 0.5 steps, 11.0 the default */
    OP_PC_BUSA = 75,            /* osc.perc.bus.a: the drawbar the 4' tone is taken from, 0 (16') .. 8 (1'); 3 (4') the default */
    OP_PC_BUSB = 76,            /* osc.perc.bus.b: the 2 2/3' tone's, 4 the default */
    OP_PC_TRIG = 77,            /* osc.perc.bus.trig: the drawbar muted while the percussion is on: none or 0 .. 8; 8 (1') the default */
    OP_SC_HZ = 78,              /* scanner.hz: the vibrato scanner, 4.0 .. 22.0 Hz in 0.25 Hz steps, 7.25 Hz the default */
    OP_SC_V1 = 79,              /* scanner.modulation.v1: VIB1 / CHO1 depth, 0 .. 12 in 0.5 steps, 3.0 the default */
    OP_SC_V2 = 80,              /* scanner.modulation.v2: 6.0 the default */
    OP_SC_V3 = 81,              /* scanner.modulation.v3: 9.0 the default */
    /* version 0.9.5 */
    OP_SPLIT = 82,              /* the keyboard split: 0 off (default), 1 on: the notes of the UPPER channel below SPLIT POINT play the lower manual and the pedals */
    OP_SPLIT_PT = 83,           /* the split point: the lowest note of the upper manual, MIDI 36 .. 96, G3 (55) the default */
    OP_CLICK_GAIN = 84,        /* the attack click's gain (0.9.6, the CLICK model): its burst's samples, 0 .. 200 % in 5 % steps, 100 % the default */
    OP_REL_GAIN = 85,          /* the release click's gain (0.9.6, the CLICK model): the same, for the release's burst */
    OP_VIB_UP = 86,            /* (0.10.2) the upper manual through the scanner: 0 no, 1 yes; read only when OP_VIB_ROUTE is 1 */
    OP_VIB_LO = 87,            /* (0.10.2) the lower manual through the scanner: 0 no, 1 yes; read only when OP_VIB_ROUTE is 1 */
    OP_VIB_ROUTE = 88,         /* (0.10.2) 1: OP_VIB_UP and OP_VIB_LO are set; 0 (older patches): both manuals, as before. The pedals never */
    OP_EXPR = 89,              /* (0.10.3) the output volume, HOME's EXPR knob: 0 full, i = i dB down (0..60); 0 in older patches */
    OP_NUM                      /* (90..95: reserved: 0 until then) */
};
/* ADVANCED EDIT's bytes: steps N and the default index D (the index is (byte + D) % N); the temperament and the crosstalk switch
 * are the older bytes 27 and 28 (N 3, D 0 and N 2, D 1: they keep their coding) */
typedef struct { uint8_t op, n, d; } org_adv_t;
static const org_adv_t ORG_ADV[] = {
    {OP_TUNING, 128, 64}, {OP_ATK_MODEL, 4, 0}, {OP_ATK_LEVEL, 41, 20}, {OP_ATK_MIN, 41, 5}, {OP_ATK_MAX, 41, 25},
    {OP_REL_MODEL, 4, 0}, {OP_REL_LEVEL, 41, 20}, {OP_XT_COMP, 52, 31}, {OP_XT_TRANS, 52, 0}, {OP_XT_TSTRIP, 52, 31},
    {OP_XT_WIRE, 52, 31}, {OP_XT_FLOOR, 52, 3}, {OP_XT_MIN, 52, 0}, {OP_PC_FAST, 100, 9}, {OP_PC_SLOW, 100, 39},
    {OP_PC_NORM, 13, 0}, {OP_PC_SOFT, 13, 6}, {OP_PC_GAIN, 45, 22}, {OP_PC_BUSA, 9, 3}, {OP_PC_BUSB, 9, 4},
    {OP_PC_TRIG, 10, 9}, {OP_SC_HZ, 73, 13}, {OP_SC_V1, 25, 6}, {OP_SC_V2, 25, 12}, {OP_SC_V3, 25, 18},
    {OP_SPLIT, 2, 0}, {OP_SPLIT_PT, 61, 19}, {OP_CLICK_GAIN, 41, 20}, {OP_REL_GAIN, 41, 20},
};
#define ORG_NADV (sizeof ORG_ADV / sizeof ORG_ADV[0])
static const org_adv_t *org_adv_of(uint32_t op)
{
    uint32_t i;
    for (i = 0; i < ORG_NADV; i++)
        if (ORG_ADV[i].op == op)
            return &ORG_ADV[i];
    return 0;
}
/* the index of a byte (and back) */
static uint32_t org_ci(uint32_t op, uint32_t byte)
{
    const org_adv_t *a = org_adv_of(op);
    uint32_t i = byte + a->d;
    return i >= a->n ? i - a->n : i;            /* (no division: the audio path reads these too) */
}
static uint32_t org_bi(uint32_t op, uint32_t idx)
{
    const org_adv_t *a = org_adv_of(op);
    uint32_t i = idx + a->n - a->d;
    return i >= a->n ? i - a->n : i;
}
/* the "zero is the default" coding of the 9-step bytes (the default in the middle, so a patch of zeros is the default): the
 * index 0..8 the panel shows, and the byte for an index */
static inline uint32_t org_c9(uint32_t s) { return (s + 4u) % 9u; }
static inline uint32_t org_b9(uint32_t idx) { return (idx + 5u) % 9u; }
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
    if (i == OP_VIB_UP || i == OP_VIB_LO || i == OP_VIB_ROUTE)
        return 1u;
    if (i == OP_EXPR)
        return 60u;
    if (i == OP_ROT || i == OP_ROT_HORN || i == OP_DRV)
        return 1u;
    if (i == OP_ROT_MODEL)
        return 3u;
    if (i == OP_ROT_SPEED)
        return 2u;
    if (i == OP_ROT_BAL || i == OP_DRV_FAT || (i >= OP_DRV_TONE && i <= OP_ROT_DRAMP))
        return 8u;
    if (i == OP_DRV_TYPE)
        return 1u;
    if (i == OP_DRV_GAIN)
        return 12u;
    if (i == OP_EQ)
        return 1u;
    if (i == OP_EQ_LOW || i == OP_EQ_MID || i == OP_EQ_HIGH)
        return 24u;
    if (i == OP_EQ_FREQ)
        return 11u;
    if (i >= OP_TUNING && i < OP_VIB_UP)
        return org_adv_of(i)->n - 1u;
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


/* ------------------------------------------------------------ ADVANCED EDIT --- */
/* The settings whose tables are built in the main loop (org_adv_poll), none of it in the audio path: the click envelopes, the
 * crosstalk table, the tuning. Their defaults (all bytes 0) are the flash tables of eng_organ_tables.h, which these builders
 * reproduce to within a few LSB (tests/organ_adv_test.c checks them against setBfree's own tonegen.c). */
static int32_t org_xt_acc[92][9] __attribute__((section(".pool")));       /* the key being built: wheel x bus, Q24 */

/* a % b without a divide (the build runs a few dozen of them) */
static uint32_t org_umod(uint32_t a, uint32_t b)
{
    uint32_t r = 0, i;
    for (i = 32; i; i--) {
        r = (r << 1) | (a >> 31);
        a <<= 1;
        if (r >= b)
            r -= b;
    }
    return r;
}

/* glibc's rand(), continued from the point setBfree's initEnvelopes starts (ORG_RAND_RING: srand(1), the wavetables' dither done) */
typedef struct { uint32_t r[31]; uint32_t c; } org_rnd_t;
static uint32_t org_rnd(org_rnd_t *g)
{
    uint32_t i0 = g->c, i3 = g->c + 28u, v;
    if (i3 >= 31u)
        i3 -= 31u;
    v = g->r[i0] + g->r[i3];
    g->r[i0] = v;
    g->c = i0 + 1u >= 31u ? 0u : i0 + 1u;
    return v >> 1;
}

/* a burst envelope in Q20 (zeros to `start`, `burst` samples of 1 - level * rand, then 1.0), smoothed by setBfree's two-point
 * average, to Q15 */
static void org_env_burst(org_rnd_t *g, int16_t *out, uint32_t start, uint32_t burst, int32_t lvq15, int32_t gq14, int32_t sgn)
{
    int32_t x[128], y = 0;
    uint32_t i;
    for (i = 0; i < 128u; i++) {
        if (i < start)
            x[i] = 0;
        else if (i < start + burst)
            x[i] = (1 << 20) - (int32_t)(((int64_t)lvq15 * (int32_t)(org_rnd(g) >> 11)) >> 15);
        else
            x[i] = 1 << 20;
    }
    for (i = 0; i < 128u; i++) {
        y = i ? (y + x[i]) >> 1 : x[0] >> 1;
        out[i] = (int16_t)clamp((y + 32) >> 6, -32768, 32767);        /* Q14 (16384 = 1.0; a level above 1 dips below zero) */
    }
    /* the click gain (0.9.5): the burst's samples scaled, the envelope's steady parts untouched */
    if (gq14 == 16384)
        return;
    if (gq14 > 16384) {                                                    /* above 100 %: a bump of 4 e (1 - e) on the burst (0 at 0 and at 1.0: no step at either end) */
        for (i = start; i < start + burst && i < 128u; i++) {
            int64_t e = out[i], bump = (4 * e * (16384 - e)) >> 14;
            out[i] = (int16_t)clamp(e + sgn * ((((int64_t)gq14 - 16384) * bump) >> 14), -32768, 32767);   /* (the release's gate is 1 - e: its bump goes down) */
        }
        return;
    }
    for (i = start; i < start + burst && i < 128u; i++)                    /* below 100 %: scaled (0 silences the click) */
        out[i] = (int16_t)clamp(((int32_t)out[i] * gq14) >> 14, -32768, 32767);
    for (i = start + burst; i < start + burst + 8u && i < 128u; i++) {    /* and faded back over 8 samples: no step at the burst's end */
        int32_t f = 16384 + (((gq14 - 16384) * (int32_t)(start + burst + 8u - i)) >> 3);
        out[i] = (int16_t)clamp(((int32_t)out[i] * f) >> 14, -32768, 32767);
    }
}
static void org_env_shelf(org_rnd_t *g, int16_t *out, uint32_t bound)
{
    uint32_t start = org_umod(org_rnd(g), bound), i;
    if (start > 126u)
        start = 126u;
    for (i = 0; i < 128u; i++)
        out[i] = (int16_t)(i < start ? 0 : i == start ? 5461 : i == start + 1u ? 10923 : 16384);
}
/* setBfree's initEnvelopes for the eight envelopes the organ uses, with the ADVANCED EDIT settings p[OP_ATK_MODEL .. OP_REL_LEVEL] */
static void org_env_build(const uint8_t *p)
{
    org_rnd_t g;
    uint32_t am = org_ci(OP_ATK_MODEL, p[OP_ATK_MODEL]), rm = org_ci(OP_REL_MODEL, p[OP_REL_MODEL]), b, i;
    int32_t alq = (int32_t)((org_ci(OP_ATK_LEVEL, p[OP_ATK_LEVEL]) << 13) / 5u);       /* 5 % steps: n / 20 in Q15 (0 .. 2.0) */
    int32_t rlq = (int32_t)((org_ci(OP_REL_LEVEL, p[OP_REL_LEVEL]) << 12) / 5u);
    int32_t cgq = (int32_t)((org_ci(OP_CLICK_GAIN, p[OP_CLICK_GAIN]) << 14) / 20u);   /* the attack click's gain: n / 20 in Q14 (0 .. 2.0) */
    int32_t rgq = (int32_t)((org_ci(OP_REL_GAIN, p[OP_REL_GAIN]) << 14) / 20u);       /* the release click's gain, the same */
    uint32_t mn = (org_ci(OP_ATK_MIN, p[OP_ATK_MIN]) * 16u) / 5u;                       /* (int)(128 * n / 40) */
    uint32_t mx = (org_ci(OP_ATK_MAX, p[OP_ATK_MAX]) * 16u) / 5u;
    uint32_t bound = mx > mn ? mx - mn : 1u;
    memcpy(g.r, ORG_RAND_RING, sizeof g.r);
    g.c = 0;
    for (b = 0; b < 8u; b++) {
        int16_t *a = org_atk_ram[b], *r = org_rel_ram[b];
        if (am == 0u) {
            uint32_t burst = mn + org_umod(org_rnd(&g), bound), start;
            if (burst >= 128u)
                burst = 127u;
            start = org_umod(org_rnd(&g), 128u - burst);
            org_env_burst(&g, a, start, burst, alq, cgq, 1);
        } else if (am == 3u) {
            org_env_shelf(&g, a, bound);
        }
        if (rm == 3u) {
            org_env_shelf(&g, r, bound);
        } else if (rm == 0u) {
            uint32_t burst = 8u + org_umod(org_rnd(&g), 32u), start = org_umod(org_rnd(&g), 128u - burst);
            org_env_burst(&g, r, start, burst, rlq, rgq, -1);
        }
        if (am == 1u)                                               /* the sigmoid: 0.5 + 0.5 cos(pi (127 - i) / 127) */
            for (i = 0; i < 128u; i++)
                a[i] = (int16_t)clamp((32768 + sine_i((127u - i) * 16909320u + 0x40000000u)) >> 2, 0, 16384);
        else if (am == 2u)
            for (i = 0; i < 128u; i++)
                a[i] = (int16_t)(i << 7);
        if (rm == 1u)                                               /* 0.5 - 0.5 cos(pi i / 127) */
            for (i = 0; i < 128u; i++)
                r[i] = (int16_t)clamp((32768 - sine_i(i * 16909320u + 0x40000000u)) >> 2, 0, 16384);
        else if (rm == 2u)
            for (i = 0; i < 128u; i++)
                r[i] = (int16_t)(i << 7);
    }
}


/* ---- the crosstalk model: setBfree's applyDefaultConfiguration + compilePlayMatrix, in integer ----
 * A key's taper (the entries of ORG_KCX: terminal, bus, level) gives the signal each of its buses takes from a terminal; the
 * key's wiring crosstalk adds, to each bus, the other buses' signals divided by the distance; and every terminal reaches
 * its own wheel (1 - compartment) and the pair's, the transformer row's and the terminal strip's neighbours. */
static void org_xt_add(uint32_t wheel, uint32_t b, int32_t lev, int32_t mix)
{
    if (wheel && mix > 0)
        org_xt_acc[wheel][b] += (int32_t)(((int64_t)lev * mix) >> 24);
}
static void org_xt_term(uint32_t term, uint32_t b, int32_t lev, int32_t comp, int32_t trans, int32_t tstrip)
{
    org_xt_add(term, b, lev, (1 << 24) - comp);
    if (comp)
        org_xt_add(ORG_XT_PAIR[term], b, lev, comp);
    if (trans) {
        org_xt_add(ORG_XT_TRE[term], b, lev, trans);
        org_xt_add(ORG_XT_TRW[term], b, lev, trans);
    }
    if (tstrip) {
        org_xt_add(ORG_XT_TSE[term], b, lev, tstrip);
        org_xt_add(ORG_XT_TSW[term], b, lev, tstrip);
    }
}
/* the build; returns the floor it ended with (Q24; higher than asked when the table would not fit, ORG_XT_CAP) */
static uint32_t org_xt_build(const uint8_t *p)
{
    static const int32_t RD[9] = {0, 65536, 32768, 21845, 16384, 13107, 10923, 9362, 8192};     /* 2^16 / distance */
    int32_t comp = (int32_t)ORG_XT_G[org_ci(OP_XT_COMP, p[OP_XT_COMP])], trans = (int32_t)ORG_XT_G[org_ci(OP_XT_TRANS, p[OP_XT_TRANS])];
    int32_t tstrip = (int32_t)ORG_XT_G[org_ci(OP_XT_TSTRIP, p[OP_XT_TSTRIP])], wire = (int32_t)ORG_XT_G[org_ci(OP_XT_WIRE, p[OP_XT_WIRE])];
    int32_t fl = (int32_t)ORG_XT_G[org_ci(OP_XT_FLOOR, p[OP_XT_FLOOR])], mn = (int32_t)ORG_XT_G[org_ci(OP_XT_MIN, p[OP_XT_MIN])];
    uint32_t attempt;
    memset(org_xt_acc, 0, sizeof org_xt_acc);
    for (attempt = 0; attempt < 24u; attempt++) {
        uint32_t kk, n = 0, over = 0;
        for (kk = 0; kk < 96u && !over; kk++) {
            uint32_t j, nt = 0, w, b, base = kk < 64u ? 0u : 18u;
            uint32_t tt[9], tb[9];
            int32_t tl[9];
            org_xt_off[kk] = (uint16_t)n;
            if (kk > 60u && kk < 64u)
                continue;
            for (j = ORG_KCX_OFF[kk]; j < ORG_KCX_OFF[kk + 1u] && nt < 9u; j++, nt++) {
                uint32_t e = ORG_KCX[j];
                tt[nt] = e >> 25;
                tb[nt] = ((e >> 20) & 31u) - base;
                tl[nt] = (int32_t)(e & 0xFFFFFu) << 6;                      /* Q18 -> Q24 */
            }
            for (j = 0; j < nt; j++)
                org_xt_term(tt[j], tb[j], tl[j], comp, trans, tstrip);
            if (wire && kk < 64u)                                          /* (the manuals only: no model for the pedals) */
                for (b = 0; b < 9u; b++)
                    for (j = 0; j < nt; j++)
                        if (tb[j] != b) {
                            uint32_t d = tb[j] > b ? tb[j] - b : b - tb[j];
                            int32_t xl = (int32_t)(((((int64_t)wire * tl[j]) >> 24) * RD[d]) >> 16);
                            org_xt_term(tt[j], b, xl, comp, trans, tstrip);
                        }
            for (w = 1; w < 92u; w++)
                for (b = 0; b < 9u; b++) {
                    int32_t v = org_xt_acc[w][b];
                    if (!v)
                        continue;
                    org_xt_acc[w][b] = 0;
                    if (v < fl)
                        continue;
                    if (v < mn)
                        v = mn;
                    v = (v + 32) >> 6;                                       /* Q24 -> Q18 */
                    if (v > 0xFFFFF)
                        v = 0xFFFFF;
                    if (v <= 0)
                        continue;
                    if (n >= ORG_XT_CAP) {
                        over = 1;
                        continue;
                    }
                    org_xt_tab[n++] = ((uint32_t)w << 25) | ((base + b) << 20) | (uint32_t)v;
                }
        }
        if (!over) {
            org_xt_off[96] = (uint16_t)n;
            return (uint32_t)fl;
        }
        memset(org_xt_acc, 0, sizeof org_xt_acc);
        fl += (fl >> 1) + 64;                                               /* full: a higher floor (the faintest entries go first) */
    }
    org_xt_off[96] = 0;
    return (uint32_t)fl;
}

/* Main loop, every pass: the tables the patch asks for. The RAM crosstalk table is rebuilt only while no key holds it (a key
 * subtracts, on release, what it added); until then new keys keep the table that is there */
static void org_adv_poll(void)
{
    static const uint8_t ZERO[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t *p = org_patch;
    if (memcmp(p + OP_ATK_MODEL, org_b_env, 6) || org_b_env[6] != p[OP_CLICK_GAIN] || org_b_env[7] != p[OP_REL_GAIN]) {
        memcpy(org_b_env, p + OP_ATK_MODEL, 6);
        org_b_env[6] = p[OP_CLICK_GAIN];
        org_b_env[7] = p[OP_REL_GAIN];
        if (memcmp(org_b_env, ZERO, 8)) {
            org_env_build(p);
            org.atk = org_atk_ram;
            org.rel = org_rel_ram;
        } else {
            org.atk = ORG_ATK;
            org.rel = ORG_REL;
        }
    }
    if (!memcmp(p + OP_XT_COMP, ZERO, 6)) {
        org.xt_use = 1;                                                    /* setBfree's defaults: the flash table */
    } else if (org_xt_valid && !memcmp(p + OP_XT_COMP, org_b_xt, 6)) {
        org.xt_use = 2;
    } else {
        uint32_t free_;
        fm1_irq_off();
        free_ = org.nram == 0u;
        if (free_)
            org.xt_use = 1;                                                /* (new keys take the flash table while it is built) */
        fm1_irq_on();
        if (free_) {
            org_xt_build(p);
            memcpy(org_b_xt, p + OP_XT_COMP, 6);
            org_xt_valid = 1;
            org.xt_use = 2;
        }
    }
}

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
    org.temp = 0u;                                          /* 0.9.7: the tuning is fixed (60 Hz gear, A 440); OP_TEMP and OP_TUNING are kept, not read */
    org.xtalk = org_patch[OP_XTALK];
    org.perc = org_patch[OP_PERC];
    org.psoft = org_patch[OP_PERC_SOFT];
    org.pfast = org_patch[OP_PERC_FAST];
    org.psel = org_patch[OP_PERC_SEL];
    org.vibm = org_patch[OP_VIB];
    org.mg = ORG_EXPR_Q15[org_patch[OP_EXPR] <= 60u ? org_patch[OP_EXPR] : 0u];
    org.vup = org_patch[OP_VIB_ROUTE] ? (org_patch[OP_VIB_UP] & 1u) : 1u;   /* older patches (route 0): both manuals, as 0.10.1 */
    org.vlo = org_patch[OP_VIB_ROUTE] ? (org_patch[OP_VIB_LO] & 1u) : 1u;
    {   /* ADVANCED EDIT: the percussion and the scanner (the tables are built in the main loop: org_adv_poll) */
        uint32_t sc = org_ci(OP_PC_GAIN, org_patch[OP_PC_GAIN]), ni = org_ci(OP_PC_NORM, org_patch[OP_PC_NORM]);
        uint32_t si = org_ci(OP_PC_SOFT, org_patch[OP_PC_SOFT]);
        org.pe_norm = (sc == 22u && ni == 0u) ? ORG_PERC_NORM : sc * ORG_PG[ni];
        org.pe_soft = (sc == 22u && si == 6u) ? ORG_PERC_SOFT : sc * ORG_PG[si];
        org.pdec_fast = ORG_PERC_DEC[org_ci(OP_PC_FAST, org_patch[OP_PC_FAST])];
        org.pdec_slow = ORG_PERC_DEC[org_ci(OP_PC_SLOW, org_patch[OP_PC_SLOW])];
        org.pbusa = (uint8_t)org_ci(OP_PC_BUSA, org_patch[OP_PC_BUSA]);
        org.pbusb = (uint8_t)org_ci(OP_PC_BUSB, org_patch[OP_PC_BUSB]);
        org.ptrig = (int8_t)((int32_t)org_ci(OP_PC_TRIG, org_patch[OP_PC_TRIG]) - 1);
        org.vinc = ORG_SCAN_INC[org_ci(OP_SC_HZ, org_patch[OP_SC_HZ])];
        org.vamp2[0] = (uint8_t)org_ci(OP_SC_V1, org_patch[OP_SC_V1]);
        org.vamp2[1] = (uint8_t)org_ci(OP_SC_V2, org_patch[OP_SC_V2]);
        org.vamp2[2] = (uint8_t)org_ci(OP_SC_V3, org_patch[OP_SC_V3]);
    }
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
