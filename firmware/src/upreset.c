/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Guilherme Ruppert (FMini-1, organvave-1) */
/* User presets (the SAVE dialog, the PRESETS list after the factory patches; editor protocol cmds 16..21): 32 slots
 * in two storage.c objects (OBJ_UPRESET0/1, A/B sectors at 0xDC000..0xDFFFF), 16 records each, mirrored in RAM so
 * browsing never reads flash. The format is FMini-1's (and Felucca's before it), so presets saved there read here.
 *
 * A record: engine, name, the instrument's parameters, a 16-step pattern (note 0 = rest; flags 1 accent, 2 slide, 4
 * tie), and in the spare bytes at the end of the values the tempo, the octave and the FX pop-ups' settings (UP_*_AT).
 * A record keeps np = the P_COUNT it was stored with: its last 8 values are P_E0..P_E7, the first np - 8 are P_LEVEL..
 * in order, and parameters it does not have take their defaults (common parameters are only ever added just before
 * P_E0). Version 4 (today's) holds each value in a byte (value + 64); versions 1..3 (Felucca's of before) an int16
 * each; version 5 is a drum grid (Felucca's DRUM engine: it reads, its grid is no pattern here).
 *
 * The organ patch of a preset (eng_organ.c OP_*) is kept beside the records, in one more object (OBJ_UPFM6, "UPF6":
 * one entry per slot, tagged with a hash of its record without the name, so a rename keeps it and any other change of
 * the record leaves the record without one: it then loads the first factory patch). FMini-1's entries (FMUG
 * patches, of records of engine 14) are never read: those records load as the organ with the first factory patch. */
#define UP_PER_BANK 16u
#define UP_PMAX 72u                              /* room for P_COUNT to grow */
#define UP_USED 0xA5u
#define UP_VER 4u
#define UP_VER_GRID 5u
#define UP_BANK_MAGIC 0x31425055u                /* "UPB1" */
typedef struct {
    uint8_t used, ver, engine, np;               /* UP_USED, UP_VER, engine, P_COUNT when stored */
    char name[12];                               /* ASCII 32..126, 0-padded (no 0 when 12 long) */
    union { int16_t p[UP_PMAX]; uint8_t packed[UP_PMAX * 2u]; };
    uint8_t note[16], flags[16];                 /* note 0 = rest; flags 1 accent, 2 slide, 4 tie */
} up_rec_t;
typedef struct {
    uint32_t magic;
    uint16_t rsize, nslot;
    up_rec_t r[UP_PER_BANK];
} up_bank_t;
_Static_assert(sizeof(up_rec_t) == 192, "user preset record layout");
_Static_assert(P_COUNT <= UP_PMAX * 2u && P_COUNT < 128, "user preset record: P_COUNT");
static up_bank_t up_bank[UP_SLOTS / UP_PER_BANK] __attribute__((section(".pool")));   /* (main loop only) */
static uint8_t up_gen;                           /* +1 after every change (the UI's lists redraw) */

/* the extras in the spare bytes after the values (version 4: np = P_COUNT < UP_FX2_AT); 0 = none */
#define UP_CAT_AT (UP_PMAX * 2u - 1u)            /* (Felucca's sound category: kept as stored) */
#define UP_BPM_AT (UP_CAT_AT - 1u)               /* the tempo (BPM, 40..240) */
#define UP_OCT_AT (UP_CAT_AT - 6u)               /* the octave (OCT- / OCT+, -3..3) + 4 */
#define UP_FX2_AT (UP_CAT_AT - 14u)              /* G_RTYPE G_RDAMP G_DTIME G_DFDBK G_ENGGO G_CDEPTH G_INFO G_MIDI + 1 */
#define UP_FX_AT (UP_CAT_AT - 5u)                /* the FX page's G_RSIZE G_DRREV G_CRATE G_DRLVL + 1 */
static const uint8_t UP_FXG[4] = {G_RSIZE, G_DRREV, G_CRATE, G_DRLVL};
static const uint8_t UP_FXG2[8] = {G_RTYPE, G_RDAMP, G_DTIME, G_DFDBK, G_ENGGO, G_CDEPTH, G_INFO, G_MIDI};

static up_rec_t *up_rec(uint32_t k) { return &up_bank[(k % UP_SLOTS) / UP_PER_BANK].r[k % UP_PER_BANK]; }

static int up_valid(const up_rec_t *r)
{
    uint32_t i;
    if (!(r->used == UP_USED && r->ver >= 1u && r->ver <= UP_VER_GRID && r->engine < NENGINES &&
          r->np >= 8u && r->np <= (r->ver >= 4u ? UP_PMAX * 2u : UP_PMAX) && r->name[0]))
        return 0;
    if (r->ver >= 4u)
        for (i = 0; i < r->np; i++)
            if (r->packed[i] > 191u)
                return 0;
    return 1;
}
static int up_used(uint32_t k) { return k < UP_SLOTS && up_valid(up_rec(k)); }
static int16_t up_value(const up_rec_t *r, uint32_t k) { return r->ver >= 4u ? (int16_t)r->packed[k] - 64 : r->p[k]; }
static void up_set_value(up_rec_t *r, uint32_t k, int16_t v) { r->packed[k] = (uint8_t)(clamp(v, -64, 127) + 64); }
static uint32_t up_cat(uint32_t k)               /* (the category byte as stored: the editor's list shows it) */
{
    const up_rec_t *r = up_rec(k);
    return r->ver >= 4u && r->np <= UP_CAT_AT && r->packed[UP_CAT_AT] < 9u ? r->packed[UP_CAT_AT] : 0u;
}
/* the engine a used slot's sound plays on (a retired engine's: ORGAN, its values clamped) */
static uint32_t up_engine(uint32_t k) { return eng_ok(up_rec(k)->engine) ? up_rec(k)->engine : ENGI_ORGAN; }

static void up_pat_norm(uint8_t *note, uint8_t *flags)   /* tie: no note; rest: no flags */
{
    *note &= 127u;
    if (*flags & 4u) {
        *note = 0;
        *flags = 4;
    } else {
        *flags = *note ? (uint8_t)(*flags & (SF_ACCENT | SF_SLIDE)) : 0u;
    }
}

/* after loading bank b (len bytes, -1 = none): wrong shape -> empty; each record's pattern inside its fields */
static void up_bank_check(uint32_t b, int len)
{
    up_bank_t *bk = &up_bank[b];
    uint32_t i, k;
    if (len != (int)sizeof *bk || bk->magic != UP_BANK_MAGIC || bk->rsize != sizeof(up_rec_t) ||
        bk->nslot != UP_PER_BANK)
        memset(bk, 0, sizeof *bk);
    for (i = 0; i < UP_PER_BANK; i++) {
        up_rec_t *r = &bk->r[i];
        if (!up_valid(r) || r->ver == UP_VER_GRID)
            continue;
        for (k = 0; k < 16u; k++)
            up_pat_norm(&r->note[k], &r->flags[k]);
    }
}

/* the record's values in today's P_* order (mapped by count, see the top), each inside its range */
static void up_values(const up_rec_t *r, int16_t *v)
{
    uint32_t i, e = eng_ok(r->engine) ? r->engine : ENGI_ORGAN, nc = r->np - 8u;
    for (i = 0; i < P_E0; i++)
        v[i] = i < nc ? up_value(r, i) : param_desc_of(e, i)->def;
    for (i = 0; i < 8u; i++)
        v[P_E0 + i] = up_value(r, nc + i);
    for (i = 0; i < P_COUNT; i++)
        v[i] = (int16_t)param_fit(param_desc_of(e, i), v[i]);
}

static int up_name_ok(const uint8_t *s, uint32_t n)   /* 1..12 printable ASCII */
{
    uint32_t i;
    if (!n || n > 12u)
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] < 32u || s[i] > 126u)
            return 0;
    return 1;
}

static void up_name(uint32_t k, char *b)       /* upper case, 0-terminated: b holds 13 */
{
    const up_rec_t *r = up_rec(k);
    uint32_t i;
    for (i = 0; i < 12u && r->name[i]; i++)
        b[i] = r->name[i] >= 'a' && r->name[i] <= 'z' ? (char)(r->name[i] - 32) : r->name[i];
    b[i] = 0;
}

static void up_slot_label(char *b, uint32_t k)  /* "U07" */
{
    b[0] = 'U';
    b[1] = (char)('0' + (k + 1u) / 10u);
    b[2] = (char)('0' + (k + 1u) % 10u);
    b[3] = 0;
}

/* ------------------------------------------------ the presets' organ patches --- */
#define UPF_MAGIC 0x36465055u                    /* "UPF6" */
#define UPF_N 128u                               /* an entry's patch: 128 7-bit values (the organ: its ORG_PSIZE first) */
#define UPF_PK 112u                              /* .. packed 8 to 7 bytes */
typedef struct {
    uint32_t tag;                                /* upf_tag of the record it belongs to */
    uint8_t pk[UPF_PK];
} upf_ent_t;
typedef struct {
    uint32_t magic;
    uint16_t ver, nslot;                         /* 1, UP_SLOTS */
    uint32_t used;                               /* bit k: e[k] holds a patch */
    uint32_t rsv;
    upf_ent_t e[UP_SLOTS];
} upf_t;
_Static_assert(sizeof(upf_t) == 3728u, "user preset patches layout (at most ST_PAYLOAD_MAX, 3840)");
_Static_assert(ORG_PSIZE <= UPF_N, "the organ patch in a user preset patch entry");
static upf_t upf __attribute__((section(".pool")));

static int upf_valid(const upf_t *u) { return u->magic == UPF_MAGIC && u->ver == 1u && u->nslot == UP_SLOTS; }

static void upf_empty(void)
{
    memset(&upf, 0, sizeof upf);
    upf.magic = UPF_MAGIC;
    upf.ver = 1;
    upf.nslot = UP_SLOTS;
}

static uint32_t upf_tag(const up_rec_t *r)       /* FNV-1a of the record without its name (bytes 4..15) */
{
    const uint8_t *b = (const uint8_t *)r;
    uint32_t h = 2166136261u, i;
    for (i = 0; i < sizeof *r; i++)
        if (i < 4u || i >= 16u)
            h = (h ^ b[i]) * 16777619u;
    return h;
}

static int upf_organ(uint32_t k) { return up_used(k) && up_rec(k)->engine == ENGI_ORGAN; }

/* 128 7-bit values, 8 to 7 bytes (UPF_PK) and back */
static void upf_pk_get(const uint8_t *d, uint8_t *pk)
{
    uint32_t i, acc = 0, n = 0, o = 0;
    for (i = 0; i < UPF_N; i++) {
        while (n < 7u) {
            acc |= (uint32_t)d[o++] << n;
            n += 8u;
        }
        pk[i] = (uint8_t)(acc & 127u);
        acc >>= 7;
        n -= 7u;
    }
}
static void upf_pk_put(const uint8_t *pk, uint8_t *d)
{
    uint32_t i, acc = 0, n = 0, o = 0;
    memset(d, 0, UPF_PK);
    for (i = 0; i < UPF_N; i++) {
        acc |= (uint32_t)(pk[i] & 127u) << n;
        n += 7u;
        while (n >= 8u) {
            d[o++] = (uint8_t)acc;
            acc >>= 8;
            n -= 8u;
        }
    }
}

/* slot k's patch -> pk (UPF_N bytes), 0 = it has one */
static int upf_get(uint32_t k, uint8_t *pk)
{
    if (k >= UP_SLOTS || !upf_valid(&upf) || !((upf.used >> k) & 1u) || !upf_organ(k) ||
        upf.e[k].tag != upf_tag(up_rec(k)))
        return 1;
    upf_pk_get(upf.e[k].pk, pk);
    return 0;
}

static void upf_boot(void)                       /* up_boot, after the banks */
{
    if (flash_ok && st_load(OBJ_UPFM6, &upf, sizeof upf) == (int)sizeof upf && upf_valid(&upf))
        return;
    upf_empty();
}

/* up_store: slot k (its record just written) gets the organ patch; 0 ok, 2 flash error, 3 no flash (kept in RAM) */
static int upf_store(uint32_t k)
{
    uint8_t c[UPF_N];
    if (!upf_valid(&upf))
        upf_empty();
    memset(c, 0, sizeof c);
    memcpy(c, org_patch, ORG_PSIZE);
    org_sanitize(c);
    upf_pk_put(c, upf.e[k].pk);
    upf.e[k].tag = upf_tag(up_rec(k));
    upf.used |= 1u << k;
    if (!flash_ok)
        return 3;
    return st_save(OBJ_UPFM6, &upf, sizeof upf) ? 2 : 0;
}

/* up_load: the instrument (its values just loaded from slot k) gets the preset's own organ patch, else the first
 * factory one */
static void upf_track_load(track_t *t, uint32_t k)
{
    uint8_t pk[UPF_N];
    if (t->eng_req != ENGI_ORGAN)
        return;
    if (upf_get(k, pk))
        memcpy(pk, ORG_FACTORY[0], ORG_PSIZE);
    org_set_patch(pk);
}

/* ------------------------------------------------------------- the store --- */
static void up_boot(void)                      /* power-on: the banks from flash */
{
    uint32_t b;
    for (b = 0; b < UP_SLOTS / UP_PER_BANK; b++)
        up_bank_check(b, flash_ok ? st_load(OBJ_UPRESET0 + b, &up_bank[b], sizeof up_bank[b]) : -1);
    upf_boot();
}

/* record k = *r (0: erase), then the bank to flash: 0 ok, 1 bad slot, 2 flash error or the transport runs (nothing
 * written), 3 no flash (kept in RAM) */
static int up_put(uint32_t k, const up_rec_t *r)
{
    up_bank_t *bk;
    up_rec_t old;
    uint32_t magic;
    uint16_t rsize, nslot;
    if (k >= UP_SLOTS)
        return 1;
    if (transport_busy()) {                            /* no flash erase while playing */
        ui_message("STOP TO SAVE");
        return 2;
    }
    bk = &up_bank[k / UP_PER_BANK];
    old = *up_rec(k);
    magic = bk->magic;
    rsize = bk->rsize;
    nslot = bk->nslot;
    bk->magic = UP_BANK_MAGIC;
    bk->rsize = sizeof(up_rec_t);
    bk->nslot = UP_PER_BANK;
    if (r)
        *up_rec(k) = *r;
    else
        memset(up_rec(k), 0, sizeof(up_rec_t));
    if (flash_ok && st_save(OBJ_UPRESET0 + k / UP_PER_BANK, bk, sizeof *bk)) {
        *up_rec(k) = old;
        bk->magic = magic;
        bk->rsize = rsize;
        bk->nslot = nslot;
        return 2;
    }
    if (!r && TSEL->user == k + 1u)
        TSEL->user = 0;
    up_gen++;
    return flash_ok ? 0 : 3;
}

/* the record's name = name (at most 12), 0 or "": the automatic one (engine name + slot number, "ORGAN 07") */
static void up_set_name(up_rec_t *r, uint32_t k, const char *name)
{
    char b[16], l[4];
    uint32_t i;
    if (!name || !name[0]) {
        str_cpy(b, ENGINES[eng_ok(r->engine) ? r->engine : ENGI_ORGAN]->name, 9);
        up_slot_label(l, k);
        str_cpy(b + str_len(b), " ", 2);
        str_cpy(b + str_len(b), l + 1, 3);
        name = b;
    }
    memset(r->name, 0, sizeof r->name);
    for (i = 0; i < 12u && name[i]; i++)
        r->name[i] = name[i];
}

/* the instrument's sound (its values, the first 16 steps, the tempo, the octave, the FX settings) -> record *r */
static void up_capture(up_rec_t *r, uint32_t k, const char *name)
{
    const track_t *t = TSEL;
    uint32_t i;
    memset(r, 0, sizeof *r);
    r->used = UP_USED;
    r->ver = UP_VER;
    r->engine = t->eng_req;
    r->np = P_COUNT;
    up_set_name(r, k, name);
    for (i = 0; i < P_COUNT; i++)
        up_set_value(r, i, t->p[i]);
    for (i = 0; i < 16u; i++) {
        const step_t *st = &t->step[i];
        r->note[i] = st->time == ST_NOTE && st->n ? st->note[0] : 0u;
        r->flags[i] = st->time == ST_TIE ? 4u : st->flags;
        up_pat_norm(&r->note[i], &r->flags[i]);
    }
    r->packed[UP_BPM_AT] = (uint8_t)clamp(song.g[G_BPM], 40, 240);
    r->packed[UP_OCT_AT] = (uint8_t)(clamp(song.octave, -3, 3) + 4);
    for (i = 0; i < 8u; i++)
        r->packed[UP_FX2_AT + i] = (uint8_t)(clamp(song.g[UP_FXG2[i]], 0, 127) + 1);
    for (i = 0; i < 4u; i++)
        r->packed[UP_FX_AT + i] = (uint8_t)(clamp(song.g[UP_FXG[i]], 0, 127) + 1);
}

/* the instrument's sound -> slot k, with its organ patch; name 0 or "": the automatic name; up_put's result */
static int up_store(uint32_t k, const char *name)
{
    up_rec_t r;
    int rc;
    up_capture(&r, k, name);
    rc = up_put(k, &r);                                /* the record first, then its patch */
    if ((rc == 0 || rc == 3) && r.engine == ENGI_ORGAN && upf_store(k) == 2)
        rc = 2;
    return rc;
}

/* the extras a record keeps (tempo, octave, FX settings) -> the song */
static void up_extras(const up_rec_t *r)
{
    uint32_t i;
    if (r->ver < 4u)
        return;
    if (r->np <= UP_BPM_AT && r->packed[UP_BPM_AT] >= 40u && r->packed[UP_BPM_AT] <= 240u)
        song.g[G_BPM] = r->packed[UP_BPM_AT];
    if (r->np <= UP_OCT_AT && r->packed[UP_OCT_AT] >= 1u && r->packed[UP_OCT_AT] <= 7u)
        song.octave = (int8_t)((int)r->packed[UP_OCT_AT] - 4);
    if (r->np <= UP_FX2_AT)
        for (i = 0; i < 8u; i++)
            song.g[UP_FXG2[i]] = (int16_t)(r->packed[UP_FX2_AT + i] >= 1u && r->packed[UP_FX2_AT + i] <= 128u
                                               ? r->packed[UP_FX2_AT + i] - 1u : (uint32_t)GP[UP_FXG2[i]].def);
    if (r->np <= UP_FX_AT)
        for (i = 0; i < 4u; i++)
            if (r->packed[UP_FX_AT + i] >= 1u && r->packed[UP_FX_AT + i] <= 128u)
                song.g[UP_FXG[i]] = (int16_t)(r->packed[UP_FX_AT + i] - 1u);
}

/* slot k -> the instrument: engine and every parameter except the track's own (param_kept: the mix, the scale, the
 * pattern parameters), the preset's patch and extras. The steps stay. 0 ok, 1 empty */
static int up_load(uint32_t k)
{
    const up_rec_t *r;
    int16_t v[P_COUNT];
    uint32_t i;
    track_t *t = TSEL;
    if (!up_used(k))
        return 1;
    r = up_rec(k);
    up_values(r, v);
    panic_req |= (uint8_t)(1u << song.sel);
    fm1_irq_off();                                      /* the audio ISR must not see half a sound */
    t->eng_req = (uint8_t)up_engine(k);
    for (i = 0; i < P_COUNT; i++)
        if (!param_kept(i) || (i == P_LEVEL && t->eng_req == ENGI_ORGAN))   /* (the organ: a preset's level is its own) */
            t->p[i] = v[i];
    t->preset = 0;
    fm1_irq_on();
    upf_track_load(t, k);                               /* its own patch */
    t->user = (uint8_t)(k + 1u);
    up_extras(r);
    sync_reload = 1;
    ui.force = 1;
    return 0;
}
