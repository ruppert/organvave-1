/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Effects: the track's DIST, then sends into three shared buses (chorus, delay, reverb). Mono buses, stereo dry mix. */
#define DLY_LEN 65536u           /* 1.49 s: 1/4 fits from 41 BPM, 1/4D from 61, 1/2 from 81 */
#define CHO_LEN 2048u
static int16_t dly_buf[DLY_LEN] __attribute__((section(".pool")));
static int16_t cho_buf[CHO_LEN] __attribute__((section(".pool")));
static const uint16_t REV_COMB[4] = {1116, 1188, 1277, 1356};
static const uint16_t REV_AP[2] = {556, 441};
static int16_t rev_comb[1116 + 1188 + 1277 + 1356] __attribute__((section(".pool")));
static union {                          /* ROOM's allpasses; SPRING's allpass chain (int32: no clamps) */
    int16_t ap[556 + 441];
    int32_t sp[(556 + 441) / 2];
} rev_u __attribute__((section(".pool")));
#define rev_ap (rev_u.ap)
/* FMini-1: the reverb types' own memory, shared (only one runs; a change clears it all). ROOM's right channel: 4 combs + 2 allpasses (+23
 * samples each, Freeverb's stereo spread); SPRING's second spring (4096 loop + its allpass states); PLATE (Dattorro's, half rate);
 * CLOUD's two extra diffusers. 23 KB of the pool */
#define RX_LEN 11520u
static union { int16_t s[RX_LEN]; int32_t w[RX_LEN / 2u]; } rev_x __attribute__((section(".pool")));
static struct {
    uint32_t dly_w, cho_w, cho_ph;
    int32_t dly_lp;
    uint16_t comb_i[4], ap_i[2];
    int32_t comb_lp[4];
    uint8_t rtype;                       /* the reverb model running (G_RTYPE: 0 ROOM, 1 SPRING, 2 HALL) */
    uint16_t sp_w;                       /* SPRING: the loop's write index (SP_MASK) */
    int32_t sp_lp, sp_hp, sp_he, sp_size;   /* .. its loop low-pass, low cut (and its remainder), the loop
                                             * length (Q8, glides) */
    uint32_t sp_ph;                      /* .. the output tap's wobble */
    uint8_t cho_on, dly_on, rev_on;      /* FMini-1: the bus ran last block (a bus whose send is 0 and input silent does not run at all) */
    uint32_t dly_start;                  /* .. the delay's write index when it last started (older samples are silence) */
    int32_t cho_lp[2];                   /* the chorus's two sides' low-passes (the BBD chain's band) */
    int32_t dl_q;                        /* the delay time, Q8 samples, gliding to the TIME knob (a click-free tape-like bend) */
    int32_t dly_hp;                      /* TAPE's low cut in the loop */
    uint32_t dly_ph, dly_ph2;            /* the modulation: wow / BBD drift, flutter */
    uint32_t rv_c[2], rv_n[2], rv_i[2];  /* REVERSE: the two heads' position in their sweep, its length and its triangle's slope (65536 / length) */
    uint32_t rv_p[2];                    /* .. their read positions (absolute write counts) */
    uint8_t ds_on;                       /* dside holds this block's stereo difference of the delay / chorus */
    uint16_t comb_ir[4], ap_ir[2];       /* ROOM's right channel: the same, its own indices and low-passes */
    int32_t comb_lpr[4];
    uint16_t sp2_w;                      /* SPRING's second spring: the same as sp_* */
    int32_t sp2_lp, sp2_hp, sp2_he, sp2_size;
    uint32_t sp2_ph;
} fx;
#define RDAMP (127 - song.g[G_RDAMP])    /* FMini-1: the REVERB's TONE (open at 127) is the high cut the models call damping */

/* SPRING (G_RTYPE 1): one spring of a spring tank, mono like the other buses, in the ROOM's own buffers (no
 * RAM of its own): the input and the loop's return -> a low cut (~110 Hz: a spring carries little bass) ->
 * SP_N stretched first-order allpasses, (a + z^-4) / (1 + a z^-4) (after Valimaki, Parker and Abel: below
 * fs / 8 = 5.5 kHz the group delay rises with frequency, the chirp; each pass round the loop adds more of
 * it: the "boing", the drips) -> the loop's delay line (rev_comb, SP_LEN) -> back through a one-pole
 * low-pass (DAMP) and the decay gain (SIZE). The output: the spring's far end, half way along the loop
 * (the first sound 15 .. 30 ms after the send: the tank's own pre-delay; a slow wobble of a sample or two
 * on it), plus a second, quieter pickup at three quarters (a shorter spring beside it: denser). SIZE sets
 * the loop's length (30 .. 60 ms) and its decay; DAMP the loop's low-pass. The allpasses' states: 4
 * samples each (int32), in rev_ap's memory. Changing the model fades the old one's block out and clears both buffers. */
#define SP_LEN 4096u                     /* the loop's line in rev_comb (4937 samples) */
#define SP_MASK (SP_LEN - 1u)
#define SP_N 10u                         /* allpass stages */
#define SP_A 2867                        /* their coefficient, Q12 (0.7: Q12 keeps (x - o) * a in 32 bits up to
                                          * |x - o| < 749000, far past any peak the chain reaches) */
_Static_assert(sizeof rev_comb / 2u >= SP_LEN && sizeof rev_u.sp / 4u >= 4u * (SP_N + 1u), "SPRING in ROOM's buffers");

/* HALL (G_RTYPE 2, 1.2): a smooth, dense, stereo reverb, again in ROOM's buffers (its few states and one block
 * of work in the pool, no .bss). ROOM's 4 combs each ring on their own (the metallic tone); here every echo
 * goes round 8 lines that all feed each other, and they run at half the rate (22.05 kHz: the same memory holds
 * twice the time, so twice the resonances, at half the work; the tail stops near 10 kHz, as a hall's air
 * does). The send (every other sample of it, low-passed 1 2 1; ROOM's level) -> a low cut (~110 Hz: no boom
 * in a long tail) -> 2 allpass diffusers (rev_ap: a drum's click smeared into a burst) -> into the 8 lines
 * (rev_comb, mutually prime lengths, 21 .. 34 ms). Lines 1, 2, 4, 7 are read through slow sines (+-HL_MOD
 * samples, between samples; 0.37 or 0.74 Hz, each in its own phase): the resonances drift instead of ringing.
 * Lines 0, 3, 5, 6, read at their ends, go through a one-pole low-pass (DAMP: the mixing takes every echo
 * through them); every
 * line through its gain (from its length: all decay alike; RT60 below the damping 0.5 s at SIZE 0, 4 s at
 * 90, 9 s at 127; ~2.4 s heard at 90 with DAMP 60); then the 8 x 8 mixing: a conference matrix (Paley,
 * order 8: every line feeds the 7 others, +-1 each, none itself: no flutter at a line's own length), with
 * the send added. Stereo: left lines 0 3 4 -7, right 1 2 5 6 (sums spread over the matrix's rows); the wet
 * bus (mono) gets left + right, rev_side left - right, which mix_block adds to the left and takes from the
 * right (rev_side_mix). Back to 44.1 kHz between samples. Rounding: a loud line's gain rounds to the nearest
 * (the tail decays as it should), a quiet one's towards zero (it ends in exact silence: no limit cycle), and
 * the low-passes keep their steps' remainders (no offset). */
#define HL_NL 8u                         /* lines */
#define HL_NAP 2u                        /* diffusers */
#define HL_NC (CTL / 2u)                 /* a block at half the rate */
#define HL_MOD 8                         /* the taps' swing, samples (read 2 .. 18 samples short of the end) */
#define HL_LPM 0x69u                     /* the lines with a low-pass */
#define HL_KD(d) (32767 - (d) * 180)     /* its coefficient (Q15) for DAMP d: none at 0 .. ~1 kHz at 127 */
#define HL_LOUD 32                       /* a line this loud (two samples' sum) rounds its gain to the nearest */
#define HL_OUT 4                         /* the outputs' gain: the level of ROOM's tail */
static const uint16_t HL_LEN[HL_NL] = {467, 509, 547, 593, 631, 673, 709, 743};   /* (+1 each: the mirror) */
#define HL_NAPX 4u                       /* CLOUD: two more diffusers (in rev_x), denser */
static const uint16_t HL_AP[HL_NAPX] = {131, 307, 89, 223};
static const int16_t HL_APG[HL_NAPX] = {11469, 10240, 9830, 9011};  /* Q14: 0.7, 0.625, 0.6, 0.55 */
_Static_assert(467 + 509 + 547 + 593 + 631 + 673 + 709 + 743 + HL_NL <= sizeof rev_comb / 2u &&
               131 + 307 <= sizeof rev_u.ap / 2u, "HALL in ROOM's buffers");
_Static_assert(CTL % 2u == 0u && HL_NC + 2u * HL_MOD + 2u < 467u && HL_NC <= 131u, "HALL: one wrap a block at most");
static struct {                          /* HALL's state (in the pool: no .bss) */
    uint16_t i[HL_NL], d[HL_NAPX];       /* the lines' write indices, the diffusers' */
    uint8_t mode;                        /* 0 HALL, 1 CLOUD (longer, darker, 4 diffusers) */
    int32_t lp[HL_NL], le[HL_NL];        /* the low-passes and their steps' remainders */
    int32_t x, m, s, hp, he;             /* the send's last sample, its low cut and that step's remainder; the
                                          * last mid and side out (half rate) */
    int16_t g[HL_NL], sz;                /* the lines' gains / sqrt(7) (Q14), for SIZE sz - 1 (0: none yet) */
    uint32_t ph;                         /* the taps' drift */
    uint8_t side;                        /* sd holds this block's stereo difference (HALL running) */
    int32_t sd[CTL];                     /* the block's left minus right (mix_block: rev_side_mix) */
    int32_t bx[HL_NC], l[HL_NC], r[HL_NC];   /* the block at half the rate: the diffused send, left, right */
    int16_t y[HL_NL][HL_NC];             /* the lines' ends, then what goes back into them */
} hl __attribute__((section(".pool")));
#define rev_side (hl.sd)
static int32_t dside[CTL];               /* the delay's / chorus's left minus right / 2 (mix_block: rev_side_mix adds it) */

/* DIST: low cut -> drive (1x..8x, exponential) -> asymmetric soft clip
 * (a little bias = even harmonics) -> tone low-pass that closes with drive ->
 * make-up gain (straight into tanh over the full band, it would sound like a
 * broken digital fuzz). State per part (track_t dist_*). */
static int32_t tube_cap[NTRK];                          /* TUBE's coupling capacitor, per part (track_t has no room: a multiple of 16 bytes) */
static void track_dist(track_t *t, int32_t *b, uint32_t n)
{
    int32_t d = t->p[P_DIST], i, g, k, mk, bias = 2400, b0;
    if (!d)
        return;                                         /* states kept: switching on does not click */
    g = 4096 + d * d * 2;                                /* Q12: 1x .. ~9x, gentle at first */
    k = 32000 - d * 95 + ((int32_t)song.g[G_DRLVL] - 64) * 150;   /* tone: transparent at low drive .. ~3 kHz, Q15; G_DRLVL (TONE) moves it */
    k = clamp(k, 2500, 32500);
    mk = 30000 - d * 120;                                /* make-up */
    mk = (int32_t)(((int64_t)mk * song.g[G_MIDI] * 512) >> 15);   /* OUTPUT: 64 = as before (x1), 127 = x1.98, 0 = silent */
    if (song.g[G_INFO] == 1) {
        /* TUBE: two triode stages with a coupling capacitor between them: the first biased (a grid bias: even harmonics, a soft, lopsided clip),
         * the second biased too (more even harmonics: a warm 2nd): rounder, thicker than SOFT, and
         * it compresses before it clips. Each stage gains: GAIN raises the first 1 .. 9x, the second 1 .. 2.5x */
        int32_t g1 = 4096 + d * d * 2, g2 = 4096 + d * 48, bias1 = 6500, bias2 = 1500, c1 = softclip(bias1), c2 = softclip(bias2);
        int32_t *cap = &tube_cap[t - trk];
        for (i = 0; i < (int32_t)n; i++) {
            int32_t x = b[i], y;
            t->dist_hp += (x - t->dist_hp + 64) >> 7;
            x = clamp(x - t->dist_hp, -230000, 230000);
            y = softclip((((x >> 2) * g1) >> 10) + bias1) - c1;
            *cap += (y - *cap + 128) >> 8;                       /* the coupling capacitor (~30 Hz) */
            y -= *cap;
            y = softclip((((y >> 2) * g2) >> 10) + bias2) - c2;
            t->dist_lp1 += mulq15(y - t->dist_lp1, k);
            t->dist_lp2 += mulq15(t->dist_lp1 - t->dist_lp2, k);
            b[i] = mulq15(t->dist_lp2, mk);
        }
        return;
    }
    b0 = softclip(bias);
    for (i = 0; i < (int32_t)n; i++) {
        int32_t x = b[i], y;
        t->dist_hp += (x - t->dist_hp + 64) >> 7;           /* ~55 Hz low cut: keep the bass out of the clipper */
        x = clamp(x - t->dist_hp, -230000, 230000);         /* (x >> 2) * g fits 32 bits; the clip is flat out there */
        y = softclip((((x >> 2) * g) >> 10) + bias) - b0;   /* >> 2 first: no overflow for loud poly */
        t->dist_lp1 += mulq15(y - t->dist_lp1, k);         /* two poles: tames the fizz */
        t->dist_lp2 += mulq15(t->dist_lp1 - t->dist_lp2, k);
        b[i] = mulq15(t->dist_lp2, mk);
    }
}

/* master: peak limiter in front of the soft clipper. Fast attack (~0.1 ms),
 * ~150 ms release, threshold where tanh is still nearly linear, so chords
 * get quieter instead of crushed. */
#define LIM_T 18000
static int32_t lim_env = LIM_T;
/* MENU > USB LEVEL FIXED (for #42: record over USB with the speaker turned down): the mix goes to master_out at the
 * full MASTER level, USB audio takes that (audio.c uac_tap), and only then does MASTER scale what the DAC gets
 * (usb_fixed_dac). MASTER (0, the default): MASTER before master_out, as always (USB follows the knob) */
static volatile uint8_t fx_usb_fixed;
#define MASTER_FULL 4096                /* main.c: the MASTER knob's top, Q12 */
/* the MASTER pot (ADC 0..1023, smoothed in main.c) to its level, square law: 0 .. 4088 (Q12) */
static inline uint32_t master_of_pot(uint32_t k10) { return (k10 * k10) >> 8; }
static __attribute__((noinline)) void usb_fixed_dac(int32_t *out, uint32_t n)   /* audio ISR, after uac_tap */
{
    uint32_t i;
    int32_t m = (int32_t)song.master_q12;
    for (i = 0; i < 2u * n; i++)
        out[i] = (out[i] * m) >> 12;                /* (|out| <= 32767 after the soft clip: fits) */
}
static volatile uint8_t fx_lowcut;     /* settings: 1 LOWCUT 12 dB/oct ~110 Hz, 2 BASS+ (the small speaker):
                                        * 12 dB/oct ~220 Hz plus the harmonics of the bass (spk_bass) */
static int32_t lc_l1, lc_l2, lc_r1, lc_r2, dc_l, dc_r, dce_l, dce_r;

/* DC blocker (~2 Hz), always on: a leaky integrator of the input (Q6 state) subtracted from it.
 * The >> 12 step keeps its remainder (error feedback, 0..4095) and adds it to the next one, so no
 * part of the step is lost: the state follows the input exactly, down to 0 after the sound stops.
 * (A rounded step of (x - dc) / 4096 would stop moving at |x - dc| < 2048 and leave an offset of up
 * to +-31 at the output after silence.) */
static inline int32_t dc_block(int32_t x, int32_t *dc, int32_t *err)
{
    int32_t e = (x << 6) - *dc + *err, d = e >> 12;
    *err = e - (d << 12);
    *dc += d;
    return x - ((*dc + 32) >> 6);
}

static int32_t lce[4];
static inline int32_t lowcut1(int32_t x, int32_t *lc, int32_t *err, uint32_t sh)   /* x minus its one-pole low-pass */
{
    int32_t e = x - *lc + *err, d = e >> sh;
    *err = e - (d << sh);
    *lc += d;
    return x - *lc;
}

/* BASS+: what the speaker cannot play, heard through its harmonics. The bass below ~150 Hz is clipped at its
 * own envelope (a level-following trapezoid: odd harmonics), then band-passed ~220 Hz..1 kHz and added.
 * The low-pass has 4 poles (#42: with 2, the trapezoid rebuilt the 300 .. 600 Hz of the mix itself, late,
 * and cancelled up to 6 dB of it; now under 0.5 dB) */
static int32_t sb_lp1, sb_lp2, sb_lp3, sb_lp4, sb_env, sb_h1, sb_h2, sb_hl;
static inline int32_t spk_bass(int32_t m)
{
    int32_t a, t, u;
    sb_lp1 += ((m - sb_lp1) * 692) >> 15;
    sb_lp2 += ((sb_lp1 - sb_lp2) * 692) >> 15;
    sb_lp3 += ((sb_lp2 - sb_lp3) * 692) >> 15;
    sb_lp4 += ((sb_lp3 - sb_lp4) * 692) >> 15;
    a = sb_lp4 < 0 ? -sb_lp4 : sb_lp4;
    if (a > sb_env)
        sb_env += (a - sb_env) >> 2;
    else if (sb_env > 0)
        sb_env -= (sb_env >> 11) + 1;
    t = clamp(sb_lp4 * 8, -sb_env, sb_env);
    sb_h1 += (t - sb_h1) >> 5;
    u = t - sb_h1;
    sb_h2 += (u - sb_h2) >> 5;
    u -= sb_h2;
    sb_hl += (u - sb_hl) >> 3;
    return sb_hl + (sb_hl >> 1);      /* x1.5 (#180: x3 peaked at 1.5 .. 3.5 x a full-scale kick, the limiter */
}                                     /* pulled the mix down up to 16 dB on each hit and the buzz took over) */

static inline void master_out(int32_t *l, int32_t *r)
{
    int32_t al, ar, a;
    *l = dc_block(*l, &dc_l, &dce_l);
    *r = dc_block(*r, &dc_r, &dce_r);
    if (fx_lowcut) {                  /* two one-pole high-passes, error feedback as dc_block (the */
        uint32_t sh = fx_lowcut == 2u ? 5u : 6u;    /* rounded step stopped at |x - lc| < 32: an offset) */
        int32_t b = fx_lowcut == 2u ? spk_bass((*l + *r) >> 1) : 0;
        *l = lowcut1(*l, &lc_l1, &lce[0], sh);
        *l = lowcut1(*l, &lc_l2, &lce[1], sh) + b;
        *r = lowcut1(*r, &lc_r1, &lce[2], sh);
        *r = lowcut1(*r, &lc_r2, &lce[3], sh) + b;
    }
    al = *l < 0 ? -*l : *l;
    ar = *r < 0 ? -*r : *r;
    a = al > ar ? al : ar;
    if (a > lim_env)
        lim_env += (a - lim_env) >> 2;
    else if (lim_env > LIM_T)
        lim_env -= ((lim_env - LIM_T) >> 12) + 1;
    if (lim_env > LIM_T) {
        int32_t g = (int32_t)(((uint32_t)LIM_T << 15) / (uint32_t)lim_env);   /* < 32768 */
        *l = ((*l >> 4) * g) >> 11;                      /* >> 4 first: |l| may be far above Q15 */
        *r = ((*r >> 4) * g) >> 11;
    }
    *l = softclip(*l);
    *r = softclip(*r);
}

/* length of one division (N_DIV order) in samples at the song tempo */
static const uint8_t DIV_DEN[6] = {1, 2, 4, 8, 3, 6};    /* original IDs stay fixed; slow rates append */
static uint32_t beat_samples(void) { return (uint32_t)FS * 60u / (uint32_t)song.g[G_BPM]; }
static uint32_t div_samples(uint32_t div)
{
    uint32_t quarter = beat_samples();
    return div < 6u ? quarter / DIV_DEN[div] : div < 10u ? quarter << (div - 5u) : quarter / DIV_DEN[div % 6u];
}


/* the delay's time in samples: FX > TIME (G_DRREV, dly_ms), at most DLY_LEN - 1 */
static uint32_t delay_samples(void)
{
    uint32_t m = (uint32_t)dly_ms(song.g[G_DRREV]) * (uint32_t)FS / 1000u;   /* FMini-1: the time in ms (FX > TIME) */
    return m < 16u ? 16u : m >= DLY_LEN ? DLY_LEN - 1u : m;
}

static const uint16_t REV_COMB_R[4] = {1139, 1211, 1300, 1379};   /* ROOM's right channel: +23 samples (Freeverb's stereo spread) */
static const uint16_t REV_AP_R[2] = {579, 464};
_Static_assert(1139 + 1211 + 1300 + 1379 + 579 + 464 <= RX_LEN, "ROOM's right channel in rev_x");
/* ROOM (type 0): Freeverb's stereo form: the send into 4 damped combs + 2 allpasses a side; left and right differ by the +23 samples
 * (different resonances: decorrelated). The wet bus gets (left + right) / 2, rev_side (left - right) / 2 */
static __attribute__((noinline)) void rev_room(const int32_t *rev_in, int32_t *out, uint32_t n)
{
    uint32_t i, k;
    int32_t size = 25000 + song.g[G_RSIZE] * 50, damp = 32767 - RDAMP * 200;
    for (i = 0; i < n; i++) {
        int32_t a = 0, b = 0;
        int16_t *c = rev_comb, *cr = rev_x.s;
        int32_t in = mulq15(rev_in[i], 2580);
        for (k = 0; k < 4u; k++) {
            int32_t o = c[fx.comb_i[k]], q = cr[fx.comb_ir[k]];
            fx.comb_lp[k] = o + mulq15(fx.comb_lp[k] - o, 32767 - damp);
            c[fx.comb_i[k]] = (int16_t)clamp(in + mulq15(fx.comb_lp[k], size), -32768, 32767);
            fx.comb_lpr[k] = q + mulq15(fx.comb_lpr[k] - q, 32767 - damp);
            cr[fx.comb_ir[k]] = (int16_t)clamp(in + mulq15(fx.comb_lpr[k], size), -32768, 32767);
            if (++fx.comb_i[k] >= REV_COMB[k])
                fx.comb_i[k] = 0;
            if (++fx.comb_ir[k] >= REV_COMB_R[k])
                fx.comb_ir[k] = 0;
            a += o;
            b += q;
            c += REV_COMB[k];
            cr += REV_COMB_R[k];
        }
        c = rev_ap;
        cr = rev_x.s + (1139 + 1211 + 1300 + 1379);
        for (k = 0; k < 2u; k++) {
            int32_t o = c[fx.ap_i[k]], q = cr[fx.ap_ir[k]];
            c[fx.ap_i[k]] = (int16_t)clamp(a + (o >> 1), -32768, 32767);
            a = o - a;
            cr[fx.ap_ir[k]] = (int16_t)clamp(b + (q >> 1), -32768, 32767);
            b = q - b;
            if (++fx.ap_i[k] >= REV_AP[k])
                fx.ap_i[k] = 0;
            if (++fx.ap_ir[k] >= REV_AP_R[k])
                fx.ap_ir[k] = 0;
            c += REV_AP[k];
            cr += REV_AP_R[k];
        }
        out[i] += (a + b) >> 1;
        rev_side[i] = (a - b) >> 1;
    }
    hl.side = 1;
}

/* SPRING (see the top), stereo: a tank has two springs: the left output is the first (the loop in rev_comb, as before), the right a second, 19 %
 * longer one (its loop in rev_x, its own wobble): the two sound apart. spring_one: one spring's block into dst (assigned) */
static __attribute__((noinline)) void spring_one(int16_t *ln, int32_t *ap, uint16_t *spw, int32_t *splp, int32_t *sphp, int32_t *sphe,
                                                 int32_t *spsz, uint32_t *spph, uint32_t lfo, uint32_t lenmul, const int32_t *rev_in,
                                                 int32_t *dst, uint32_t n)
{
    uint32_t i, k, s = (uint32_t)song.g[G_RSIZE];
    int32_t g = 19661 + (int32_t)s * 85;
    int32_t kl = 26000 - RDAMP * 160;
    int32_t len = (int32_t)(((1323u + ((s * 1323u) >> 7)) * lenmul) >> 8) << 8, L, L2, L3, f, w;
    if (!*spsz)
        *spsz = len;
    *spsz += clamp(len - *spsz, -256, 256);
    L = *spsz >> 8;
    *spph += lfo;
    w = (*spsz >> 1) + ((osc_sine(*spph) * 3) >> 8);
    L2 = w >> 8;
    f = w & 255;
    L3 = (L * 3) >> 2;
    for (i = 0; i < n; i++) {
        uint32_t wp = *spw, j = (wp & 3u) * (SP_N + 1u);
        int32_t x = mulq15(rev_in[i], 2580), r = ln[(wp - (uint32_t)L) & SP_MASK], p, o;
        int32_t t0 = ln[(wp - (uint32_t)L2) & SP_MASK], t1 = ln[(wp - (uint32_t)L2 - 1u) & SP_MASK];
        *splp += mulq15(r - *splp, kl);
        o = *splp * g;
        x += (o + ((o >> 31) & 32767)) >> 15;
        o = x - *sphp + *sphe;
        *sphe = o & 63;
        *sphp += o >> 6;
        x -= *sphp;
        p = ap[j];
        ap[j] = x;
        for (k = 1; k <= SP_N; k++) {
            int32_t v = (x - ap[j + k]) * SP_A;
            o = ap[j + k];
            x = ((v + ((v >> 31) & 4095)) >> 12) + p;
            p = o;
            ap[j + k] = x;
        }
        ln[wp & SP_MASK] = (int16_t)clamp(x, -32768, 32767);
        *spw = (uint16_t)(wp + 1u);
        dst[i] = (t0 + (((t1 - t0) * f) >> 8)) * 4 + ln[(wp - (uint32_t)L3) & SP_MASK] * 2;
    }
}
_Static_assert(SP_LEN + 4u * (SP_N + 1u) * 2u <= RX_LEN, "SPRING's second spring in rev_x");
static __attribute__((noinline)) void rev_spring(const int32_t *rev_in, int32_t *out, uint32_t n)
{
    int32_t a[CTL], b[CTL];
    uint32_t i;
    spring_one(rev_comb, rev_u.sp, &fx.sp_w, &fx.sp_lp, &fx.sp_hp, &fx.sp_he, &fx.sp_size, &fx.sp_ph, 2u * LFO_INC[24], 256u, rev_in, a, n);
    spring_one(rev_x.s, rev_x.w + SP_LEN / 2u, &fx.sp2_w, &fx.sp2_lp, &fx.sp2_hp, &fx.sp2_he, &fx.sp2_size, &fx.sp2_ph, 2u * LFO_INC[27],
               304u, rev_in, b, n);
    for (i = 0; i < n; i++) {
        out[i] += (a[i] + b[i]) >> 1;
        rev_side[i] = (a[i] - b[i]) >> 1;
    }
    hl.side = 1;
}

/* HALL's helpers: m / 2^sh rounded towards zero (a product that feeds back must not floor: a loop of floors
 * holds an offset and a little noise for ever) */
static inline int32_t sh_tz(int32_t m, uint32_t sh) { return (m + ((m >> 31) & ((1 << sh) - 1))) >> sh; }
/* 2^-x, x Q16 (0 .. 16): Q15 (32768 at x = 0). 2^-x = 2^u / 2^(i + 1), u = 1 - frac(x); 2^u by a cubic (1e-4) */
static uint32_t exp2_neg(uint32_t x)
{
    uint32_t i = x >> 16, u = 32768u - ((x & 0xFFFFu) >> 1), p = 2579u;
    p = ((p * u) >> 15) + 7409u;
    p = ((p * u) >> 15) + 22780u;
    p = ((p * u) >> 15) + 32768u;
    return i < 15u ? p >> (i + 1u) : 0u;
}
/* the lines' gains for SIZE s: RT60 = 0.5 s * 2^(s / 30); a line of D samples (22.05 kHz) loses 60 dB in
 * RT60 * 22050 / D passes: g = 2^(-D * 3 log2(10) / (22050 * RT60)) = 2^(-D * 59.2 / 65536 * 2^(-s / 30)),
 * at most 0.99, over sqrt(7) (the matrix's rows have 7 entries of +-1: / sqrt(7) makes it orthogonal) */
/* FMini-1: per mode: HALL RT60 0.5 s * 2^(s / 30) (0.5 .. 9.4 s); CLOUD 3 s * 2^(s / 42) (3 .. 24 s), its lines allowed up to 0.9985 */
static __attribute__((noinline)) void hall_gains(int32_t s, uint32_t mode)
{
    uint32_t kd = mode != 0u ? 42u : 30u, co = mode == 0u ? 59u : 10u, gm = mode == 1u ? 32720u : 32440u;
    uint32_t q = exp2_neg((uint32_t)s * 65536u / kd), k, g;
    for (k = 0; k < HL_NL; k++) {
        g = exp2_neg((HL_LEN[k] * co * q) >> 15);
        hl.g[k] = (int16_t)(((g > gm ? gm : g) * 6193u) >> 15);
    }
    hl.sz = (int16_t)(s + 1 + (int32_t)mode * 256);
}

/* an allpass diffuser over x[0 .. n) from q (its next samples, no wrap): its state rounded towards zero */
static inline void hall_ap(int16_t *q, int32_t g, int32_t *x, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t y = q[i], v = x[i] + sh_tz(y * g, 14);
        q[i] = (int16_t)clamp(v, -32768, 32767);
        x[i] = y - ((v * g + 8192) >> 14);
    }
}
/* a line's end over y[0 .. n) from b (no wrap; b[n] is still the line's: its mirror at the end): between b[i]
 * and b[i + 1], f / 256 of the way (rounded to the nearest: it stays between the two), times its gain g (Q14).
 * The gain's product: rounded to the nearest while the line is loud (lo 0: the tail decays as it should; a
 * truncation would make it die early, the sooner the quieter), towards zero once it is quiet (lo 1: the last
 * steps of the tail, which then ends in exact silence) */
static inline void hall_end(const int16_t *b, int32_t f, int16_t *y, uint32_t n, int32_t g, int lo)
{
    uint32_t i;
    if (lo)
        for (i = 0; i < n; i++)
            y[i] = (int16_t)sh_tz((b[i] + (((b[i + 1u] - b[i]) * f + 128) >> 8)) * g, 14);
    else
        for (i = 0; i < n; i++)
            y[i] = (int16_t)(((b[i] + (((b[i + 1u] - b[i]) * f + 128) >> 8)) * g + 8192) >> 14);
}
/* a line's end over y[0 .. n) from b (no wrap), through the low-pass (kd, Q15; the state *lp and its step's
 * remainder *le), times its gain g (rounded as hall_end's) */
static inline void hall_end_lp(const int16_t *b, int16_t *y, uint32_t n, int32_t g, int lo, int32_t *lp, int32_t *le,
                               int32_t kd)
{
    uint32_t i;
    int32_t l = *lp, r = *le, e;
    if (lo)
        for (i = 0; i < n; i++) {
            e = (b[i] - l) * kd + r, r = e & 32767, l += e >> 15;
            y[i] = (int16_t)sh_tz(l * g, 14);
        }
    else
        for (i = 0; i < n; i++) {
            e = (b[i] - l) * kd + r, r = e & 32767, l += e >> 15;
            y[i] = (int16_t)((l * g + 8192) >> 14);
        }
    *lp = l;
    *le = r;
}
/* the block's HL_NC samples of a circular buffer of len from index d in at most two runs, no wrap inside them:
 * body with s (the run's start in the block) and k (its length); d moves on */
#define HL_RUNS(d, len, k, s, body)                                                         \
    for (s = 0; s < HL_NC; s += k) {                                                        \
        k = (len) - (d) < HL_NC - s ? (len) - (d) : HL_NC - s;                              \
        body;                                                                               \
        d = (d) + k >= (len) ? 0u : (d) + k;                                                \
    }

/* the block's send at half the rate (hl.bx): every other sample, 1 2 1, ROOM's level, a low cut (~110 Hz) with its step's remainder kept */
static inline void hall_send(const int32_t *rev_in)
{
    uint32_t i;
    int32_t xp = hl.x, hp = hl.hp, he = hl.he;
    for (i = 0; i < HL_NC; i++) {
        int32_t x = (xp + 2 * rev_in[2u * i] + rev_in[2u * i + 1u]) >> 2, v;   /* every other sample, 1 2 1 */
        xp = rev_in[2u * i + 1u];
        x = mulq15(clamp(x, -262143, 262143), 2580);    /* (ROOM's level) */
        v = x - hp + he;                                /* the low cut, its step's remainder kept */
        he = v & 31;
        hp += v >> 5;
        hl.bx[i] = x - hp;
    }
    hl.x = xp, hl.hp = hp, hl.he = he;
}

/* HALL (see the top): a block of the send (n = CTL samples: audio.c) -> the mid added to out, the side into
 * rev_side. At half the rate (HL_NC samples) in passes over the block, each with few states (every line is
 * longer than a block: what the lines give in this block was written before it): the send, every other
 * sample, low cut, diffusers (hl.bx); the lines' ends at their drifting taps, low-passed, scaled (hl.y); the
 * outputs and the matrix; back into the lines; the outputs back to 44.1 kHz. */
#define HL_LFO ((uint32_t)(0.37 * CTL / FS * 4294967296.0))   /* the drift: 0.37 Hz (even lines), 0.74 Hz (odd) */
static __attribute__((noinline)) void rev_hall(const int32_t *rev_in, int32_t *out, uint32_t n)
{
    int16_t *b = rev_comb, *a = rev_ap;
    uint32_t i, k, s, m, d;
    int32_t kd = HL_KD(hl.mode == 1u ? 30 + RDAMP * 97 / 127 : RDAMP);   /* (CLOUD: never wide open: a dark, soft tail) */
    uint32_t nap = hl.mode == 1u ? HL_NAPX : HL_NAP;
    int32_t mp = hl.m, sp = hl.s;
    (void)n;
    if (hl.sz != song.g[G_RSIZE] + 1 + (int32_t)hl.mode * 256)
        hall_gains(song.g[G_RSIZE], hl.mode);
    hall_send(rev_in);
    for (k = 0; k < nap; k++) {                         /* the diffusers (CLOUD's extra two in rev_x) */
        int16_t *q = k < HL_NAP ? a : rev_x.s + (k == 2u ? 0u : HL_AP[2]);
        d = hl.d[k];
        HL_RUNS(d, HL_AP[k], m, s, hall_ap(q + d, HL_APG[k], hl.bx + s, m));
        hl.d[k] = (uint16_t)d;
        if (k < HL_NAP)
            a += HL_AP[k];
    }
    hl.ph += HL_LFO;
    for (k = 0; k < HL_NL; k++) {                       /* the lines' ends */
        int32_t g = hl.g[k], f = 0, lo;
        if ((HL_LPM >> k) & 1u)
            d = hl.i[k];                                /* (low-passed: the line's end) */
        else {                                          /* (drifting: HL_MOD + 2 +- HL_MOD samples short of it) */
            uint32_t r = (uint32_t)(((HL_MOD + 2) << 8) +
                                    ((osc_sine(hl.ph * (1u + (k & 1u)) + k * 0x2C000000u) * HL_MOD) >> 7));
            f = (int32_t)(r & 255u);
            d = hl.i[k] + (r >> 8);
            d = d >= HL_LEN[k] ? d - HL_LEN[k] : d;
        }
        s = d + HL_LEN[k] / 2u;                         /* quiet? (two samples half a line apart) */
        s = s >= HL_LEN[k] ? s - HL_LEN[k] : s;
        lo = (b[d] < 0 ? -b[d] : b[d]) + (b[s] < 0 ? -b[s] : b[s]) < HL_LOUD;
        if ((HL_LPM >> k) & 1u) {
            HL_RUNS(d, HL_LEN[k], m, s, hall_end_lp(b + d, hl.y[k] + s, m, g, lo, &hl.lp[k], &hl.le[k], kd));
        } else {
            HL_RUNS(d, HL_LEN[k], m, s, hall_end(b + d, f, hl.y[k] + s, m, g, lo));
        }
        b += HL_LEN[k] + 1u;
    }
    for (i = 0; i < HL_NC; i++) {
        int32_t y0 = hl.y[0][i], y1 = hl.y[1][i], y2 = hl.y[2][i], y3 = hl.y[3][i];
        int32_t y4 = hl.y[4][i], y5 = hl.y[5][i], y6 = hl.y[6][i], y7 = hl.y[7][i], x = hl.bx[i];
        int32_t t, p0, p1, p2, p3, p4, p5, p6;
        hl.l[i] = y0 + y3 + y4 - y7;                    /* the outputs */
        hl.r[i] = y1 + y2 + y5 + y6;
        /* the matrix: C = [0 1; -1 Q], Q (7 x 7) = chi(j - i), the quadratic character mod 7 (+1 at distances
         * 1 2 4, -1 at 3 5 6): row 1 + i of Q y = 2 (the three at +1) - (the sum of the 7) + y_1+i */
        t = y1 + y2 + y3 + y4 + y5 + y6 + y7;
        p0 = y2 + y3 + y5, p1 = y3 + y4 + y6, p2 = y4 + y5 + y7, p3 = y5 + y6 + y1;
        p4 = y6 + y7 + y2, p5 = y7 + y1 + y3, p6 = y1 + y2 + y4;
        y0 += t;
        hl.y[0][i] = (int16_t)clamp(t + x, -32768, 32767);   /* (+ the send: signs spread over the rows) */
        hl.y[1][i] = (int16_t)clamp(2 * p0 + y1 - y0 + x, -32768, 32767);
        hl.y[2][i] = (int16_t)clamp(2 * p1 + y2 - y0 + x, -32768, 32767);
        hl.y[3][i] = (int16_t)clamp(2 * p2 + y3 - y0 + x, -32768, 32767);
        hl.y[4][i] = (int16_t)clamp(2 * p3 + y4 - y0 + x, -32768, 32767);
        hl.y[5][i] = (int16_t)clamp(2 * p4 + y5 - y0 + x, -32768, 32767);
        hl.y[6][i] = (int16_t)clamp(2 * p5 + y6 - y0 + x, -32768, 32767);
        hl.y[7][i] = (int16_t)clamp(2 * p6 + y7 - y0 - x, -32768, 32767);
    }
    b = rev_comb;
    for (k = 0; k < HL_NL; k++) {                       /* back into the lines (and the mirror after a wrap) */
        d = hl.i[k];
        HL_RUNS(d, HL_LEN[k], m, s, for (i = 0; i < m; i++) b[d + i] = hl.y[k][s + i]; if (!d) b[HL_LEN[k]] = b[0]);
        hl.i[k] = (uint16_t)d;
        b += HL_LEN[k] + 1u;
    }
    for (i = 0; i < HL_NC; i++) {                       /* back to 44.1 kHz, between samples */
        int32_t mm = (hl.l[i] + hl.r[i]) * HL_OUT, ss = (hl.l[i] - hl.r[i]) * HL_OUT;
        out[2u * i] += (mp + mm) >> 1;
        out[2u * i + 1u] += mm;
        rev_side[2u * i] = (sp + ss) >> 1;
        rev_side[2u * i + 1u] = ss;
        mp = mm;
        sp = ss;
    }
    hl.m = mp, hl.s = sp;
    hl.side = 1;
}

/* PLATE (type 2): Dattorro's plate (J. Audio Eng. Soc. 1997, "Effect Design" part 1), at half the rate and half his lengths (a smaller, denser
 * plate), in rev_x: the send (hall_send) -> 4 input diffusers -> a figure-eight tank: each side a modulated allpass, a delay, a low-pass
 * (TONE: the plate's damping), the decay gain (DECAY: the loop's gain 0.12 .. 0.7 a tank: ~0.8 .. 5 s), an allpass and a delay, its end feeding the other side; the
 * outputs are signed taps all over both sides (left and right each its own seven: decorrelated, wide). The loops' gains round
 * towards zero (it ends in silence). */
#define PL_N 12u
static const uint16_t PL_LEN[PL_N] = {71, 54, 190, 139, 344, 2227, 900, 1860, 462, 2109, 1328, 1582};
static const int16_t PL_G[4] = {24576, 24576, 20480, 20480};        /* the input diffusers: 0.75 0.75 0.625 0.625 (Q15) */
static struct {
    uint16_t i[PL_N];                    /* the lines' write indices */
    int32_t lpl, lpr, lel, ler, tl, tr;  /* the damping low-passes and their steps' remainders; the tanks' last ends (each into the other's next sample) */
    uint32_t ph, ph2;                    /* the modulation: 1 Hz (left), 1.27 Hz (right) */
} pl __attribute__((section(".pool")));
#define PL_OUT 40                                /* the taps' sum -> the level of HALL's tail (Q3) */
static inline int32_t pl_ap(int16_t *b, uint16_t *ix, uint32_t len, int32_t g, int32_t x)   /* y = z + g (x - g z), z the oldest */
{
    int32_t z = b[*ix], v = x - sh_tz(z * g, 15);
    b[*ix] = (int16_t)clamp(v, -32768, 32767);
    if (++*ix >= len)
        *ix = 0;
    return z + sh_tz(v * g, 15);
}
static inline int32_t pl_tap(const int16_t *b, uint32_t ix, uint32_t len, uint32_t t)           /* t samples ago (ix: the next write) */
{
    return b[ix >= t ? ix - t : ix + len - t];
}
static __attribute__((noinline)) void rev_plate(const int32_t *rev_in, int32_t *out, uint32_t n)
{
    int16_t *bf[PL_N];
    uint32_t i, k, o = 0;
    int32_t mp = hl.m, sp = hl.s, s = song.g[G_RSIZE];
    int32_t dk = 3932 + s * 150, g2 = clamp(dk + 4915, 8192, 16384), kdp = clamp(32767 - RDAMP * 160, 6000, 32767);
    (void)n;
    hall_send(rev_in);
    for (k = 0; k < PL_N; k++) {
        bf[k] = rev_x.s + o;
        o += PL_LEN[k];
    }
    for (i = 0; i < HL_NC; i++) {
        int32_t x = hl.bx[i] >> 1, a, b, d, z, f, L, R, w;
        uint32_t di, idx;
        for (k = 0; k < 4u; k++)
            x = pl_ap(bf[k], &pl.i[k], PL_LEN[k], PL_G[k], x);
        /* left side: x + the right's end -> modulated allpass (its delay swings 336 +- 4) -> delay -> low-pass -> decay -> allpass -> delay */
        pl.ph += (uint32_t)(1.0 * 4294967296.0 / 22050.0);
        pl.ph2 += (uint32_t)(1.27 * 4294967296.0 / 22050.0);
        a = x + sh_tz(pl.tr * dk, 15);
        d = (336 << 8) + ((osc_sine(pl.ph) * 4) >> 7);
        di = (uint32_t)d >> 8, f = d & 255;
        w = pl.i[4];
        idx = w >= di ? w - di : w + PL_LEN[4] - di;
        z = bf[4][idx];
        idx = idx ? idx - 1u : PL_LEN[4] - 1u;
        z += ((bf[4][idx] - z) * f) >> 8;
        b = a - sh_tz(z * 22938, 15);
        bf[4][w] = (int16_t)clamp(b, -32768, 32767);
        if (++pl.i[4] >= PL_LEN[4])
            pl.i[4] = 0;
        a = z + sh_tz(b * 22938, 15);
        z = bf[5][pl.i[5]];
        bf[5][pl.i[5]] = (int16_t)clamp(a, -32768, 32767);
        if (++pl.i[5] >= PL_LEN[5])
            pl.i[5] = 0;
        a = (z - pl.lpl) * kdp + pl.lel;                /* the step's remainder kept: the tail ends in exact silence */
        pl.lel = a & 32767;
        pl.lpl += a >> 15;
        a = pl_ap(bf[6], &pl.i[6], PL_LEN[6], g2, pl.lpl);
        z = bf[7][pl.i[7]];
        bf[7][pl.i[7]] = (int16_t)clamp(a, -32768, 32767);
        if (++pl.i[7] >= PL_LEN[7])
            pl.i[7] = 0;
        pl.tl = z;
        /* right side: the same, its own lengths, fed by the left's end */
        a = x + sh_tz(pl.tl * dk, 15);
        d = (454 << 8) + ((osc_sine(pl.ph2) * 4) >> 7);
        di = (uint32_t)d >> 8;
        f = d & 255;
        w = pl.i[8];
        idx = w >= di ? w - di : w + PL_LEN[8] - di;
        z = bf[8][idx];
        idx = idx ? idx - 1u : PL_LEN[8] - 1u;
        z += ((bf[8][idx] - z) * f) >> 8;
        b = a - sh_tz(z * 22938, 15);
        bf[8][w] = (int16_t)clamp(b, -32768, 32767);
        if (++pl.i[8] >= PL_LEN[8])
            pl.i[8] = 0;
        a = z + sh_tz(b * 22938, 15);
        z = bf[9][pl.i[9]];
        bf[9][pl.i[9]] = (int16_t)clamp(a, -32768, 32767);
        if (++pl.i[9] >= PL_LEN[9])
            pl.i[9] = 0;
        a = (z - pl.lpr) * kdp + pl.ler;
        pl.ler = a & 32767;
        pl.lpr += a >> 15;
        a = pl_ap(bf[10], &pl.i[10], PL_LEN[10], g2, pl.lpr);
        z = bf[11][pl.i[11]];
        bf[11][pl.i[11]] = (int16_t)clamp(a, -32768, 32767);
        if (++pl.i[11] >= PL_LEN[11])
            pl.i[11] = 0;
        pl.tr = z;
        /* the outputs: Dattorro's taps (halved), seven each */
        L = pl_tap(bf[9], pl.i[9], PL_LEN[9], 133) + pl_tap(bf[9], pl.i[9], PL_LEN[9], 1487) - pl_tap(bf[10], pl.i[10], PL_LEN[10], 957) +
            pl_tap(bf[11], pl.i[11], PL_LEN[11], 998) - pl_tap(bf[5], pl.i[5], PL_LEN[5], 995) - pl_tap(bf[6], pl.i[6], PL_LEN[6], 94) -
            pl_tap(bf[7], pl.i[7], PL_LEN[7], 533);
        R = pl_tap(bf[5], pl.i[5], PL_LEN[5], 177) + pl_tap(bf[5], pl.i[5], PL_LEN[5], 1814) - pl_tap(bf[6], pl.i[6], PL_LEN[6], 614) +
            pl_tap(bf[7], pl.i[7], PL_LEN[7], 1337) - pl_tap(bf[9], pl.i[9], PL_LEN[9], 1056) - pl_tap(bf[10], pl.i[10], PL_LEN[10], 168) -
            pl_tap(bf[11], pl.i[11], PL_LEN[11], 61);
        hl.l[i] = L;
        hl.r[i] = R;
    }
    for (i = 0; i < HL_NC; i++) {                       /* back to 44.1 kHz, between samples */
        int32_t mm = (hl.l[i] + hl.r[i]) * PL_OUT >> 3, ss = (hl.l[i] - hl.r[i]) * PL_OUT >> 3;
        out[2u * i] += (mp + mm) >> 1;
        out[2u * i + 1u] += mm;
        rev_side[2u * i] = (sp + ss) >> 1;
        rev_side[2u * i + 1u] = ss;
        mp = mm;
        sp = ss;
    }
    hl.m = mp, hl.s = sp;
    hl.side = 1;
}

/* the reverb's buffers and states to silence (the model changed) */
static void rev_clear(void)
{
    uint32_t i;
    for (i = 0; i < sizeof rev_comb / 2u; i++)
        rev_comb[i] = 0;
    for (i = 0; i < sizeof rev_u.ap / 2u; i++)          /* (int16: 997 of them, an odd count the int32 view misses one of) */
        rev_u.ap[i] = 0;
    for (i = 0; i < 4u; i++)
        fx.comb_lp[i] = 0;
    for (i = 0; i < HL_NL; i++)
        hl.lp[i] = hl.le[i] = 0;
    fx.sp_lp = fx.sp_hp = fx.sp_he = 0;
    hl.x = hl.m = hl.s = hl.hp = hl.he = 0;
    for (i = 0; i < RX_LEN; i++)
        rev_x.s[i] = 0;
    for (i = 0; i < 4u; i++)
        fx.comb_lpr[i] = 0;
    for (i = 0; i < HL_NAPX; i++)
        hl.d[i] = 0;
    fx.sp2_lp = fx.sp2_hp = fx.sp2_he = 0;
    for (i = 0; i < PL_N; i++)
        pl.i[i] = 0;
    pl.lpl = pl.lpr = pl.lel = pl.ler = pl.tl = pl.tr = 0;
}

static int32_t part_buf[CTL];                            /* the part's block (mix_part); the fade of a model change */

/* run reverb model ty over a block, added to out (+ rev_side). FMini-1: ty is the TYPE (0 ROOM, 1 HALL, 2 PLATE, 3 SPRING, 4 CLOUD);
 * Felucca's: 0 ROOM, 1 SPRING, 2 HALL */
static void rev_run(uint32_t ty, const int32_t *rev_in, int32_t *out, uint32_t n)
{
    if (ty == 0u)
        rev_room(rev_in, out, n);
    else if (ty == 2u)
        rev_plate(rev_in, out, n);
    else if (ty == 3u)
        rev_spring(rev_in, out, n);
    else {
        hl.mode = (uint8_t)(ty == 1u ? 0u : 1u);
        rev_hall(rev_in, out, n);
    }
}

/* the model changed (to rt): the old one's block fades out (HALL's stereo difference with it), its buffers are
 * cleared, the new one starts from silence */
static __attribute__((noinline)) void rev_switch(const int32_t *rev_in, int32_t *wet, uint32_t n, int32_t rt)
{
    int32_t *t = part_buf, g = 65536, d = 65536 / (int32_t)n;
    uint32_t i;
    for (i = 0; i < n; i++)
        t[i] = 0;
    rev_run(fx.rtype, rev_in, t, n);
    for (i = 0; i < n; i++, g -= d) {
        wet[i] += mulq16(t[i], (uint32_t)g);
        rev_side[i] = mulq16(rev_side[i], (uint32_t)g);   /* (HALL only: hl.side set) */
    }
    rev_clear();
    fx.rtype = (uint8_t)rt;
}

static int32_t fx_any(const int32_t *x, uint32_t n)      /* a bus with anything on it */
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (x[i])
            return 1;
    return 0;
}

/* FMini-1: the chorus's three TYPEs (params.c N_CHOTYPE), all stereo (wet: the mid in wet, left minus right in dside), on cho_buf, wet only (MIX
 * is the send): the delay swing is cdepth (the typical pitch swing, scaled by DEPTH and by RATE: see fx_buses), the LFO runs at RATE.
 *   STEREO after Boss's CE-2: a single BBD line's soft chorus: a triangle LFO about 6.8 ms, dark (the chain's ~4 kHz), the right side the same
 *   line a quarter of the LFO later (a round swirl rather than a ping-pong). JUNO after the Roland Juno-60/106 chorus: a triangle LFO, the two
 *   sides' delays in opposite phase about 3.4 ms, a shallower swing but brighter (~7 kHz). MULTI: a multi-chorus: four voices at 11 .. 17 ms in
 *   quadrature (sine), 0 and 180 degrees on the left, 90 and 270 on the right: a thick, slow ensemble. */
static inline int32_t cho_tap(uint32_t wp, int32_t r)
{
    uint32_t ri = (uint32_t)r >> 8;
    int32_t f = r & 255, c0 = cho_buf[(wp - ri) & (CHO_LEN - 1u)], c1 = cho_buf[(wp - ri - 1u) & (CHO_LEN - 1u)];
    return (c0 + (((c1 - c0) * f) >> 8)) << 1;
}
static __attribute__((noinline)) void cho_block(const int32_t *cho_in, int32_t *wet, uint32_t n, int32_t cdepth, uint32_t cinc)
{
    uint32_t i, ty = (uint32_t)song.g[G_ENGGO] % 3u;
    int32_t d = ty == 0u ? (cdepth * 3) >> 2 : ty == 1u ? cdepth >> 1 : (cdepth * 5) >> 3, kc = ty == 0u ? 14500 : ty == 1u ? 21500 : 26000;
    for (i = 0; i < n; i++) {
        uint32_t wp = fx.cho_w;
        int32_t l, r;
        cho_buf[wp & (CHO_LEN - 1u)] = (int16_t)clamp(cho_in[i] >> 1, -32768, 32767);
        fx.cho_ph += cinc;
        if (ty == 0u) {
            l = cho_tap(wp, (300 << 8) + (((osc_tri(fx.cho_ph) + 32768) * d) >> 8));
            r = cho_tap(wp, (300 << 8) + (((osc_tri(fx.cho_ph + 0x40000000u) + 32768) * d) >> 8));
        } else if (ty == 1u) {
            int32_t a = osc_tri(fx.cho_ph) + 32768;
            l = cho_tap(wp, (150 << 8) + ((a * d) >> 8));
            r = cho_tap(wp, (150 << 8) + (((65535 - a) * d) >> 8));
        } else {
            int32_t v1 = cho_tap(wp, (500 << 8) + (((osc_sine(fx.cho_ph) + 32768) * d) >> 8));
            int32_t v2 = cho_tap(wp, (640 << 8) + (((osc_sine(fx.cho_ph + 0x40000000u) + 32768) * d) >> 8));
            int32_t v3 = cho_tap(wp, (420 << 8) + (((osc_sine(fx.cho_ph + 0x80000000u) + 32768) * d) >> 8));
            int32_t v4 = cho_tap(wp, (760 << 8) + (((osc_sine(fx.cho_ph + 0xC0000000u) + 32768) * d) >> 8));
            l = (6 * v1 + 2 * v2 + 6 * v3 + 2 * v4) >> 4;
            r = (2 * v1 + 6 * v2 + 2 * v3 + 6 * v4) >> 4;
        }
        fx.cho_lp[0] += mulq15(l - fx.cho_lp[0], kc);
        fx.cho_lp[1] += mulq15(r - fx.cho_lp[1], kc);
        wet[i] += (fx.cho_lp[0] + fx.cho_lp[1]) >> 1;
        dside[i] += (fx.cho_lp[0] - fx.cho_lp[1]) >> 1;
        fx.cho_w++;
    }
}

/* FMini-1: the delay's seven TYPEs (params.c N_DLYTYPE: ANALOG DIGITAL TAPE P-PONG MULTI SLAP REVERSE), one loop each over the block, all on
 * dly_buf: the sample d back (1 .. 65535), or silence if the delay started less than d samples ago (older ones are from before) */
static inline int32_t dly_at(uint32_t d)
{
    return d <= fx.dly_w - fx.dly_start ? dly_buf[(fx.dly_w - d) & (DLY_LEN - 1u)] : 0;
}
static inline int32_t dly_atq(uint32_t dq)                  /* Q8 samples back, linear between two samples */
{
    uint32_t d = dq >> 8;
    int32_t a = dly_at(d), b = dly_at(d + 1u);
    return a + (((b - a) * (int32_t)(dq & 255u)) >> 8);
}
static inline void dly_put(int32_t v)                       /* the loop's input (clamped) */
{
    dly_buf[fx.dly_w & (DLY_LEN - 1u)] = (int16_t)clamp(v, -32768, 32767);
    fx.dly_w++;
}
#define DLY_MAXQ ((int32_t)(DLY_LEN - 4u) << 8)
/* ANALOG: a BBD chain's echo: dark (the loop's low-pass ~2.6 kHz: every repeat darker), a slow drift of the clock (+-12 samples at 1 Hz), the
 *   loop saturating softly (the chain's compander and its clipping). TAPE: wow (+-24 samples at 0.6 Hz) and flutter (+-1.5 at 6.3 Hz), a low
 *   cut in the loop (~110 Hz), darker still (~3.4 kHz), and a harder saturation: repeats compress and blur. DIGITAL: clean, exact, the loop's
 *   low-pass at ~11 kHz only. SLAP: one short echo (31 .. 170 ms from TIME), its feedback a third (a repeat or two), a touch of tape.
 *   P-PONG: one line of 2T: the left is the line's T, the right its 2T (the feedback's source): echoes alternate L R L R from T. MULTI: four taps
 *   at T/4 T/2 3T/4 T spread over the two sides, the feedback from the last. REVERSE: two heads read the last T backwards through triangles (a
 *   chunk of T played backwards, crossfaded), the feedback takes the reversed sound (the next repeat is forward again). P-PONG and REVERSE
 *   reach 743 ms (the line's 65 k samples hold 2T). */
static __attribute__((noinline)) void dly_block(const int32_t *dly_in, int32_t *wet, uint32_t n, int32_t fb)
{
    uint32_t i, ty = (uint32_t)song.g[G_DTIME] % 7u, w;
    int32_t dmix = song.g[G_DMIX] * 258, tgt = (int32_t)delay_samples() << 8, st = tgt - fx.dl_q, slap;
    fx.dl_q += clamp(st, -2048, 2048);                  /* up to 8 samples a block: +-25 % speed: a quick, smooth bend */
    slap = (1323 << 8) + (((fx.dl_q >> 6) * 3277) >> 9);   /* SLAP's time, 30 ms + TIME / 10 */
    for (i = 0; i < n; i++) {
        int32_t x, in = dly_in[i] >> 1, y;
        switch (ty) {
        case 0u:                                        /* ANALOG */
            fx.dly_ph += 97392u;
            x = dly_atq((uint32_t)clamp(fx.dl_q + ((osc_sine(fx.dly_ph) * 12) >> 7), 4096, DLY_MAXQ));
            fx.dly_lp += mulq15(x - fx.dly_lp, 11500);
            dly_put(in + mulq15(softclip(fx.dly_lp), fb));
            wet[i] += mulq15(fx.dly_lp << 1, dmix);
            break;
        case 1u:                                        /* DIGITAL */
            x = dly_atq((uint32_t)clamp(fx.dl_q, 4096, DLY_MAXQ));
            fx.dly_lp += mulq15(x - fx.dly_lp, 28000);
            dly_put(in + mulq15(fx.dly_lp, fb));
            wet[i] += mulq15(x << 1, dmix);
            break;
        case 2u:                                        /* TAPE */
            fx.dly_ph += 58437u;
            fx.dly_ph2 += 629846u;
            x = dly_atq((uint32_t)clamp(fx.dl_q + ((osc_sine(fx.dly_ph) * 24 + osc_sine(fx.dly_ph2) * 3 / 2) >> 7), 4096, DLY_MAXQ));
            fx.dly_lp += mulq15(x - fx.dly_lp, 8800);
            fx.dly_hp += (fx.dly_lp - fx.dly_hp) >> 6;
            dly_put(in + mulq15((softclip((fx.dly_lp - fx.dly_hp) * 3 >> 1) * 21845) >> 15, fb));   /* (unity at small signals: the loop's gain stays fb) */
            wet[i] += mulq15(fx.dly_lp << 1, dmix);
            break;
        case 3u: {                                      /* P-PONG */
            uint32_t t = (uint32_t)clamp(fx.dl_q >> 8, 16, 32767);
            int32_t l = dly_at(t), r = dly_at(2u * t);
            fx.dly_lp += mulq15(r - fx.dly_lp, 22000);
            dly_put(in + mulq15(fx.dly_lp, fb));
            wet[i] += mulq15(l + r, dmix);
            dside[i] += mulq15(l - r, dmix);
            break;
        }
        case 4u: {                                      /* MULTI */
            uint32_t t = (uint32_t)clamp(fx.dl_q >> 8, 64, 65000);
            int32_t t1 = dly_at(t >> 2), t2 = dly_at(t >> 1), t3 = dly_at((t * 3u) >> 2), t4 = dly_at(t), l, r;
            fx.dly_lp += mulq15(t4 - fx.dly_lp, 24000);
            dly_put(in + mulq15(fx.dly_lp, fb));
            l = (22 * t1 + 16 * t3 + 13 * t4) >> 5;
            r = (19 * t2 + 8 * t3 + 13 * t4) >> 5;
            wet[i] += mulq15(l + r, dmix);
            dside[i] += mulq15(l - r, dmix);
            break;
        }
        case 5u:                                        /* SLAP */
            fx.dly_ph += 48696u;
            x = dly_atq((uint32_t)clamp(slap + ((osc_sine(fx.dly_ph) * 2) >> 7), 4096, DLY_MAXQ));
            fx.dly_lp += mulq15(x - fx.dly_lp, 20000);
            dly_put(in + mulq15(softclip(fx.dly_lp), mulq15(fb, 11000)));
            wet[i] += mulq15(fx.dly_lp << 1, dmix);
            break;
        default: {                                      /* REVERSE */
            int32_t o = 0, k;
            w = fx.dly_w;
            for (k = 0; k < 2; k++) {
                uint32_t c = fx.rv_c[k], nn = fx.rv_n[k];
                int32_t wt = (int32_t)(c < nn / 2u ? c : nn - c) * (int32_t)fx.rv_i[k];
                uint32_t age = w - fx.rv_p[k];
                o += mulq15(age <= w - fx.dly_start && age < DLY_LEN - 2u ? dly_buf[fx.rv_p[k] & (DLY_LEN - 1u)] : 0, wt);
                fx.rv_p[k]--;
                if (++c >= nn) {                        /* a new sweep from the newest sample, the length from TIME now */
                    c = 0;
                    nn = (uint32_t)clamp(fx.dl_q >> 8, 441, 32000);
                    fx.rv_p[k] = w;
                    fx.rv_n[k] = nn;
                    fx.rv_i[k] = 65536u / nn;
                }
                fx.rv_c[k] = c;
            }
            fx.dly_lp += mulq15(o - fx.dly_lp, 20000);
            dly_put(in + mulq15(fx.dly_lp, fb));
            wet[i] += mulq15(o << 1, dmix);
            break;
        }
        }
        (void)y;
    }
}

/* process the three buses for one block; sends in, wet stereo-equal out. FMini-1: a bus whose send is 0 on every part
 * (the FX page's MIX at 0) and whose input is silent is OFF: nothing of it runs (its state cleared once), the CPU is free */
static void fx_buses(const int32_t *cho_in, const int32_t *dly_in, const int32_t *rev_in, int32_t *wet,
                     uint32_t n)
{
    uint32_t i, dl = delay_samples();
    int32_t fb = song.g[G_DFDBK] * 230, col = 2000 + song.g[G_DCOLOR] * 240;
    int32_t dmix = song.g[G_DMIX] * 258;
    int32_t cdepth = song.g[G_CDEPTH] * 6, rt;
    uint32_t cinc = LFO_INC[song.g[G_CRATE] & 127] / CTL;
    uint32_t cho_act = 1, dly_act = 1, rev_act = 1;
    {
        uint32_t sc = 0, sd = 0, sr = 0, k;
        for (k = 0; k < NTRK; k++) {
            sc |= (uint32_t)(trk[k].p[P_CHOR] > 0);
            sd |= (uint32_t)(trk[k].p[P_DLY] > 0);
            sr |= (uint32_t)(trk[k].p[P_REV] > 0);
        }
        cho_act = sc || fx_any(cho_in, n);
        dly_act = !cpu_high && (sd || fx_any(dly_in, n));
        rev_act = sr || fx_any(rev_in, n);
    }
    /* FMini-1: RATE only changes the rate. The depth is a fixed, typical pitch swing (about +-16 cents, 25% above the first pick): the delay swing
     * A (samples, half the peak to peak) shrinks as the rate rises (pitch deviation = 2 pi f A / FS), at most +-3.1 ms */
    cdepth = (int32_t)clamp((int32_t)(6400000u / (cinc ? cinc : 1u)), 2, 137) * 3;
    cdepth = cdepth * (int32_t)song.g[G_CDEPTH] / 60;             /* DEPTH (pop-up): 60 = the typical swing, 0 = none, 127 = about twice */
    if (!cho_act && fx.cho_on) {
        for (i = 0; i < CHO_LEN; i++)
            cho_buf[i] = 0;
        fx.cho_lp[0] = fx.cho_lp[1] = 0;
        fx.cho_on = 0;
    }
    if (dly_act && !fx.dly_on) {
        fx.dly_start = fx.dly_w;                         /* what the buffer holds is old: it reads as silence until rewritten */
        fx.dly_lp = fx.dly_hp = 0;
        fx.dl_q = (int32_t)dl << 8;                      /* (no bend from the last time) */
        fx.rv_n[0] = 0;                                  /* REVERSE: its heads start afresh */
    }
    fx.dly_on = (uint8_t)dly_act;
    fx.cho_on = (uint8_t)cho_act;
    for (i = 0; i < n; i++)
        wet[i] = dside[i] = 0;
    fx.ds_on = 0;
    if (cho_act) {
        cho_block(cho_in, wet, n, cdepth, cinc);
        fx.ds_on = 1;
    }
    if (dly_act) {
        uint32_t ty = (uint32_t)song.g[G_DTIME] % 7u;
        fx.ds_on |= (uint8_t)(ty == 3u || ty == 4u);
        if (ty == 6u && !fx.rv_n[0]) {                   /* REVERSE: two heads half a sweep apart, the newest sample first */
            uint32_t nn = (uint32_t)clamp(fx.dl_q >> 8, 441, 32000);
            fx.rv_n[0] = fx.rv_n[1] = nn;
            fx.rv_i[0] = fx.rv_i[1] = 65536u / nn;
            fx.rv_c[0] = 0;
            fx.rv_c[1] = nn / 2u;
            fx.rv_p[0] = fx.dly_w - 1u;
            fx.rv_p[1] = fx.dly_w - 1u - 2u * (nn / 2u);
        }
        dly_block(dly_in, wet, n, fb);
    }
    (void)dmix, (void)col;
    rt = song.g[G_RTYPE];
    rt = rt < 0 ? 0 : rt > 4 ? 4 : rt;                    /* (a stored 5, CLOUD before SHIMMER went, is the last one too) */
    if (!rev_act) {                                       /* OFF: its buffers cleared once, nothing runs */
        if (fx.rev_on) {
            rev_clear();
            fx.rev_on = 0;
        }
        hl.side = 0;
        return;
    }
    fx.rev_on = 1;
    hl.side = 0;
    if (rt != fx.rtype)
        rev_switch(rev_in, wet, n, rt);
    else
        rev_run((uint32_t)rt, rev_in, wet, n);
}

/* after the buses: HALL's stereo difference to the left and from the right of the dry mix (the wet bus adds the
 * same to both) */
static __attribute__((noinline)) void rev_side_mix(int32_t *l, int32_t *r, uint32_t n)
{
    uint32_t i;
    if (fx.ds_on)
        for (i = 0; i < n; i++) {
            l[i] += dside[i];
            r[i] -= dside[i];
        }
    if (!hl.side)
        return;
    for (i = 0; i < n; i++) {
        l[i] += rev_side[i];
        r[i] -= rev_side[i];
    }
}

static int32_t send_c[CTL], send_d[CTL], send_r[CTL], wet[CTL], mix_l[CTL], mix_r[CTL];

/* the synth part into the dry mix and the sends; with no voice sounding it costs the LFO tick and a cleared buffer only
 * (after the DIST tail has run out) */
static void mix_part(track_t *t, uint32_t n)
{
    int32_t *b = part_buf;
    uint32_t i;
    if (track_render(t, b, n))
        t->tail = 16;                                   /* blocks of DIST state to run out after the last voice */
    else if (!t->tail || !t->p[P_DIST] || !--t->tail)
        return;
    track_dist(t, b, n);
    {
        int32_t lvl = LEVEL_Q12[t->p[P_LEVEL] & 127], pan = t->p[P_PAN];
        int32_t gl = 4096 - (pan > 0 ? pan * 64 : 0), gr = 4096 + (pan < 0 ? pan * 64 : 0);
        int32_t c = t->p[P_CHOR] * 258, d = t->p[P_DLY] * 258, r = t->p[P_REV] * 258, pk = t->peak;
        int32_t xmax = c > d ? c : d;
        xmax = 0x7FFFFFFF / ((xmax > r ? xmax : r) | 1);   /* sends: loud chords at a high LEVEL */
        for (i = 0; i < n; i++) {
            int32_t x = ((b[i] >> 2) * lvl) >> 10, a = x < 0 ? -x : x;   /* pre-shift: 8 loud voices */
            int32_t xs = clamp(x, -xmax, xmax);         /* sends: mulq15 would overflow */
            if (a > pk)
                pk = a;
            if (c)
                send_c[i] += mulq15(xs, c);
            if (d)
                send_d[i] += mulq15(xs, d);
            if (r)
                send_r[i] += mulq15(xs, r);
            mix_l[i] += (x * gl) >> 12;
            mix_r[i] += (x * gr) >> 12;
        }
        t->peak = pk;
    }
}

static void events_block(uint32_t n);                 /* (seq.c) */
#include "eng_rotary.c"             /* the rotary speaker, CLASSIC */
#include "eng_rotary_light.c"       /* ... and LIGHT, and the switch between the two */
#include "eng_drive.c"              /* the drive: the organ's preamp, before the master and the rotary */
#include "eng_eq.c"                 /* the equaliser: after MASTER, before the limiter */
_Static_assert(CTL <= DR_MAX, "the drive's longest block");
static dr_t dr_st;
static eq_t eq_st;
static volatile uint8_t eq_on;                          /* the equaliser, on or off (the patch) */
static volatile uint8_t dr_on;                          /* 0 off, 1 on (the effects section sets it) */
/* the rotary speaker (eng_rotary.c) between the mix and the master: off (rot_on 0) the mix is as it was; on, the mono
 * sum of the mix and the reverb's return goes through the cabinet and comes out in stereo */
static rot_t rot_st __attribute__((section(".pool")));
static lt_t lt_st __attribute__((section(".pool")));
static rotmix_t rot_mix;
static volatile uint8_t rot_on;
static volatile uint8_t rot_model_req;                  /* 0 CLASSIC, 1 LIGHT (the effects section sets it) */

/* the organ's effects settings (the patch bytes OP_ROT .. OP_DRV_FAT), taken at the next block after a change */
/* the drive's output gain: setBfree's 0.8795 and 3 dB (the curve saturates: with a few keys it comes out 2 to 16 dB below the dry mix, with a full chord about 2 dB) */
#define DRV_OG 5090
static const uint16_t DRV_IG[13] = {3670, 4621, 5817, 7324, 9220, 11607, 14612, 18396, 23159, 29156, 36705, 40960, 40960};
static const int32_t DRV_FB[9] = {9766017, 9766017, 9766017, 13150415, 15099494, 15099494, 15099494, 15099494, 15099494};
static const int32_t DRV_FB2[9] = {16760439, 13376041, 9766017, 9766017, 9766017, 11431356, 13207717, 14984078, 16760439};
static const uint16_t ROT_BAL_HL[9] = {5734, 6820, 8110, 9644, 11469, 13639, 16220, 19288, 22938};
static uint8_t fxo_seen = 0xFF;
/* CPU overload (audio.c shed_voice): CPU_HIGH_BLOCKS blocks (2 s) of "CPU HIGH" per request, during which the delay is off and
 * the oldest key is let go (eng_organ.c org_fragment). The rotary and the drive are left as they are */
#define CPU_HIGH_BLOCKS 2756u
static void fx_cpu_high(void)
{
    cpu_high = CPU_HIGH_BLOCKS;
    org_shed_req = 1;
}
static void fx_organ_apply(void)
{
    uint32_t sp, g, f;
    if (org_pgen == fxo_seen)
        return;
    fxo_seen = org_pgen;
    rot_on = org_patch[OP_ROT];
    rot_model_req = org_patch[OP_ROT_MODEL];
    sp = org_patch[OP_ROT_SPEED];       /* setBfree's speeds: horn 0.672 / 7.056 Hz, drum 0.600 / 5.955 Hz */
    rot_target(&rot_st, sp ? rot_inc(sp == 1u ? 672u : 7056u) : 0u, sp ? rot_inc(sp == 1u ? 600u : 5955u) : 0u);
    rot_st.hset = org_patch[OP_ROT_HORN];
    rot_mic(&rot_st, 180u - 30u * org_patch[OP_ROT_MIC]);
    rot_st.hl = ROT_BAL_HL[org_patch[OP_ROT_BAL] % 9u];
    g = org_patch[OP_DRV_GAIN] % 13u;
    f = org_patch[OP_DRV_FAT] % 9u;
    dr_set(&dr_st, DRV_IG[g], DRV_OG, DRV_FB[f], DRV_FB2[f], 10425000, 35043, 157700, 16626000);
    dr_on = org_patch[OP_DRV];
    eq_set(&eq_st, org_patch[OP_EQ_LOW], org_patch[OP_EQ_MID], org_patch[OP_EQ_FREQ], org_patch[OP_EQ_HIGH]);
    eq_on = org_patch[OP_EQ];
}

/* the effects come in and out over about 20 ms (FX_FADE a block, Q15) */
#define FX_FADE 1170
static int32_t dr_w, rot_w, eq_w;                             /* Q15: how much of the drive / the rotary is in the mix */
#define ROT_IDLE_BLOCKS 1500u                           /* quiet blocks (32 samples) before the cabinet is emptied */
static uint32_t rot_idle, eq_idle;
static inline int32_t fx_lerp(int32_t a, int32_t b, int32_t w)      /* a + (b - a) w, w Q15; |b - a| < 2^20 */
{
    return a + ((((b - a) >> 2) * (w >> 3)) >> 10);
}

/* the equaliser on a block of the stereo output (after MASTER, before the limiter), in place: its share eq_w fades in and out
 * like the other effects' (a block's step, Q15); the filters' states are emptied at turn-on and after 1500 quiet blocks (1.1 s:
 * the rounding of a filter this low leaves a faint tone running after the last sound) */
static void fx_eq_apply(int32_t *l, int32_t *r, uint32_t n)
{
    int32_t el[CTL], er[CTL], pk = 0;
    uint32_t i;
    if (eq_on && !eq_w)
        eq_clear(&eq_st);
    eq_w += eq_on ? FX_FADE : -FX_FADE;
    eq_w = eq_w < 0 ? 0 : eq_w > 32768 ? 32768 : eq_w;
    for (i = 0; i < n; i++) {
        int32_t m = (l[i] < 0 ? -l[i] : l[i]) + (r[i] < 0 ? -r[i] : r[i]);
        pk = m > pk ? m : pk;
        el[i] = l[i];
        er[i] = r[i];
    }
    eq_block(&eq_st, el, er, n);
    if (eq_w == 32768)
        for (i = 0; i < n; i++) {
            l[i] = el[i];
            r[i] = er[i];
        }
    else
        for (i = 0; i < n; i++) {
            l[i] = fx_lerp(l[i], el[i], eq_w);
            r[i] = fx_lerp(r[i], er[i], eq_w);
        }
    if (pk >= 8)
        eq_idle = 0;
    else if (eq_idle < ROT_IDLE_BLOCKS + 1u && ++eq_idle == ROT_IDLE_BLOCKS)
        eq_clear(&eq_st);
}

static void mix_block(int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t mg = fx_usb_fixed ? MASTER_FULL : (int32_t)song.master_q12;   /* (USB LEVEL FIXED: MASTER after) */
    int32_t ol[CTL], orr[CTL], eq_a[CTL], eq_b[CTL];
    fx_organ_apply();
    if (cpu_high)
        cpu_high--;
    for (i = 0; i < n; i++)
        send_c[i] = send_d[i] = send_r[i] = mix_l[i] = mix_r[i] = 0;
    events_block(n);
    for (i = 0; i < NPART; i++)
        mix_part(&trk[i], n);
    for (i = 0; i < NPART; i++)                         /* the LFOs' blocks (voice.c) */
        track_lfo_end(&trk[i]);
    fx_buses(send_c, send_d, send_r, wet, n);
    if (dr_on || dr_w) {                                /* the preamp: the mono dry sum, before MASTER. The mix has setBfree's */
        int32_t m[CTL];                                 /* 1.0 at 16384 (the organ is half its scale): the drive takes Q13 (1.0 = 8192, */
        if (dr_on && !dr_w)                             /* up to 4.0) and gives Q15 */
            dr_clear(&dr_st);
        for (i = 0; i < n; i++) {
            int32_t v = (mix_l[i] + mix_r[i]) >> 2;
            m[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
        dr_run(&dr_st, m, m, n);
        dr_w += dr_on ? FX_FADE : -FX_FADE;
        dr_w = dr_w < 0 ? 0 : dr_w > 32768 ? 32768 : dr_w;
        for (i = 0; i < n; i++) {
            int32_t dry = (mix_l[i] + mix_r[i]) >> 1;
            mix_l[i] = mix_r[i] = fx_lerp(dry, m[i] >> 1, dr_w);
        }
    }
    if (rot_on || rot_w) {                              /* the rotary: the mono sum of the mix and the reverb's return, in stereo */
        int32_t xin[CTL];
        if (rot_on && !rot_w) {                         /* coming in: the cabinet starts empty */
            rot_flush(&rot_st);
            lt_init(&lt_st, &rot_st);
            rot_mix.fade = 0;
        }
        int32_t pk = 0;
        for (i = 0; i < n; i++) {
            int32_t a;
            int32_t v = ((mix_l[i] + mix_r[i]) >> 1) + wet[i];  /* (MASTER after the cabinet: at 0 it is silent, whatever the cabinet's states hold) */
            a = v < 0 ? -v : v;
            pk = a > pk ? a : pk;
            xin[i] = v >> 2;                            /* the cabinet takes at most 1.0 (32767): a chord is several times that, and the dry */
        }                                               /* path leaves it to the limiter; a hard clip here was the distortion. Back x4 below */
        /* the rounding of the cabinet's filters leaves a faint tone (up to 20 / 32768) running after the last sound: once the
         * input has been below 8 for 1.1 s (the cabinet's tail is long gone) its states are emptied: silence is silence */
        if (pk >= 8)
            rot_idle = 0;
        else if (rot_idle < ROT_IDLE_BLOCKS + 1u && ++rot_idle == ROT_IDLE_BLOCKS) {
            rot_flush(&rot_st);
            lt_init(&lt_st, &rot_st);
        }
        if (rot_model_req != rot_mix.model)
            rotary_model(&rot_mix, &rot_st, &lt_st, rot_model_req);
        rotary_run(&rot_mix, &rot_st, &lt_st, xin, ol, orr, n);
        rot_w += rot_on ? FX_FADE : -FX_FADE;
        rot_w = rot_w < 0 ? 0 : rot_w > 32768 ? 32768 : rot_w;
        for (i = 0; i < n; i++) {
            ol[i] = (ol[i] * mg) >> 10;                 /* (x4: the input was taken down by 4) */
            orr[i] = (orr[i] * mg) >> 10;
        }
        if (rot_w == 32768) {
            for (i = 0; i < n; i++)
                eq_a[i] = ol[i], eq_b[i] = orr[i];
            if (eq_on || eq_w)
                fx_eq_apply(eq_a, eq_b, n);
            for (i = 0; i < n; i++) {
                int32_t l = eq_a[i], r = eq_b[i];
                master_out(&l, &r);
                out[2u * i] = l;
                out[2u * i + 1u] = r;
            }
            return;
        }
    }
    if (hl.side || fx.ds_on)
        rev_side_mix(mix_l, mix_r, n);                  /* the reverb's, the delay's / chorus's stereo */
    for (i = 0; i < n; i++) {
        int32_t l = (((mix_l[i] + wet[i]) >> 2) * mg) >> 10;
        int32_t r = (((mix_r[i] + wet[i]) >> 2) * mg) >> 10;
        if (rot_w) {                                    /* (fading in or out) */
            l = fx_lerp(l, ol[i], rot_w);
            r = fx_lerp(r, orr[i], rot_w);
        }
        eq_a[i] = l;
        eq_b[i] = r;
    }
    if (eq_on || eq_w)
        fx_eq_apply(eq_a, eq_b, n);
    for (i = 0; i < n; i++) {
        int32_t l = eq_a[i], r = eq_b[i];
        master_out(&l, &r);
        out[2u * i] = l;
        out[2u * i + 1u] = r;
    }
}
