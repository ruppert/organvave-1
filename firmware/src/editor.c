/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Guilherme Ruppert (FMini-1, organvave-1) */
/* Editor protocol: SysEx for the web editor (web/EDITOR_PROTOCOL.md), the commands the instrument's editor uses
 * (web/editor_fmini.html): INFO GET SET DUMP DESC PRESET NAMES, the user presets (UP_LIST GET STORE LOAD ERASE),
 * live sync (WATCH, the CHANGED / RELOAD pushes, PING), TRACK, and the FMUG patch (82..84, editor_fmug.c).
 * Felucca's other commands (steps, projects, samples, the mixer, songs, motion, backups, menus, palettes, MIDI LEARN,
 * FM6) are gone: a frame of one gets no reply.
 *   F0 7D 46 4C cmd args.. F7     (7D = non-commercial ID, "FL")
 * Values are 14 bit, two 7-bit bytes LSB first, offset by 8192 (so -8192..8191).
 * Every request gets a reply with the same cmd; CHANGED / RELOAD are also pushed while watched. Frames arrive through
 * sx_frame (usb.c), replies leave through ota_wire_send(). */
#define ED_HDR0 0x7D
#define ED_HDR1 0x46
#define ED_HDR2 0x4C
enum { ED_INFO = 1, ED_GET, ED_SET, ED_DUMP, ED_DESC, ED_PRESET = 8, ED_NAMES = 10,
       ED_UP_LIST = 16, ED_UP_GET, ED_UP_STORE = 19, ED_UP_LOAD, ED_UP_ERASE,   /* v2: user presets */
       ED_WATCH, ED_CHANGED, ED_RELOAD, ED_PING,                              /* v2: live sync */
       ED_TRACK = 27 };                                                       /* v3 */

static uint8_t ed_out[8u + 2u * (P_COUNT + G_COUNT)] __attribute__((section(".pool")));   /* (the longest reply: DUMP,
                                                                    * 8 + 2 x (P_COUNT + G_COUNT) bytes; UP_GET is
                                                                    * 278, DESC at most 249) */
static uint32_t ed_n;

static void ed_begin(uint32_t cmd)
{
    ed_out[0] = 0xF0;
    ed_out[1] = ED_HDR0;
    ed_out[2] = ED_HDR1;
    ed_out[3] = ED_HDR2;
    ed_out[4] = (uint8_t)cmd;
    ed_n = 5;
}
static void ed_b(uint32_t v)
{
    if (ed_n < sizeof ed_out - 1u)
        ed_out[ed_n++] = (uint8_t)(v & 0x7Fu);
}
static void ed_v(int32_t v)
{
    uint32_t u = (uint32_t)(clamp(v, -8192, 8191) + 8192);
    ed_b(u);
    ed_b(u >> 7);
}
static void ed_str(const char *s, uint32_t max)   /* ASCII, 0-terminated */
{
    uint32_t i;
    for (i = 0; s && s[i] && i < max; i++)
        ed_b((uint8_t)s[i] & 0x7Fu);
    ed_b(0);
}
static void ed_send(void)
{
    ed_out[ed_n++] = 0xF7;
    ota_wire_send(ed_out, ed_n);
}
static int32_t ed_rv(const uint8_t *p) { return (int32_t)(p[0] | p[1] << 7) - 8192; }

/* the engine byte of DUMP / RELOAD / TRACK */
static uint32_t ed_eng(const track_t *t) { return t->eng_req % NENGINES; }

/* ---- live sync (v2): while the editor WATCHes, device-side changes are pushed. A shadow of the track's p[] and
 * song.g[] is kept in step with what the editor knows (its own SET updates it); the main loop compares and pushes
 * CHANGED, or RELOAD after a load. Pushes go out only into a half-empty SysEx ring, so they never wait */
#define ED_PUSH_MAX 4u                                   /* frames per pass */
#define ED_NV (P_COUNT + G_COUNT)
static struct {
    uint8_t on, eng, preset;
    uint32_t pos, last_ms, run_ms, resets;
    int16_t v[ED_NV];                                    /* TSEL->p[], then song.g[] */
    uint16_t t[ED_NV];                                   /* ms (low 16 bits) of the last push */
} ed_w;

static int16_t *ed_val(uint32_t i) { return i < P_COUNT ? &TSEL->p[i] : &song.g[i - P_COUNT]; }
static void ed_shadow(void)                              /* the editor is in sync */
{
    uint32_t i;
    for (i = 0; i < ED_NV; i++)
        ed_w.v[i] = *ed_val(i);
    ed_w.eng = (uint8_t)ed_eng(TSEL);
    ed_w.preset = TSEL->preset;
    sync_reload = 0;
}
/* the editor's own sound load (PRESET, SET of G_ENGSEL): the editor re-reads DUMP after the reply, so the shadow takes
 * the load and no RELOAD echoes back (INFO 53 01 bit 1). A RELOAD already due before it still goes out */
typedef struct { int16_t p[P_COUNT]; uint8_t eng, preset, due; } ed_load_t;
static void ed_load_before(ed_load_t *b)
{
    memcpy(b->p, TSEL->p, sizeof b->p);
    b->eng = (uint8_t)ed_eng(TSEL);
    b->preset = TSEL->preset;
    b->due = sync_reload || b->eng != ed_w.eng || b->preset != ed_w.preset;
}
static void ed_load_after(const ed_load_t *b)
{
    uint32_t i;
    if (!ed_w.on || b->due)
        return;
    sync_reload = 0;
    ed_w.eng = (uint8_t)ed_eng(TSEL);
    ed_w.preset = TSEL->preset;
    for (i = 0; i < P_COUNT; i++)                        /* the load's values; a pending push of another stays */
        if (TSEL->p[i] != b->p[i])
            ed_w.v[i] = TSEL->p[i];
}
static int ed_room(void) { return so_w - so_r + 8u <= SXQ / 2u; }

static void ed_sync(void)                                /* main loop */
{
    uint32_t i, n = 0, now = fm1_ms;
    if (!ed_w.on || now - ed_w.run_ms < 5u)
        return;
    ed_w.run_ms = now;
    if (!usb.config || usb.resets != ed_w.resets || now - ed_w.last_ms > 3000u) {
        ed_w.on = 0;                                     /* no host, USB reset, or 3 s without a request */
        return;
    }
    if (sync_reload || ed_eng(TSEL) != ed_w.eng || TSEL->preset != ed_w.preset) {
        if (!ed_room())
            return;
        ed_shadow();
        ed_begin(ED_RELOAD);
        ed_b(ed_eng(TSEL));
        ed_b(TSEL->preset);
        ed_b(song.sel);
        ed_send();
        return;
    }
    for (i = 0; i < ED_NV && n < ED_PUSH_MAX; i++) {    /* round robin: nothing starves */
        uint32_t k = (ed_w.pos + i) % ED_NV;
        int16_t v = *ed_val(k);
        if (v == ed_w.v[k] || (uint16_t)(now - ed_w.t[k]) < 20u)
            continue;                                    /* coalesced: the latest value goes out later */
        if (!ed_room())
            return;
        ed_w.v[k] = v;
        ed_w.t[k] = (uint16_t)now;
        ed_begin(ED_CHANGED);
        ed_b(k < P_COUNT ? 0u : 1u);
        ed_b(k < P_COUNT ? k : k - P_COUNT);
        ed_v(v);
        ed_send();
        n++;
        ed_w.pos = k + 1u;
    }
}

/* descriptor and value slot of (scope, id): scope 0 = the track (the engine it asked for), 1 = global */
static const param_desc_t *ed_desc(uint32_t scope, uint32_t id, int16_t **vp)
{
    if (scope == 0 && id < P_COUNT) {
        *vp = &TSEL->p[id];
        return param_desc_of(TSEL->eng_req % NENGINES, id);
    }
    if (scope == 1 && id < G_COUNT) {
        *vp = &song.g[id];
        return &GP[id];
    }
    return 0;
}

/* a flash write from the editor while the transport runs: stop it first (the erase silences the audio and stalls the
 * sequencer; the device's own SAVE refuses with "STOP TO SAVE", the editor's confirmed save stops) */
static int ed_flash_stop(void)
{
    uint32_t t0 = fm1_ms;
    if (!transport_busy())
        return 0;
    transport_req = 2;
    while (song.playing && fm1_ms - t0 < 100u)          /* the audio ISR stops it at its next block */
        fm1_wdt_feed();
    return song.playing;
}

#include "editor_fmug.c"                     /* FMini-1: the FMUG patch (cmds 82, 83, 84) */

static int ed_args_ok(uint32_t cmd, uint32_t n)
{
    switch (cmd) {
    case ED_INFO: case ED_DUMP: case ED_PING:
        return !n;
    case ED_GET: case ED_DESC: case ED_PRESET: case ED_UP_LIST:
        return n == 2u;
    case ED_SET:
        return n == 4u;
    case ED_NAMES: case ED_UP_GET: case ED_UP_LOAD: case ED_UP_ERASE: case ED_WATCH:
        return n == 1u;
    case ED_TRACK:
        return n <= 1u;
    default:
        return 1;                                  /* variable payloads validate in their handler */
    }
}

static void ed_handle(const uint8_t *f, uint32_t n)   /* f: the bytes between F0 and F7 */
{
    if (n < 4u)
        return;
    uint32_t cmd = f[3], i;
    const uint8_t *a = f + 4;
    uint32_t na = n - 4u;
    int16_t *vp;
    const param_desc_t *d;
    if (!ed_args_ok(cmd, na))
        return;
    ed_begin(cmd);
    if (ed_fmug_handle(cmd, a, na)) { ed_send(); return; }
    switch (cmd) {
    case ED_INFO:
        ed_str("FELUCCA " FELUCCA_VERSION, 24);             /* (the editors look for "FELUCCA") */
        ed_b(NENGINES);
        ed_b(P_COUNT);
        ed_b(G_COUNT);
        ed_b(NSTEP);
        ed_b(P_E0);
        for (i = 0; i < NENGINES; i++)
            ed_str(ENGINES[i]->name, 8);
        ed_b(NTRK);                                       /* v3 */
        ed_b(0x53); ed_b(1); ed_b(3);   /* live sync: bit 0 WATCH while on keeps the shadow, bit 1 no RELOAD echo */
        ed_b(0x47); ed_b(1); ed_b(FG_SIZE);   /* FMini-1: the FMUG patch, FG_SIZE bytes (cmds 82, 83; editor_fmug.c) */
        ed_b(0x48); ed_b(1); ed_b(1);   /* FMini-1 1.6.1: the octave on 82 / 83, and cmd 84 (what a user preset keeps besides the values) */
        break;
    case ED_GET:
    case ED_SET:
        if (!(d = ed_desc(a[0], a[1], &vp)))
            return;
        if (cmd == ED_SET) {
            if (a[0] == 1 && a[1] == G_ENGSEL) {          /* engine change: the safe path */
                ed_load_t b;
                ed_load_before(&b);
                set_engine_of(TSEL, (uint32_t)clamp(ed_rv(a + 2), 0, NENGINES - 1));
                ed_load_after(&b);
            } else if (d->max > d->min) {
                *vp = (int16_t)clamp(ed_rv(a + 2), d->min, d->max);
            }
            ui.force = 1;
            ed_w.v[a[0] ? P_COUNT + a[1] : a[1]] = *vp;   /* the editor's own change: no push */
        }
        ed_b(a[0]);
        ed_b(a[1]);
        ed_v(*vp);
        break;
    case ED_DUMP:
        for (i = 0; i < ED_NV; i++)                       /* the editor gets them all here */
            ed_w.v[i] = *ed_val(i);
        ed_b(ed_eng(TSEL));
        ed_b(TSEL->preset);
        for (i = 0; i < P_COUNT; i++)
            ed_v(TSEL->p[i]);
        for (i = 0; i < G_COUNT; i++)
            ed_v(song.g[i]);
        break;
    case ED_DESC:
        if (!(d = ed_desc(a[0], a[1], &vp)))
            return;
        ed_b(a[0]);
        ed_b(a[1]);
        ed_b(d->fmt);
        ed_v(d->min);
        ed_v(d->max);
        ed_v(d->def);
        ed_str(d->label, 8);
        ed_str(d->unit, 8);
        if (d->fmt == F_ENUM && d->names)                  /* up to 24 */
            for (i = 0; i <= (uint32_t)(d->max - d->min) && i < 24u; i++)
                ed_str(d->names[i], 8);
        break;
    case ED_PRESET: {                                      /* engine, preset */
        ed_load_t b;
        if (!eng_ok(a[0]))
            return;
        ed_load_before(&b);
        if (a[0] != TSEL->eng_req)
            set_engine_of(TSEL, a[0]);
        apply_preset(a[1]);
        ed_load_after(&b);
        ui.force = 1;
        ed_b(ed_eng(TSEL));
        ed_b(TSEL->preset);
        break;
    }
    case ED_NAMES:                                         /* preset names of an engine */
        if (a[0] >= NENGINES)
            return;
        ed_b(a[0]);
        ed_b(ENGINES[a[0]]->npresets);
        for (i = 0; i < ENGINES[a[0]]->npresets; i++)
            ed_str(ENGINES[a[0]]->presets[i].name, 12);
        for (i = 0; i < 2u; i++)                           /* then the two edit-page titles */
            ed_str(ENGINES[a[0]]->page_title[i], 8);
        break;
    case ED_UP_LIST: {                                     /* start, count -> start, count, total, per slot: used, engine, name;
                                                            * then per slot its category (0: none kept here) */
        uint32_t s0 = a[0], cnt = a[1] > 16u ? 16u : a[1];
        if (s0 >= UP_SLOTS)
            cnt = 0;
        else if (s0 + cnt > UP_SLOTS)
            cnt = UP_SLOTS - s0;
        ed_b(s0);
        ed_b(cnt);
        ed_b(UP_SLOTS);
        for (i = s0; i < s0 + cnt; i++) {
            char nm[13] = {0};
            uint32_t j, u = (uint32_t)up_used(i);
            for (j = 0; u && j < 12u; j++)
                nm[j] = up_rec(i)->name[j];
            ed_b(u);
            ed_b(u ? up_rec(i)->engine : 0u);
            ed_str(nm, 12);
        }
        for (i = s0; i < s0 + cnt; i++)
            ed_b(up_used(i) ? up_cat(i) : 0u);
        break;
    }
    case ED_UP_GET: {                                      /* slot -> slot, used, engine, name, P_COUNT x v14, 16 x (note,
                                                            * flags), kind (0), category */
        int16_t v[P_COUNT];
        char nm[13] = {0};
        const up_rec_t *r;
        uint32_t u;
        if (a[0] >= UP_SLOTS)
            return;
        r = up_rec(a[0]);
        u = (uint32_t)up_used(a[0]);
        if (u)
            up_values(r, v);                               /* today's P_* order, clamped */
        for (i = 0; u && i < 12u; i++)
            nm[i] = r->name[i];
        ed_b(a[0]);
        ed_b(u);
        ed_b(u ? r->engine : 0u);
        ed_str(nm, 12);
        for (i = 0; i < P_COUNT; i++)
            ed_v(u ? v[i] : 0);
        for (i = 0; i < 16u; i++) {
            ed_b(u ? r->note[i] & 0x7Fu : 0u);
            ed_b(u ? r->flags[i] & 0x7Fu : 0u);
        }
        ed_b(0);                                           /* kind: a note pattern (never a drum grid here) */
        ed_b(u ? up_cat(a[0]) : 0u);
        break;
    }
    case ED_UP_STORE: {                                    /* slot, name -> slot, rc */
        char nm[13] = {0};
        uint32_t n0, rc = 1;
        if (na < 1u)
            return;
        for (n0 = 0; 1u + n0 < na && a[1 + n0] && n0 < 13u; n0++)
            ;
        if (a[0] < UP_SLOTS && (!n0 || up_name_ok(a + 1, n0)) &&
            (1u + n0 == na || (2u + n0 == na && !a[1u + n0]))) {   /* "" = automatic name */
            for (i = 0; i < n0; i++)
                nm[i] = (char)a[1 + i];
            rc = ed_flash_stop() || up_store(a[0], nm) ? 2u : 0u;
        }
        ed_b(a[0]);
        ed_b(rc);
        break;
    }
    case ED_UP_LOAD:                                       /* slot -> slot, rc */
        ed_b(a[0]);
        ed_b(a[0] < UP_SLOTS && !up_load(a[0]) ? 0u : 1u);
        break;
    case ED_UP_ERASE:
        ed_b(a[0]);
        ed_b(a[0] >= UP_SLOTS ? 1u : ed_flash_stop() || up_put(a[0], 0) ? 2u : 0u);
        break;
    case ED_WATCH: {                                       /* on -> on */
        uint32_t keep = ed_w.on && ed_w.resets == usb.resets && (a[0] & 1u);   /* already watching: the changes not
                                                                               * yet pushed stay pending (INFO 53 01 bit 0) */
        ed_w.on = a[0] & 1u;
        ed_w.resets = usb.resets;
        if (ed_w.on && !keep)
            ed_shadow();
        ed_b(ed_w.on);
        break;
    }
    case ED_PING:
        ed_b(0);
        break;
    case ED_TRACK:                                         /* [track] -> selected, NTRK, per track: engine, preset, level, mute, armed */
        ed_b(song.sel);
        ed_b(NTRK);
        for (i = 0; i < NTRK; i++) {
            ed_b(ed_eng(&trk[i]));
            ed_b(trk[i].preset);
            ed_v(trk[i].p[P_LEVEL]);
            ed_b(trk[i].p[P_MUTE] != 0);
            ed_b(0);
        }
        break;
    default:
        return;
    }
    ed_send();
}

/* main loop: editor frames first; anything else stays for ota_service() */
static void ed_service(void)
{
    const uint8_t *p;
    uint32_t n;
    ed_sync();                                             /* v2 pushes (while watched) */
    if (!ota_frame_get(&p, &n) || n < 4u || p[0] != ED_HDR0 || p[1] != ED_HDR1 || p[2] != ED_HDR2)
        return;
    ed_w.last_ms = fm1_ms;                                 /* any request keeps WATCH alive */
    {
        static uint8_t f[64];                              /* (the longest request: FMUG_PUT, 39 bytes) */
        uint32_t i;
        for (i = 0; i < n && i < sizeof f; i++)
            f[i] = p[i];
        ota_frame_done();                                  /* free the frame buffer before replying */
        if (n <= sizeof f)                                 /* longer: no request of ours, dropped whole */
            ed_handle(f, n);
    }
}
