/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Guilherme Ruppert (organvave-1) */
/* The editor's host harness (editor_test.c, editor_bridge.c, fuzz_ed.c): host.h with the SysEx plumbing (FELUCCA_OTA),
 * the editor and the instrument's screen code; the replies are collected in host_wire.
 *   reset()                  power-on state: the globals, track 1 on the first factory patch, empty stores
 *   request(cmd, args, n)    one editor frame through usb.c's SysEx parser; returns the reply's length (host_wire) */
#define FELUCCA_OTA 1
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "TEST"
#endif
#include "host.h"

static uint32_t host_progress = 1;              /* 0: the transport never stops (a STOP that times out) */
static uint8_t host_wire[4096];
static uint32_t host_wire_n;
static void host_drain(void)
{
    while (so_r != so_w) {
        uint32_t p = sx_out_q[so_r++ % SXQ], cin = p & 15u, i;
        uint32_t n = cin == 4u || cin == 7u ? 3u : cin == 6u ? 2u : 1u;
        for (i = 0; i < n && host_wire_n < sizeof host_wire; i++)
            host_wire[host_wire_n++] = (uint8_t)(p >> (8u * (i + 1u)));
    }
}
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void) { host_drain(); fm1_ms++; }
static void host_editor_wdt(void)               /* time passes in a blocking loop; the "audio ISR" stops the transport */
{
    fm1_ms++;
    if (host_progress && transport_req == 2u) { seq_stop(); transport_req = 0; }
}
#include "../firmware/src/editor.c"
#include "../firmware/src/organ_app.c"

#ifndef CHECK_TAG
#define CHECK_TAG "editor"
#endif
static int check(const char *what, int ok)
{
    printf(CHECK_TAG ": %-70s %s\n", what, ok ? "ok" : "FAIL");
    return !ok;
}

static void reset(void)
{
    memset(&song, 0, sizeof song); memset(trk, 0, sizeof trk);
    memset(&ed_w, 0, sizeof ed_w); memset(&ui, 0, sizeof ui);
    memset(up_bank, 0, sizeof up_bank); memset(host_nor, 0xFF, sizeof host_nor);
    host_wdt_hook = host_editor_wdt;
    flash_ok = 1; host_progress = 1; host_erases = host_progs = host_wire_n = 0;
    transport_req = panic_req = 0; sx_ready = sx_collect = sx_busy = 0;
    so_r = so_w = mi_r = mi_w = 0; midi_in_overflow = 0; usb.config = 1;
    memset(&um, 0, sizeof um);
    persist_boot();
    host_tracks_init();
    track_defaults(&trk[0]);
    org_init();
    set_engine_of(&trk[0], ENGI_ORGAN);
    trk[0].engine = trk[0].eng_req;
    song.sel = 0;
}

static uint32_t request(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    uint32_t i;
    host_wire_n = 0; ota_frame_done();
    sysex_byte(0xF0); sysex_byte(ED_HDR0); sysex_byte(ED_HDR1); sysex_byte(ED_HDR2); sysex_byte((uint8_t)cmd);
    for (i = 0; i < n; i++) sysex_byte(a[i]);
    sysex_byte(0xF7); ed_service(); host_drain();
    return host_wire_n;
}

/* F0..F7 -> USB-MIDI event packets (cable 0) */
static uint32_t usb_frame(const uint8_t *f, uint32_t n, uint8_t *usbp)
{
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint32_t k = n - i >= 3u ? 3u : n - i, cin = k == 3u && i + 3u < n ? 4u : k == 3u ? 7u : 4u + k;
        usbp[o++] = (uint8_t)cin;
        usbp[o++] = f[i];
        usbp[o++] = k > 1u ? f[i + 1] : 0;
        usbp[o++] = k > 2u ? f[i + 2] : 0;
        i += k;
    }
    return o;
}
