/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The SMK-37 port's organ mapping (firmware/src/smk_organ.c, smk37-port.yaml) on the host: the UI built with
 * -DBOARD_SMK37=1 over host.h's SMK input (the test drives the 37 keys, the pads, the faders, the wheels, the pedal, the
 * buttons). What the board layer reads from the hardware (smk_input.h: the scan, the ADC, the pads' thresholds) is not
 * under test here: that is the device's (the diagnostics page).
 *  - the outer keys: C3..E3 and G#5..C6 play the notes past the FM-1's 27 (with the octave), silent in DRAWBAR PRESET MODE
 *  - the pads: the top row sets the upper drawbars to DRAWBAR PRESET MODE's sets (and lights the one playing); the
 *    bottom row switches the Leslie, the upper VIB / CHORUS, the percussion and its SOFT FAST 3RD, OVERDRIVE, the reverb
 *  - the faders: soft takeover, the banks A B C of UPPER, LOWER and PEDAL (KNOB BANK, FADER BANK), EXPR, DRIVE, REVERB
 *  - the mod wheel and the pedal: the Leslie; < and >: the presets; KNOB BANK + FADER BANK: the diagnostics page
 *  - the knobs: K2 PRESETS, K3 SELECT, K4 ALGORITHM (the panel's SMK default) */
#define CHECK_TAG "smk organ"
#include "editor_host.h"
#include "../firmware/src/smk_organ.c"

static void frame(void) { fm1_in.frames += 10u; fm1_ms += 16u; smk_frame(); oa_leds(); oa_input(); smk_poll(); }   /* (main.c's loop) */
static void pad(uint32_t p) { host_q_pad[host_np++ % 64u] = (int32_t)(p << 8 | 100u); host_q_pad[host_np++ % 64u] = (int32_t)(p << 8); frame(); }
static void btn(uint32_t id)                     /* a press and a release of button id */
{
    fm1_in.buttons |= 1u << id;
    host_pressed = 1u << id;
    frame();
    fm1_in.buttons &= ~(1u << id);
    frame();
}
static void fader(uint32_t f, uint32_t v) { fm1_in.fader[f] = (uint8_t)v; frame(); }
static uint32_t notes_out[8], n_out;
static void midi_take(void)                      /* the note-ons sent (usb.c's MIDI OUT queue) */
{
    n_out = 0;
    while (mo_r != mo_w) {
        uint32_t p = midi_out_q[mo_r++ % MQ];
        if ((p & 15u) == 9u && n_out < 8u)
            notes_out[n_out++] = (p >> 16) & 0x7Fu;
    }
}
static void ext(uint32_t kk, uint32_t vel) { host_q_ext[host_ne++ % 64u] = (int32_t)(kk << 8 | vel); keyboard_block(); midi_take(); }

int main(void)
{
    int bad = 0;
    uint32_t i, ok;

    reset();
    usb.config = 1;
    panel.magic = 0;
    panel_init();
    oa_init();
    fm1_in.frames = 400u;                        /* (past the settling of the faders, the wheels and the pedal) */
    for (i = 0; i < 4u; i++)
        fm1_in.fader[i] = 64;
    fm1_in.wheel[0] = 540;
    fm1_in.wheel[1] = 615;                       /* the mod wheel down (its reading high) */
    fm1_in.pedal = 1023;
    frame();
    frame();

    /* the knobs: the panel's SMK default (owner) */
    bad += check("knobs: K2 PRESETS, K3 SELECT, K4 ALGORITHM, K5..K8 KNOB 1..4 (encoders 1, 2, 3, 4..7)",
                 panel.enc[EN_PRESET] == 1 && panel.enc[EN_SELECT] == 2 && panel.enc[EN_ALGO] == 3 && panel.enc[EN_K1] == 4 &&
                 panel.enc[EN_K4] == 7);

    /* the outer keys */
    song.octave = 0;
    ext(0, 90);                                  /* C3, the lowest key */
    bad += check("outer keys: C3 (keybed 0) plays 48, five below the lowest F (53)", n_out == 1 && notes_out[0] == 48);
    ext(36, 90);                                 /* C6, the highest */
    bad += check("  .. C6 (keybed 36) plays 84, above the highest G (79)", n_out == 1 && notes_out[0] == 84);
    ext(0, 0);
    ext(36, 0);
    song.octave = 1;
    ext(4, 90);
    bad += check("  .. E3 an octave up (OCT+): 64", n_out == 1 && notes_out[0] == 64);
    ext(4, 0);
    song.octave = 0;
    org_dbp_mode = 1;
    ext(1, 90);
    bad += check("  .. silent in DRAWBAR PRESET MODE", n_out == 0);
    ext(1, 0);
    org_dbp_mode = 0;

    /* the pads: the drawbar sets */
    pad(3);                                      /* pad 4: 800000008 */
    for (i = 0, ok = 1; i < 9u; i++)
        ok &= org_patch[OP_UB + i] == OA_DBP[3][i];
    frame();
    bad += check("pads: pad 4 sets the upper drawbars to DRAWBAR PRESET MODE's fourth set, lit bright, the others dim",
                 ok && host_padrgb[3] == (90u << 16 | 64u << 8 | 28u) && host_padrgb[0] == (7u << 16 | 5u << 8 | 2u));
    bad += check("  .. no note from a pad (the pads are commands)", fm1_in.notes == 0 && smk_pad_cmd == 1);
    /* the switches */
    org_set(OP_ROT, 1);
    org_set(OP_ROT_SPEED, 1);
    pad(8);
    frame();
    bad += check("pad 9: the Leslie FAST (bright red)", org_patch[OP_ROT_SPEED] == 2 && (host_padrgb[8] >> 16) == 100u);
    pad(8);
    bad += check("  .. again SLOW", org_patch[OP_ROT_SPEED] == 1);
    {
        uint32_t v = vib_up() != 0;
        pad(9);
        bad += check("pad 10: the upper VIB / CHORUS switched", (vib_up() != 0) != v);
    }
    org_set(OP_PERC, 0);
    pad(10);
    bad += check("pad 11: the percussion on", org_patch[OP_PERC] == 1);
    i = org_patch[OP_PERC_SOFT];
    pad(11);
    bad += check("pad 12: SOFT switched", org_patch[OP_PERC_SOFT] != i);
    i = org_patch[OP_PERC_FAST];
    pad(12);
    bad += check("pad 13: FAST switched", org_patch[OP_PERC_FAST] != i);
    i = org_patch[OP_PERC_SEL];
    pad(13);
    bad += check("pad 14: 3RD switched", org_patch[OP_PERC_SEL] != i);
    org_set(OP_DRV, 0);
    pad(14);
    frame();
    bad += check("pad 15: OVERDRIVE: TUBE DRIVE at 2.5, lit orange", org_patch[OP_DRV] == 1 && org_patch[OP_DRV_GAIN] == 25 &&
                 dr_type_of(org_patch[OP_DRV_TYPE]) == 0 && (host_padrgb[14] >> 16) == 100u);
    pad(14);
    bad += check("  .. again: off", org_patch[OP_DRV] == 0);
    TSEL->p[P_REV] = 40;
    pad(15);
    bad += check("pad 16: the reverb off, then back to its level", TSEL->p[P_REV] == 0);
    pad(15);
    bad += check("  ..", TSEL->p[P_REV] == 40);

    /* the faders */
    org_set(OP_UB + 2, 0);
    frame();
    bad += check("faders: soft takeover: nothing moves until a fader does", org_patch[OP_UB + 2] == 0);
    fader(2, 127);
    bad += check("  .. fader 3 on bank A, UPPER: 8' to 8", org_patch[OP_UB + 2] == 8);
    fader(2, 0);
    bad += check("  .. and to 0", org_patch[OP_UB + 2] == 0);
    btn(SMK_BTN_FADER_BANK);
    fm1_in.fader[0] = 100;                       /* (moved while it waits: its first move after the change) */
    frame();
    bad += check("FADER BANK: bank B; the faders wait for a move (soft takeover again)", smk_bank == 1 && org_patch[OP_UB + 4] != 6);
    fader(0, 127);
    bad += check("  .. fader 1 on B: 2 2/3' to 8", org_patch[OP_UB + 4] == 8);
    btn(SMK_BTN_FADER_BANK);
    frame();
    fader(1, 127);
    bad += check("bank C: fader 2 is EXPR: up = 0 dB", org_patch[OP_EXPR] == 0);
    fader(1, 0);
    bad += check("  .. down = 60 dB down", org_patch[OP_EXPR] == 60);
    fader(2, 127);
    bad += check("  .. fader 3 DRIVE GAIN 10.0, the drive on", org_patch[OP_DRV_GAIN] == 100 && org_patch[OP_DRV] == 1);
    fader(2, 0);
    bad += check("  .. 0: the drive off", org_patch[OP_DRV_GAIN] == 0 && org_patch[OP_DRV] == 0);
    fader(3, 90);
    bad += check("  .. fader 4 the reverb send", TSEL->p[P_REV] == 90);
    fader(0, 127);
    bad += check("  .. fader 1 the upper 1'", org_patch[OP_UB + 8] == 8);
    btn(SMK_BTN_KNOB_BANK);
    frame();
    fader(0, 0);
    bad += check("KNOB BANK: the LOWER manual: fader 1 on C its 1'", smk_man == 1 && org_patch[OP_LB + 8] == 0 &&
                 org_patch[OP_UB + 8] == 8 && fm1_led[SMK_BTN_KNOB_BANK] == 1);
    btn(SMK_BTN_KNOB_BANK);
    btn(SMK_BTN_FADER_BANK);                     /* PEDAL, bank A */
    frame();
    fader(1, 127);
    fader(0, 0);
    bad += check("PEDAL, bank A: faders 1 and 2 its 16' and 8'", smk_man == 2 && smk_bank == 0 &&
                 org_patch[OP_PB + 2] == 8 && org_patch[OP_PB] == 0 && fm1_led[SMK_BTN_FADER_BANK] == 0);
    i = org_patch[OP_PB + 4];
    fader(2, 127);
    bad += check("  .. fader 3 nothing there", org_patch[OP_PB + 4] == i);
    btn(SMK_BTN_KNOB_BANK);                      /* back to UPPER */

    /* the mod wheel: the Leslie */
    org_set(OP_ROT_SPEED, 1);
    fm1_in.wheel[1] = 470;                       /* up (a low reading) */
    frame();
    bad += check("mod wheel up: the Leslie FAST", org_patch[OP_ROT_SPEED] == 2);
    fm1_in.wheel[1] = 545;
    frame();
    bad += check("  .. half way: no change (hysteresis)", org_patch[OP_ROT_SPEED] == 2);
    fm1_in.wheel[1] = 615;
    frame();
    bad += check("  .. down: SLOW", org_patch[OP_ROT_SPEED] == 1);
    pad(8);
    fm1_in.wheel[1] = 600;
    frame();
    bad += check("  .. a pad switched it FAST meanwhile: the wheel resting low leaves it", org_patch[OP_ROT_SPEED] == 2);

    /* the pedal */
    fm1_in.pedal = 10;                           /* pressed (its rest was high) */
    frame();
    bad += check("pedal pressed: the Leslie switched (SLOW)", org_patch[OP_ROT_SPEED] == 1);
    frame();
    bad += check("  .. held: once", org_patch[OP_ROT_SPEED] == 1);
    fm1_in.pedal = 1020;
    frame();
    fm1_in.pedal = 5;
    frame();
    bad += check("  .. released and pressed again: FAST", org_patch[OP_ROT_SPEED] == 2);
    fm1_in.pedal = 1023;
    frame();

    /* < and > */
    i = TSEL->preset;
    btn(SMK_BTN_RIGHT);
    bad += check("> : the next preset", TSEL->preset == (i + 1u) % ORG_NFACTORY || oa_on_user());
    btn(SMK_BTN_LEFT);
    bad += check("< : back", TSEL->preset == i);

    /* the diagnostics page */
    fm1_in.buttons = 1u;                         /* KNOB BANK held */
    host_pressed = 1u;
    frame();
    fm1_in.buttons = 3u;                         /* FADER BANK too */
    host_pressed = 2u;
    frame();
    i = smk_man;
    fm1_in.buttons = 0;
    frame();
    bad += check("KNOB BANK + FADER BANK: the diagnostics page; neither one's own job", smk_diag == 1 && smk_man == i &&
                 smk_sig() != 0u);
    oa_draw();
    bad += check("  .. drawn over the page", host_screen[5 * 240 + 120] == 0);
    fm1_in.buttons = 3u;
    host_pressed = 3u;
    frame();
    fm1_in.buttons = 0;
    frame();
    bad += check("  .. again: closed", smk_diag == 0 && smk_sig() == 0u);

    printf("%s\n", bad ? "SMK ORGAN TEST FAILED" : "smk organ test passed");
    return bad != 0;
}
