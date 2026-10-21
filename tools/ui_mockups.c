/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* OrganVave-1: mock-ups of the organ's screens, drawn after the real console's panel with the firmware's own canvas and
 * fonts (gfx.c, build/gen/ui_fonts.h) into DIR/mock_<name>.ppm (240 x 240). For the owner's approval before 0.1.0; the
 * drawing helpers here are integer-only so they can move into the firmware as they are.
 *   cc -O1 -w -Ibuild/gen -Ifirmware/src -Itests -o /tmp/mock tools/ui_mockups.c -lm && /tmp/mock DIR
 * (after the generation step of tools/build.py has made build/gen) */
#include "host.h"

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
/* the palette: the console's black control rail, its wood, chrome, the drawbars' three colours, the brand's orange */
#define C_PANEL  RGB565(0x14, 0x11, 0x10)
#define C_PANEL2 RGB565(0x22, 0x1d, 0x1a)
#define C_SLOT   RGB565(0x08, 0x07, 0x06)
#define C_CREAM  RGB565(0xef, 0xe6, 0xd2)
#define C_DIM    RGB565(0x8c, 0x82, 0x72)
#define C_ORANGE RGB565(0xff, 0x6a, 0x13)
#define C_AMBER  RGB565(0xff, 0xb3, 0x4a)
#define C_CHR_HI RGB565(0xe6, 0xe2, 0xda)
#define C_CHR    RGB565(0xa8, 0xa3, 0x9a)
#define C_CHR_LO RGB565(0x5e, 0x5a, 0x54)
#define C_BROWN  RGB565(0x6e, 0x33, 0x14)
#define C_BROWN2 RGB565(0x8e, 0x48, 0x22)
#define C_WHITE  RGB565(0xee, 0xe8, 0xda)
#define C_WHITE2 RGB565(0xc8, 0xc0, 0xae)
#define C_BLACK  RGB565(0x1c, 0x1b, 0x1a)
#define C_BLACK2 RGB565(0x3a, 0x38, 0x36)
#define C_TAB    RGB565(0xf2, 0xec, 0xdc)
#define C_TABDN  RGB565(0xc9, 0xc1, 0xae)

static const char *dir;

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

/* a rounded rectangle, anti-aliased (4 x 4 samples a pixel); r in pixels */
static int rr_in(int32_t sx, int32_t sy, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r)   /* samples: 1/4 px */
{
    int32_t x0 = 4 * x, y0 = 4 * y, x1 = 4 * (x + w), y1 = 4 * (y + h), R = 4 * r, cx, cy;
    if (sx < x0 || sx >= x1 || sy < y0 || sy >= y1)
        return 0;
    cx = sx < x0 + R ? x0 + R : sx >= x1 - R ? x1 - R - 1 : sx;
    cy = sy < y0 + R ? y0 + R : sy >= y1 - R ? y1 - R - 1 : sy;
    return (sx - cx) * (sx - cx) + (sy - cy) * (sy - cy) <= R * R;
}
static void rrect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    int32_t i, j, a, b;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++) {
            int32_t n = 0, edge = (i - x < r || x + w - 1 - i < r) && (j - y < r || y + h - 1 - j < r);
            if (!edge) {
                cv_pset(i, j, c);
                continue;
            }
            for (b = 0; b < 4; b++)
                for (a = 0; a < 4; a++)
                    n += rr_in(4 * i + a, 4 * j + b, x, y, w, h, r);
            px_blend(i, j, c, n * 16);
        }
}
/* a vertical gradient in a rounded rectangle: top colour, bottom colour */
static void rrect_v(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t top, uint16_t bot)
{
    int32_t j;
    for (j = 0; j < h; j++) {
        uint16_t c = mix(top, bot, h > 1 ? j * 256 / (h - 1) : 0);
        int32_t i, a, b;
        for (i = x; i < x + w; i++) {
            int32_t n = 0;
            for (b = 0; b < 4; b++)
                for (a = 0; a < 4; a++)
                    n += rr_in(4 * i + a, 4 * (y + j) + b, x, y, w, h, r);
            px_blend(i, y + j, c, n * 16);
        }
    }
}
/* a disc, anti-aliased; centre and radius in 1/4 px */
static void disc4(int32_t cx4, int32_t cy4, int32_t r4, uint16_t c)
{
    int32_t x0 = (cx4 - r4) / 4 - 1, x1 = (cx4 + r4) / 4 + 1, y0 = (cy4 - r4) / 4 - 1, y1 = (cy4 + r4) / 4 + 1, i, j, a, b;
    for (j = y0; j <= y1; j++)
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

/* sine and cosine of an angle in degrees, Q8 (a table of 0..90 in steps of 5, interpolated) */
static int32_t sin8(int32_t deg)
{
    static const int16_t S[19] = {0, 22, 44, 66, 88, 108, 128, 147, 165, 181, 196, 210, 222, 232, 241, 247, 252, 255, 256};
    int32_t s = 1, k, f;
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg >= 180) { deg -= 180; s = -1; }
    if (deg > 90) deg = 180 - deg;
    k = deg / 5; f = deg % 5;
    return s * (S[k] + (k < 18 ? (S[k + 1] - S[k]) * f / 5 : 0));
}
static int32_t cos8(int32_t deg) { return sin8(deg + 90); }

/* a thick line, anti-aliased roughly (discs along it) */
static void thick(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w4, uint16_t c)   /* ends in 1/4 px */
{
    int32_t n = (abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0)) / 2 + 1, k;
    for (k = 0; k <= n; k++)
        disc4(x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n, w4, c);
}

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
        for (i = x; i < x + w; i++) {
            int32_t t = (j - y) * 512 / (h > 1 ? h - 1 : 1);
            uint16_t c = t < 256 ? mix(W[0], W[1], t) : mix(W[1], W[2], t - 256);
            int32_t g = (i * 3 + (j - y) * 17 + ((i * i) >> 7) + ((i >> 3) * (j - y) & 7)) % 23;
            if (g < 2) c = mix(c, RGB565(0x3a, 0x1a, 0x0a), 110);
            else if (g == 9) c = mix(c, RGB565(0xb0, 0x62, 0x30), 70);
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
/* a hint line at the foot: what the four knobs and the encoders do here */
static void footer(const char *s)
{
    cv_rect(0, 226, 240, 1, C_PANEL2);
    ctext(120, 230, &AF_X, s, C_DIM, C_PANEL);
}

/* the drawbars' colours by footage, as on the console: brown 16' 5 1/3', white the octaves, black the mutations */
static const char *const FOOT[9] = {"16'", "5", "8'", "4'", "2", "2'", "1", "1", "1'"};
static const char *const FRAC[9] = {"", "1/3", "", "", "2/3", "", "3/5", "1/3", ""};   /* (the mutations' fractions, a line below) */
static int32_t bar_kind(int32_t k) { return k < 2 ? 0 : (k == 4 || k == 6 || k == 7) ? 2 : 1; }   /* 0 brown 1 white 2 black */

/* one drawbar hanging under the wood at y0, as on the console: a dark stem with two light rails and the numbers v .. 1 on
 * it (v = how far it is pulled, 0..8, `step` px each), then the cap: a block with a lit top face and a darker front with
 * its finger notch. The cap's colour by footage (brown, white, black); its footage engraved small on the top face */
static void drawbar(int32_t cx, int32_t y0, int32_t v, int32_t step, int32_t w, int32_t k, int hot, int compact)
{
    static const uint16_t TOP[3] = {RGB565(0x7a, 0x52, 0x46), RGB565(0xf4, 0xf1, 0xea), RGB565(0x5a, 0x5a, 0x5a)},
                          FRONT[3] = {RGB565(0x4c, 0x2c, 0x24), RGB565(0xc4, 0xc0, 0xb6), RGB565(0x2c, 0x2c, 0x2c)},
                          NOTCH[3] = {RGB565(0x30, 0x1a, 0x14), RGB565(0x9a, 0x96, 0x8e), RGB565(0x18, 0x18, 0x18)},
                          ENGR[3] = {RGB565(0xc8, 0xa8, 0x98), RGB565(0x6a, 0x66, 0x5e), RGB565(0xa8, 0xa8, 0xa8)};
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
        (void)ENGR;
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

/* a rocker tab as on the console: tall, cream, the upper word, the function in the middle, the lower word; `up` = the
 * upper half pressed in (that word is the one in force, drawn dark, the other grey); the pressed half darker */
static void tab3(int32_t x, int32_t y, int32_t w, int32_t h, const char *top, const char *m1, const char *m2, const char *bot,
                 int up, int hot)
{
    uint16_t lit = RGB565(0xf0, 0xec, 0xe0), mid = RGB565(0xd2, 0xcc, 0xbe), dark = RGB565(0xa8, 0xa2, 0x94), ink = RGB565(0x2a, 0x28, 0x24),
             off = RGB565(0x8a, 0x86, 0x7e);
    int32_t hh = h / 2;
    rrect(x - 3, y - 3, w + 6, h + 6, 4, hot ? C_ORANGE : RGB565(0x06, 0x06, 0x06));
    rrect(x - 1, y - 1, w + 2, h + 2, 3, RGB565(0x30, 0x2e, 0x2a));
    rrect_v(x, y, w, hh + 2, 3, up ? dark : lit, up ? mid : mid);         /* the upper half */
    rrect_v(x, y + hh, w, h - hh, 3, up ? mid : mid, up ? lit : dark);    /* the lower half */
    ctext(x + w / 2, y + 6, &AF_X, top, up ? ink : off, up ? dark : lit);
    if (m2[0]) {
        ctext(x + w / 2, y + hh - 10, &AF_X, m1, ink, mid);
        ctext(x + w / 2, y + hh + 2, &AF_X, m2, ink, mid);
    } else {
        ctext(x + w / 2, y + hh - 4, &AF_X, m1, ink, mid);
    }
    ctext(x + w / 2, y + h - 13, &AF_X, bot, up ? off : ink, up ? lit : dark);
}

/* a slide switch on an engraved grey plate (the rotary speaker's SLOW STOP FAST, the horn's character): a glossy black
 * ball in a slot, the positions' names below; pos 0 .. n-1 */
static void slide_switch(int32_t x, int32_t y, int32_t w, const char *title, const char *const *names, int32_t n, int32_t pos, int hot)
{
    uint16_t plate = RGB565(0x3c, 0x3a, 0x37), plate2 = RGB565(0x2c, 0x2a, 0x28), engr = RGB565(0xb8, 0xb4, 0xac);
    int32_t h = 58, sx0 = x + 22, sx1 = x + w - 22, sy = y + 30, k, bx;
    rrect(x - 2, y - 2, w + 4, h + 4, 5, hot ? C_ORANGE : RGB565(0x08, 0x08, 0x08));
    rrect_v(x, y, w, h, 4, plate, plate2);
    cv_rect(x + 3, y + 1, w - 6, 1, RGB565(0x5a, 0x58, 0x54));
    ctext(x + w / 2, y + 5, &AF_S, title, engr, plate);
    rrect(sx0 - 8, sy - 7, sx1 - sx0 + 16, 14, 7, RGB565(0x14, 0x13, 0x12));          /* the slot */
    rrect(sx0 - 6, sy - 1, sx1 - sx0 + 12, 2, 1, RGB565(0x6c, 0x6a, 0x66));
    for (k = 0; k < n; k++) {
        int32_t lx = sx0 + (sx1 - sx0) * k / (n - 1);
        ctext(lx, y + h - 13, &AF_X, names[k], k == pos ? C_AMBER : engr, mix(plate, plate2, 200));
    }
    bx = sx0 + (sx1 - sx0) * pos / (n - 1);
    disc(bx + 1, sy + 2, 11, RGB565(0x05, 0x05, 0x05));                               /* the ball: shadow, body, gloss */
    disc(bx, sy, 11, RGB565(0x1c, 0x22, 0x20));
    disc4(4 * bx - 2, 4 * sy - 2, 4 * 8, RGB565(0x2c, 0x34, 0x31));
    disc4(4 * bx - 10, 4 * sy - 12, 4 * 4, RGB565(0x7a, 0x86, 0x82));
    disc4(4 * bx - 12, 4 * sy - 14, 6, RGB565(0xd8, 0xe4, 0xe0));
}

/* a chickenhead knob with a chrome skirt (the vibrato selector, the amplifier's knobs): pointer angle in degrees, 0 = up */
static void knob(int32_t cx, int32_t cy, int32_t r, int32_t deg, int hot)
{
    disc(cx, cy, r + 2, hot ? C_ORANGE : C_SLOT);
    disc(cx, cy, r, C_CHR_LO);
    disc4(4 * cx + 1, 4 * cy + 1, 4 * r - 6, C_CHR);
    disc4(4 * cx - 2, 4 * cy - 2, 4 * r - 18, C_CHR_HI);
    disc4(4 * cx + 2, 4 * cy + 2, 4 * (r * 2 / 3), C_BLACK);
    disc4(4 * cx + 1, 4 * cy + 1, 4 * (r * 2 / 3) - 4, C_BLACK2);
    thick(4 * cx + 2, 4 * cy + 2, 4 * cx + 2 + (4 * (r - 2) * sin8(deg) >> 8), 4 * cy + 2 - (4 * (r - 2) * cos8(deg) >> 8), 5, C_CREAM);
}
/* an amplifier knob with its scale 0..10 around it and the value below */
static void amp_knob(int32_t cx, int32_t cy, const char *label, int32_t v100, const char *val, int hot)
{
    int32_t k, deg = -135 + 270 * v100 / 100;
    ctext(cx, cy - 34, &AF_X, label, hot ? C_AMBER : C_CREAM, C_PANEL);
    for (k = 0; k <= 10; k++) {                               /* the scale's ticks */
        int32_t a = -135 + 27 * k, r0 = 21, r1 = k % 5 ? 23 : 25;
        thick(4 * cx + 2 + (4 * r0 * sin8(a) >> 8), 4 * cy + 2 - (4 * r0 * cos8(a) >> 8),
              4 * cx + 2 + (4 * r1 * sin8(a) >> 8), 4 * cy + 2 - (4 * r1 * cos8(a) >> 8), 2, k * 10 <= v100 ? C_AMBER : C_DIM);
    }
    knob(cx, cy, 15, deg, hot);
    ctext(cx, cy + 27, &AF_S, val, hot ? C_AMBER : C_CREAM, C_PANEL);
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

/* ------------------------------------------------------------ screens --- */
static const int8_t REG[9] = {8, 8, 8, 0, 0, 0, 0, 0, 0};    /* the patch shown: a classic gospel registration */

/* a row of drawbars under the wood: `n` bars from footage k0, the hot ones (the knobs' four) marked, their footages below */
static void bar_row(int32_t x0, int32_t pitch, int32_t w, int32_t step, const int8_t *v, int32_t k0, int32_t n, int32_t hot0,
                    int32_t ylab)
{
    int32_t i;
    bar_slot(x0 - pitch / 2, 26, pitch * n);
    for (i = 0; i < n; i++) {
        int32_t cx = x0 + i * pitch, hot = i >= hot0 && i < hot0 + 4;
        drawbar(cx, 26, v[i], step, w, k0 + i, hot, 0);
        ctext(cx, ylab, &AF_X, FOOT[k0 + i], hot ? C_ORANGE : C_DIM, C_PANEL);
        if (FRAC[k0 + i][0])
            ctext(cx, ylab + 9, &AF_X, FRAC[k0 + i], hot ? C_ORANGE : C_DIM, C_PANEL);
    }
}

static void scr_drawbars(void)
{
    static const int8_t R[9] = {8, 8, 8, 6, 0, 0, 0, 0, 4};
    header("UPPER", "07 GOSPEL");
    bar_row(20, 25, 22, 15, R, 0, 9, 2, 180);
    ctext(120, 203, &AF_N, "888 6000 4", C_CREAM, C_PANEL);
    footer("KNOBS: THE FOUR MARKED   SELECT: MOVE THEM");
}

static void scr_home(void)
{
    int32_t k;
    header("ORGAN", "UPPER");
    ctext(120, 36, &AF_L, "07", C_AMBER, C_PANEL);
    ctext(120, 72, &AF_M, "GOSPEL", C_CREAM, C_PANEL);
    cv_rect(44, 94, 152, 1, C_PANEL2);
    for (k = 0; k < 9; k++)
        drawbar(56 + k * 16, 100, REG[k], 7, 14, k, 0, 1);
    lamp(22, 182, "PERC 3RD", 1);
    lamp(22, 198, "VIB C3", 1);
    lamp(130, 182, "ROTOR FAST", 1);
    lamp(130, 198, "DRIVE 4", 0);
    footer("PRESETS: CHOOSE   OCT- / OCT+: OCTAVE");
}

static void scr_percvib(void)
{
    static const char *const L[6] = {"V1", "C1", "V2", "C2", "V3", "C3"};
    int32_t k;
    header("PERC / VIB", "07 GOSPEL");
    tab3(10, 36, 48, 74, "ON", "PERCUS-", "SION", "OFF", 1, 1);
    tab3(68, 36, 48, 74, "SOFT", "PERC", "VOLUME", "NORMAL", 0, 0);
    tab3(126, 36, 48, 74, "FAST", "PERC", "DECAY", "SLOW", 1, 0);
    tab3(184, 36, 48, 74, "THIRD", "PERC", "HARM", "SECOND", 1, 0);
    tab3(10, 130, 48, 74, "ON", "VIBRATO", "UPPER", "OFF", 1, 0);
    tab3(68, 130, 48, 74, "ON", "VIBRATO", "LOWER", "OFF", 0, 0);
    {   /* the scanner's selector: V1 C1 V2 C2 V3 C3 around a chickenhead knob, pointing at C3 */
        int32_t cx = 178, cy = 172, r = 34;
        ctext(cx, 126, &AF_X, "VIBRATO & CHORUS", C_DIM, C_PANEL);
        for (k = 0; k < 6; k++) {
            int32_t a = -100 + 40 * k, tx = cx + (r * sin8(a) >> 8), ty = cy - (r * cos8(a) >> 8);
            ctext(tx, ty - 3, &AF_X, L[k], k == 5 ? C_AMBER : C_CREAM, C_PANEL);
        }
        knob(cx, cy, 18, 100, 0);
    }
    footer("KNOBS: THE FOUR TABS   ALGO: SELECTOR");
}

static void scr_rotary(void)
{
    static const char *const SPD[3] = {"SLOW", "STOP", "FAST"}, *const HORN[3] = {"DEFAULT", "MELLOW", "SHARP"};
    header("ROTARY", "07 GOSPEL");
    slide_switch(30, 34, 180, "ROTARY SPEAKER", SPD, 3, 2, 1);
    tab3(12, 108, 48, 74, "CLASSIC", "MODEL", "", "LIGHT", 1, 0);
    slide_switch(74, 112, 156, "HORN", HORN, 3, 0, 0);
    {   /* balance: horn against drum */
        int32_t x = 74, w = 154, v = 60;
        ltext(12, 196, &AF_X, "BALANCE", C_DIM, C_PANEL);
        rrect(x, 196, w, 6, 3, C_SLOT);
        rrect(x, 196, w * v / 100, 6, 3, C_AMBER);
        disc(x + w * v / 100, 199, 6, C_CHR);
        ltext(x, 208, &AF_X, "DRUM", C_DIM, C_PANEL);
        rtext(x + w, 208, &AF_X, "HORN", C_DIM, C_PANEL);
    }
    footer("KNOBS: SPEED MODEL HORN BALANCE");
}

static void scr_rotary2(void)
{
    header("ROTARY 2/3", "SPEEDS");
    amp_knob(36, 76, "HORN SLOW", 40, "40 RPM", 0);
    amp_knob(92, 76, "HORN FAST", 70, "423", 1);
    amp_knob(148, 76, "DRUM SLOW", 35, "36 RPM", 0);
    amp_knob(204, 76, "DRUM FAST", 66, "357", 0);
    amp_knob(36, 166, "HORN UP", 20, "0.16 S", 0);
    amp_knob(92, 166, "HORN DOWN", 30, "0.32 S", 0);
    amp_knob(148, 166, "DRUM UP", 55, "4.1 S", 0);
    amp_knob(204, 166, "DRUM DOWN", 25, "1.4 S", 0);
    footer("KNOBS: THE ROW   SELECT: ROW   FX: NEXT");
}

static void scr_drive(void)
{
    header("DRIVE", "07 GOSPEL");
    amp_knob(36, 92, "DRIVE", 40, "4", 1);
    amp_knob(92, 92, "CHARACTER", 55, "WARM", 0);
    amp_knob(148, 92, "TONE", 60, "6", 0);
    amp_knob(204, 92, "OUTPUT", 70, "-3 DB", 0);
    lamp(22, 168, "DRIVE ON", 1);
    ltext(22, 186, &AF_X, "THE PREAMP OF THE CONSOLE'S AMPLIFIER: SOFT", C_DIM, C_PANEL);
    ltext(22, 198, &AF_X, "OVERDRIVE WITH SAG; TONE BEFORE AND AFTER", C_DIM, C_PANEL);
    footer("ALGO: ON / OFF   FX: NEXT EFFECT");
}

static void scr_voice(void)
{
    header("VOICE", "ALL PRESETS");
    amp_knob(36, 92, "CLICK", 50, "5", 1);
    amp_knob(92, 92, "CROSSTALK", 20, "2", 0);
    amp_knob(148, 92, "TUNING", 0, "60 HZ", 0);
    amp_knob(204, 92, "LEVEL", 75, "0 DB", 0);
    ltext(14, 168, &AF_X, "TUNING: 60 HZ / 50 HZ GEARS, OR EQUAL", C_DIM, C_PANEL);
    ltext(14, 182, &AF_X, "CLICK: THE KEY CONTACTS' BOUNCE", C_DIM, C_PANEL);
    ltext(14, 196, &AF_X, "CROSSTALK: THE GENERATOR'S LEAKAGE ON A KEY", C_DIM, C_PANEL);
    footer("HOME: BACK");
}

static void scr_lower(void)
{
    static const int8_t L[9] = {0, 0, 8, 8, 8, 0, 0, 0, 0}, P[2] = {6, 3};
    header("LOWER / PEDAL", "MIDI CH 2 / 3");
    bar_row(12, 19, 17, 13, L, 0, 9, 2, 167);
    bar_slot(184, 26, 52);
    drawbar(198, 26, P[0], 13, 17, 0, 0, 0);                  /* the pedals: 16' (brown) and 8' (white) */
    drawbar(222, 26, P[1], 13, 17, 2, 0, 0);
    ctext(198, 167, &AF_X, "16'", C_DIM, C_PANEL);
    ctext(222, 167, &AF_X, "8'", C_DIM, C_PANEL);
    cv_rect(178, 30, 1, 150, C_PANEL2);
    ctext(88, 190, &AF_X, "LOWER", C_DIM, C_PANEL);
    ctext(210, 190, &AF_X, "PEDAL", C_DIM, C_PANEL);
    ctext(88, 205, &AF_N, "008 880 000", C_CREAM, C_PANEL);
    ctext(210, 205, &AF_N, "63", C_CREAM, C_PANEL);
    footer("SELECT: MOVE THE FOUR   ALGO: PEDAL");
}

/* ------------------------------------------------------------- output --- */
static void render(void (*scene)(void))
{
    uint32_t s;
    for (s = 0; s < 2u; s++) {                               /* two strips of 120 rows, as the firmware draws */
        cv_begin(240, 120, C_PANEL);
        cv_oy = -(int32_t)(120u * s);
        scene();
        cv_oy = 0;
        cv_blit(0, 120u * s);
    }
}
static void save(const char *name)
{
    char p[256];
    uint32_t i;
    FILE *f;
    snprintf(p, sizeof p, "%s/mock_%s.ppm", dir, name);
    f = fopen(p, "wb");
    if (!f) { perror(p); exit(1); }
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = swap16(host_screen[i]);
        uint8_t b[3] = {(uint8_t)((c >> 11) * 255u / 31u), (uint8_t)(((c >> 5) & 63u) * 255u / 63u), (uint8_t)((c & 31u) * 255u / 31u)};
        fwrite(b, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*f)(void); } S[] = {
        {"1_home", scr_home}, {"2_drawbars", scr_drawbars}, {"3_percvib", scr_percvib}, {"4_rotary", scr_rotary},
        {"5_rotary_speeds", scr_rotary2}, {"6_drive", scr_drive}, {"7_voice", scr_voice}, {"8_lower_pedal", scr_lower}};
    uint32_t i;
    dir = argc > 1 ? argv[1] : ".";
    for (i = 0; i < sizeof S / sizeof S[0]; i++) {
        render(S[i].f);
        save(S[i].name);
    }
    printf("%u mock-ups in %s\n", (unsigned)(sizeof S / sizeof S[0]), dir);
    return 0;
}
