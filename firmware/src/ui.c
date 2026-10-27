/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Guilherme Ruppert (FMini-1, organvave-1) */
/* The UI's shared state and helpers (Felucca's ui.c and ui_input.c, cut down to what the instrument's screen,
 * main.c, the editor and the stores use): a full redraw pending, the UPDATE MODE countdown, the message line; the
 * sound loads (a preset, an engine, MIDI Program Change); the LED picture's helpers; the panel calibration.
 * The screen itself is organ_app.c. */
static struct {
    uint8_t force;               /* full redraw pending */
    uint8_t uboot;               /* main.c: seconds left before UPDATE MODE (OCT- + OCT+ held), 0 = none */
    uint8_t home, page;          /* (main.c's breadcrumbs, felucca_dbg) */
    uint8_t hot_col, hot_t;      /* the knob just turned (drawn lit), frames it stays so (ui_fmug.c) */
    char msg[24];                /* a message for the screen, "" none */
} ui;

/* a message (a + b, at most 23 characters) for the screen */
static void ui_say(const char *a, const char *b)
{
    str_cpy(ui.msg, a, sizeof ui.msg);
    str_cpy(ui.msg + str_len(ui.msg), b, sizeof ui.msg - str_len(ui.msg));
    ui.force = 1;
}
static void ui_message(const char *s) { ui_say(s, ""); }

/* -------------------------------------------------------- sound loads --- */
static uint8_t sync_reload;      /* a load happened: the editor's live sync sends RELOAD (editor.c) */

/* parameters a preset load keeps: the mix (LEVEL, PAN, MUTE), the scale and the sequence's settings. The arpeggiator
 * (MODE RATE OCT GATE, ARP 2) is part of the preset (FMini-1): a load sets it (a factory patch: off), SAVE stores it
 * with the tempo (upreset.c) */
static int param_kept(uint32_t i)
{
    if (i >= P_AMODE && i <= P_AORDER)
        return 0;
    return i == P_LEVEL || i == P_PAN || i == P_MUTE || (i >= P_AMODE && i <= P_SGATE) ||
           (i >= P_SLCR && i <= P_SLDEPTH) || i == P_CHRD || i == P_VOIC || i == P_SQNT;
}

static void step_clear(step_t *st)
{
    st->n = 0;
    st->time = ST_REST;
    st->flags = 0;
    st->vel = 0;
}

/* power-on: every parameter at its default, the steps empty */
static void track_defaults(track_t *t)
{
    uint32_t i;
    for (i = 0; i < P_E0; i++)
        t->p[i] = TP[i].def;
    for (i = 0; i < NSTEP; i++)
        step_clear(&t->step[i]);
}

/* the transport runs or is about to (no flash erase then: it stops the audio for ~40 ms) */
static int transport_busy(void)
{
    int busy;
    fm1_irq_off();
    busy = song.playing || transport_req == 1u;
    fm1_irq_on();
    return busy;
}

static void set_engine_of(track_t *t, uint32_t ei);
/* preset pi of the engine the track asked for: the sound only (not the steps, not param_kept) */
static void apply_preset_to(track_t *t, uint32_t pi)
{
    const engine_t *e = ENGINES[t->eng_req % NENGINES];
    uint32_t i;
    panic_req |= (uint8_t)(1u << trk_index(t));       /* MONO/POLY may change: release what sounds */
    t->user = 0;
    if (t == TSEL)
        sync_reload = 1;
    if (!e->npresets)
        return;
    pi %= e->npresets;
    t->preset = (uint8_t)pi;
    for (i = 0; i < P_E0; i++)                        /* the rest of the sound to its defaults: a preset */
        if (!param_kept(i))                           /* sounds the same after any edit */
            t->p[i] = TP[i].def;
    for (i = 0; i < 8u; i++)
        t->p[P_E0 + i] = (int16_t)e->presets[pi].e[i];
    t->p[P_ATK] = e->presets[pi].env[0];
    t->p[P_DEC] = e->presets[pi].env[1];
    t->p[P_SUS] = e->presets[pi].env[2];
    t->p[P_REL] = e->presets[pi].env[3];
    t->p[P_ED_FLT] = e->presets[pi].fenv;
    t->p[P_VOICE] = e->presets[pi].mono ? V_LEGATO : V_POLY;   /* mono presets keep the legato feel */
    {   /* the sends */
        static const uint8_t FX_DEF[4] = {0, 24, 28, 36};
        const preset_t *pr = &e->presets[pi];
        for (i = 0; i < 4u; i++)
            t->p[P_DIST + i] = (int16_t)(pr->fx[i] ? pr->fx[i] - 1 : FX_DEF[i]);
    }
    if (t->eng_req == ENGI_ORGAN) {                   /* the organ: the preset's patch and its level */
        org_set_patch(ORG_FACTORY[pi % ORG_NFACTORY]);
        t->p[P_LEVEL] = ORG_LEVEL[pi % ORG_NFACTORY];
        song.g[G_RTYPE] = (int16_t)ORG_RVB[pi % ORG_NFACTORY][0];      /* 0.9.8: the reverb's type and size are the preset's */
        song.g[G_RSIZE] = (int16_t)ORG_RVB[pi % ORG_NFACTORY][1];
    }
}

/* the engine's defaults and its first preset. With the audio IRQ off: the ISR sees the old engine with its values or
 * the new one with its own (voice.c engine_block), never one with the other's */
static void set_engine_of(track_t *t, uint32_t ei)
{
    const engine_t *e = ENGINES[ei % NENGINES];
    uint32_t i;
    fm1_irq_off();
    t->eng_req = (uint8_t)(ei % NENGINES);
    for (i = 0; i < 8u; i++)
        t->p[P_E0 + i] = e->edit[i].def;
    apply_preset_to(t, 0);
    fm1_irq_on();
}

static void apply_preset(uint32_t pi) { apply_preset_to(TSEL, pi); }

/* MIDI Program Change (1.5.1, #179 by renebohne): program n: the engine's n-th preset, loaded as from PRESETS. A
 * program past the last is ignored, not wrapped: a number is always the same sound */
static void midi_pc_poll(void)                           /* main loop (organ_app.c oa_input) */
{
    uint32_t v = midi_pc, e;
    if (!v)
        return;
    midi_pc = 0;
    v &= 127u;
    e = TSEL->eng_req % NENGINES;
    if (v >= ENGINES[e]->npresets)
        return;
    apply_preset_to(TSEL, v);
    ui.force = 1;
}

/* ----------------------------------------------------------------- LEDs --- */
/* The LED picture is built off-line and copied one byte per column: clearing and relighting would let the 10 kHz scan
 * catch the dark gap and flicker (organ_app.c oa_leds) */
static uint8_t led_pos[41];                        /* (col << 3) | row bit, 0xFF = none */

static void led_pos_init(void)
{
    uint32_t id, p, r;
    for (id = 0; id < 41u; id++) {
        led_pos[id] = 0xFF;
        for (p = 0; p < FM1_NCOL; p++)
            for (r = 1; r < 5u; r++)
                if (FM1_KEYMAP[r][p] == (int8_t)id)
                    led_pos[id] = (uint8_t)((p << 3) | r);
    }
}

static void led_put(uint8_t *nl, uint32_t id, int on)
{
    uint8_t q = led_pos[id];
    if (q != 0xFF && on)
        nl[q >> 3] |= (uint8_t)(1u << (q & 7u));
}

/* the idle glow (hal/fm1_input.h fm1_led_idle_start: a soft light drifting over the keys, the buttons breathing with
 * it) after IDLE_LEDS_MS with no key, button or knob and nothing sounding; any input ends it at once (and is not
 * swallowed). 1 = it has the LEDs */
#define IDLE_LEDS_MS 60000u
static uint32_t ui_touch_ms;                       /* the last key, button or knob (ui_touched) */
static void ui_touched(void) { ui_touch_ms = fm1_ms; }
static int idle_leds(void)
{
    uint32_t i;
    int quiet = !(fm1_in.notes | fm1_in.buttons) && !song.playing && !ui.uboot;
    for (i = 0; i < NVOICE && quiet; i++)
        quiet = !trk[0].v[i].active;
    if (!quiet)
        ui_touch_ms = fm1_ms;
    if ((uint32_t)(fm1_ms - ui_touch_ms) < IDLE_LEDS_MS) {
        if (fm1_led_idle_on())
            fm1_led_anim_stop();
        return 0;
    }
    if (!fm1_led_anim_on())                            /* (not over the power-on sweep) */
        fm1_led_idle_start(settings_leds == LEDS_DIM || settings_leds == LEDS_DIM_LO);
    return fm1_led_idle_on();
}

/* ------------------------------------------------------ panel setup --- */
/* OCT- + OCT+ held at power-on: teach each button and knob (panel.c's table). 30 s without input gives up and keeps
 * the old table (a stuck key cannot hang the boot); a piano key cancels at any time: the keys are not part of what is
 * taught, so one works whatever the buttons' table says */
#define SETUP_IDLE_MS 30000u
#define SETUP_GIVE_UP(t0) (fm1_ms - (t0) > SETUP_IDLE_MS || fm1_in.notes != 0u)
static void setup_show(const char *what, const char *name)     /* "PRESS" / "TURN RIGHT", the control */
{
    lcd_fill(0, 76, 240, 60, T_BG);
    draw_text_box(0, 80, 240, &AF_S, what, T_MID, 1);
    draw_text_box(0, 100, 240, &AF_L, name, T_THEME, 1);
}
static void panel_setup(void)
{
    uint32_t i, used = 0, t0 = fm1_ms;
    const panel_t old = panel;
    lcd_fill(0, 0, 240, 240, T_BG);
    draw_text_box(0, 8, 240, &AF_M, "HARDWARE CALIBRATION", T_TEXT, 1);
    draw_text_box(0, 32, 240, &AF_S, "TEACH EACH BUTTON AND KNOB", T_MID, 1);
    lcd_fill(16, 56, 208, 1, T_LINE);
    draw_text_box(0, 206, 240, &AF_S, "ANY KEY: CANCEL", T_DIM, 1);
    while (fm1_in.buttons) {                             /* wait for OCT-/OCT+ release */
        fm1_wdt_feed();
        if (SETUP_GIVE_UP(t0))
            goto timeout;
    }
    fm1_input_edges(0);
    for (i = 0; i < NB; i++) {
        uint32_t p = 0, id;
        setup_show("PRESS", B_NAME[i]);
        t0 = fm1_ms;
        while (!(p & ~used)) {
            fm1_wdt_feed();
            p |= fm1_input_edges(0);
            if (SETUP_GIVE_UP(t0))
                goto timeout;
        }
        for (id = 0; id < 14u; id++)
            if (((p & ~used) >> id) & 1u)
                break;
        panel.btn[i] = (uint8_t)id;
        used |= 1u << id;
    }
    used = 0;
    for (i = 0; i < NE; i++) {
        uint32_t e;
        int32_t st = 0;
        setup_show("TURN RIGHT", E_NAME[i]);
        for (e = 0; e < 7u; e++)
            fm1_enc_take(e);
        t0 = fm1_ms;
        for (;;) {
            fm1_wdt_feed();
            if (SETUP_GIVE_UP(t0))
                goto timeout;
            for (e = 0; e < 7u; e++)
                if (!((used >> e) & 1u) && (st = fm1_enc_take(e)) != 0)
                    break;
            if (e < 7u)
                break;
        }
        panel.enc[i] = (uint8_t)e;
        panel.dir[i] = (int8_t)(st > 0 ? 1 : -1);
        used |= 1u << e;
        fm1_delay_ms(300);
        fm1_enc_take(e);
    }
    panel.magic = PANEL_MAGIC;
    lcd_fill(0, 0, 240, 240, T_BG);
    ui.force = 1;
    return;
timeout:
    panel = old;
    lcd_fill(0, 0, 240, 240, T_BG);
    ui.force = 1;
    ui_message("SETUP CANCELLED");
}
