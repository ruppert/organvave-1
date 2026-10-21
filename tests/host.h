/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Guilherme Ruppert (FMini-1, organvave-1) */
/* The firmware on the host: the same sources as firmware/src/felucca.c, in its order, with the HAL stubbed (a screen
 * in RAM, the keys and knobs from the test, a 1 MiB RAM flash behind storage.c's hooks). The tests include this, then
 * what they test on top (organ_app.c, editor.c, audio.c ...).
 *   HOST_NO_UI     only the sound and the sequencer (the DSP tests: no screen, no stores)
 *   HOST_FLASH_OFF the flash part not found (flash_ok 0: everything in RAM only)
 *   HOST_TICKS     the test defines fm1_ticks and FM1_TICKS_PER_US */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/resource.h>
#endif
#define __attribute__(x)                                /* (after the system headers: theirs need it) */
#define memset felucca_memset
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "felucca_tables.h"
#include "../firmware/src/libc.c"
#undef memset
#undef memcpy
#undef memcmp
static struct { volatile uint32_t notes, buttons; } fm1_in;

/* ------------------------------------------------------------ HAL stubs --- */
#define FM1_NCOL 11u
static const int8_t FM1_KEYMAP[6][FM1_NCOL];           /* (zero: led_put finds no LED; the LED tests map their own) */
static uint8_t fm1_led[FM1_NCOL], fm1_led_dim[FM1_NCOL], fm1_led_breath[FM1_NCOL], fm1_led_mid[FM1_NCOL];
static uint32_t host_dim_lo;
static void fm1_led_dim_level(uint32_t lo) { host_dim_lo = lo; }
static int host_anim, host_idle, host_idle_glow;
static int fm1_led_anim_on(void) { return host_anim || host_idle; }
static void fm1_led_idle_start(uint32_t glow) { host_idle = 1; host_idle_glow = (int)glow; }
static int fm1_led_idle_on(void) { return host_idle; }
static void fm1_led_anim_stop(void) { host_anim = host_idle = 0; }
static uint32_t host_ticks, host_pressed, host_notes;
static int32_t host_enc[7];
#ifndef HOST_TICKS                                      /* (a test with its own clock: audio_test.c) */
#define FM1_TICKS_PER_US 1u
static uint32_t fm1_ticks(void) { return host_ticks; }
#endif
static uint32_t fm1_input_edges(int x) { uint32_t p = host_pressed; (void)x; host_pressed = 0; return p; }
static uint32_t fm1_input_note_edges(void) { uint32_t n = host_notes; host_notes = 0; return n; }
static int32_t fm1_enc_take(uint32_t e)
{
    int32_t s = host_enc[e % 7u];
    host_enc[e % 7u] = 0;
    return s;
}
static void (*host_wdt_hook)(void);                     /* (a test's: time passing in a blocking loop) */
static void fm1_wdt_feed(void) { if (host_wdt_hook) host_wdt_hook(); }
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static void fm1_delay_ms(uint32_t ms) { (void)ms; }

/* the screen: RGB565 big-endian, as the panel takes it */
static uint16_t host_screen[240 * 240];
static uint32_t host_blit_rows;                         /* rows blitted (what a frame draws) */
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++)
            host_screen[(y + j) * 240u + x + i] = (uint16_t)((c >> 8) | (c << 8));
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{
    uint32_t i, j;
    host_blit_rows += h;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++)
            host_screen[(y + j) * 240u + x + i] = p[j * w + i];
}
static void lcd_sync(void) {}

/* ------------------------------------------------------- the firmware --- */
#include "../firmware/src/gfx.c"
#include "../firmware/src/core.h"
#include "../firmware/src/engines.c"
#include "../firmware/src/params.c"
#include "../firmware/src/voice.c"
#include "../firmware/src/fx.c"
#include "../firmware/src/usb.c"
#include "../firmware/src/midi_uart.c"                  /* TRS MIDI IN: its parser (um_byte) feeds midi_in_q */
#include "../firmware/src/seq.c"
#ifndef HOST_NO_UI
#include "../firmware/src/panel.c"

/* the flash: 1 MiB of RAM behind storage.c's hooks (storage_hw.c is the chip's) */
static uint8_t host_nor[0x100000];
static uint32_t host_erases, host_progs, host_fail_erase;
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    if (off + n > sizeof host_nor)
        return -1;
    memcpy(dst, host_nor + off, n);
    return 0;
}
static int st_erase(uint32_t off)
{
    if (host_fail_erase || off + 4096u > sizeof host_nor)
        return -1;
    memset(host_nor + off, 0xFF, 4096);
    host_erases++;
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (off + n > sizeof host_nor)
        return -1;
    for (i = 0; i < n; i++)
        host_nor[off + i] &= ((const uint8_t *)src)[i];   /* NOR: bits only go from 1 to 0 */
    host_progs++;
    return 0;
}
static uint8_t flash_ok;
#ifdef HOST_FLASH_OFF
static uint32_t fl_jedec_ram(void) { return 0; }
#else
static uint32_t fl_jedec_ram(void) { return 0x856014u; }
#endif
#define FL_FAR(fn) (fn)
#include "../firmware/src/storage.c"
#include "../firmware/src/ui.c"
#include "../firmware/src/upreset.c"
#include "../firmware/src/settings.c"
#endif

/* ------------------------------------------------------------ helpers --- */
static int host_check(const char *what, int ok)
{
    printf("%-84s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static void wav_hdr(FILE *f, uint32_t frames)
{
    uint32_t v;
    uint16_t a = 1, ch = 2, ba = 4, bits = 16;
    uint32_t sr = FS, br = FS * 4;
    fwrite("RIFF", 1, 4, f); v = 36 + frames * 4; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    fwrite(&a, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); v = frames * 4; fwrite(&v, 4, 1, f);
}
static void wav_put(FILE *f, int32_t l, int32_t r)
{
    int16_t s[2] = {(int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l),
                    (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r)};
    fwrite(s, 2, 2, f);
}

/* the power-on values (main.c felucca_init, before the factory patch): the globals, the track's defaults, empty steps */
static void host_tracks_init(void)
{
    uint32_t i, k;
    for (i = 0; i < G_COUNT; i++)
        song.g[i] = GP[i].def;
    for (k = 0; k < NTRK; k++) {
        for (i = 0; i < P_E0; i++)
            trk[k].p[i] = TP[i].def;
        for (i = 0; i < NSTEP; i++)
            trk[k].step[i].time = ST_REST;
    }
    song.master_q12 = 4096;
}

/* engine e's preset pi into track t as a preset load sets the sound (the sends too), at once (no block between) */
static void host_preset(track_t *t, uint32_t e, uint32_t pi)
{
    static const uint8_t FX_DEF[4] = {0, 24, 28, 36};
    const preset_t *p;
    uint32_t i;
    t->eng_req = t->engine = (uint8_t)e;
    if (!ENGINES[e]->npresets)
        return;
    pi %= ENGINES[e]->npresets;
    p = &ENGINES[e]->presets[pi];
    t->preset = (uint8_t)pi;
    for (i = 0; i < 8u; i++)
        t->p[P_E0 + i] = p->e[i];
    t->p[P_ATK] = p->env[0];
    t->p[P_DEC] = p->env[1];
    t->p[P_SUS] = p->env[2];
    t->p[P_REL] = p->env[3];
    t->p[P_ED_FLT] = p->fenv;
    t->p[P_VOICE] = p->mono ? V_LEGATO : V_POLY;
    for (i = 0; i < 4u; i++)
        t->p[P_DIST + i] = (int16_t)(p->fx[i] ? p->fx[i] - 1 : FX_DEF[i]);
    if (e == ENGI_ORGAN) {
        org_set_patch(ORG_FACTORY[pi % ORG_NFACTORY]);
        t->p[P_LEVEL] = ORG_LEVEL[pi % ORG_NFACTORY];
    }
}

static void put_step(track_t *t, uint32_t i, uint32_t n, const uint8_t *notes, uint32_t time, uint32_t flags)
{
    step_t *s = &t->step[i];
    uint32_t k;
    s->n = (uint8_t)n;
    for (k = 0; k < 4u; k++)
        s->note[k] = k < n ? notes[k] : 0;
    s->time = (uint8_t)time;
    s->flags = (uint8_t)flags;
    s->vel = n ? 100 : 0;
}

static uint32_t busy_now(void)                          /* sounding voices */
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += trk[0].v[i].active;
    return n;
}
