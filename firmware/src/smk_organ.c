/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The SMK-37 port's organ mapping (BOARD_SMK37 only; the owner's decisions are smk37-port.yaml), on top of the board layer
 * of genkigenki/fm1-to-smk37 (hal/smk_input.h). main.c calls smk_frame() once a UI frame (before the LEDs and the screen)
 * and smk_poll() between frames; organ_app.c hands it the button presses (smk_buttons), draws its page (smk_draw) and
 * redraws for it (smk_sig).
 *  - The 16 pads are a Hammond's preset keys (the pads as commands: hal smk_pad_cmd, never notes). Top row (pads 1-8,
 *    hardware 0..7): the upper manual's drawbar sets, DRAWBAR PRESET MODE's first eight (organ_app.c OA_DBP, the keys
 *    C .. G of its middle octave); the one playing bright. Bottom row (9-16): LESLIE SLOW / FAST, the upper manual's
 *    VIB / CHORUS (UP), PERCUSSION, its SOFT, FAST, 3RD, OVERDRIVE (TUBE DRIVE at 2.5), REVERB, lit by their state:
 *    each runs DRAWBAR PRESET MODE's own switch (oa_dbp_key), with its 0.5 s message.
 *  - The 4 faders set drawbars in banks (FADER BANK: A 16' 5 1/3' 8' 4', B 2 2/3' 2' 1 3/5' 1 1/3', C 1', EXPR, DRIVE
 *    GAIN, REVERB SEND) of the manual KNOB BANK picks (UPPER, LOWER, PEDAL: its 16' and 8' on A's first two). A fader
 *    acts only once it moves (also after a bank or manual change), so nothing jumps; a move shows its value for 0.5 s.
 *  - The mod wheel is the Leslie's speed: up past 2/3 FAST, down under 1/3 SLOW (a switch with hysteresis, acting on the
 *    crossing, so the pads and the pedal can switch it too). The pitch wheel does nothing (an organ has no bend).
 *  - The pedal input (a footswitch) switches the Leslie SLOW / FAST, either polarity (its rest learnt).
 *  - < and >: the preset before / after. KNOB BANK and FADER BANK together: the diagnostics page (the keys, pads,
 *    faders, wheels, pedal, buttons, the ADC live: the serial console the kit's bring-up uses is not in OrganVave-1). */
#ifndef SMK_ORGAN_C
#define SMK_ORGAN_C

static uint8_t smk_man, smk_bank, smk_diag;      /* the faders' manual (0 UPPER, 1 LOWER, 2 PEDAL) and bank (0 A .. 2 C) */
static uint8_t smk_ftake = 1;                    /* a bank or manual change: every fader waits for its first move */

static uint32_t smk_len(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}
static char *smk_cpy(char *d, const char *s)     /* (the messages here are short: the buffers are 24 and 64 bytes) */
{
    char *o = d;
    while ((*o++ = *s++) != 0)
        ;
    return d;
}
static char *smk_cat(char *d, const char *s)
{
    smk_cpy(d + smk_len(d), s);
    return d;
}

/* ------------------------------------------------------------------ pads --- */
/* bottom row (hardware pads 8..15): the DRAWBAR PRESET MODE key that is each one's switch */
static const uint8_t SMK_PAD_KEY[8] = {0, 1, 21, 23, 24, 26, 20, 25};

static int smk_set_playing(uint32_t p)          /* the upper drawbars are set p */
{
    uint32_t i;
    for (i = 0; i < 9u; i++)
        if (org_patch[OP_UB + i] != OA_DBP[p][i])
            return 0;
    return 1;
}

static void smk_pads_in(void)
{
    int32_t e;
    smk_pad_cmd = 1;
    while ((e = smk_padcmd_take()) >= 0) {
        uint32_t p = (uint32_t)e >> 8;
        if (!(e & 0x7F) || p >= SMK_NPAD)
            continue;                            /* (a release: the switches act on the hit) */
        if (p < 8u) {
            oa_dbp_key(7u + p);                  /* the set: the keys C .. G of DRAWBAR PRESET MODE's middle octave */
            oa_flash("DRAWBARS");
        } else
            oa_dbp_key(SMK_PAD_KEY[p - 8u]);
        ui_touched();
    }
}

static void smk_pad_leds(void)
{
    uint32_t p, on;
    for (p = 0; p < 8u; p++)                     /* the drawbar sets: cream, the one playing bright */
        if (smk_set_playing(p))
            padled_set(p, 90, 64, 28);
        else
            padled_set(p, 7, 5, 2);
    /* LESLIE: red, bright FAST, half SLOW, dim off */
    if (org_patch[OP_ROT] && org_patch[OP_ROT_SPEED] == 2u)
        padled_set(8, 100, 0, 0);
    else if (org_patch[OP_ROT])
        padled_set(8, 24, 0, 0);
    else
        padled_set(8, 4, 0, 0);
    on = vib_up() != 0;                          /* VIB / CHORUS (UP): blue */
    padled_set(9, 0, on ? 24u : 1u, on ? 100u : 5u);
    for (p = 0; p < 4u; p++) {                   /* PERCUSSION, SOFT, FAST, 3RD: white (the three dimmer while it is off) */
        static const uint8_t OP[4] = {OP_PERC, OP_PERC_SOFT, OP_PERC_FAST, OP_PERC_SEL};
        uint32_t lv = org_patch[OP[p]] ? (p && !org_patch[OP_PERC] ? 22u : 70u) : 5u;
        padled_set(10u + p, (uint8_t)lv, (uint8_t)lv, (uint8_t)lv);
    }
    on = org_patch[OP_DRV] && dr_type_of(org_patch[OP_DRV_TYPE]) == 0u && org_patch[OP_DRV_GAIN] == 25u;
    padled_set(14, on ? 100u : 6u, on ? 36u : 2u, 0);   /* OVERDRIVE: orange */
    on = TSEL->p[P_REV] != 0;
    padled_set(15, 0, on ? 80u : 4u, on ? 12u : 1u);    /* REVERB: green */
}

/* ---------------------------------------------------------------- faders --- */
static const char *const SMK_BAR_NAME[9] = {"16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'"};
static const char *const SMK_MAN_NAME[3] = {"UPPER", "LOWER", "PEDAL"};
enum { SMK_F_NONE = 0xFF, SMK_F_EXPR = 0xF0, SMK_F_DRIVE, SMK_F_REVERB };

/* what fader f sets: a drawbar's patch byte, or one of SMK_F_*; *bar its footage (0..8) */
static uint32_t smk_fader_target(uint32_t f, uint32_t *bar)
{
    if (smk_bank == 2u && f > 0u)                /* C: EXPR, DRIVE GAIN, REVERB SEND (any manual) */
        return f == 1u ? SMK_F_EXPR : f == 2u ? SMK_F_DRIVE : SMK_F_REVERB;
    if (smk_man == 2u) {                         /* PEDAL: its 16' and 8' (setBfree's buses 0 and 2) on A */
        if (smk_bank || f > 1u)
            return SMK_F_NONE;
        *bar = f ? 2u : 0u;
        return OP_PB + *bar;
    }
    *bar = smk_bank * 4u + f;                    /* A 0..3, B 4..7, C 8 (1') */
    return (smk_man ? OP_LB : OP_UB) + *bar;
}

static void smk_fader_set(uint32_t f, uint32_t v)   /* v 0..127 */
{
    char m[32];
    uint32_t bar = 0, t = smk_fader_target(f, &bar), n;
    if (t == SMK_F_NONE)
        return;
    if (t == SMK_F_EXPR) {                       /* EXPR: up louder, 0 dB at the top, 60 dB down at the bottom */
        org_set(OP_EXPR, (127u - v) * 60u / 127u);
        n = org_patch[OP_EXPR];
        smk_cat(smk_cpy(m, "EXPR "), n ? "-" : "0 dB");
        if (n) {
            fmt_fix(m + smk_len(m), (int32_t)n, 0);
            smk_cat(m, " dB");
        }
    } else if (t == SMK_F_DRIVE) {               /* DRIVE GAIN 0.0 .. 10.0 (0 off), as HOME's DRIVE knob */
        org_set(OP_DRV_GAIN, (v * 100u + 63u) / 127u);
        org_set(OP_DRV, org_patch[OP_DRV_GAIN] > 0u ? 1u : 0u);
        smk_cpy(m, "DRIVE ");
        fmt_fix(m + smk_len(m), (int32_t)org_patch[OP_DRV_GAIN], 1);
    } else if (t == SMK_F_REVERB) {              /* REVERB SEND, as HOME's REVERB knob */
        TSEL->p[P_REV] = (int16_t)v;
        smk_cpy(m, "REVERB ");
        fmt_fix(m + smk_len(m), (int32_t)(v * 100u / 127u), 0);
        smk_cat(m, "%");
    } else {                                     /* a drawbar, 0..8 */
        org_set(t, (v * 8u + 63u) / 127u);
        smk_cat(smk_cat(smk_cpy(m, SMK_MAN_NAME[smk_man]), " "), SMK_BAR_NAME[bar]);
        smk_cat(m, ": ");
        fmt_fix(m + smk_len(m), (int32_t)org_patch[t], 0);
    }
    oa_flash(m);
}

static void smk_faders(void)
{
    static uint8_t last[4];
    uint32_t i;
    if (fm1_in.frames < 200u)                    /* (the first readings settle) */
        return;
    for (i = 0; i < 4u; i++) {
        uint32_t v = fm1_in.fader[i];
        if (smk_ftake) {                         /* soft takeover: from here, a fader acts once it moves */
            last[i] = (uint8_t)v;
            continue;
        }
        if (v == last[i])
            continue;
        last[i] = (uint8_t)v;
        smk_fader_set(i, v);
        ui_touched();
    }
    smk_ftake = 0;
}

/* ----------------------------------------------------- wheels and pedal --- */
static void smk_leslie(uint32_t fast)            /* SLOW / FAST (the Leslie on if it was off) */
{
    if (fast == (org_patch[OP_ROT] && org_patch[OP_ROT_SPEED] == 2u))
        return;
    org_set(OP_ROT, 1u);
    org_set(OP_ROT_SPEED, fast ? 2u : 1u);
    oa_flash(fast ? "FAST" : "SLOW");
}

/* the mod wheel, self-calibrating as the layer's (stock mux C address 5; up = a lower reading): its range grows to what
 * it reaches; 0..127. The Leslie on the crossings of 2/3 (FAST) and 1/3 (SLOW) */
static void smk_wheels(void)
{
    enum { DEAD = 12, MOD_LO = 485, MOD_HI = 615 };
    static int32_t mlo, mhi;
    static uint8_t zone = 0xFF;                  /* 0 low, 1 between, 2 high */
    int32_t m = fm1_in.wheel[1], mw;
    uint32_t z;
    if (fm1_in.frames < 300u)                    /* (the readings settle) */
        return;
    if (!mhi) {
        mlo = m < MOD_LO ? m : MOD_LO;
        mhi = m > MOD_HI ? m : MOD_HI;
    }
    if (m < mlo)
        mlo = m;
    if (m > mhi)
        mhi = m;
    mw = mhi - m <= DEAD ? 0 : (mhi - m - DEAD) * 127 / (mhi - mlo - DEAD > 1 ? mhi - mlo - DEAD : 1);
    z = mw > 85 ? 2u : mw < 42 ? 0u : 1u;
    if (zone == 0xFF)
        zone = (uint8_t)z;                       /* (where it rests at power-on: no switch) */
    else if (z != 1u && z != zone) {
        zone = (uint8_t)z;
        smk_leslie(z == 2u);
        ui_touched();
    } else if (z == 1u && zone == 0xFF)
        zone = 1;
}

/* the pedal input as a footswitch: its rest is the reading at power-on (or one held 5 s: plugged in later, the other
 * polarity); a press is 300 counts away from it, a release back within 150. A press switches the Leslie */
static void smk_pedal(void)
{
    static int32_t rest = -1;
    static uint32_t away_since;
    static uint8_t down;
    int32_t p = fm1_in.pedal, d;
    if (fm1_in.frames < 300u)
        return;
    if (rest < 0)
        rest = p;
    d = p > rest ? p - rest : rest - p;
    if (!down && d > 300) {
        down = 1;
        away_since = fm1_ms;
        oa_dbp_key(0);                           /* LESLIE SLOW / FAST, as pad 9 */
        ui_touched();
    } else if (down && d < 150)
        down = 0;
    else if (down && fm1_ms - away_since > 5000u) {
        rest = p;                                /* held 5 s: its rest after all (a pedal plugged in, or the other polarity) */
        down = 0;
    }
}

/* --------------------------------------------------------------- buttons --- */
/* the SMK's own four (KNOB BANK 0, FADER BANK 1, < 2, > 3): taken out of the presses oa_buttons gets. KNOB BANK and
 * FADER BANK act on their release, unless they were held together (the diagnostics page) */
static uint32_t smk_buttons(uint32_t pressed)
{
    static uint32_t prev;
    static uint8_t combo;
    uint32_t b = fm1_in.buttons, up = prev & ~b;
    if ((pressed & 3u) && (b & 3u) == 3u) {
        smk_diag = (uint8_t)!smk_diag;
        combo = 1;
    }
    if (!combo && ((up >> SMK_BTN_KNOB_BANK) & 1u)) {
        char m[24];
        smk_man = (uint8_t)((smk_man + 1u) % 3u);
        smk_ftake = 1;
        oa_flash(smk_cat(smk_cpy(m, "FADERS: "), SMK_MAN_NAME[smk_man]));
    }
    if (!combo && ((up >> SMK_BTN_FADER_BANK) & 1u)) {
        static const char *const BN[3] = {"BANK A: 16' - 4'", "BANK B: 2 2/3' - 1 1/3'", "BANK C: 1' EXPR DRV REV"};
        smk_bank = (uint8_t)((smk_bank + 1u) % 3u);
        smk_ftake = 1;
        oa_flash(BN[smk_bank]);
    }
    if (!(b & 3u))
        combo = 0;
    if ((pressed >> SMK_BTN_LEFT) & 1u)
        oa_preset(-1);
    if ((pressed >> SMK_BTN_RIGHT) & 1u)
        oa_preset(1);
    prev = b;
    return pressed & ~0xFu;
}

static void smk_btn_leds(void)                   /* (organ_app.c oa_leds) KNOB BANK off UPPER, FADER BANK off bank A */
{
    fm1_led_key(SMK_BTN_KNOB_BANK, smk_man != 0u);
    fm1_led_key(SMK_BTN_FADER_BANK, smk_bank != 0u);
    fm1_led_key(SMK_BTN_LEFT, 0);
    fm1_led_key(SMK_BTN_RIGHT, 0);
}

/* ----------------------------------------------------------- diagnostics --- */
static char *smk_num(char *m, uint32_t v)
{
    fmt_fix(m + smk_len(m), (int32_t)v, 0);
    return m;
}
static char *smk_hex(char *m, uint32_t v, uint32_t digits)
{
    static const char H[] = "0123456789ABCDEF";
    char *o = m + smk_len(m);
    while (digits--)
        *o++ = H[(v >> (4u * digits)) & 15u];
    *o = 0;
    return m;
}

static uint32_t smk_sig(void)                    /* (organ_app.c oa_signature) the page redraws every frame while open */
{
    return smk_diag ? fm1_in.frames : 0u;
}

static void smk_draw(void)
{
    char m[64];
    uint32_t i, y = 10;
    const uint16_t BG = RGB565(0x00, 0x00, 0x00);
    if (!smk_diag)
        return;
    cv_rect(0, 0, 240, 240, BG);
    ctext(120, y, &AF_M, "SMK-37 DIAGNOSTICS", C_AMBER, BG);
    y += AF_M.h + 6;
    m[0] = 0;                                    /* the 37 keys, lowest C first: bit k */
    ltext(6, y, &AF_X, smk_hex(smk_cpy(m, "KEYS "), (uint32_t)(fm1_in.keys >> 32), 2), C_CREAM, BG);
    ltext(6 + text_w(&AF_X, m), y, &AF_X, smk_hex(smk_cpy(m, ""), (uint32_t)fm1_in.keys, 8), C_CREAM, BG);
    y += AF_X.h + 3;
    for (i = 0, m[0] = 0; i < FM1_NKEY; i++)     /* the last key pressed and its velocity */
        if ((fm1_in.keys >> i) & 1u) {
            smk_cpy(m, "KEY ");
            smk_num(m, i);
            smk_cat(m, "  VEL ");
            smk_num(m, fm1_in.key_vel[i]);
        }
    ltext(6, y, &AF_X, m[0] ? m : "KEY -", C_CREAM, BG);
    y += AF_X.h + 3;
    for (i = 0; i < SMK_NPAD; i += 8u) {        /* the pads' raw readings (top row, then bottom) */
        uint32_t j;
        smk_cpy(m, i ? "PADS B " : "PADS T ");
        for (j = 0; j < 8u; j++) {
            smk_num(m, fm1_in.pad_raw[i + j] >= 1000u ? 999u : fm1_in.pad_raw[i + j]);
            smk_cat(m, (fm1_in.pad_st[i + j] & 0x80u) ? "* " : " ");
        }
        ltext(6, y, &AF_X, m, C_CREAM, BG);
        y += AF_X.h + 3;
    }
    smk_cpy(m, "FADERS ");
    for (i = 0; i < 4u; i++) {
        smk_num(m, fm1_in.fader[i]);
        smk_cat(m, " ");
    }
    ltext(6, y, &AF_X, m, C_CREAM, BG);
    y += AF_X.h + 3;
    smk_cpy(m, "PITCH ");
    smk_num(m, fm1_in.wheel[0]);
    smk_cat(m, "  MOD ");
    smk_num(m, fm1_in.wheel[1]);
    smk_cat(m, "  PEDAL ");
    smk_num(m, fm1_in.pedal);
    ltext(6, y, &AF_X, m, C_CREAM, BG);
    y += AF_X.h + 3;
    smk_hex(smk_cpy(m, "BUTTONS "), fm1_in.buttons, 5);
    smk_cat(m, "  MASTER ");
    smk_num(m, (uint32_t)smk_master);
    ltext(6, y, &AF_X, m, C_CREAM, BG);
    y += AF_X.h + 3;
    smk_cpy(m, "ADC ");
    smk_num(m, fm1_in.adc_ok);
    smk_cat(m, " / MISS ");
    smk_num(m, fm1_in.adc_miss);
    ltext(6, y, &AF_X, m, C_CREAM, BG);
    y += AF_X.h + 3;
    smk_cpy(m, "BATT ");
    smk_num(m, fm1_in.batt);
    smk_cat(m, "  FLASH ");
    smk_cat(m, flash_ok ? "OK" : "OFF");
    ltext(6, y, &AF_X, m, C_CREAM, BG);
    y += AF_X.h + 8;
    ctext(120, y, &AF_X, "KNOB BANK + FADER BANK: close", C_AMBER, BG);
}

/* ----------------------------------------------------------------- hooks --- */
static void smk_frame(void)
{
    smk_pads_in();
    smk_faders();
    smk_wheels();
    smk_pedal();
    smk_pad_leds();
}

static void smk_poll(void)
{
    smk_pads_in();
    smk_wheels();
    smk_pedal();
}

#endif /* SMK_ORGAN_C */
