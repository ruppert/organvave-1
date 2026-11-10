/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Persistent storage on the SPI NOR.
 *
 * Every object has an A/B sector pair. A save goes to the copy that is not
 * the current one: erase the sector, program the payload pages (from offset
 * 256), then the 32-byte header at offset 0 LAST. The header is the commit
 * record (magic, seq, length, payload CRC, header CRC); on load the valid
 * copy with the newest seq wins (including wrap), so a write torn at any point leaves the
 * previous copy in charge.
 *
 * Flash access goes through three hooks (also used by the host test):
 *   st_read(off, dst, n)   st_erase(off)   st_prog(off, src, n)
 */
#define ST_MAGIC 0x554C4546u                   /* "FELU" */
#define ST_SECTOR 4096u
#define ST_PAYLOAD_OFF 256u
#define ST_PAYLOAD_MAX (ST_SECTOR - ST_PAYLOAD_OFF)

/* flash map: settings 0xFC000 / 0xFD000 (settings.c), the user preset banks 0xDC000..0xDFFFF (upreset.c), the user
 * presets' patches A 0x9F000, B 0xFE000 (upreset.c), the autosave A 0xE5000, B 0xE6000 (settings.c), above the OTA
 * staging (0xE0000..0xE4FFF). The object numbers are the commit record's type and stay FMini-1's (and Felucca's), so the
 * settings and the user presets of FMini-1 read here: Felucca's projects (1..4) and FM6 bank (7) are retired numbers,
 * their sectors (0x97000..0x9EFFF) unused. The autosave's record is this firmware's own: FMini-1's (a project) reads
 * as none */
enum { OBJ_SETTINGS, OBJ_UPRESET0 = 5, OBJ_UPFM6 = 8, OBJ_AUTOSAVE, OBJ_COUNT };

typedef struct {
    uint32_t magic;
    uint16_t type, slot;
    uint32_t seq, len, crc, rsv[2];
    uint32_t hcrc;
} st_hdr_t;
_Static_assert(sizeof(st_hdr_t) == 32u, "storage commit record layout");

static int st_read(uint32_t off, void *dst, uint32_t n);
static int st_erase(uint32_t off);
static int st_prog(uint32_t off, const void *src, uint32_t n);

static uint32_t st_crc32_add(uint32_t c, const void *p, uint32_t n)   /* zlib CRC-32, 4 bits per step; running:
                                                                         * from 0xFFFFFFFF, the CRC is ~c */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    const uint8_t *b = p;
    while (n--) {
        c ^= *b++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return c;
}
static uint32_t st_crc32(const void *p, uint32_t n) { return ~st_crc32_add(0xFFFFFFFFu, p, n); }

#if BOARD_SMK37
/* SMK-37 Pro (OrganVave-1's objects; the layer's places were Felucca's): every object in FL_DATA 0x9D000..0xBAFFF (the
 * stock VM), A/B neighbours: the user preset banks 0x9D000..0xA0FFF (OBJ_UPRESET0, +1), the user presets' patches
 * 0xA1000 / 0xA2000 (OBJ_UPFM6), the autosave 0xA3000 / 0xA4000, the settings 0xA5000 / 0xA6000; 0xA7000..0xAEFFF
 * spare (cleared with the rest on a foreign store); the store owner tag 0xAF000 (st_owner_check); 0xB0000..0xBAFFF
 * spare. Never near BTIF / USRTRIM (0xC0000..0xC1FFF) or key_mac (0xFF000). */
#define ST_SMK_BASE 0x9D000u
#define ST_SMK_PAIRS 5u
#define ST_SMK_SECTORS (2u * ST_SMK_PAIRS)
#define ST_SMK_AREA 18u                        /* the sectors cleared on a foreign store: 0x9D000..0xAEFFF */
_Static_assert(OBJ_COUNT == 10u && OBJ_UPRESET0 == 5u && OBJ_UPFM6 == 8u && OBJ_AUTOSAVE == 9u,
               "SMK store: a new object needs a place");
_Static_assert(ST_SMK_BASE + ST_SMK_SECTORS * ST_SECTOR <= 0xAF000u, "SMK store: the objects end before the owner tag");
_Static_assert(ST_SMK_BASE + ST_SMK_AREA * ST_SECTOR == 0xAF000u, "SMK store: the cleared area ends at the owner tag");
static uint32_t st_sector(uint32_t obj, uint32_t copy)  /* flash offset of copy A (0) / B (1) */
{
    static const uint8_t ORDER[OBJ_COUNT] = {
        [0] = 0xFF, [1] = 0xFF, [2] = 0xFF, [3] = 0xFF, [4] = 0xFF, [7] = 0xFF,   /* (retired numbers: no sector) */
        [OBJ_UPRESET0] = 0, [OBJ_UPRESET0 + 1] = 1, [OBJ_UPFM6] = 2, [OBJ_AUTOSAVE] = 3, [OBJ_SETTINGS] = 4};
    uint32_t k = obj < OBJ_COUNT ? ORDER[obj] : 0xFFu;
    return k == 0xFFu ? 0xFFFFFFFFu : ST_SMK_BASE + (k * 2u + (copy & 1u)) * ST_SECTOR;
}

/* The owner of the store. The store area is the stock firmware's VM, so after an install it may hold the stock
 * firmware's (or any other firmware's) data, which OrganVave-1 must never parse as its own. The sector 0xAF000 holds
 * the tag "SMK37DATAORGVAVE" (the data family, SMK_DATA_FAMILY in build.py). Tagged: load as ever. Anything else
 * (another tag, or none): clear 0x9D000..0xAEFFF, then write the tag. FELUCCA_DATA_UNTAGGED_OK 1 would keep a store
 * with an erased tag (0: the SMK build never wrote untagged data). Calibration, BT pairing and key_mac are outside
 * FL_DATA: never touched. Called first at boot (main.c), before any store read. */
#ifndef FELUCCA_DATA_UNTAGGED_OK
#define FELUCCA_DATA_UNTAGGED_OK 0
#endif
#define ST_OWNER_OFF 0xAF000u
_Static_assert(ST_OWNER_OFF == ST_SMK_BASE + ST_SMK_AREA * ST_SECTOR, "SMK store owner: the sector after the area");
/* The stock firmware's mapping in USRFLASH (global settings at 0xC2000, magic A3 3A; 8 presets from 0xC3000 every
 * 0x5000, 0x29E3 bytes each, the rest erased; the end marker "usrdata\0" at 0xEAFF8, FL_STOCKTAG): the stock firmware
 * trusts the area while the marker is there and never rebuilds it, so after a return to stock the user samples
 * would be its key, knob and fader mapping (every key one note). So: an intact stock mapping is kept (it reads as
 * empty slots), and the marker is released (its sector erased: the stock firmware rebuilds its factory mapping at
 * its next start) before the first erase in USRFLASH (storage_hw.c st_erase, editor.c) and at boot when it stands
 * over anything else (st_owner_check: ours, an older build's, another firmware's). */
static int st_stock_tagged(void)
{
    static const uint8_t TAG[8] = {'u', 's', 'r', 'd', 'a', 't', 'a', 0};
    uint8_t t[8];
    uint32_t i;
    if (st_read(FL_STOCKTAG_OFF, t, 8))
        return 0;
    for (i = 0; i < 8u; i++)
        if (t[i] != TAG[i])
            return 0;
    return 1;
}
static int st_stock_intact(void)                /* the marker over the stock layout */
{
    uint8_t b[8];
    uint32_t n, k, i, ff;
    if (!st_stock_tagged() || st_read(FL_SMP_LO, b, 2) || b[0] != 0xA3u || b[1] != 0x3Au)
        return 0;
    for (n = 0; n < 8u; n++)                    /* each preset: its head written, its last two sectors erased */
        for (k = 0; k < 3u; k++) {
            if (st_read(FL_SMP_LO + 0x1000u + n * 0x5000u + (k ? 0x2000u + k * 0x1000u : 0u), b, 8))
                return 0;
            for (ff = 1, i = 0; i < 8u; i++)
                ff &= b[i] == 0xFFu;
            if (ff != (k != 0u))
                return 0;
        }
    return 1;
}
static void st_stock_release(void)              /* erase the marker's sector, once: later calls find no marker */
{
    if (st_stock_tagged())
        st_erase(FL_STOCKTAG_LO);
}
static inline int st_in_usrflash(uint32_t off) { return off >= FL_SMP_LO && off < FL_SMP_HI; }
static uint8_t st_owner_cleared;                /* (the UI may say so) */
static void st_owner_check(void)
{
#ifndef SMK_DATA_FAMILY
#define SMK_DATA_FAMILY "ORGVAVE"
#endif
    static const char FAM[] = SMK_DATA_FAMILY;      /* the tag: "SMK37DATA" + this (1..7 of A-Z 0-9), zero-padded to 16 B */
    uint8_t tag[16], want[16];
    uint32_t i, erased = 1, ours = 1, s;
    for (i = 0; i < 16u; i++)
        want[i] = i < 9u ? (uint8_t)"SMK37DATA"[i] : i - 9u < sizeof FAM - 1u ? (uint8_t)FAM[i - 9u] : 0u;
    if (!st_stock_intact())
        st_stock_release();                     /* the marker over anything but the stock mapping: let it rebuild */
    if (st_read(ST_OWNER_OFF, tag, 16))
        return;
    for (i = 0; i < 16u; i++) {
        erased &= tag[i] == 0xFFu;
        ours &= tag[i] == want[i];
    }
    if (ours)
        return;
    if (!erased || !FELUCCA_DATA_UNTAGGED_OK) {     /* not ours (or untagged data): clear it */
        for (s = 0; s < ST_SMK_AREA; s++)
            st_erase(ST_SMK_BASE + s * ST_SECTOR);
        st_owner_cleared = 1;
    }
    st_erase(ST_OWNER_OFF);
    st_prog(ST_OWNER_OFF, want, 16);
}
#else
static void st_owner_check(void) {}
static uint32_t st_sector(uint32_t obj, uint32_t copy)  /* flash offset of copy A (0) / B (1) */
{
    if (obj == OBJ_SETTINGS)
        return 0xFC000u + copy * ST_SECTOR;
    if (obj == OBJ_UPFM6)
        return copy ? 0xFE000u : 0x9F000u;
    if (obj == OBJ_AUTOSAVE)
        return 0xE5000u + copy * ST_SECTOR;
    if (obj == OBJ_UPRESET0 || obj == OBJ_UPRESET0 + 1u)
        return 0xDC000u + (obj - OBJ_UPRESET0) * 2u * ST_SECTOR + copy * ST_SECTOR;
    return 0xFFFFFFFFu;                               /* (a retired number: no sector) */
}
#endif

static uint8_t st_buf[ST_PAYLOAD_MAX] __attribute__((aligned(4), section(".pool")));   /* (main loop only) */

static int st_head(uint32_t obj, uint32_t copy, st_hdr_t *h)   /* commit record valid: 0 */
{
    if (obj >= OBJ_COUNT || copy > 1u || st_sector(obj, copy) == 0xFFFFFFFFu)
        return -1;
    if (st_read(st_sector(obj, copy), h, sizeof *h))
        return -1;
    if (h->magic != ST_MAGIC || h->type != obj || h->slot != copy || h->len > ST_PAYLOAD_MAX ||
        h->hcrc != st_crc32(h, sizeof *h - 4u))
        return -1;
    return 0;
}

static int st_body(uint32_t obj, uint32_t copy, const st_hdr_t *h)   /* payload -> st_buf, CRC ok: 0 */
{
    if (st_read(st_sector(obj, copy) + ST_PAYLOAD_OFF, st_buf, h->len) || st_crc32(st_buf, h->len) != h->crc)
        return -1;
    return 0;
}

/* the current copy: the newest valid sequence (A on a tie), -1 when
 * neither is valid. Headers first, so only the winner's payload is read (it
 * is left in st_buf); *h gets its header. */
static int st_current(uint32_t obj, st_hdr_t *h)
{
    st_hdr_t a, b;
    int va = st_head(obj, 0, &a) == 0, vb = st_head(obj, 1, &b) == 0;
    if (vb && (!va || (b.seq != a.seq && b.seq - a.seq < 0x80000000u))) {
        if (st_body(obj, 1, &b) == 0) {
            *h = b;
            return 1;
        }
        vb = 0;
    }
    if (va && st_body(obj, 0, &a) == 0) {
        *h = a;
        return 0;
    }
    if (vb && st_body(obj, 1, &b) == 0) {
        *h = b;
        return 1;
    }
    return -1;
}

/* load the whole object into dst; returns its length, or -1 if it does not fit */
static int st_load(uint32_t obj, void *dst, uint32_t max)
{
    uint32_t i;
    st_hdr_t h;
    if (obj >= OBJ_COUNT || st_current(obj, &h) < 0 || h.len > max)
        return -1;
    for (i = 0; i < h.len; i++)
        ((uint8_t *)dst)[i] = st_buf[i];
    return (int)h.len;
}

/* save into copy `to` (0 A, 1 B; -1: the one that is not the current copy, as st_save) */
static int st_save_to(uint32_t obj, const void *src, uint32_t len, int to)
{
    uint32_t seq, base, off;
    int cur, rc;
    st_hdr_t h;
    if (obj >= OBJ_COUNT || len > ST_PAYLOAD_MAX || to > 1 || st_sector(obj, 0) == 0xFFFFFFFFu)
        return -1;
    cur = st_current(obj, &h);
    seq = cur < 0 ? 0u : h.seq;
    if (to < 0)
        to = cur == 0 ? 1 : 0;                        /* write the other copy */
    base = st_sector(obj, (uint32_t)to);
    for (off = 0; off < len; off++)
        st_buf[off] = ((const uint8_t *)src)[off];    /* the driver wants RAM sources */
    if ((rc = st_erase(base)) != 0)
        return rc;
    for (off = 0; off < len; off += 256u) {
        uint32_t n = len - off > 256u ? 256u : len - off;
        if ((rc = st_prog(base + ST_PAYLOAD_OFF + off, st_buf + off, n)) != 0)
            return rc;
    }
    h.magic = ST_MAGIC;
    h.type = (uint16_t)obj;
    h.slot = (uint16_t)to;
    h.seq = seq + 1u;
    h.len = len;
    h.crc = st_crc32(st_buf, len);
    h.rsv[0] = h.rsv[1] = 0xFFFFFFFFu;
    h.hcrc = st_crc32(&h, sizeof h - 4u);
    if ((rc = st_prog(base, &h, sizeof h)) != 0)       /* the commit record, last */
        return rc;
    {   /* read back: a write-protected or failing part must not report SAVED */
        st_hdr_t chk;
        uint32_t c = (uint32_t)to;
        if (st_head(obj, c, &chk) || memcmp(&chk, &h, sizeof h) || st_body(obj, c, &chk))
            return -7;
    }
    return 0;
}

static int st_save(uint32_t obj, const void *src, uint32_t len) { return st_save_to(obj, src, len, -1); }
