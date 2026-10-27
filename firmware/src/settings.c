/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Guilherme Ruppert (FMini-1, organvave-1) */
/* The settings and the autosave, kept in flash (storage.c).
 *
 * Settings (OBJ_SETTINGS): the panel's calibration (panel.c), the speaker EQ (MENU's LOW CUT, fx_lowcut), the LEDs
 * (settings_leds) and a block of bytes the UI keeps its own settings in (set_bytes: organ_app.c's GLO, row 7).
 * The record is FMini-1's and Felucca's ("PER4", persist_t), so FMini-1's calibration and GLO settings carry over; the
 * fields this firmware does not use are written back as they were read.
 *
 * The autosave (OBJ_AUTOSAVE, magic "OVA2"): the instrument as it is (a user preset record: the part's values, the
 * octave, the FX settings; and the organ patch, the preset it came from). Since 0.9.9 it is written but not put back at
 * power-on: the instrument starts from the first factory preset. A flash erase stops the audio for its ~40 ms, so a write only
 * when the record changed and has stayed the same for AS_IDLE_MS with the transport stopped, nothing sounding and no
 * key or button down, and at least AS_GAP_MS after the one before (AS_RETRY_MS after a failed write). The settings are
 * written under the same quiet rule. FMini-1's autosave and 0.0.x's ("OVA1", an FMUG patch) read as none. */
typedef struct {
    uint32_t magic, palette, lowcut, zoom;     /* zoom: LEDS (panel.c leds_*_stored) */
    panel_t panel;
    uint32_t bold;
    struct { uint8_t factory[16][32]; uint32_t user, filter; } bytes;   /* (Felucca's favourites; set_bytes) */
} persist_t;
#define PERSIST_MAGIC 0x50455234u              /* "PER4" */
static persist_t persist_saved __attribute__((section(".pool")));   /* what flash holds (no write when unchanged) */
#define set_bytes (persist_saved.bytes.factory) /* the UI's bytes (organ_app.c oa_cfg_byte), saved with the rest */
static uint8_t persist_pending;                /* 1 asked for, 2 waiting after a flash error */
static uint32_t persist_retry_ms;
static persist_t persist_flash __attribute__((section(".pool")));   /* the copy flash holds */

/* power-on, before panel_init: the flash probe, the settings, the user presets */
static void persist_boot(void)
{
    persist_t p;
    uint32_t f = irq_save();
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;       /* the expected 1 MiB part, else stay RAM-only */
    irq_restore(f);
    persist_saved.magic = PERSIST_MAGIC;
    if (flash_ok && st_load(OBJ_SETTINGS, &p, sizeof p) == (int)sizeof p && p.magic == PERSIST_MAGIC) {
        persist_saved = persist_flash = p;
        if (p.panel.magic == PANEL_MAGIC)
            panel = p.panel;
        fx_lowcut = (uint8_t)(p.lowcut % 3u);
        settings_leds = (uint8_t)leds_from_stored(p.zoom);
    }
    up_boot();                                     /* user presets */
}

/* nothing going on that an erase (the audio stopped ~40 ms) would be heard in, or would race */
static int persist_quiet(void)
{
    uint32_t i;
    if (transport_busy() || fm1_in.notes || fm1_in.buttons)
        return 0;
    for (i = 0; i < NVOICE; i++)
        if (trk[0].v[i].active)
            return 0;
    return !org.nacl;                              /* (the organ: no wheel sounding) */
}

static void settings_poll(void)                /* main loop, every pass */
{
    persist_t *p = &persist_saved;
    if (!persist_pending || !flash_ok || !persist_quiet() ||
        (persist_pending == 2u && (uint32_t)(fm1_ms - persist_retry_ms) < 1000u))
        return;
    p->magic = PERSIST_MAGIC;
    p->lowcut = fx_lowcut;
    p->zoom = leds_to_stored(persist_flash.zoom, settings_leds);
    p->panel = panel;
    if (!memcmp(p, &persist_flash, sizeof *p)) {
        persist_pending = 0;
        return;                                    /* unchanged: no erase cycle */
    }
    if (st_save(OBJ_SETTINGS, p, sizeof *p) == 0) {
        persist_flash = *p;
        persist_pending = 0;
    } else {
        persist_pending = 2;
        persist_retry_ms = fm1_ms;
    }
}

static void settings_save(void)                /* written when nothing sounds (settings_poll) */
{
    persist_pending = 1;
    settings_poll();
}

static void settings_init(void)                /* after persist_boot and panel_init */
{
    palette_set(UI_GREY_INDEX);
}

/* ------------------------------------------------------------ autosave --- */
#define AS_MAGIC 0x3241564Fu                   /* "OVA2" (0.1.0: the organ patch) */
#define AS_POLL_MS 250u
#define AS_IDLE_MS 10000u
#define AS_GAP_MS 60000u
#define AS_RETRY_MS 30000u
#define AS_PATCH0 64u                          /* the patch bytes the first autosave records held */
_Static_assert(ORG_PSIZE >= AS_PATCH0, "the patch grows");
typedef struct {
    uint32_t magic;
    up_rec_t r;                                /* the part's values and extras (upreset.c up_capture) */
    uint8_t patch[AS_PATCH0];                  /* the organ patch (eng_organ.c OP_*): its first AS_PATCH0 bytes (the 0.9.3 record ends after rsv) */
    uint8_t preset, user, rsv[2];              /* the factory preset, the user preset + 1 it came from; spare */
    uint8_t patch_ext[ORG_PSIZE - AS_PATCH0];  /* 0.9.4: the rest of the patch (ADVANCED EDIT); an older record has none, the bytes read as 0 (the defaults) */
} as_rec_t;
typedef struct {                               /* the 0.9.3 record (ORG_PSIZE 64), whose length an old autosave still has */
    uint32_t magic;
    up_rec_t r;
    uint8_t patch[AS_PATCH0];
    uint8_t preset, user, rsv[2];
} as_rec0_t;
#define AS_OLD_SIZE sizeof(as_rec0_t)
_Static_assert(sizeof(as_rec_t) <= ST_PAYLOAD_MAX, "the autosave in one flash sector");
static struct {
    uint32_t saved, seen;                      /* the signature written last (or restored), seen last */
    uint32_t t, last, poll;                    /* fm1_ms: seen changed / busy, the last write, the last check */
    uint8_t wrote, err;                        /* a write since power-on (last is valid); the last one failed */
} as;

static void as_capture(as_rec_t *a)
{
    memset(a, 0, sizeof *a);
    a->magic = AS_MAGIC;
    up_capture(&a->r, 0, "AUTOSAVE");
    memcpy(a->patch, org_patch, AS_PATCH0);
    memcpy(a->patch_ext, org_patch + AS_PATCH0, ORG_PSIZE - AS_PATCH0);
    a->preset = TSEL->preset;
    a->user = TSEL->user;
}

static uint32_t autosave_sig(void)             /* FNV-1a of what the record holds */
{
    as_rec_t a;
    uint32_t h = 2166136261u, i;
    const uint8_t *b = (const uint8_t *)&a;
    as_capture(&a);
    for (i = 0; i < sizeof a; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

/* power-on (0.9.9): the instrument always starts from the first factory preset (JAZZ B3), not from the last session's: the
 * autosave is not restored. It goes on being written (autosave_poll), and its baseline is set here so that the boot is not
 * saved as a change. Returns 0 (nothing restored) */
static int autosave_boot(int allowed)
{
    (void)allowed;
    as.saved = as.seen = autosave_sig();
    as.t = fm1_ms;
    return 0;
}

static void autosave_poll(void)                /* main loop, every pass (after settings_poll) */
{
    as_rec_t a;
    uint32_t sig;
    if (!flash_ok || fm1_ms - as.poll < AS_POLL_MS)
        return;
    as.poll = fm1_ms;
    sig = autosave_sig();
    if (sig != as.seen || !persist_quiet()) {
        as.seen = sig;
        as.t = fm1_ms;
        return;
    }
    if (sig == as.saved || fm1_ms - as.t < AS_IDLE_MS ||
        (as.wrote && fm1_ms - as.last < (as.err ? AS_RETRY_MS : AS_GAP_MS)))
        return;
    as.last = fm1_ms;
    as.wrote = 1;
    as_capture(&a);
    as.err = st_save(OBJ_AUTOSAVE, &a, sizeof a) != 0;
    if (!as.err)
        as.saved = sig;
}
