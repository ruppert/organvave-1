/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Guilherme Ruppert (organvave-1) */
/* The organ's screens and controls (firmware/src/organ_app.c) on the host: HOME and DRAWBARS drawn and told apart, the
 * drawbar knobs (the four marked bars, SELECT moves them, the range clamped), PRESET over the factory and the user
 * presets (wrapping), SAVE (the first free slot, the name, the patch kept), GLO (MIDI IN, MIDI CHANNEL, ABOUT closes),
 * the octave keys. */
#define CHECK_TAG "organ app"
#include "editor_host.h"

static void press(uint32_t btn) { host_pressed = 1u << panel.btn[btn]; oa_input(); }
/* a turn, then the next frame (the main loop's): a preset change is seen there, and closes the SAVE dialog */
static void turn(uint32_t enc, int32_t s) { host_enc[panel.enc[enc]] = s; oa_input(); oa_input(); }
static uint32_t frame_hash(void)                        /* what a frame draws (oa_draw into host_screen) */
{
    uint32_t i, h = 2166136261u;
    oa_draw();
    for (i = 0; i < 240u * 240u; i++)
        h = (h ^ host_screen[i]) * 16777619u;
    return h;
}

int main(void)
{
    int bad = 0;
    uint32_t h_home, h_draw, h_save, k, v0, i;
    uint8_t pk[UPF_N];

    reset();
    panel.magic = 0;
    panel_init();                                         /* (main.c: the panel's ids as the boot leaves them) */
    oa_init();
    h_home = frame_hash();
    bad += check("power-on: HOME drawn, the instrument is the organ, the first factory preset",
                 oa_page == 0 && TSEL->eng_req == ENGI_ORGAN && TSEL->preset == 0 && host_blit_rows > 0u);

    press(B_EDIT);
    h_draw = frame_hash();
    bad += check("EDIT: DRAWBARS, a different picture", oa_page == 1 && h_draw != h_home);

    /* drawbars: the four marked bars under the knobs, SELECT moves them, the range 0..8 */
    v0 = org_patch[OP_UB + oa_win];
    turn(EN_K1, v0 < 8 ? 1 : -1);
    bad += check("K1 moves the first marked bar by one step",
                 org_patch[OP_UB + oa_win] == (v0 < 8 ? v0 + 1 : v0 - 1));
    turn(EN_K1, 20);
    bad += check("  .. a big turn clamps at 8", org_patch[OP_UB + oa_win] == 8);
    turn(EN_K1, -20);
    bad += check("  .. and at 0", org_patch[OP_UB + oa_win] == 0);
    org_set(OP_UB + 0u, 3u);
    k = oa_win;
    turn(EN_SELECT, 1);
    bad += check("SELECT moves the marked bars one bar along", oa_win == (uint8_t)(k + 1u));
    for (i = 0; i < 4u; i++)                              /* (one bar a detent, as the encoder sends it) */
        turn(EN_SELECT, -1);
    bad += check("  .. to the top bar and no further", oa_win == 0);
    turn(EN_K2, -1);
    bad += check("K2 is the second marked bar", org_patch[OP_UB + 1] == 7 && org_patch[OP_UB + 0] == 3);
    for (i = 0; i < 3u; i++) { press(B_HOME); press(B_EDIT); }
    bad += check("HOME, EDIT: the page changes back and again", oa_page == 1);

    /* HOME: a knob opens DRAWBARS; PRESET steps through the factory patches and wraps */
    press(B_HOME);
    bad += check("HOME: the first page", oa_page == 0);
    turn(EN_K1, 1);
    bad += check("  .. a knob there opens DRAWBARS and moves its bar", oa_page == 1 && org_patch[OP_UB + oa_win] > 0);
    press(B_HOME);
    turn(EN_PRESET, 1);
    bad += check("PRESET +1: the second factory patch, its LEVEL",
                 TSEL->preset == 1 && !memcmp(org_patch, ORG_FACTORY[1], ORG_PSIZE) && TSEL->p[P_LEVEL] == ORG_LEVEL[1]);
    turn(EN_PRESET, -1);
    turn(EN_PRESET, -1);
    bad += check("PRESET -2: wraps to the last factory patch", TSEL->preset == ORG_NFACTORY - 1u &&
                 TSEL->p[P_LEVEL] == ORG_LEVEL[ORG_NFACTORY - 1u]);
    turn(EN_PRESET, 1);
    bad += check("  .. and forward to the first again", TSEL->preset == 0);

    /* SAVE: the first free slot, the name shown, the patch kept with it */
    press(B_SAVE);
    bad += check("SAVE: the dialog opens on the first free slot", oa_sv == 1 && oa_sv_slot == 0);
    h_save = frame_hash();
    bad += check("  .. drawn differently", h_save != h_draw);
    press(B_SAVE);
    k = oa_sv_slot;
    bad += check("SAVE again: the preset is written to that slot", !oa_sv && up_used(k) && TSEL->user == k + 1u &&
                 up_rec(k)->engine == ENGI_ORGAN);
    bad += check("  .. with the patch", upf_get(k, pk) == 0 && !memcmp(pk, org_patch, ORG_PSIZE));
    bad += check("  .. and a message: SAVED", oa_msg_on() && !memcmp(oa_msg, "SAVED", 5));

    /* the user presets follow the factory ones in PRESET: the saved one is on the instrument now */
    bad += check("the saved preset plays", TSEL->user == k + 1u);
    turn(EN_PRESET, 1);
    bad += check("PRESET +1 from the user preset: the first factory patch", TSEL->user == 0 && TSEL->preset == 0);
    turn(EN_PRESET, -1);
    bad += check("  .. and back: the user preset", TSEL->user == k + 1u);

    /* GLO: MIDI IN, MIDI CHANNEL, ABOUT; a second GLO closes */
    fm_midi_src = 3; fm_midi_ch = 0;
    press(B_GLO);
    bad += check("GLO opens the list", oa_gl == 1);
    turn(EN_ALGO, -1);
    bad += check("  .. MIDI IN: ALL to MIDI", fm_midi_src == 1);
    turn(EN_SELECT, 1);
    turn(EN_ALGO, 1);
    bad += check("  .. MIDI CHANNEL 1", fm_midi_ch == 1);
    turn(EN_SELECT, 1);
    turn(EN_ALGO, 1);
    bad += check("  .. ABOUT", oa_gl == 3);
    turn(EN_ALGO, 1);
    bad += check("  .. an encoder goes back to the list", oa_gl == 1);
    press(B_GLO);
    bad += check("GLO again: closed", oa_gl == 0);

    /* octave keys: -2..2 */
    for (i = 0; i < 4u; i++) press(B_OCTUP);
    bad += check("OCT+ four times: the top octave", song.octave == 2);
    for (i = 0; i < 6u; i++) press(B_OCTDN);
    bad += check("OCT- six times: the bottom octave", song.octave == -2);

    printf("%s\n", bad ? "ORGAN APP TEST FAILED" : "organ app test passed");
    return bad != 0;
}
