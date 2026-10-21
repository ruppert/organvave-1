/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Guilherme Ruppert (organvave-1) */
/* The firmware's editor handler, the USB and UART MIDI framing and the user presets over SysEx, against a RAM NOR
 * (editor_host.h). The organ's patch (cmds 85, 86: editor_organ.c) and the user presets that hold one. */
#include "editor_host.h"

static uint32_t usb_feed(const uint8_t *p, uint32_t n)   /* whole 64-byte USB packets, as the host sends them */
{
    uint32_t o, refused = 0;
    for (o = 0; o < n; o += 64u)
        while (!ep1_take(p + o, n - o > 64u ? 64u : n - o) && refused < 1000u) {
            refused++;                                  /* NAK: the host sends it again */
            ed_service(); host_drain();
        }
    return refused;
}

static int info(void)
{
    int bad = 0;
    uint32_t n;
    reset();
    n = request(ED_INFO, 0, 0);
    bad += check("INFO starts with FELUCCA", n > 20u && !memcmp(host_wire + 5, "FELUCCA", 7));
    bad += check("INFO ends with the organ patch tag 4F 01 ORG_PSIZE",
                 n > 8u && host_wire[n - 4] == 0x4F && host_wire[n - 3] == 1 && host_wire[n - 2] == ORG_PSIZE &&
                 host_wire[n - 1] == 0xF7);
    {
        uint8_t a[2] = {ENGI_ORGAN, 0};
        n = request(ED_NAMES, a, 1);
        bad += check("NAMES of ORGAN: the factory patches", n > 8u && host_wire[5] == ENGI_ORGAN && host_wire[6] == ORG_NFACTORY);
        a[0] = 3; a[1] = 1;
        bad += check("PRESET of a retired engine: no reply", !request(ED_PRESET, a, 2) && TSEL->eng_req == ENGI_ORGAN);
        a[0] = ENGI_ORGAN; a[1] = 7;
        n = request(ED_PRESET, a, 2);
        bad += check("PRESET ORGAN 8 loads it", n == 8u && host_wire[5] == ENGI_ORGAN && host_wire[6] == 7 && TSEL->preset == 7);
    }
    n = request(ED_DUMP, 0, 0);
    bad += check("DUMP: every value (the longest reply, not cut short)", n == 8u + 2u * (P_COUNT + G_COUNT) &&
                 host_wire[n - 1] == 0xF7 && ed_rv(host_wire + 7 + 2 * (P_E0 + 7)) == TSEL->p[P_E7]);
    n = request(ED_UP_GET, (const uint8_t[]){0}, 1);
    bad += check("UP_GET of an empty slot: the whole frame", n == 5u + 3u + 1u + 2u * P_COUNT + 32u + 2u + 1u &&
                 host_wire[n - 1] == 0xF7);
    n = request(ED_TRACK, 0, 0);
    bad += check("TRACK: one track", n == 5u + 2u + 6u + 1u && host_wire[6] == NTRK);
    return bad;
}

static int framing(void)
{
    uint32_t i;
    int bad = 0;
    static const uint8_t rt[] = {0xF0, 0x7D, 0xF8, 0x46, 0xFE, 0x4C, ED_PING, 0xFF, 0xF7};
    static const uint8_t aborted[] = {0xF0, 0x7D, 0x46, 0x4C, ED_SET, 0, P_LEVEL, 0x90, 0, 64, 0xF7};
    int16_t lvl0;
    reset();
    lvl0 = TSEL->p[P_LEVEL];
    for (i = 0; i < sizeof rt; i++) sysex_byte(rt[i]);
    ed_service(); host_drain();
    bad += check("realtime bytes interleaved in SysEx do not enter the editor frame",
                 host_wire_n == 7u && host_wire[4] == ED_PING && host_wire[5] == 0);
    ota_frame_done(); host_wire_n = 0;
    for (i = 0; i < sizeof aborted; i++) sysex_byte(aborted[i]);
    ed_service(); host_drain();
    bad += check("a non-realtime status aborts SysEx without changing parameters",
                 !sx_ready && !host_wire_n && TSEL->p[P_LEVEL] == lvl0);
    bad += check("a valid request works after an aborted frame", request(ED_PING, 0, 0) == 7u);
    ota_frame_done();
    midi_in_event(0x467DF004u);                          /* F0 7D 46 */
    midi_in_event(0x00004C04u);                          /* unfinished SysEx */
    midi_in_event(0x643C9009u);
    sysex_byte(0xF7);
    bad += check("a channel event in another USB packet aborts an unfinished SysEx",
                 !sx_ready && mi_w == 1u && midi_in_q[0] == 0x643C9009u);
    midi_in_event(0x643C8009u);                          /* wrong CIN */
    midi_in_event(0x643CFF09u);                          /* wrong status */
    midi_in_event(0xFF3C9009u);                          /* not a seven-bit velocity */
    midi_in_event(0x64FF9009u);                          /* not a seven-bit note */
    bad += check("USB channel packets reject mismatched CIN and high data bits", mi_w == 1u);
    midi_in_event(0xFF05C00Cu);                          /* one-byte status: padding is ignored */
    midi_in_event(0xFF07D00Du);
    bad += check("USB program and channel pressure keep their one-byte payload", mi_w == 3u);
    mi_w = UINT32_MAX - 5u; mi_r = mi_w;
    for (i = 0; i < MQ + 8u; i++) midi_in_event(0x643C9009u);
    bad += check("USB MIDI ring stops at capacity across counter wrap", mi_w - mi_r == MQ && midi_in_overflow);
    mi_r = mi_w; midi_in_event(0x643C9009u);
    bad += check("USB rejects new messages until the audio consumer clears overflow", mi_w == mi_r);
    midi_in_overflow = 0; midi_in_event(0x643C9009u);
    bad += check("USB resumes after the audio consumer clears overflow", mi_w - mi_r == 1u);
    return bad;
}

static int uart_recovery(void)
{
    uint32_t i;
    int bad = 0;
    reset();
    um_byte(0x90); um_byte(50);
    for (i = 0; i < UM_RING; i++) um_ring[i] = 0;
    um_ring[126] = 0x90; um_ring[127] = 72; um_ring[0] = 99;
    uart_midi_take(UM_RING + 1u);
    bad += check("UART DMA overrun never combines a stale partial note with new data",
                 !mi_w && midi_in_overflow && um.drops == 2u && um.bytes == UM_RING && !um.pend);
    midi_in_overflow = 0; um_byte(0x90); um_byte(72); um_byte(99);
    bad += check("UART receives a complete fresh note after DMA recovery",
                 mi_w == 1u && midi_in_q[0] == 0x63489009u);
    reset();
    um_byte(0x90); um_byte(50);
    for (i = 0; i < UM_RING; i++) um_ring[i] = 60;
    uart_midi_take(UM_RING + 2u);
    bad += check("UART discards running-status data after lost bytes until a new status",
                 !mi_w && midi_in_overflow && um.drops == 2u && !um.st && !um.got);
    midi_in_overflow = 0;
    um_byte(0x91); um_byte(70); um_byte(100);
    bad += check("UART recovers immediately on a complete channel message", mi_w == 1u && midi_in_q[0] == 0x64469109u);
    reset(); mi_w = MQ;
    um_byte(0x90); um_byte(60); um_byte(100);
    bad += check("UART queue overflow latches the same recovery flag as USB", midi_in_overflow && um.drops == 1u && mi_w == MQ);
    mi_r = mi_w; um_byte(61); um_byte(101);
    bad += check("UART rejects a new stream until audio clears overflow", mi_w == mi_r && um.drops == 2u);
    midi_in_overflow = 0; um_byte(62); um_byte(102);
    bad += check("UART resumes valid running status after audio clears overflow",
                 mi_w - mi_r == 1u && midi_in_q[mi_r % MQ] == 0x663E9009u);
    return bad;
}

/* a whole ORG_PUT of count bytes at offset (target 0, the instrument's own patch) */
static uint32_t org_put(uint32_t off, uint8_t v)
{
    uint8_t a[4];
    a[0] = ED_ORG_INST; a[1] = (uint8_t)off; a[2] = 1; a[3] = v;
    return request(ED_ORG_PUT, a, 4);
}

/* user presets over SysEx: store (with the organ patch), list, read, load, erase; malformed names; a STOP that times out */
static int user_presets(void)
{
    static const uint8_t bad_name[] = {0, 'X', 0, 'Y'}, name[] = {4, 'M', 'Y', ' ', 'P', 'A', 'T'};
    uint8_t a[3 + ORG_PSIZE];
    uint32_t n;
    int bad = 0;
    reset();
    bad += check("UP_STORE rejects bytes after its name terminator without saving",
                 request(ED_UP_STORE, bad_name, sizeof bad_name) == 8u && host_wire[6] == 1 && !up_used(0));
    org_put(OP_TEMP, 2);
    TSEL->p[P_LEVEL] = 90;
    bad += check("UP_STORE U05 \"MY PAT\": rc 0, in flash",
                 request(ED_UP_STORE, name, sizeof name) == 8u && host_wire[5] == 4 && host_wire[6] == 0 && up_used(4) &&
                 host_progs > 0u);
    a[0] = 4; a[1] = 2;
    n = request(ED_UP_LIST, a, 2);
    bad += check("UP_LIST 5..6: U05 used, ORGAN, its name; U06 empty",
                 n > 12u && host_wire[5] == 4 && host_wire[6] == 2 && host_wire[7] == UP_SLOTS && host_wire[8] == 1 &&
                 host_wire[9] == ENGI_ORGAN && !memcmp(host_wire + 10, "MY PAT", 7) && host_wire[17] == 0);
    a[0] = 4;
    n = request(ED_UP_GET, a, 1);
    bad += check("UP_GET U05: used, the name, LEVEL 90",
                 host_wire[6] == 1 && !memcmp(host_wire + 8, "MY PAT", 7) &&
                 (int32_t)(host_wire[15 + 2 * P_LEVEL] | host_wire[16 + 2 * P_LEVEL] << 7) - 8192 == 90);
    a[0] = ED_ORG_USER; a[1] = 4;
    n = request(ED_ORG_GET, a, 2);
    bad += check("ORG_GET user 5: the patch stored with it",
                 n == 5u + 3u + ORG_PSIZE + 1u && host_wire[7] == 0 && host_wire[8 + OP_TEMP] == 2);
    reset();                                             /* (reset clears the RAM NOR too: store again, then load) */
    bad += check("UP_LOAD of an empty slot: rc 1", request(ED_UP_LOAD, (const uint8_t[]){4}, 1) == 8u && host_wire[6] == 1);
    org_put(OP_TEMP, 2);
    request(ED_UP_STORE, name, sizeof name);
    apply_preset(0);
    bad += check("UP_LOAD U05: rc 0, the patch back",
                 request(ED_UP_LOAD, (const uint8_t[]){4}, 1) == 8u && host_wire[6] == 0 && org_patch[OP_TEMP] == 2);
    bad += check("UP_ERASE U05: rc 0, empty", request(ED_UP_ERASE, (const uint8_t[]){4}, 1) == 8u && host_wire[6] == 0 &&
                 !up_used(4));
    bad += check("UP_ERASE of slot 33: rc 1", request(ED_UP_ERASE, (const uint8_t[]){UP_SLOTS}, 1) == 8u && host_wire[6] == 1);
    song.playing = 1; host_progress = 0;
    bad += check("UP_STORE while a STOP times out: rc 2, nothing written",
                 request(ED_UP_STORE, name, sizeof name) == 8u && host_wire[6] == 2 && !up_used(4));
    song.playing = 0; host_progress = 1;
    return bad;
}

/* the organ patch (cmds 85, 86): the instrument's own patch, a factory patch, malformed frames */
static int organ_patches(void)
{
    int bad = 0;
    uint8_t a[3 + ORG_PSIZE];
    uint32_t n, i;
    reset();
    a[0] = ED_ORG_FACTORY; a[1] = 3;
    n = request(ED_ORG_GET, a, 2);
    bad += check("ORG_GET factory 4: the patch", n == 5u + 3u + ORG_PSIZE + 1u && host_wire[7] == 0 &&
                 !memcmp(host_wire + 8, ORG_FACTORY[3], ORG_PSIZE));
    a[0] = ED_ORG_INST; a[1] = 0; a[2] = 12;
    for (i = 0; i < 12u; i++) a[3 + i] = (uint8_t)(i < 9u ? 9u : 0u);     /* nine drawbars past 8, three zeros */
    request(ED_ORG_PUT, a, 3u + 12u);
    bad += check("ORG_PUT the drawbars: rc 0, clamped into their ranges (8 each)",
                 host_wire[7] == 0 && org_patch[0] == 8 && org_patch[8] == 8);
    org_put(OP_TEMP, 2);
    bad += check("ORG_PUT the tuning: rc 0", host_wire[7] == 0 && org_patch[OP_TEMP] == 2);
    org_put(OP_XTALK, 5);
    bad += check("  .. the crosstalk clamped to 0..1", host_wire[7] == 0 && org_patch[OP_XTALK] == 1);
    n = request(ED_ORG_GET, (const uint8_t[]){ED_ORG_INST, 0}, 2);
    bad += check("ORG_GET the instrument: as put", n == 5u + 3u + ORG_PSIZE + 1u && host_wire[7] == 0 &&
                 host_wire[8 + OP_TEMP] == 2 && host_wire[8 + OP_XTALK] == 1 && host_wire[8] == 8);
    bad += check("ORG_PUT to a factory patch: rc 1", (request(ED_ORG_PUT, (const uint8_t[]){ED_ORG_FACTORY, 0, 1, 1}, 4), host_wire[7] == 1u));
    bad += check("ORG_PUT past the patch: rc 1", (request(ED_ORG_PUT, (const uint8_t[]){ED_ORG_INST, ORG_PSIZE, 1, 1}, 4), host_wire[7] == 1u));
    bad += check("ORG_PUT with the count and the bytes not matching: rc 1",
                 (request(ED_ORG_PUT, (const uint8_t[]){ED_ORG_INST, 0, 2, 1}, 4), host_wire[7] == 1u));
    request(ED_ORG_GET, (const uint8_t[]){9, 0}, 2);
    bad += check("ORG_GET of an unknown target: rc 1", host_wire[7] == 1u);
    request(ED_ORG_GET, (const uint8_t[]){ED_ORG_FACTORY, ORG_NFACTORY}, 2);
    bad += check("ORG_GET of factory patch 9: rc 1", host_wire[7] == 1u);
    request(ED_ORG_GET, (const uint8_t[]){ED_ORG_USER, 0}, 2);
    bad += check("ORG_GET of an empty user slot: rc 2", host_wire[7] == 2u);
    request(ED_ORG_GET, (const uint8_t[]){ED_ORG_INST}, 1);
    bad += check("ORG_GET with a wrong length: rc 1", host_wire[7] == 1u);
    return bad;
}

/* the frames of cmd in host_wire: how many, and the args of the last (into *args) */
static uint32_t wire_frames(uint32_t cmd, const uint8_t **args)
{
    uint32_t i, k = 0;
    for (i = 0; i + 5u < host_wire_n; i++)
        if (host_wire[i] == 0xF0 && host_wire[i + 1] == ED_HDR0 && host_wire[i + 4] == cmd) {
            k++;
            if (args) *args = host_wire + i + 5;
        }
    return k;
}
static uint32_t sync_pass(void)                          /* one main-loop pass of the pushes */
{
    host_wire_n = 0;
    fm1_ms += 30u;
    ed_sync();
    host_drain();
    return host_wire_n;
}

/* #65: WATCH while watching keeps what is not pushed yet; the editor's own sound load is not echoed as RELOAD */
static int live_sync(void)
{
    int bad = 0;
    uint8_t a[4] = {1, 0, 0, 0};
    const uint8_t *x = 0;
    reset();
    usb.resets = 0;
    request(ED_WATCH, a, 1);
    bad += check("WATCH 1 starts watching", host_wire[5] == 1 && ed_w.on && !sync_pass());
    TSEL->p[P_LEVEL] = 77;                               /* a device change, not pushed yet */
    request(ED_WATCH, a, 1);
    bad += check("WATCH 1 while watching keeps a pending change: CHANGED still goes out",
                 sync_pass() && wire_frames(ED_CHANGED, &x) == 1u && x[1] == P_LEVEL && ed_rv(x + 2) == 77);
    TSEL->p[P_LEVEL] = 66;
    a[0] = 0; request(ED_WATCH, a, 1);
    a[0] = 1; request(ED_WATCH, a, 1);
    bad += check("WATCH 0 then WATCH 1 starts from the values as they are (no push)", !sync_pass());
    usb.resets++;
    TSEL->p[P_LEVEL] = 44;
    request(ED_WATCH, a, 1);
    bad += check("WATCH after a USB reset starts over", !sync_pass());
    TSEL->p[P_PAN] = 33;                                 /* pending; an organ preset sets LEVEL and the patch, not PAN */
    a[0] = ENGI_ORGAN; a[1] = 2;
    request(ED_PRESET, a, 2);
    sync_pass();
    bad += check("the editor's own PRESET: no RELOAD, no CHANGED for what it loaded, a pending CHANGED stays",
                 TSEL->preset == 2 && !wire_frames(ED_RELOAD, 0) && wire_frames(ED_CHANGED, &x) == 1u &&
                 x[1] == P_PAN && !sync_pass());
    sync_reload = 1;                                     /* a load on the device, not pushed yet */
    a[0] = ENGI_ORGAN; a[1] = 1;
    request(ED_PRESET, a, 2);
    sync_pass();
    bad += check("a RELOAD due before the editor's PRESET still goes out", wire_frames(ED_RELOAD, 0) == 1u);
    fm1_ms += 4000u;
    bad += check("3 s without a request ends watching", !sync_pass() && !ed_w.on);
    return bad;
}

/* SysEx through the USB packet path (ep1_take) at full speed: an ORG_PUT (49 bytes, many event packets) arrives whole;
 * a frame sent before the reply to the one before is dropped whole (no reply, never a partial frame) */
static int usb_burst(void)
{
    static uint8_t f[64], u[256], u2[512];
    uint32_t n, k, i;
    const uint8_t *x = 0;
    int bad = 0;
    reset();
    usb.rx_pend = 0;
    n = 0; f[n++] = 0xF0; f[n++] = ED_HDR0; f[n++] = ED_HDR1; f[n++] = ED_HDR2; f[n++] = ED_ORG_PUT;
    f[n++] = ED_ORG_INST; f[n++] = 0; f[n++] = 40;           /* 40 bytes from offset 0: a frame of many packets */
    for (i = 0; i < 40u; i++) f[n++] = 5;
    f[n++] = 0xF7;
    usb_feed(u, usb_frame(f, n, u)); host_wire_n = 0; ed_service(); host_drain();
    bad += check("ORG_PUT through ep1_take: rc 0, the patch", wire_frames(ED_ORG_PUT, &x) == 1u && !x[2] &&
                 org_patch[0] == 5 && org_patch[OP_TEMP] == 2);
    k = usb_frame(f, n, u2);
    for (i = 0; i < 40u; i++) f[8 + i] = 6;                  /* (the second frame: all 6s) */
    k += usb_frame(f, n, u2 + k);
    usb_feed(u2, k);
    host_wire_n = 0; ed_service(); ed_service(); host_drain();
    bad += check("pipelined: the first frame answered, the second (sent before its reply) dropped whole",
                 wire_frames(ED_ORG_PUT, &x) == 1u && !x[2] && org_patch[0] == 5 && !sx_ready);
    return bad;
}

int main(void)
{
    int bad = info() + framing() + uart_recovery() + user_presets() + organ_patches() + live_sync() + usb_burst();
    printf("%s\n", bad ? "EDITOR TEST FAILED" : "editor test passed");
    return bad != 0;
}
