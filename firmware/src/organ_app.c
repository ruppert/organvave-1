/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* OrganVave-1: the user interface. main.c calls oa_input / oa_leds / oa_draw from its loop and oa_splash at power-on.
 * The screens are drawn after the real console's panel (owner, 2026-10-21; the approved mock-ups: tools/ui_mockups.c,
 * docs/mockups/): the wood lid with an engraved brass plate for the page's name, a chrome trim, the black control
 * rail, the drawbars as on the console (a dark stem with light rails and the numbers 8 .. 1, a block cap with a lit
 * top face and a finger notch, brown / white / black by footage) and the console's pilot lamps.
 *
 * Pages (0.1.0): HOME (the preset's number and name, the upper drawbars small, the lamps) and DRAWBARS (the nine upper
 * drawbars; KNOB 1..4 pull the four marked ones, SELECT moves the four along; the registration below, as the console
 * writes it: 88 8000 000). A knob turned on HOME opens DRAWBARS and moves its bar. Buttons: HOME, EDIT (DRAWBARS), SAVE
 * (the dialog: PRESETS the slot, SELECT / ALGORITHM the name, SAVE again writes, HOME or EDIT cancels), GLO (MIDI IN,
 * MIDI CHANNEL, ABOUT), OCT- / OCT+ (the keys' octave, -2 .. +2). PRESETS: the factory presets, then the user ones.
 * PERC & VIB (FX, 0.2.0): KNOBS 1..4 switch the percussion (on, soft, fast, the 2 2/3' third), ALGORITHM turns the
 * vibrato's selector (off, VIB 1..3, CHO 1..3). ENV switches the percussion and LFO turns the vibrato on HOME too (the
 * panel's labels are the best guess until the calibration: HARDWARE CALIBRATION). The rotary and drive wait for 0.3.0.
 * Included by felucca.c before main.c. */

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
/* the palette: the console's black control rail, its wood, chrome, the drawbars' three colours, the brand's orange */
#define C_PANEL  RGB565(0x14, 0x11, 0x10)
#define C_PANEL2 RGB565(0x22, 0x1d, 0x1a)
#define C_SLOT   RGB565(0x08, 0x07, 0x06)
#define C_CREAM  RGB565(0xef, 0xe6, 0xd2)
#define C_DIM    RGB565(0x8c, 0x82, 0x72)
#define C_ORANGE RGB565(0xff, 0x6a, 0x13)
#define C_AMBER  RGB565(0xff, 0xb3, 0x4a)
#define C_RED    RGB565(0xd6, 0x38, 0x2b)
#define C_CHR_HI RGB565(0xe6, 0xe2, 0xda)
#define C_CHR    RGB565(0xa8, 0xa3, 0x9a)
#define C_CHR_LO RGB565(0x5e, 0x5a, 0x54)
#define C_POP    RGB565(0x1b, 0x19, 0x17)
#define C_HOT    RGB565(0x2e, 0x28, 0x22)

static uint8_t oa_page;                                  /* 0 HOME, 1 DRAWBARS, 2 PERC & VIB, 3 ROTARY, 4 DRIVE, 5 REVERB, 6 DELAY, 7 EQ (FX walks these), 8 ADVANCED EDIT (EDIT walks DRAWBARS and it) */
static uint8_t oa_man;                                   /* the manual the DRAWBARS page edits: 0 UPPER, 1 LOWER with the PEDAL's two bars (MIDI channels 2 and 3) */
static uint8_t oa_sel;                                   /* the bar SELECT has picked on DRAWBARS: UPPER 0..8; LOWER / PEDAL 0..10 (0, 1 the pedal's 16' and 8', then the lower's nine) */
static uint8_t oa_row[9];                                /* each FX page: the row SELECT has picked */
static uint8_t oa_top[9];                                /* ... and the first row on the screen */
static uint32_t oa_sig_prev = 0xFFFFFFFFu;

/* ------------------------------------------------------------ pixels --- */
static uint16_t px_get(int32_t x, int32_t y)
{
    y += cv_oy;
    if ((uint32_t)x >= cv_w || (uint32_t)y >= cv_h)
        return 0;
    return swap16(cv_px[(uint32_t)y * cv_w + (uint32_t)x]);
}
static uint16_t mix(uint16_t a, uint16_t b, int32_t t)      /* a -> b, t 0..256 */
{
    int32_t r = (a >> 11) + ((((b >> 11) - (a >> 11)) * t) >> 8);
    int32_t g = ((a >> 5) & 63) + (((((b >> 5) & 63) - ((a >> 5) & 63)) * t) >> 8);
    int32_t bl = (a & 31) + ((((b & 31) - (a & 31)) * t) >> 8);
    return (uint16_t)(r << 11 | g << 5 | bl);
}
static void px_blend(int32_t x, int32_t y, uint16_t c, int32_t t)
{
    if (t <= 0)
        return;
    cv_pset(x, y, t >= 256 ? c : mix(px_get(x, y), c, t));
}
/* a row of the canvas strip that will be drawn (the screen is drawn in two strips of 120 rows: skip the other one) */
static int oa_row_in(int32_t y) { return (uint32_t)(y + cv_oy) < cv_h; }

/* a rounded rectangle, anti-aliased (4 x 4 samples a pixel at the corners); r in pixels */
static int rr_in(int32_t sx, int32_t sy, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r)   /* samples: 1/4 px */
{
    int32_t x0 = 4 * x, y0 = 4 * y, x1 = 4 * (x + w), y1 = 4 * (y + h), R = 4 * r, cx, cy;
    if (sx < x0 || sx >= x1 || sy < y0 || sy >= y1)
        return 0;
    cx = sx < x0 + R ? x0 + R : sx >= x1 - R ? x1 - R - 1 : sx;
    cy = sy < y0 + R ? y0 + R : sy >= y1 - R ? y1 - R - 1 : sy;
    return (sx - cx) * (sx - cx) + (sy - cy) * (sy - cy) <= R * R;
}
static void rr_px(int32_t i, int32_t j, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    int32_t n = 0, a, b;
    if ((i - x >= r && x + w - 1 - i >= r) || (j - y >= r && y + h - 1 - j >= r)) {
        cv_pset(i, j, c);
        return;
    }
    for (b = 0; b < 4; b++)
        for (a = 0; a < 4; a++)
            n += rr_in(4 * i + a, 4 * j + b, x, y, w, h, r);
    px_blend(i, j, c, n * 16);
}
static void rrect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    int32_t i, j;
    for (j = y; j < y + h; j++)
        if (oa_row_in(j))
            for (i = x; i < x + w; i++)
                rr_px(i, j, x, y, w, h, r, c);
}
/* a vertical gradient in a rounded rectangle: top colour, bottom colour */
static void rrect_v(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t top, uint16_t bot)
{
    int32_t i, j;
    for (j = 0; j < h; j++)
        if (oa_row_in(y + j)) {
            uint16_t c = mix(top, bot, h > 1 ? j * 256 / (h - 1) : 0);
            for (i = x; i < x + w; i++)
                rr_px(i, y + j, x, y, w, h, r, c);
        }
}
/* a disc, anti-aliased; centre and radius in 1/4 px */
static void disc4(int32_t cx4, int32_t cy4, int32_t r4, uint16_t c)
{
    int32_t x0 = (cx4 - r4) / 4 - 1, x1 = (cx4 + r4) / 4 + 1, y0 = (cy4 - r4) / 4 - 1, y1 = (cy4 + r4) / 4 + 1, i, j, a, b;
    for (j = y0; j <= y1; j++)
        if (oa_row_in(j))
            for (i = x0; i <= x1; i++) {
                int32_t n = 0;
                for (b = 0; b < 4; b++)
                    for (a = 0; a < 4; a++) {
                        int32_t dx = 4 * i + a - cx4, dy = 4 * j + b - cy4;
                        n += dx * dx + dy * dy <= r4 * r4;
                    }
                px_blend(i, j, c, n * 16);
            }
}
static void disc(int32_t cx, int32_t cy, int32_t r, uint16_t c) { disc4(4 * cx + 2, 4 * cy + 2, 4 * r, c); }

/* -------------------------------------------------------------- text --- */
static int32_t cap_y(const aafont_t *f)
{
    return f == &AF_L ? AF_L_CAP_Y : f == &AF_N ? AF_N_CAP_Y : f == &AF_X ? AF_X_CAP_Y : f == &AF_S ? AF_S_CAP_Y : AF_M_CAP_Y;
}
static void ctext(int32_t cx, int32_t ytop, const aafont_t *f, const char *s, uint16_t fg, uint16_t bg)
{
    cv_text_on(cx - text_w(f, s) / 2, ytop - cap_y(f), f, s, fg, bg);
}
static void ltext(int32_t x, int32_t ytop, const aafont_t *f, const char *s, uint16_t fg, uint16_t bg)
{
    cv_text_on(x, ytop - cap_y(f), f, s, fg, bg);
}
static void rtext(int32_t xr, int32_t ytop, const aafont_t *f, const char *s, uint16_t fg, uint16_t bg)
{
    cv_text_on(xr - text_w(f, s), ytop - cap_y(f), f, s, fg, bg);
}

/* ----------------------------------------------------- panel elements --- */
/* the wood of the console's cheek blocks and lid: a warm gradient with a wandering grain */
static void wood(int32_t x, int32_t y, int32_t w, int32_t h)
{
    static const uint16_t W[3] = {RGB565(0x5a, 0x2b, 0x12), RGB565(0x86, 0x45, 0x1f), RGB565(0x6a, 0x33, 0x16)};
    int32_t i, j;
    for (j = y; j < y + h; j++)
        if (oa_row_in(j))
            for (i = x; i < x + w; i++) {
                int32_t t = (j - y) * 512 / (h > 1 ? h - 1 : 1);
                uint16_t c = t < 256 ? mix(W[0], W[1], t) : mix(W[1], W[2], t - 256);
                int32_t g = (i * 3 + (j - y) * 17 + ((i * i) >> 7) + ((i >> 3) * (j - y) & 7)) % 23;
                if (g < 2)
                    c = mix(c, RGB565(0x3a, 0x1a, 0x0a), 110);
                else if (g == 9)
                    c = mix(c, RGB565(0xb0, 0x62, 0x30), 70);
                cv_pset(i, j, c);
            }
}
/* a chrome trim strip: light on top, dark below */
static void chrome(int32_t x, int32_t y, int32_t w, int32_t h)
{
    int32_t j;
    for (j = 0; j < h; j++)
        cv_rect(x, y + j, w, 1, j == 0 ? C_CHR_HI : j == h - 1 ? C_CHR_LO : mix(C_CHR_HI, C_CHR, j * 256 / h));
}
/* the header every screen shares: the wood lid with the page's name engraved left and the patch right, a chrome trim */
static void header(const char *page, const char *right)
{
    wood(0, 0, 240, 22);
    chrome(0, 22, 240, 3);
    {   /* an engraved brass plate for the page name */
        int32_t w = text_w(&AF_S, page) + 12;
        rrect_v(4, 3, w, 16, 2, RGB565(0xd8, 0xb8, 0x6a), RGB565(0x9c, 0x7c, 0x38));
        ctext(4 + w / 2, 7, &AF_S, page, RGB565(0x2a, 0x1a, 0x08), RGB565(0xbc, 0x9c, 0x52));
    }
    rtext(234, 8, &AF_S, right, C_CREAM, RGB565(0x78, 0x3d, 0x1c));
}
/* a hint line at the foot: what the knobs and the encoders do here */
static void footer(const char *s)
{
    cv_rect(0, 226, 240, 1, C_PANEL2);
    if (cpu_high)                                       /* overload (audio.c): the warning replaces the hint, on every page */
        ctext(120, 230, &AF_X, "CPU HIGH", RGB565(0xff, 0x50, 0x38), C_PANEL);
    else
        ctext(120, 230, &AF_X, s, C_DIM, C_PANEL);
}

/* the drawbars' colours by footage, as on the console: brown 16' 5 1/3', white the octaves, black the mutations */
static const char *const FOOT[9] = {"16'", "5", "8'", "4'", "2", "2'", "1", "1", "1'"};
static const char *const FRAC[9] = {"", "1/3", "", "", "2/3", "", "3/5", "1/3", ""};   /* (the mutations' fractions, a line below) */
static int32_t bar_kind(int32_t k) { return k < 2 ? 0 : (k == 4 || k == 6 || k == 7) ? 2 : 1; }   /* 0 brown 1 white 2 black */

/* one drawbar hanging under the wood at y0, as on the console: a dark stem with two light rails and the numbers v .. 1 on
 * it (v = how far it is pulled, 0..8, `step` px each), then the cap: a block with a lit top face and a darker front with
 * its finger notch, its colour by footage (brown, white, black); hot: the knobs move it (an orange mark under it) */
static void drawbar(int32_t cx, int32_t y0, int32_t v, int32_t step, int32_t w, int32_t k, int hot, int compact)
{
    static const uint16_t TOP[3] = {RGB565(0x7a, 0x52, 0x46), RGB565(0xf4, 0xf1, 0xea), RGB565(0x5a, 0x5a, 0x5a)},
                          FRONT[3] = {RGB565(0x4c, 0x2c, 0x24), RGB565(0xc4, 0xc0, 0xb6), RGB565(0x2c, 0x2c, 0x2c)},
                          NOTCH[3] = {RGB565(0x30, 0x1a, 0x14), RGB565(0x9a, 0x96, 0x8e), RGB565(0x18, 0x18, 0x18)};
    int32_t kind = bar_kind(k), sw = w * 2 / 3, x = cx - sw / 2, len = v * step, caph = compact ? 16 : 28, ct = caph * 11 / 20, i;
    char n[2] = {0, 0};
    if (len) {                                                /* the stem */
        cv_rect(x, y0, sw, len, RGB565(0x24, 0x22, 0x20));
        cv_rect(x + 1, y0, 1, len, RGB565(0xd8, 0xd4, 0xcc));
        cv_rect(x + sw - 2, y0, 1, len, RGB565(0xd8, 0xd4, 0xcc));
        cv_rect(x, y0, 1, len, RGB565(0x10, 0x0f, 0x0e));
        cv_rect(x + sw - 1, y0, 1, len, RGB565(0x10, 0x0f, 0x0e));
        for (i = 0; i < v && !compact; i++) {                 /* v at the top .. 1 above the cap */
            n[0] = (char)('0' + v - i);
            ctext(cx, y0 + i * step + (step - 7) / 2, &AF_X, n, RGB565(0xe8, 0xe4, 0xdc), RGB565(0x24, 0x22, 0x20));
        }
    }
    {   /* the cap: a shadow, the lit top face, the front, the notch */
        int32_t xc = cx - w / 2, yc = y0 + len;
        rrect(xc + 1, yc + 1, w, caph, 2, RGB565(0x06, 0x05, 0x05));
        rrect_v(xc, yc, w, ct, 2, mix(TOP[kind], RGB565(0xff, 0xff, 0xff), 40), TOP[kind]);
        rrect_v(xc, yc + ct - 2, w, caph - ct + 2, 2, FRONT[kind], mix(FRONT[kind], RGB565(0, 0, 0), 60));
        cv_rect(xc + 1, yc + ct - 2, w - 2, 1, mix(FRONT[kind], RGB565(0xff, 0xff, 0xff), 50));
        rrect(cx - w / 4, yc + ct + 1, w / 2, caph - ct - 4, 1, NOTCH[kind]);
        if (hot)
            rrect(xc - 1, yc + caph + 3, w + 2, 2, 1, C_ORANGE);
    }
}
/* the shadow the wood rail casts on the black panel, where the bars come out */
static void bar_slot(int32_t x, int32_t y, int32_t w)
{
    int32_t j;
    for (j = 0; j < 5; j++)
        cv_rect(x, y + j, w, 1, mix(RGB565(0x02, 0x02, 0x02), C_PANEL, j * 50));
}
/* a small lamp (the console's pilot light) and its label */
static void lamp(int32_t x, int32_t y, const char *label, int on)
{
    disc(x, y, 4, C_SLOT);
    disc(x, y, 3, on ? C_AMBER : RGB565(0x3a, 0x26, 0x14));
    if (on)
        disc4(4 * x + 1, 4 * y, 4, RGB565(0xff, 0xe8, 0xb8));
    ltext(x + 8, y - 3, &AF_X, label, on ? C_CREAM : C_DIM, C_PANEL);
}

/* the registration as the console writes it: "88 8000 000" (16' 5 1/3' 8', 4' 2 2/3' 2' 1 3/5', 1 1/3' 1') */
static void oa_reg(char *b, const uint8_t *v)
{
    uint32_t i, o = 0;
    for (i = 0; i < 9u; i++) {
        if (i == 3u || i == 7u)
            b[o++] = ' ';
        b[o++] = (char)('0' + (v[i] > 8u ? 8u : v[i]));
    }
    b[o] = 0;
}

/* ------------------------------------------------------------- presets --- */
/* the list PRESETS walks: the factory presets, then the user presets of the organ (SAVE) in slot order */
static int oa_user_ok(uint32_t k) { return up_used(k) && up_rec(k)->engine == ENGI_ORGAN; }
static uint32_t oa_nuser(void)
{
    uint32_t k, n = 0;
    for (k = 0; k < UP_SLOTS; k++)
        n += (uint32_t)oa_user_ok(k);
    return n;
}
static uint32_t oa_user_nth(uint32_t n)                    /* the slot of the n-th user preset */
{
    uint32_t k;
    for (k = 0; k < UP_SLOTS; k++)
        if (oa_user_ok(k) && !n--)
            return k;
    return 0;
}
static uint32_t oa_user_rank(uint32_t slot)
{
    uint32_t k, n = 0;
    for (k = 0; k < slot && k < UP_SLOTS; k++)
        n += (uint32_t)oa_user_ok(k);
    return n;
}
static int oa_on_user(void) { return TSEL->user && oa_user_ok((uint32_t)TSEL->user - 1u); }
static uint32_t oa_factory(void) { return TSEL->preset < ORG_NFACTORY ? TSEL->preset : 0u; }

/* the preset playing: its number ("07", a user one "U05") and its name */
static void oa_preset_label(char *num, char *name)
{
    if (oa_on_user()) {
        up_slot_label(num, (uint32_t)TSEL->user - 1u);
        up_name((uint32_t)TSEL->user - 1u, name);
    } else {
        uint32_t k = oa_factory() + 1u;
        num[0] = (char)('0' + k / 10u);
        num[1] = (char)('0' + k % 10u);
        num[2] = 0;
        str_cpy(name, ORG_PRESETS[k - 1u].name, 13);
    }
}

/* PRESETS: the previous / next preset of the list, round */
static void oa_preset(int32_t s)
{
    uint32_t nu = oa_nuser(), n = ORG_NFACTORY + nu, cur, nx;
    cur = oa_on_user() ? ORG_NFACTORY + oa_user_rank((uint32_t)TSEL->user - 1u) : oa_factory();
    nx = (cur + (s > 0 ? 1u : n - 1u)) % n;
    if (nx < ORG_NFACTORY)
        apply_preset_to(TSEL, nx);
    else
        up_load(oa_user_nth(nx - ORG_NFACTORY));
}

/* the pop-up of a preset change: number and name large, 1.5 s */
static uint8_t oa_last_preset = 0xFF, oa_last_user = 0xFF, oa_primed;
static uint32_t oa_pp_until;
static int oa_pp_on(void) { return oa_pp_until && (int32_t)(fm1_ms - oa_pp_until) < 0; }
static uint32_t oa_track_preset(void)                     /* every frame; 1 = the preset changed now */
{
    uint32_t u = oa_on_user() ? TSEL->user : 0u, p = u ? 0xFEu : oa_factory();
    if (u == oa_last_user && p == oa_last_preset)
        return 0;
    oa_last_user = (uint8_t)u;
    oa_last_preset = (uint8_t)p;
    if (!oa_primed)
        return 0;
    oa_pp_until = fm1_ms + 1500u;
    return 1;
}

/* SAVE: a small dialog. PRESETS picks the slot U01..U32, SELECT moves along the name and ALGORITHM changes the letter,
 * SAVE writes it, HOME or EDIT cancels */
static uint8_t oa_sv;                                     /* 1: the dialog is open */
static uint8_t oa_sv_slot, oa_sv_pos;
static char oa_sv_name[13];
static uint32_t oa_msg_until;                             /* "SAVED U05" shown until fm1_ms passes this */
static char oa_msg[20];
static const char OA_CH[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
static int oa_msg_on(void) { return oa_msg_until && (int32_t)(fm1_ms - oa_msg_until) < 0; }

static void oa_sv_fill(void)                              /* the slot's own name (overwrite) or the playing preset's */
{
    char num[4];
    if (oa_user_ok(oa_sv_slot))
        up_name(oa_sv_slot, oa_sv_name);
    else
        oa_preset_label(num, oa_sv_name);
}
static void oa_sv_open(void)
{
    uint32_t k;
    oa_sv = 1;
    oa_sv_pos = 0;
    if (oa_on_user())
        oa_sv_slot = (uint8_t)(TSEL->user - 1u);
    else {                                                /* the first free slot */
        for (k = 0; k < UP_SLOTS && up_used(k); k++)
            ;
        oa_sv_slot = (uint8_t)(k < UP_SLOTS ? k : 0u);
    }
    oa_sv_fill();
}
static void oa_say(const char *a, int32_t k)
{
    str_cpy(oa_msg, a, 12);
    if (k >= 0) {
        char l[4];
        up_slot_label(l, (uint32_t)k);
        str_cpy(oa_msg + str_len(oa_msg), l, 4);
    }
    oa_msg_until = fm1_ms + 1500u;
}
static void oa_sv_do(void)
{
    uint32_t k = oa_sv_slot, n = str_len(oa_sv_name);
    int rc;
    while (n && oa_sv_name[n - 1u] == ' ')                /* no trailing blanks */
        oa_sv_name[--n] = 0;
    rc = up_store(k, oa_sv_name);
    oa_sv = 0;
    if (rc == 0 || rc == 3) {
        TSEL->user = (uint8_t)(k + 1u);
        oa_last_user = (uint8_t)(k + 1u);                 /* (no pop-up: the message says it) */
        oa_last_preset = 0xFE;
        oa_say(rc == 3 ? "SAVED(RAM) " : "SAVED ", (int32_t)k);
    } else
        oa_say("SAVE ERROR", -1);
    ui.force = 1;
}
static void oa_sv_letter(int32_t s)
{
    char *c = &oa_sv_name[oa_sv_pos];
    uint32_t i, n = (uint32_t)sizeof OA_CH - 1u, at = 0;
    for (i = 0; i < oa_sv_pos; i++)                       /* a short name: pad with blanks up to the cursor */
        if (!oa_sv_name[i])
            oa_sv_name[i] = ' ';
    if (!*c)
        *c = ' ';
    for (i = 0; i < n; i++)
        if (OA_CH[i] == *c)
            at = i;
    *c = OA_CH[(at + n + (s > 0 ? 1u : n - 1u)) % n];
    if (!oa_sv_name[oa_sv_pos + 1u] && oa_sv_pos < 11u)
        oa_sv_name[oa_sv_pos + 1u] = 0;
}

/* -------------------------------------------------------------- GLO --- */
/* GLO opens the global settings: MIDI IN (USB / MIDI / ALL), MIDI CHANNEL (ALL / 1..16), ABOUT (the splash with the
 * version). SELECT picks a line, ALGORITHM changes it (or opens ABOUT); GLO goes back, then closes. Kept in the
 * settings record (settings.c set_bytes, row 7: FMini-1's layout for the two MIDI bytes, so they carry over) */
static uint8_t oa_gl;                                     /* 0 closed, 1 the list, 3 ABOUT */
static uint8_t oa_gl_row;                                 /* 0 MIDI IN, 1 MIDI CHANNEL, 2 ABOUT */
static uint8_t oa_cfg_dirty;
static const char *const OA_SRC[3] = {"USB", "MIDI", "ALL"};       /* fm_midi_src 0, 1, 3 */
static uint8_t *oa_cfg_byte(uint32_t i) { return &set_bytes[7u][i % 32u]; }
static void oa_cfg_load(void)
{
    if (*oa_cfg_byte(0) == 0xA7u && *oa_cfg_byte(1) == 0x4Fu) {
        uint32_t src = *oa_cfg_byte(2), ch = *oa_cfg_byte(3);
        fm_midi_src = (uint8_t)(src == 0u || src == 1u ? src : 3u);
        fm_midi_ch = (uint8_t)(ch > 16u ? 0u : ch);
    } else {
        fm_midi_src = 3;
        fm_midi_ch = 0;
    }
    fm_bend_mode = 0;
    fm_bt = 0;
}
static void oa_cfg_save(void)
{
    if (!oa_cfg_dirty)
        return;
    oa_cfg_dirty = 0;
    *oa_cfg_byte(0) = 0xA7;
    *oa_cfg_byte(1) = 0x4F;
    *oa_cfg_byte(2) = fm_midi_src;
    *oa_cfg_byte(3) = fm_midi_ch;
    settings_save();                                      /* (deferred while anything sounds) */
}
static void oa_gl_close(void)
{
    oa_gl = 0;
    oa_cfg_save();
}

/* ------------------------------------------------------------ scenes --- */
/* a pop-up's frame: the orange border, the title */
static void oa_frame(const char *title)
{
    cv_rect(10, 24, 220, 186, C_ORANGE);
    cv_rect(12, 26, 216, 182, C_POP);
    ctext(120, 36, &AF_M, title, C_CREAM, C_POP);
}
/* a line of a pop-up: the label on the left, the value on the right, the highlight under the chosen one */
static void oa_line(int32_t y, const char *lb, const char *val, int hot)
{
    uint16_t bg = hot ? C_HOT : C_POP;
    if (hot)
        cv_rect(16, y - 5, 208, 21, C_HOT);
    ltext(22, y, &AF_M, lb, hot ? C_AMBER : C_CREAM, bg);
    rtext(218, y, &AF_M, val, hot ? C_AMBER : C_CREAM, bg);
}

static void oa_gl_dialog(void)
{
    char b[8];
    oa_frame("GLOBAL");
    oa_line(70, "MIDI IN", OA_SRC[fm_midi_src == 3u ? 2u : fm_midi_src & 1u], oa_gl_row == 0u);
    if (fm_midi_ch)
        fmt_int(b, fm_midi_ch);
    else
        str_cpy(b, "ALL", 4);
    oa_line(96, "MIDI CHANNEL", b, oa_gl_row == 1u);
    oa_line(122, "ABOUT", ">", oa_gl_row == 2u);
    if (fm_midi_ch) {                                     /* the manuals follow the channel: N upper, N + 1 lower, N + 2 pedals */
        char c[24];
        uint32_t n1 = (uint32_t)fm_midi_ch, n2 = n1 % 16u + 1u, n3 = n2 % 16u + 1u;
        fmt_int(c, n1);
        str_cpy(c + str_len(c), " UPPER  ", 9);
        fmt_int(c + str_len(c), n2);
        str_cpy(c + str_len(c), " LOWER  ", 9);
        fmt_int(c + str_len(c), n3);
        str_cpy(c + str_len(c), " PEDAL", 7);
        ctext(120, 160, &AF_X, c, C_DIM, C_POP);
    } else
        ctext(120, 160, &AF_X, "1 UPPER  2 LOWER  3 PEDAL", C_DIM, C_POP);
    ctext(120, 190, &AF_S, "SELECT  ALGO  GLO = BACK", C_DIM, C_POP);
}

static void oa_sv_dialog(void)
{
    uint32_t k = oa_sv_slot, i;
    char l[4];
    int used = up_used(k);
    cv_rect(16, 62, 208, 112, C_ORANGE);
    cv_rect(18, 64, 204, 108, C_POP);
    up_slot_label(l, k);
    ctext(120, 72, &AF_S, "SAVE PRESET", C_CREAM, C_POP);
    ltext(28, 92, &AF_M, l, C_AMBER, C_POP);
    ltext(28 + text_w(&AF_M, l) + 8, 95, &AF_X, used ? "OVERWRITE" : "EMPTY", used ? C_RED : C_DIM, C_POP);
    for (i = 0; i < 12u; i++) {                           /* the name, letter by letter, the cursor underlined */
        char c[2] = {oa_sv_name[i] ? oa_sv_name[i] : ' ', 0};
        int32_t x = 28 + (int32_t)i * 14;
        cv_rect(x, 124, 13, 1, i == oa_sv_pos ? C_ORANGE : C_PANEL2);
        if (i == oa_sv_pos)
            cv_rect(x, 125, 13, 1, C_ORANGE);
        if (c[0] != ' ')
            ctext(x + 6, 108, &AF_M, c, i == oa_sv_pos ? C_AMBER : C_CREAM, C_POP);
    }
    ctext(120, 140, &AF_X, "PRESETS SLOT   SELECT/ALGO NAME", C_DIM, C_POP);
    ctext(120, 154, &AF_X, "SAVE = WRITE   HOME = CANCEL", C_DIM, C_POP);
}

static void oa_pp_draw(void)                              /* the preset pop-up */
{
    char num[8], nm[16];
    const aafont_t *f = &AF_L;
    oa_preset_label(num, nm);
    if (text_w(&AF_L, nm) > 204)
        f = &AF_M;
    cv_rect(12, 70, 216, 100, C_ORANGE);
    cv_rect(14, 72, 212, 96, C_POP);
    ctext(120, 84, &AF_L, num, C_AMBER, C_POP);
    ctext(120, 126, f, nm, C_CREAM, C_POP);
}

static const char *const VIB_SHORT[7] = {"VIBRATO", "VIB 1", "VIB 2", "VIB 3", "CHO 1", "CHO 2", "CHO 3"};
static const char *const VIB_FULL[7] = {"VIBRATO OFF   (ALGORITHM)", "VIB 1   (ALGORITHM)", "VIB 2   (ALGORITHM)",
                                        "VIB 3   (ALGORITHM)", "CHO 1   (ALGORITHM)", "CHO 2   (ALGORITHM)",
                                        "CHO 3   (ALGORITHM)"};

static void scr_perc(void)                              /* PERC & VIB: the percussion's switches, the vibrato's selector */
{
    uint32_t v = org_patch[OP_VIB] <= 6u ? org_patch[OP_VIB] : 0u;
    header("PERC & VIB", "");
    lamp(22, 58, org_patch[OP_PERC] ? "PERCUSSION ON   (KNOB 1)" : "PERCUSSION OFF   (KNOB 1)", org_patch[OP_PERC] != 0);
    lamp(22, 80, org_patch[OP_PERC_SOFT] ? "SOFT   (KNOB 2)" : "NORMAL   (KNOB 2)", org_patch[OP_PERC_SOFT] != 0);
    lamp(22, 102, org_patch[OP_PERC_FAST] ? "FAST, 1 S   (KNOB 3)" : "SLOW, 4 S   (KNOB 3)", org_patch[OP_PERC_FAST] != 0);
    lamp(22, 124, org_patch[OP_PERC_SEL] ? "2 2/3' THIRD   (KNOB 4)" : "4' SECOND   (KNOB 4)", org_patch[OP_PERC_SEL] != 0);
    lamp(22, 152, VIB_FULL[v], v != 0);
    footer("KNOBS 1-4: PERC   ALGORITHM: VIBRATO");
}

/* a row of n small cells, the first v lit (a value on a scale) */
static void meter(int32_t x, int32_t y, int32_t n, int32_t v)
{
    int32_t i;
    for (i = 0; i < n; i++)
        rrect(x + i * 8, y, 6, 9, 1, i < v ? C_AMBER : RGB565(0x3a, 0x26, 0x14));
}
static const char *const ROT_SPD[3] = {"STOP", "SLOW", "FAST"};
static int32_t oa_step(int32_t v, int32_t s, int32_t mx)   /* one detent: 8 on the 0..mx scale */
{
    return clamp(v + (s > 0 ? 8 : -8), 0, mx);
}
/* ------------------------------------------------------------- the FX pages --- */
/* Every FX page is a list of rows. SELECT picks a row (it is drawn lit), ALGORITHM changes the picked row's value (clockwise: more),
 * and the knobs 1 to 4 go straight to the first four rows. A list longer than the screen scrolls with the pick. The rows of a page
 * are what oa_val shows and oa_adj1 changes (one detent). */
#define OA_VIS 8u                                       /* rows on the screen */
static const uint8_t OA_NR[9] = {0, 0, 5, 12, 5, 4, 6, 5, 27};
static const char *const OA_LB[9][27] = {
    {0},
    {0},
    {"PERCUSSION", "VOLUME", "DECAY", "HARMONIC", "VIBRATO"},
    {"ROTARY", "MODEL", "HORN", "SPEED", "WIDTH", "HORN LEVEL", "HORN SLOW", "HORN FAST", "DRUM SLOW", "DRUM FAST", "HORN RAMP", "DRUM RAMP"},
    {"DRIVE", "TYPE", "GAIN", "TONE", "OUTPUT"},
    {"SEND", "TYPE", "SIZE", "TONE"},
    {"SEND", "TYPE", "TIME", "FEEDBACK", "COLOUR", "MIX"},
    {"EQ", "LOW", "MID", "HIGH", "MID FREQ"},
    {"TUNING", "TEMPERAMENT", "ATTACK CLICK", "RELEASE CLICK", "ATTACK MODEL", "CLICK LEN MIN", "CLICK LEN MAX", "RELEASE MODEL",
     "CROSSTALK", "COMPARTMENT", "TRANSFORMER", "TERMINAL STRIP", "WIRING", "XT FLOOR", "XT MIN",
     "PERC FAST", "PERC SLOW", "PERC NORMAL", "PERC SOFT", "PERC GAIN", "PERC BUS A", "PERC BUS B", "PERC TRIGGER",
     "SCAN RATE", "VIB1 DEPTH", "VIB2 DEPTH", "VIB3 DEPTH"},
};
/* ADVANCED EDIT: row -> the patch byte (eng_organ.c OP_*). The knobs 1-4 go to the first four rows */
static const uint8_t OA_AOP[27] = {
    OP_TUNING, OP_TEMP, OP_ATK_LEVEL, OP_REL_LEVEL, OP_ATK_MODEL, OP_ATK_MIN, OP_ATK_MAX, OP_REL_MODEL,
    OP_XTALK, OP_XT_COMP, OP_XT_TRANS, OP_XT_TSTRIP, OP_XT_WIRE, OP_XT_FLOOR, OP_XT_MIN,
    OP_PC_FAST, OP_PC_SLOW, OP_PC_NORM, OP_PC_SOFT, OP_PC_GAIN, OP_PC_BUSA, OP_PC_BUSB, OP_PC_TRIG,
    OP_SC_HZ, OP_SC_V1, OP_SC_V2, OP_SC_V3,
};
static const char *const OA_FOOT[9] = {"16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'"};
static const char *const OA_ENVM[4] = {"CLICK", "COSINE", "LINEAR", "SHELF"};
typedef struct {
    char t[16];                                         /* the value's text */
    int8_t mn, mv;                                      /* a meter of mn cells with mv lit (mn 0: none) */
    uint8_t on;                                         /* the value is "on" (drawn bright) */
} oa_val_t;

static void oa_vt(oa_val_t *v, const char *s) { str_cpy(v->t, s, 16); }
static void oa_vnum(oa_val_t *v, int32_t x, int32_t dec, const char *unit)   /* x with dec decimals, then the unit */
{
    fmt_fix(v->t, x, (uint32_t)dec);
    if (unit)
        str_cpy(v->t + str_len(v->t), unit, 16 - (unsigned)str_len(v->t));
}
static void oa_vdb(oa_val_t *v, int32_t tenths)         /* "+1.5 DB" */
{
    char *p = v->t;
    if (tenths > 0)
        *p++ = '+';
    if (tenths % 10)
        fmt_fix(p, tenths, 1u);
    else
        fmt_fix(p, tenths / 10, 0u);
    str_cpy(v->t + str_len(v->t), " DB", 4);
}
static void oa_vpct(oa_val_t *v, int32_t x)             /* a 0..127 value as a percentage; its meter has 10 cells */
{
    oa_vnum(v, (x * 100 + 63) / 127, 0, " %");
    v->mn = 10;
    v->mv = (int8_t)((x * 10 + 63) / 127);
    v->on = x != 0;
}
static int32_t oa_u(uint32_t op, uint32_t mx) { return (int32_t)(org_patch[op] <= mx ? org_patch[op] : 0u); }
static void oa_vsw(oa_val_t *v, uint32_t op, const char *off, const char *on)
{
    oa_vt(v, org_patch[op] ? on : off);
    v->on = org_patch[op] != 0;
}
/* the horn / drum speeds and ramps of the ROTARY page: the base values (fx.c ROT_SPF / ROT_RTF: 1.12^(i-4) and 1.4^(i-4)) */
static void oa_vspeed(oa_val_t *v, uint32_t op, uint32_t base_mhz)      /* rpm */
{
    uint32_t i = org_c9(org_patch[op] <= 8u ? org_patch[op] : 0u);
    oa_vnum(v, (int32_t)((base_mhz * ROT_SPF[i] >> 8) * 3u / 50u), 0, " RPM");
    v->mn = 9;
    v->mv = (int8_t)(i + 1u);
    v->on = 1;
}
static void oa_vramp(oa_val_t *v, uint32_t op, uint32_t base_us, int32_t dec)
{
    uint32_t i = org_c9(org_patch[op] <= 8u ? org_patch[op] : 0u), us = (base_us >> 4) * ROT_RTF[i] >> 4;
    oa_vnum(v, (int32_t)(dec ? (us + 5000u) / 10000u : (us + 50000u) / 100000u), dec ? 2 : 1, " S");
    v->mn = 9;
    v->mv = (int8_t)(i + 1u);
    v->on = 1;
}

static int32_t oa_ai(uint32_t op) { return (int32_t)org_ci(op, org_patch[op] <= org_pmax(op) ? org_patch[op] : 0u); }   /* the step an ADVANCED byte shows */
static void oa_vladder(oa_val_t *v, int32_t i, const char *zero)      /* 0 = off; else -2 dB steps up to 0 dB */
{
    if (i == 0) {
        oa_vt(v, zero);
        return;
    }
    oa_vnum(v, -2 * (51 - i), 0, " DB");
    v->on = 1;
}
static void oa_val_adv(uint32_t r, oa_val_t *v)
{
    uint32_t op = OA_AOP[r];
    int32_t i;
    if (op == OP_TEMP) {
        i = oa_u(OP_TEMP, 2u);
        oa_vt(v, i == 2 ? "EQUAL" : i == 1 ? "50 HZ GEAR" : "60 HZ GEAR");
        v->on = 1;
        return;
    }
    if (op == OP_XTALK) {
        oa_vsw(v, OP_XTALK, "OFF", "ON");
        return;
    }
    i = oa_ai(op);
    v->on = 1;
    switch (op) {
    case OP_TUNING:     oa_vnum(v, 4080 + 5 * i, 1, " HZ"); break;
    case OP_ATK_MODEL:
    case OP_REL_MODEL:  oa_vt(v, OA_ENVM[i & 3]); break;
    case OP_ATK_LEVEL:
    case OP_REL_LEVEL:  oa_vnum(v, i * 5 / 2, 0, " %"); break;
    case OP_ATK_MIN:
    case OP_ATK_MAX:    oa_vnum(v, i * 16 / 5, 0, " SMP"); break;
    case OP_XT_FLOOR:
    case OP_XT_MIN:     oa_vladder(v, i, "OFF"); break;
    case OP_XT_COMP:
    case OP_XT_TRANS:
    case OP_XT_TSTRIP:
    case OP_XT_WIRE:    oa_vladder(v, i, "NONE"); break;
    case OP_PC_FAST:
    case OP_PC_SLOW:    oa_vnum(v, i + 1, 1, " S"); break;
    case OP_PC_NORM:
    case OP_PC_SOFT:    oa_vnum(v, -i, 0, " DB"); break;
    case OP_PC_GAIN:    oa_vnum(v, 5 * i, 1, 0); break;
    case OP_PC_BUSA:
    case OP_PC_BUSB:    oa_vt(v, OA_FOOT[i % 9]); break;
    case OP_PC_TRIG:    oa_vt(v, i ? OA_FOOT[(i - 1) % 9] : "NONE"); break;
    case OP_SC_HZ:      oa_vnum(v, 400 + 25 * i, 2, " HZ"); break;
    default:            oa_vnum(v, 5 * i, 1, 0); break;                  /* the scanner's depths, in samples */
    }
}

static void oa_val(uint32_t pg, uint32_t r, oa_val_t *v)
{
    int32_t i;
    v->t[0] = 0;
    v->mn = v->mv = 0;
    v->on = 0;
    if (pg == 8u) {
        oa_val_adv(r, v);
    } else if (pg == 2u) {
        if (r == 0u)
            oa_vsw(v, OP_PERC, "OFF", "ON");
        else if (r == 1u)
            oa_vsw(v, OP_PERC_SOFT, "NORMAL", "SOFT");
        else if (r == 2u)
            oa_vsw(v, OP_PERC_FAST, "SLOW, 4 S", "FAST, 1 S");
        else if (r == 3u)
            oa_vsw(v, OP_PERC_SEL, "4' SECOND", "2 2/3' THIRD");
        else {
            i = oa_u(OP_VIB, 6u);
            oa_vt(v, i ? VIB_SHORT[i] : "OFF");
            v->on = i != 0;
        }
    } else if (pg == 3u) {
        if (r == 0u)
            oa_vsw(v, OP_ROT, "OFF", "ON");
        else if (r == 1u) {
            oa_vsw(v, OP_ROT_MODEL, "CLASSIC", "LIGHT");
            v->on = org_patch[OP_ROT] != 0;
        } else if (r == 2u)
            oa_vsw(v, OP_ROT_HORN, "NORMAL", "SHARP");
        else if (r == 3u) {
            i = oa_u(OP_ROT_SPEED, 2u);
            oa_vt(v, ROT_SPD[i]);
            v->on = i != 0;
        } else if (r == 4u) {
            i = oa_u(OP_ROT_MIC, 6u);
            oa_vnum(v, 180 - 30 * i, 0, " DEG");
            v->mn = 7;
            v->mv = (int8_t)(6 - i);
            v->on = 1;
        } else if (r == 5u) {
            i = oa_u(OP_ROT_BAL, 8u);
            oa_vdb(v, (i - 4) * 15);
            v->mn = 9;
            v->mv = (int8_t)i;
            v->on = 1;
        } else if (r == 6u)
            oa_vspeed(v, OP_ROT_HSLOW, 672u);
        else if (r == 7u)
            oa_vspeed(v, OP_ROT_HFAST, 6500u);
        else if (r == 8u)
            oa_vspeed(v, OP_ROT_DSLOW, 600u);
        else if (r == 9u)
            oa_vspeed(v, OP_ROT_DFAST, 5955u);
        else if (r == 10u)
            oa_vramp(v, OP_ROT_HRAMP, 1500000u, 1);
        else
            oa_vramp(v, OP_ROT_DRAMP, 4127000u, 0);
    } else if (pg == 4u) {
        if (r == 0u)
            oa_vsw(v, OP_DRV, "OFF", "ON");
        else if (r == 1u) {
            oa_vsw(v, OP_DRV_TYPE, "DISTORTION", "TUBE");
            v->on = org_patch[OP_DRV] != 0;
        } else if (r == 2u) {
            i = oa_u(OP_DRV_GAIN, 12u);
            oa_vdb(v, (i - 6) * 20);
            v->mn = 13;
            v->mv = (int8_t)(i + 1);
            v->on = 1;
        } else if (r == 3u) {
            i = (int32_t)org_c9(org_patch[OP_DRV_TONE] <= 8u ? org_patch[OP_DRV_TONE] : 0u);
            oa_vt(v, i == 4 ? "FLAT" : i < 4 ? "DARK" : "BRIGHT");
            if (i != 4)
                oa_vnum(v, i - 4, 0, i < 4 ? " (DARK)" : " (BRIGHT)");
            v->mn = 9;
            v->mv = (int8_t)(i + 1);
            v->on = 1;
        } else {
            i = (int32_t)org_c9(org_patch[OP_DRV_OUT] <= 8u ? org_patch[OP_DRV_OUT] : 0u);
            oa_vdb(v, (i - 4) * 20);
            v->mn = 9;
            v->mv = (int8_t)(i + 1);
            v->on = 1;
        }
    } else if (pg == 5u) {
        if (r == 0u)
            oa_vpct(v, TSEL->p[P_REV]);
        else if (r == 1u) {
            oa_vt(v, N_RTYPE[clamp(song.g[G_RTYPE], 0, 4)]);
            v->on = TSEL->p[P_REV] != 0;
        } else if (r == 2u)
            oa_vpct(v, song.g[G_RSIZE]);
        else
            oa_vpct(v, song.g[G_RDAMP]);
    } else if (pg == 6u) {
        if (r == 0u)
            oa_vpct(v, TSEL->p[P_DLY]);
        else if (r == 1u) {
            oa_vt(v, N_DLYTYPE[clamp(song.g[G_DTIME], 0, 6)]);
            v->on = TSEL->p[P_DLY] != 0;
        } else if (r == 2u)
            oa_vpct(v, song.g[G_DRREV]);
        else if (r == 3u)
            oa_vpct(v, song.g[G_DFDBK]);
        else if (r == 4u)
            oa_vpct(v, song.g[G_DCOLOR]);
        else
            oa_vpct(v, song.g[G_DMIX]);
    } else if (pg == 7u) {
        if (r == 0u)
            oa_vsw(v, OP_EQ, "OFF", "ON");
        else if (r == 4u) {
            oa_vnum(v, (int32_t)EQ_FREQ_HZ[eq_fidx(org_patch[OP_EQ_FREQ] <= 11u ? org_patch[OP_EQ_FREQ] : 0u)], 0, " HZ");
            v->on = 1;
        } else {
            i = eq_db(org_patch[r == 1u ? OP_EQ_LOW : r == 2u ? OP_EQ_MID : OP_EQ_HIGH] <= 24u ?
                      org_patch[r == 1u ? OP_EQ_LOW : r == 2u ? OP_EQ_MID : OP_EQ_HIGH] : 0u);
            oa_vdb(v, i * 10);
            v->mn = 13;
            v->mv = (int8_t)((i + 13) / 2);
            v->on = 1;
        }
    }
}

/* one detent (d = +1 / -1) on row r of page pg. A switch takes the direction (clockwise: on); the others step */
static void oa_c9(uint32_t op, int32_t d)
{
    org_set(op, org_b9((uint32_t)clamp((int32_t)org_c9(org_patch[op] <= 8u ? org_patch[op] : 0u) + d, 0, 8)));
}
static void oa_eqs(uint32_t op, int32_t d)
{
    org_set(op, eq_byte(clamp(eq_db(org_patch[op] <= 24u ? org_patch[op] : 0u) + d, -12, 12)));
}
static void oa_adj_adv(uint32_t r, int32_t d)
{
    uint32_t op = OA_AOP[r];
    if (op == OP_TEMP)
        org_set(OP_TEMP, (uint32_t)clamp(oa_u(OP_TEMP, 2u) + d, 0, 2));
    else if (op == OP_XTALK)
        org_set(OP_XTALK, d > 0 ? 1u : 0u);
    else
        org_set(op, org_bi(op, (uint32_t)clamp(oa_ai(op) + d, 0, (int32_t)org_pmax(op))));
}
static void oa_adj1(uint32_t pg, uint32_t r, int32_t d)
{
    uint32_t on = d > 0 ? 1u : 0u;
    if (pg == 8u) {
        oa_adj_adv(r, d);
    } else if (pg == 2u) {
        if (r == 0u)
            org_set(OP_PERC, on);
        else if (r == 1u)
            org_set(OP_PERC_SOFT, on);
        else if (r == 2u)
            org_set(OP_PERC_FAST, on);
        else if (r == 3u)
            org_set(OP_PERC_SEL, on);
        else
            org_set(OP_VIB, (uint32_t)clamp(oa_u(OP_VIB, 6u) + d, 0, 6));
    } else if (pg == 3u) {
        if (r == 0u)
            org_set(OP_ROT, on);
        else if (r == 1u)
            org_set(OP_ROT_MODEL, on);
        else if (r == 2u)
            org_set(OP_ROT_HORN, on);
        else if (r == 3u)
            org_set(OP_ROT_SPEED, (uint32_t)clamp(oa_u(OP_ROT_SPEED, 2u) + d, 0, 2));
        else if (r == 4u)
            org_set(OP_ROT_MIC, (uint32_t)clamp(oa_u(OP_ROT_MIC, 6u) - d, 0, 6));      /* (clockwise: wider) */
        else if (r == 5u)
            org_set(OP_ROT_BAL, (uint32_t)clamp(oa_u(OP_ROT_BAL, 8u) + d, 0, 8));
        else
            oa_c9(OP_ROT_HSLOW + r - 6u, d);
    } else if (pg == 4u) {
        if (r == 0u)
            org_set(OP_DRV, on);
        else if (r == 1u)
            org_set(OP_DRV_TYPE, on);
        else if (r == 2u)
            org_set(OP_DRV_GAIN, (uint32_t)clamp(oa_u(OP_DRV_GAIN, 12u) + d, 0, 12));
        else
            oa_c9(r == 3u ? OP_DRV_TONE : OP_DRV_OUT, d);
    } else if (pg == 5u) {
        if (r == 0u)
            TSEL->p[P_REV] = (uint8_t)oa_step(TSEL->p[P_REV], d, 127);
        else if (r == 1u)
            song.g[G_RTYPE] = (int16_t)((song.g[G_RTYPE] + (d > 0 ? 1 : 4)) % 5);
        else if (r == 2u)
            song.g[G_RSIZE] = (int16_t)oa_step(song.g[G_RSIZE], d, 127);
        else
            song.g[G_RDAMP] = (int16_t)oa_step(song.g[G_RDAMP], d, 127);
    } else if (pg == 6u) {
        if (r == 0u)
            TSEL->p[P_DLY] = (uint8_t)oa_step(TSEL->p[P_DLY], d, 127);
        else if (r == 1u)
            song.g[G_DTIME] = (int16_t)((song.g[G_DTIME] + (d > 0 ? 1 : 6)) % 7);
        else if (r == 2u)
            song.g[G_DRREV] = (int16_t)oa_step(song.g[G_DRREV], d, 127);
        else if (r == 3u)
            song.g[G_DFDBK] = (int16_t)oa_step(song.g[G_DFDBK], d, 120);
        else if (r == 4u)
            song.g[G_DCOLOR] = (int16_t)oa_step(song.g[G_DCOLOR], d, 127);
        else
            song.g[G_DMIX] = (int16_t)oa_step(song.g[G_DMIX], d, 127);
    } else if (pg == 7u) {
        if (r == 0u)
            org_set(OP_EQ, on);
        else if (r == 4u)
            org_set(OP_EQ_FREQ, eq_fbyte((uint32_t)clamp((int32_t)eq_fidx(org_patch[OP_EQ_FREQ] <= 11u ? org_patch[OP_EQ_FREQ] : 0u) + d, 0, 11)));
        else
            oa_eqs(r == 1u ? OP_EQ_LOW : r == 2u ? OP_EQ_MID : OP_EQ_HIGH, d);
    }
}
static void oa_adj(uint32_t pg, uint32_t r, int32_t s)  /* s detents, either way */
{
    int32_t n = s < 0 ? -s : s, d = s < 0 ? -1 : 1;
    if (n > 24)
        n = 24;
    while (n--)
        oa_adj1(pg, r, d);
}

/* the page: header, the rows, the scroll bar and the hint */
static void oa_rows(uint32_t pg, const char *state)
{
    uint32_t n = OA_NR[pg], sel = oa_row[pg], top = oa_top[pg], i;
    if (sel >= n)
        sel = oa_row[pg] = 0;
    if (sel < top)
        top = sel;
    if (sel >= top + OA_VIS)
        top = sel - OA_VIS + 1u;
    oa_top[pg] = (uint8_t)top;
    header(pg == 8u ? "ADVANCED EDIT" : pg == 2u ? "PERC & VIB" : pg == 3u ? "ROTARY" : pg == 4u ? "DRIVE" : pg == 5u ? "REVERB" : pg == 6u ? "DELAY" : "EQ", state);
    for (i = 0; i < OA_VIS && top + i < n; i++) {
        oa_val_t v;
        uint32_t r = top + i;
        int32_t y = 33 + (int32_t)i * 23;
        int hot = r == sel;
        uint16_t bg = hot ? C_HOT : C_PANEL;
        oa_val(pg, r, &v);
        if (hot) {
            rrect(6, y - 5, 226, 21, 3, C_ORANGE);
            rrect(7, y - 4, 224, 19, 3, C_HOT);
        }
        ltext(14, y, &AF_X, OA_LB[pg][r], hot ? C_AMBER : C_CREAM, bg);
        if (v.mn)
            meter(86, y - 1, v.mn, v.mv);
        rtext(228, y, &AF_X, v.t, v.on ? C_AMBER : C_DIM, bg);
    }
    if (n > OA_VIS) {                                   /* the scroll bar */
        cv_rect(235, 30, 3, 184, C_PANEL2);
        cv_rect(235, 30 + (int32_t)(184u * top / n), 3, (int32_t)(184u * OA_VIS / n), C_ORANGE);
    }
    footer("SELECT: ITEM    ALGORITHM: VALUE");
}


static void scr_home(void)
{
    char num[8], nm[16];
    int32_t k;
    oa_preset_label(num, nm);
    header("ORGAN", "UPPER");
    ctext(120, 36, &AF_L, num, C_AMBER, C_PANEL);
    ctext(120, 72, text_w(&AF_M, nm) > 220 ? &AF_S : &AF_M, nm, C_CREAM, C_PANEL);
    cv_rect(44, 94, 152, 1, C_PANEL2);
    for (k = 0; k < 9; k++)
        drawbar(56 + k * 16, 100, org_patch[OP_UB + (uint32_t)k], 7, 14, k, 0, 1);
    lamp(22, 182, "PERCUSSION", org_patch[OP_PERC] != 0);
    lamp(22, 198, VIB_SHORT[org_patch[OP_VIB] <= 6u ? org_patch[OP_VIB] : 0u], org_patch[OP_VIB] != 0);
    lamp(130, 182, !org_patch[OP_ROT] ? "ROTOR" : org_patch[OP_ROT_SPEED] == 2u ? "ROTOR FAST" : org_patch[OP_ROT_SPEED] == 1u ? "ROTOR SLOW" : "ROTOR STOP", org_patch[OP_ROT] != 0);
    lamp(22, 214, "DRIVE", org_patch[OP_DRV] != 0);
    lamp(130, 198, org_patch[OP_TEMP] == 2u ? "EQUAL" : org_patch[OP_TEMP] == 1u ? "50 HZ" : "60 HZ", 1);
    footer("PRESETS: CHOOSE   OCT- / OCT+: OCTAVE");
}

/* the bars of the DRAWBARS page: UPPER nine; LOWER / PEDAL eleven, the pedal's two (16' and 8') on the left, the lower's nine on the right.
 * oa_bar_op: the patch byte of bar i; oa_bar_k: its footage (0..8, FOOT) */
static uint32_t oa_nbar(void) { return oa_man ? 11u : 9u; }
static uint32_t oa_bar_op(uint32_t i) { return oa_man ? (i < 2u ? OP_PB + (i ? 2u : 0u) : OP_LB + i - 2u) : OP_UB + i; }
static uint32_t oa_bar_k(uint32_t i) { return oa_man ? (i < 2u ? (i ? 2u : 0u) : i - 2u) : i; }

static void scr_drawbars(void)
{
    char num[8], nm[16], r[20];
    uint32_t i, nb = oa_nbar();
    int32_t sp = oa_man ? 20 : 25, w = oa_man ? 17 : 22;
    oa_preset_label(num, nm);
    str_cpy(r, num, 4);
    str_cpy(r + str_len(r), " ", 2);
    str_cpy(r + str_len(r), nm, 13);
    header(oa_man ? "LOWER / PEDAL" : "UPPER", r);
    bar_slot(oa_man ? 3 : 20 - 25 / 2, 26, oa_man ? 234 : 25 * 9);
    if (oa_man)
        cv_rect(52, 32, 1, 168, C_PANEL2);               /* between the pedal's bars and the lower's */
    for (i = 0; i < nb; i++) {
        int32_t cx = oa_man ? (i < 2u ? 14 + (int32_t)i * sp : 70 + (int32_t)(i - 2u) * sp) : 20 + (int32_t)i * sp;
        uint32_t k = oa_bar_k(i), hot = i == oa_sel, near = i > oa_sel && i < oa_sel + 4u;
        const char *fo = oa_man && i < 2u ? (i ? "8'" : "16'") : FOOT[k];
        if (hot)
            rrect(cx - w / 2 - 2, 25, w + 4, 170, 3, C_HOT);       /* the picked bar's column, lit */
        drawbar(cx, 26, org_patch[oa_bar_op(i)], 15, w, (int32_t)k, (int)hot, 0);
        ctext(cx, 180, &AF_X, fo, hot ? C_ORANGE : near ? C_AMBER : C_DIM, hot ? C_HOT : C_PANEL);
        if (!(oa_man && i < 2u) && FRAC[k][0])
            ctext(cx, 189, &AF_X, FRAC[k], hot ? C_ORANGE : near ? C_AMBER : C_DIM, hot ? C_HOT : C_PANEL);
    }
    if (oa_man) {
        ctext(24, 203, &AF_X, "PEDAL", C_DIM, C_PANEL);
        oa_reg(r, &org_patch[OP_LB]);
        ctext(150, 203, &AF_N, r, C_CREAM, C_PANEL);
    } else {
        oa_reg(r, &org_patch[OP_UB]);
        ctext(120, 203, &AF_N, r, C_CREAM, C_PANEL);
    }
    footer(oa_man ? "SELECT: BAR   ALGORITHM: PULL   EDIT: ADVANCED" : "SELECT: BAR   ALGORITHM: PULL   EDIT: LOWER");
}

static void oa_scene(void)
{
    if (oa_page == 1u)
        scr_drawbars();
    else if (oa_page >= 2u && oa_page <= 8u)
        oa_rows(oa_page, oa_page == 2u || oa_page == 8u ? "" : oa_page == 3u ? (org_patch[OP_ROT] ? ROT_SPD[oa_u(OP_ROT_SPEED, 2u)] : "OFF") :
                         oa_page == 4u ? (org_patch[OP_DRV] ? (org_patch[OP_DRV_TYPE] ? "TUBE" : "DIST") : "OFF") :
                         oa_page == 5u ? (TSEL->p[P_REV] ? N_RTYPE[clamp(song.g[G_RTYPE], 0, 4)] : "OFF") :
                         oa_page == 6u ? (TSEL->p[P_DLY] ? N_DLYTYPE[clamp(song.g[G_DTIME], 0, 6)] : "OFF") :
                         (org_patch[OP_EQ] ? "ON" : "OFF"));
    else
        scr_home();
    if (oa_gl == 1u)
        oa_gl_dialog();
    else if (oa_sv)
        oa_sv_dialog();
    else if (oa_msg_on()) {
        cv_rect(30, 92, 180, 40, C_ORANGE);
        cv_rect(32, 94, 176, 36, C_POP);
        ctext(120, 104, &AF_M, oa_msg, C_AMBER, C_POP);
    } else if (oa_pp_on())
        oa_pp_draw();
}

static uint32_t oa_signature(void)
{
    const track_t *t = TSEL;
    uint32_t h = 2166136261u, i;
    for (i = 0; i < ORG_PSIZE; i++)
        h = (h ^ org_patch[i]) * 16777619u;
    h = (h ^ oa_page ^ (uint32_t)oa_man << 11 ^ (uint32_t)oa_sel << 2 ^ (uint32_t)oa_row[oa_page % 9u] << 18 ^ (uint32_t)oa_top[oa_page % 9u] << 22 ^ (uint32_t)t->preset << 6 ^ (uint32_t)t->user << 14) * 16777619u;
    h = (h ^ oa_gl ^ (uint32_t)oa_gl_row << 2 ^ (uint32_t)fm_midi_src << 4 ^ (uint32_t)fm_midi_ch << 6) * 16777619u;
    h = (h ^ oa_sv ^ (uint32_t)oa_sv_slot << 1 ^ (uint32_t)oa_sv_pos << 8 ^ (uint32_t)oa_msg_on() << 12 ^
         (uint32_t)oa_pp_on() << 13 ^ (uint32_t)up_gen << 16) * 16777619u;
    for (i = 0; i < 12u; i++)
        h = (h ^ (uint8_t)oa_sv_name[i]) * 16777619u;
    h = (h ^ t->p[P_REV] ^ (uint32_t)t->p[P_DLY] << 8 ^ (uint32_t)song.g[G_RTYPE] << 16 ^ (uint32_t)song.g[G_DTIME] << 20) * 16777619u;
    h = (h ^ (uint32_t)song.g[G_RSIZE] ^ (uint32_t)song.g[G_RDAMP] << 7 ^ (uint32_t)song.g[G_DRREV] << 14 ^
         (uint32_t)song.g[G_DFDBK] << 21) * 16777619u;
    h = (h ^ (uint32_t)song.g[G_DCOLOR] ^ (uint32_t)song.g[G_DMIX] << 7 ^ (uint32_t)(cpu_high != 0u) << 14) * 16777619u;
    return h;
}

static void oa_splash(void);
static void oa_draw(void)
{
    uint32_t sig = oa_signature(), band;
    if (sig == oa_sig_prev && !ui.force)
        return;
    oa_sig_prev = sig;
    ui.force = 0;
    if (oa_gl == 3u) {                                    /* ABOUT: the splash, the version bottom right */
        oa_splash();
        return;
    }
    for (band = 0; band < 2u; band++) {
        cv_begin(240, 120, C_PANEL);
        cv_oy = -(int32_t)band * 120;
        cv_cy0 = 0;
        oa_scene();
        cv_oy = 0;
        cv_blit(0, band * 120u);
    }
}

#include "organvave_splash.h"
static void oa_splash(void)                              /* the picture (docs/organvave-splash.png), the version bottom right: power-on (3 s, main.c) and ABOUT */
{
    uint32_t band, x, y;
    for (band = 0; band < 2u; band++) {
        cv_begin(240, 120, 0);
        cv_oy = -(int32_t)band * 120;
        for (y = band * 120u; y < band * 120u + 120u; y++)
            for (x = 0; x < 240u; x++)
                cv_pset((int32_t)x, (int32_t)y, ORGANVAVE_SPLASH_PAL[ORGANVAVE_SPLASH[y * 240u + x]]);
        cv_text_on(232 - text_w(&AF_S, ORGANVAVE_VERSION), 220, &AF_S, ORGANVAVE_VERSION, C_CREAM, 0);
        cv_oy = 0;
        cv_blit(0, band * 120u);
    }
}

/* ----------------------------------------------------------- control --- */
static void oa_close_all(void)
{
    oa_sv = 0;
    oa_gl_close();
}
static void oa_bar(uint32_t i, int32_t s)                 /* bar i (of the page's list) pulled s steps */
{
    uint32_t op;
    if (i >= oa_nbar())
        return;
    op = oa_bar_op(i);
    org_set(op, (uint32_t)clamp((int32_t)org_patch[op] + s, 0, 8));
}

static void oa_buttons(uint32_t pressed)
{
    if ((pressed >> panel.btn[B_HOME]) & 1u) {            /* HOME: the first screen; the pop-ups close */
        oa_close_all();
        oa_page = 0;
        return;
    }
    if ((pressed >> panel.btn[B_EDIT]) & 1u) {            /* EDIT: the drawbars (upper); again: the lower manual's with the pedal's two; again: ADVANCED EDIT; again: the upper's */
        oa_close_all();
        if (oa_page == 1u && oa_man) {
            oa_page = 8;
            return;
        }
        oa_man = oa_page == 1u ? (uint8_t)(oa_man ^ 1u) : 0u;
        oa_sel = (uint8_t)(oa_man && oa_page == 1u ? (oa_sel < 2u ? oa_sel : 2u) : 0u);
        oa_page = 1;
        return;
    }
    if ((pressed >> panel.btn[B_GLO]) & 1u) {             /* GLO: the global pop-up; in ABOUT, back; again, close */
        oa_sv = 0;
        if (!oa_gl)
            oa_gl = 1;
        else if (oa_gl > 1u)
            oa_gl = 1;
        else
            oa_gl_close();
        return;
    }
    if ((pressed >> panel.btn[B_SAVE]) & 1u) {            /* SAVE: open the dialog; again: write */
        oa_gl_close();
        if (oa_sv)
            oa_sv_do();
        else
            oa_sv_open();
        return;
    }
    if ((pressed >> panel.btn[B_FX]) & 1u) {              /* FX: PERC & VIB, ROTARY, DRIVE, REVERB, DELAY, EQ, then HOME */
        oa_close_all();
        oa_page = oa_page >= 2u && oa_page < 7u ? (uint8_t)(oa_page + 1u) : oa_page == 7u ? 0u : 2u;
        return;
    }
    if ((pressed >> panel.btn[B_SCL]) & 1u) {             /* SCL: the rotor's speed: off -> SLOW -> FAST -> STOP -> SLOW */
        if (!org_patch[OP_ROT]) {
            org_set(OP_ROT_SPEED, 1);
            org_set(OP_ROT, 1);
        } else
            org_set(OP_ROT_SPEED, org_patch[OP_ROT_SPEED] == 1u ? 2u : org_patch[OP_ROT_SPEED] == 2u ? 0u : 1u);
    }
    if ((pressed >> panel.btn[B_ENV]) & 1u)               /* ENV: the percussion on or off */
        org_set(OP_PERC, !org_patch[OP_PERC]);
    if ((pressed >> panel.btn[B_LFO]) & 1u)               /* LFO: the vibrato's selector, on to the next */
        org_set(OP_VIB, (org_patch[OP_VIB] + 1u) % 7u);
    if ((pressed >> panel.btn[B_OCTDN]) & 1u)
        song.octave = song.octave > -2 ? song.octave - 1 : song.octave;
    if ((pressed >> panel.btn[B_OCTUP]) & 1u)
        song.octave = song.octave < 2 ? song.octave + 1 : song.octave;
}

/* MIDI CCs the ISR noted (midi_control.c org_midi_cc): the drawbars of the three manuals and the rotor's speed */
static void oa_cc_poll(void)
{
    uint32_t m, i, rq;
    for (m = 0; m < 3u; m++) {
        if (!fm_dbar_pend[m])
            continue;
        for (i = 0; i < 9u; i++)
            if ((fm_dbar_pend[m] >> i) & 1u) {
                uint32_t v;
                fm_dbar_pend[m] &= (uint16_t)~(1u << i);
                v = (127u - fm_dbar_rx[m][i]) * 8u;               /* (setBfree: inverted, 0..8 by rounding) */
                org_set(9u * m + i, (v + 63u) / 127u);
            }
    }
    rq = fm_rot_req;
    if (rq) {
        fm_rot_req = 0;
        org_set(OP_ROT_SPEED, rq == 4u ? (org_patch[OP_ROT_SPEED] == 1u ? 2u : 1u) : rq - 1u);
    }
}

static void oa_input(void)
{
    uint32_t pressed = fm1_input_edges(0), k;
    int32_t s;
    (void)fm1_input_note_edges();
    midi_pc_poll();                                       /* Program Change: a preset */
    oa_cc_poll();
    if (TSEL->eng_req != ENGI_ORGAN) {                    /* an old autosave: the instrument is the organ */
        set_engine_of(TSEL, ENGI_ORGAN);
        apply_preset_to(TSEL, 0);
    }
    song.sel = 0;
    oa_buttons(pressed);
    if (oa_track_preset())                                /* a new preset (the knob, a Program Change, a saved one) */
        oa_sv = 0;
    if (oa_gl) {                                          /* the GLO pop-up (the knobs wait) */
        for (k = 0; k < 4u; k++)
            (void)panel_enc(EN_K1 + k);
        (void)panel_enc(EN_PRESET);
        if (oa_gl == 1u) {
            if ((s = panel_enc(EN_SELECT)) != 0)
                oa_gl_row = (uint8_t)((oa_gl_row + 3u + (s > 0 ? 1u : 2u)) % 3u);
            if ((s = panel_enc(EN_ALGO)) != 0) {
                if (oa_gl_row == 0u) {
                    static const uint8_t SRC[3] = {0, 1, 3};
                    int32_t d = (fm_midi_src == 3u ? 2 : (int32_t)(fm_midi_src & 1u)) + (s > 0 ? 1 : -1);
                    fm_midi_src = SRC[clamp(d, 0, 2)];
                    panic_req |= 1u;                      /* a note of the source left must not hang */
                    oa_cfg_dirty = 1;
                } else if (oa_gl_row == 1u) {
                    fm_midi_ch = (uint8_t)clamp((int32_t)fm_midi_ch + (s > 0 ? 1 : -1), 0, 16);
                    panic_req |= 1u;
                    oa_cfg_dirty = 1;
                } else
                    oa_gl = 3;
            }
        } else if (panel_enc(EN_SELECT) != 0 || panel_enc(EN_ALGO) != 0)   /* ABOUT: either encoder goes back */
            oa_gl = 1;
        return;
    }
    if (oa_sv) {                                          /* the SAVE dialog: PRESETS the slot, the name; the knobs wait */
        if ((s = panel_enc(EN_PRESET)) != 0) {
            oa_sv_slot = (uint8_t)((oa_sv_slot + UP_SLOTS + (s > 0 ? 1u : UP_SLOTS - 1u)) % UP_SLOTS);
            oa_sv_fill();
        }
        if ((s = panel_enc(EN_SELECT)) != 0)
            oa_sv_pos = (uint8_t)((oa_sv_pos + 12u + (s > 0 ? 1u : 11u)) % 12u);
        if ((s = panel_enc(EN_ALGO)) != 0)
            oa_sv_letter(s);
        for (k = 0; k < 4u; k++)
            (void)panel_enc(EN_K1 + k);
        return;
    }
    if ((s = panel_enc(EN_PRESET)) != 0)
        oa_preset(s);
    if (oa_page >= 2u && oa_page <= 8u) {                 /* the FX pages and ADVANCED EDIT: SELECT picks a row, ALGORITHM changes it, the knobs 1-4 go to rows 1-4 */
        uint32_t pg = oa_page, n = OA_NR[pg];
        if ((s = panel_enc(EN_SELECT)) != 0)
            oa_row[pg] = (uint8_t)((oa_row[pg] + n + (s > 0 ? 1u : n - 1u)) % n);
        if ((s = panel_enc(EN_ALGO)) != 0)
            oa_adj(pg, oa_row[pg], s);
        for (k = 0; k < 4u && k < n; k++)
            if ((s = panel_enc(EN_K1 + k)) != 0)
                oa_adj(pg, k, s);
        return;
    }
    if ((s = panel_enc(EN_SELECT)) != 0) {                /* SELECT picks a bar (on HOME: opens the upper manual's) */
        oa_man = oa_page == 1u ? oa_man : 0u;
        oa_page = 1;
        oa_sel = (uint8_t)clamp((int32_t)oa_sel + (s > 0 ? 1 : -1), 0, (int32_t)oa_nbar() - 1);
    }
    if ((s = panel_enc(EN_ALGO)) != 0 && oa_page == 1u)   /* ALGORITHM pulls the picked bar */
        oa_bar(oa_sel, s);
    for (k = 0; k < 4u; k++)
        if ((s = panel_enc(EN_K1 + k)) != 0) {
            oa_man = oa_page == 1u ? oa_man : 0u;
            oa_page = 1;                                  /* (on HOME: the knob opens DRAWBARS and moves its bar) */
            oa_bar(oa_sel + k, s);
        }
}

static void oa_leds(void)
{
    uint8_t nl[FM1_NCOL] = {0}, nd[FM1_NCOL] = {0}, nb[FM1_NCOL] = {0}, nm[FM1_NCOL] = {0};
    uint32_t k, c;
    static uint8_t ready;
    if (idle_leds() || fm1_led_anim_on())
        return;
    if (!ready) {
        led_pos_init();
        ready = 1;
    }
    for (k = 0; k < NB; k++)
        led_put(nd, panel.btn[k], 1);                       /* every button's glow */
    led_put(nl, panel.btn[B_HOME], oa_page == 0u);
    led_put(nl, panel.btn[B_EDIT], oa_page == 1u || oa_page == 8u);
    led_put(nl, panel.btn[B_FX], oa_page >= 2u && oa_page <= 7u);
    led_put(nl, panel.btn[B_SCL], org_patch[OP_ROT] != 0 && org_patch[OP_ROT_SPEED] != 0);
    led_put(nl, panel.btn[B_ENV], org_patch[OP_PERC] != 0);
    led_put(nl, panel.btn[B_LFO], org_patch[OP_VIB] != 0);
    led_put(nl, panel.btn[B_SAVE], oa_sv);
    led_put(nl, panel.btn[B_GLO], oa_gl != 0u);
    led_put(nl, panel.btn[B_OCTDN], song.octave < 0);
    led_put(nl, panel.btn[B_OCTUP], song.octave > 0);
    c = fm1_in.notes;                                       /* the keys held */
    for (k = 0; k < 27u; k++)
        led_put(nl, 14u + k, (int)((c >> k) & 1u));
    for (k = 0; k < FM1_NCOL; k++) {
        fm1_led_dim[k] = nd[k];
        fm1_led_breath[k] = nb[k];
        fm1_led_mid[k] = nm[k];
        fm1_led[k] = nl[k];
    }
}

/* power-on, after the autosave: the instrument is the organ (an autosave of another engine: the first preset) */
static void oa_init(void)
{
    track_t *t = &trk[0];
    if (t->eng_req != ENGI_ORGAN) {
        set_engine_of(t, ENGI_ORGAN);
        t->engine = t->eng_req;
    }
    t->p[P_MUTE] = 0;
    t->p[P_AMODE] = 0;                                    /* (no arpeggiator on the organ) */
    song.sel = 0;
    song.octave = (int8_t)clamp(song.octave, -2, 2);
    oa_cfg_load();
    oa_page = 0;
    oa_primed = 0;
    oa_track_preset();                                    /* the preset loaded at power-on: shown, no pop-up */
    oa_primed = 1;
    ui.force = 1;
}
