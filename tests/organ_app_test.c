/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Guilherme Ruppert (organvave-1) */
/* The organ's screens and controls (firmware/src/organ_app.c) on the host: HOME and DRAWBARS drawn and told apart, the
 * drawbar controls (SELECT picks a bar, ALGORITHM pulls it, the knobs 1-4 pull the picked bar and the next three, the range
 * clamped; LOWER shares its page with the pedal's two bars), PRESET over the factory and the user
 * presets (wrapping), SAVE (the first free slot, the name, the patch kept), GLO (MIDI IN, MIDI CHANNEL, ABOUT closes),
 * the octave keys, and the FX pages PERC & VIB, ROTARY, DRIVE, REVERB, DELAY and EQ: SELECT picks a row, ALGORITHM changes it,
 * the knobs 1-4 go to the first four rows (with a directory argument the pages are written as .ppm). */
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

static const char *shot_dir;
int main(int argc, char **argv)
{
    int bad = 0;
    shot_dir = argc > 1 ? argv[1] : 0;
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

    /* drawbars: SELECT picks a bar, ALGORITHM pulls it, the knobs 1-4 pull the picked bar and the three after it, the range 0..8 */
    v0 = org_patch[OP_UB + oa_sel];
    turn(EN_K1, v0 < 8 ? 1 : -1);
    bad += check("K1 moves the picked bar by one step",
                 org_patch[OP_UB + oa_sel] == (v0 < 8 ? v0 + 1 : v0 - 1));
    turn(EN_K1, 20);
    bad += check("  .. a big turn clamps at 8", org_patch[OP_UB + oa_sel] == 8);
    turn(EN_K1, -20);
    bad += check("  .. and at 0", org_patch[OP_UB + oa_sel] == 0);
    k = oa_sel;
    turn(EN_SELECT, 1);
    bad += check("SELECT picks the next bar", oa_sel == (uint8_t)(k + 1u));
    for (i = 0; i < 12u; i++)
        turn(EN_SELECT, 1);
    bad += check("  .. to the last of the nine and no further", oa_sel == 8);
    for (i = 0; i < 12u; i++)                             /* (one bar a detent, as the encoder sends it) */
        turn(EN_SELECT, -1);
    bad += check("  .. and back to the first and no further", oa_sel == 0);
    org_set(OP_UB + 0u, 3u);
    turn(EN_ALGO, 2);
    bad += check("ALGORITHM pulls the picked bar (two detents)", org_patch[OP_UB + 0] == 5);
    turn(EN_ALGO, -20);
    bad += check("  .. and stops at 0", org_patch[OP_UB + 0] == 0);
    org_set(OP_UB + 0u, 3u);
    org_set(OP_UB + 1u, 8u);
    turn(EN_K2, -1);
    bad += check("K2 is the bar after the picked one", org_patch[OP_UB + 1] == 7 && org_patch[OP_UB + 0] == 3);
    for (i = 0; i < 3u; i++) { press(B_HOME); press(B_EDIT); }
    bad += check("HOME, EDIT: the page changes back and again", oa_page == 1);

    /* HOME: SELECT picks the region (cyclic), the knobs work on it and stay on HOME; ALGORITHM is the Leslie (not cyclic) */
    {
        uint32_t v0, v4;
        press(B_HOME);
        bad += check("HOME: the first page", oa_page == 0);
        org_set(OP_UB + 0u, 3u);
        turn(EN_K1, 1);
        bad += check("HOME: region 1 (bars 1-4): K1 moves bar 1, the page stays HOME", oa_page == 0 && org_patch[OP_UB + 0] == 4);
        {
            uint32_t sg = oa_signature();
            turn(EN_SELECT, 1);
            bad += check("  .. a new region is a new picture (the redraw follows SELECT)", oa_signature() != sg);
        }
        org_set(OP_UB + 4u, 4u);
        turn(EN_K1, -1);
        bad += check("HOME: SELECT +1, region 2 (bars 5-8): K1 moves bar 5", oa_page == 0 && org_patch[OP_UB + 4] == 3);
        turn(EN_SELECT, 4);
        bad += check("  .. SELECT +4 goes on, 6 regions: region 6 (the knobs)", oa_home_reg == 5);
        turn(EN_SELECT, 1);
        bad += check("  .. and wraps to region 1", oa_home_reg == 0);
        turn(EN_SELECT, -1);
        bad += check("  .. and back the other way to the knobs", oa_home_reg == 5);
        turn(EN_SELECT, -2);
        bad += check("  .. SELECT -2: region 4 (vibrato), no bar moved", oa_home_reg == 3);
        org_set(OP_VIB, 0u);
        turn(EN_K1 + 1, 1);
        bad += check("  .. vibrato: K2 (LO) on: V3", org_patch[OP_VIB] == 3);
        turn(EN_K1 + 2, 1);
        bad += check("  .. K3 turns the selector clockwise: C3", org_patch[OP_VIB] == 6);
        turn(EN_K1 + 2, 9);
        bad += check("  .. and stops at C2, the last position", org_patch[OP_VIB] == 5);
        turn(EN_K1 + 1, -1);
        bad += check("  .. K2 down: off", org_patch[OP_VIB] == 0);
        turn(EN_SELECT, 1);
        bad += check("  .. SELECT +1: region 5 (percussion)", oa_home_reg == 4);
        org_set(OP_PERC, 0u);
        turn(EN_K1, 1);
        bad += check("  .. percussion: K1 turns ON", org_patch[OP_PERC] == 1);
        turn(EN_SELECT, 1);
        bad += check("  .. SELECT +1: the four knobs", oa_home_reg == 5);
        v0 = org_patch[OP_DRV_GAIN];
        turn(EN_K1, 1);
        bad += check("  .. the knobs: K1 is DRIVE, and turns it on", org_patch[OP_DRV_GAIN] == v0 + 1 && org_patch[OP_DRV] == 1 && oa_page == 0);
        turn(EN_SELECT, -4);
        bad += check("  .. SELECT -4: region 2 (bar 9)", oa_home_reg == 1);
        turn(EN_SELECT, 1);
        bad += check("  .. SELECT +1: region 3 (bar 9 only)", oa_home_reg == 2);
        v4 = org_patch[OP_UB + 8];
        turn(EN_K1, 1);
        bad += check("  .. bar 9: K1 moves it, K2 to K4 nothing", org_patch[OP_UB + 8] == (v4 < 8 ? v4 + 1 : 8));
        org_set(OP_ROT, 0u);
        turn(EN_ALGO, 1);
        bad += check("  .. ALGORITHM is the Leslie: from STOP to FAST", org_patch[OP_ROT] == 1 && org_patch[OP_ROT_SPEED] == 2);
        turn(EN_ALGO, 3);
        bad += check("  .. and stops at FAST (not cyclic)", org_patch[OP_ROT] == 1 && org_patch[OP_ROT_SPEED] == 2);
        turn(EN_ALGO, -5);
        bad += check("  .. down to SLOW and stops there", org_patch[OP_ROT] == 1 && org_patch[OP_ROT_SPEED] == 1);
        turn(EN_ALGO, 1);
        bad += check("  .. up once: STOP", org_patch[OP_ROT] == 0);
    }
    /* PRESET steps through the factory patches and wraps */
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

    /* GLO: MIDI IN, the three channels, MIDI CC, ABOUT; a second GLO closes */
    fm_midi_src = 3; fm_midi_base(0);
    oa_gl_row = 0;
    press(B_GLO);
    bad += check("GLO opens the list", oa_gl == 1);
    turn(EN_ALGO, -1);
    bad += check("  .. MIDI IN: ALL to MIDI", fm_midi_src == 1);
    turn(EN_SELECT, 1);
    bad += check("  .. UPPER defaults to ALL", fm_ch_up == 0);
    turn(EN_ALGO, 1);
    bad += check("  .. UPPER CHANNEL 1", fm_ch_up == 1);
    turn(EN_ALGO, -1);
    bad += check("  .. and back to ALL", fm_ch_up == 0);
    turn(EN_SELECT, 1);
    bad += check("  .. LOWER defaults to 2", fm_ch_lo == 2);
    turn(EN_ALGO, 1);
    bad += check("  .. LOWER CHANNEL 3", fm_ch_lo == 3);
    for (i = 0; i < 4u; i++) turn(EN_ALGO, -1);
    bad += check("  .. LOWER OFF at the bottom", fm_ch_lo == 0);
    turn(EN_ALGO, 1);
    turn(EN_SELECT, 1);
    bad += check("  .. PEDAL defaults to 3", fm_ch_pd == 3);
    turn(EN_ALGO, 1);
    bad += check("  .. PEDAL CHANNEL 4", fm_ch_pd == 4);
    turn(EN_SELECT, 1);
    turn(EN_ALGO, 1);
    bad += check("  .. MIDI CC opens its list", oa_gl == 2);
    {
        uint32_t learn_ok;
        oa_cc_row = 0;                                   /* UPPER 16' */
        midi_control(0, 21, 100);
        oa_cc_poll();
        bad += check("  .. a CC arriving is learned for the highlighted control", oa_ccmap[0] == 21);
        turn(EN_SELECT, 1);
        midi_control(0, 22, 50);
        oa_cc_poll();
        bad += check("  .. the next control takes the next CC", oa_ccmap[1] == 22 && oa_ccmap[0] == 21);
        midi_control(0, 21, 50);
        oa_cc_poll();
        bad += check("  .. a CC taken by another control moves over", oa_ccmap[1] == 21 && oa_ccmap[0] == 0);
        midi_control(0, 7, 50);
        midi_control(0, 72, 50);
        oa_cc_poll();
        bad += check("  .. the CCs with a meaning of their own are not learned", oa_ccmap[1] == 21);
        turn(EN_ALGO, 1);
        learn_ok = oa_ccmap[1] == 22;
        bad += check("  .. ALGORITHM steps to the next free CC (22)", learn_ok);
        for (i = 0; i < 40u; i++)
            turn(EN_ALGO, -1);
        bad += check("  .. and down to OFF (CC 1, 7 and the others with a meaning of their own skipped)", oa_ccmap[1] == 0);
        oa_cc_assign(oa_cc_row, 30);
        turn(EN_SELECT, -1);
        oa_cc_assign(oa_cc_row, 31);
        press(B_GLO);
        bad += check("  .. GLO goes back to the list", oa_gl == 1);
        press(B_GLO);
        bad += check("  .. and closes", oa_gl == 0);
        org_set(OP_UB + 0, 0);
        midi_control(0, 31, 127);
        oa_cc_poll();
        bad += check("a learned CC moves its control over the range (127: 8)", org_patch[OP_UB] == 8);
        midi_control(0, 31, 0);
        oa_cc_poll();
        bad += check("  .. and 0: 0", org_patch[OP_UB] == 0);
        oa_cc_assign(OP_XT_COMP, 40);
        org_set(OP_XTALK, 0);
        midi_control(0, 40, 64);
        oa_cc_poll();
        bad += check("a crosstalk CC switches the crosstalk on", org_patch[OP_XTALK] == 1);
        oa_cc_assign(OP_SPLIT_PT, 41);
        midi_control(0, 41, 127);
        oa_cc_poll();
        bad += check("a CC on SPLIT POINT: the top note", org_ci(OP_SPLIT_PT, org_patch[OP_SPLIT_PT]) == 60);
        oa_cc_assign(OP_SPLIT_PT, 0);
        oa_cc_assign(OP_XT_COMP, 0);
        oa_cc_assign(OP_UB, 0);
        oa_cc_assign(1, 0);
        oa_cfg_dirty = 1;
        oa_cfg_save();
    }
    fm_midi_base(0);
    press(B_GLO);
    oa_gl_row = 5;
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

    /* PERC & VIB (0.2.0): FX opens the page, ENV and LFO switch the percussion and the vibrato on HOME, the knobs and
     * ALGORITHM on the page; the lamps and the picture follow the patch */
    {
        uint32_t h_pv, h_home2;
        press(B_HOME);
        h_home2 = frame_hash();
        press(B_FX);
        bad += check("FX: the PERC & VIB page", oa_page == 2);
        h_pv = frame_hash();
        bad += check("  .. a different picture", h_pv != h_home2);
        turn(EN_K1, 1);
        bad += check("  .. KNOB 1 clockwise switches the percussion on", org_patch[OP_PERC] == 1);
        turn(EN_K1, -1);
        bad += check("  .. counter-clockwise, off again", org_patch[OP_PERC] == 0);
        turn(EN_K2, 1);
        bad += check("  .. KNOB 2: soft", org_patch[OP_PERC_SOFT] == 1);
        turn(EN_K3, 1);
        bad += check("  .. KNOB 3: fast", org_patch[OP_PERC_FAST] == 1);
        turn(EN_K4, 1);
        bad += check("  .. KNOB 4: the 2 2/3' third", org_patch[OP_PERC_SEL] == 1);
        bad += check("  .. SELECT starts on the first row", oa_row[2] == 0);
        for (i = 0; i < 4u; i++)
            turn(EN_SELECT, 1);
        bad += check("  .. SELECT picks the fifth row, the vibrato", oa_row[2] == 4);
        turn(EN_ALGO, 1);
        bad += check("  .. ALGORITHM: VIB 1", org_patch[OP_VIB] == 1);
        turn(EN_ALGO, 9);
        bad += check("  .. the selector stops at CHO 3", org_patch[OP_VIB] == 6);
        turn(EN_SELECT, 1);
        bad += check("  .. SELECT past the last row stays on it (the page stays)", oa_row[2] == 4 && oa_page == 2);
        for (i = 0; i < 4u; i++)
            turn(EN_SELECT, -1);
        bad += check("  .. and back to the first row", oa_row[2] == 0);
        turn(EN_ALGO, 1);
        bad += check("  .. ALGORITHM on the first row: the percussion on", org_patch[OP_PERC] == 1);
        turn(EN_ALGO, -1);
        bad += check("  .. and off", org_patch[OP_PERC] == 0);
        org_set(OP_PERC_SOFT, 0); org_set(OP_PERC_FAST, 0); org_set(OP_PERC_SEL, 0); org_set(OP_VIB, 6);
        press(B_FX);
        bad += check("FX again: the ROTARY page", oa_page == 3);
        press(B_FX);
        bad += check("FX again: the DRIVE page", oa_page == 4);
        press(B_FX);
        bad += check("FX again: the REVERB page", oa_page == 5);
        press(B_FX);
        bad += check("FX again: the DELAY page", oa_page == 6);
        press(B_FX);
        bad += check("FX again: the EQ page", oa_page == 7);
        press(B_FX);
        bad += check("FX again: HOME", oa_page == 0);
        press(B_ENV);
        bad += check("ENV on HOME: the percussion on", org_patch[OP_PERC] == 1);
        press(B_LFO);
        bad += check("LFO: the vibrato to CHO 3 and round to off", org_patch[OP_VIB] == 0);
        press(B_LFO);
        bad += check("  .. and to VIB 1", org_patch[OP_VIB] == 1);
        press(B_ENV);
        bad += check("ENV again: the percussion off", org_patch[OP_PERC] == 0);
    }

    /* ROTARY and DRIVE (0.3.0): the pages, the knobs, SCL */
    {
        uint32_t h_r, h_d;
        press(B_HOME);
        org_set(OP_ROT, 0);                                   /* (the preset on air may have it on) */
        bad += check("SCL with the rotary off: ROTARY on at SLOW", (press(B_SCL), org_patch[OP_ROT] == 1 && org_patch[OP_ROT_SPEED] == 1));
        press(B_SCL);
        bad += check("SCL again: FAST", org_patch[OP_ROT_SPEED] == 2);
        press(B_SCL);
        bad += check("SCL again: STOP", org_patch[OP_ROT_SPEED] == 0);
        press(B_SCL);
        bad += check("SCL again: SLOW", org_patch[OP_ROT_SPEED] == 1);
        press(B_FX); press(B_FX);
        bad += check("FX twice from HOME: the ROTARY page (PERC & VIB first)", oa_page == 3);
        h_r = frame_hash();
        turn(EN_K1, -1);
        bad += check("ROTARY: KNOB 1 counter-clockwise switches it off", org_patch[OP_ROT] == 0);
        turn(EN_K1, 1);
        turn(EN_K2, 1);
        bad += check("  .. KNOB 2: LIGHT", org_patch[OP_ROT_MODEL] == 1);
        turn(EN_K3, 1);
        bad += check("  .. KNOB 3: the sharp horn", org_patch[OP_ROT_HORN] == 1);
        org_set(OP_ROT_SPEED, 1);
        turn(EN_K4, 1);
        bad += check("  .. KNOB 4: FAST", org_patch[OP_ROT_SPEED] == 2);
        turn(EN_K4, 1);
        bad += check("  .. KNOB 4 stops at FAST", org_patch[OP_ROT_SPEED] == 2);
        bad += check("  .. twelve rows: the list scrolls", OA_NR[3] == 12 && oa_row[3] == 0 && oa_top[3] == 0);
        for (i = 0; i < 4u; i++)
            turn(EN_SELECT, 1);
        bad += check("  .. SELECT picks the fifth row: the width", oa_row[3] == 4);
        turn(EN_ALGO, -1);
        bad += check("  .. ALGORITHM counter-clockwise narrows the image", org_patch[OP_ROT_MIC] == 1);
        for (i = 0; i < 8u; i++)
            turn(EN_ALGO, -1);
        bad += check("  .. and stops at mono", org_patch[OP_ROT_MIC] == 6);
        turn(EN_SELECT, 1);
        org_set(OP_ROT_BAL, 4);
        turn(EN_ALGO, -1);
        bad += check("  .. SELECT then ALGORITHM: the horn level", oa_row[3] == 5 && org_patch[OP_ROT_BAL] == 3);
        {   /* the Leslie's own parameters: the speeds and the ramps, nine steps each, the byte 0 the default */
            static const uint8_t OPS[6] = {OP_ROT_HSLOW, OP_ROT_HFAST, OP_ROT_DSLOW, OP_ROT_DFAST, OP_ROT_HRAMP, OP_ROT_DRAMP};
            uint32_t j;
            for (j = 0; j < 6u; j++) {
                turn(EN_SELECT, 1);
                bad += check("  .. SELECT picks the next Leslie parameter", oa_row[3] == 6u + j);
                bad += check("  .. it starts at the default (byte 0)", org_patch[OPS[j]] == 0);
                turn(EN_ALGO, 1);
                bad += check("  .. one step up (byte 1, index 5)", org_patch[OPS[j]] == 1 && org_c9(org_patch[OPS[j]]) == 5);
                turn(EN_ALGO, 40);
                bad += check("  .. stops at the top (index 8)", org_c9(org_patch[OPS[j]]) == 8);
                turn(EN_ALGO, -40);
                bad += check("  .. and at the bottom (index 0)", org_c9(org_patch[OPS[j]]) == 0);
                org_set(OPS[j], 0);
            }
            (void)frame_hash();
            bad += check("  .. the list scrolled to keep the pick in view", oa_top[3] + OA_VIS > oa_row[3] && oa_top[3] <= oa_row[3] && oa_top[3] > 0);
            turn(EN_SELECT, 1);
            (void)frame_hash();
            bad += check("  .. past the last row it stays on it (the list does not wrap)", oa_row[3] == 11 && oa_top[3] == 12 - OA_VIS);
        }
        bad += check("  .. a different picture from the first", frame_hash() != h_r);
        press(B_FX);
        bad += check("FX: the DRIVE page", oa_page == 4);
        h_d = frame_hash();
        turn(EN_K1, 1);
        bad += check("DRIVE: KNOB 1 switches it on", org_patch[OP_DRV] == 1);
        bad += check("  .. five rows, no character", OA_NR[4] == 5 && !strcmp(OA_LB[4][1], "TYPE") && !strcmp(OA_LB[4][2], "GAIN") &&
                     !strcmp(OA_LB[4][3], "TONE") && !strcmp(OA_LB[4][4], "OUTPUT"));
        turn(EN_K2, 1);
        bad += check("  .. KNOB 2: the type TUBE", org_patch[OP_DRV_TYPE] == 1);
        turn(EN_K2, -1);
        bad += check("  .. and DISTORTION", org_patch[OP_DRV_TYPE] == 0);
        org_set(OP_DRV_GAIN, 6);
        turn(EN_K3, 1);
        bad += check("  .. KNOB 3: the gain up a step", org_patch[OP_DRV_GAIN] == 7);
        for (i = 0; i < 14u; i++)
            turn(EN_K3, -1);
        bad += check("  .. and down to the bottom", org_patch[OP_DRV_GAIN] == 0);
        turn(EN_K4, 1);
        bad += check("  .. KNOB 4: the tone up a step (byte 1, index 5)", org_patch[OP_DRV_TONE] == 1 && org_c9(org_patch[OP_DRV_TONE]) == 5);
        for (i = 0; i < 4u; i++)
            turn(EN_SELECT, 1);
        bad += check("  .. SELECT picks the fifth row, the output", oa_row[4] == 4);
        turn(EN_ALGO, -2);
        bad += check("  .. ALGORITHM: the output down two steps (-4 dB, index 2)", org_c9(org_patch[OP_DRV_OUT]) == 2);
        bad += check("  .. a different picture", frame_hash() != h_d);
        org_set(OP_DRV_TONE, 0); org_set(OP_DRV_OUT, 0); org_set(OP_DRV_GAIN, 6);
        /* EDIT walks the manuals' drawbars: UPPER, LOWER, PEDAL (the last two play over MIDI) */
        {
            uint32_t h_up, h_lo;
            press(B_HOME);
            press(B_EDIT);
            bad += check("EDIT: the DRAWBARS page, the upper manual", oa_page == 1 && oa_man == 0);
            h_up = frame_hash();
            press(B_EDIT);
            bad += check("EDIT again: the lower manual with the pedal's two bars (a different picture)", oa_page == 1 && oa_man == 1 && frame_hash() != h_up);
            h_lo = frame_hash();
            bad += check("  .. eleven bars: SELECT picks the pedal's 16' first", oa_sel == 0);
            org_set(OP_PB + 0, 3);
            org_set(OP_PB + 2, 3);
            org_set(OP_LB + 0, 3);
            org_set(OP_UB + 0, 8);
            turn(EN_K1, 1);
            turn(EN_K2, 1);
            turn(EN_K3, 1);
            bad += check("  .. the knobs pull the pedal's 16' and 8', then the lower's 16'",
                         org_patch[OP_PB + 0] == 4 && org_patch[OP_PB + 2] == 4 && org_patch[OP_LB + 0] == 4 && org_patch[OP_UB + 0] == 8);
            bad += check("  .. the other pedal bytes are not touched", org_patch[OP_PB + 1] == 0 && org_patch[OP_PB + 3] == 0 && org_patch[OP_PB + 8] == 0);
            bad += check("  .. and the picture follows", frame_hash() != h_lo);
            for (i = 0; i < 20u; i++)
                turn(EN_SELECT, 1);
            bad += check("  .. SELECT runs to the lower's 1' (the eleventh bar) and stops", oa_sel == 10);
            org_set(OP_LB + 8, 2);
            turn(EN_ALGO, 3);
            bad += check("  .. ALGORITHM pulls it", org_patch[OP_LB + 8] == 5);
            for (i = 0; i < 20u; i++)
                turn(EN_SELECT, -1);
            turn(EN_SELECT, 1);
            turn(EN_ALGO, -1);
            bad += check("  .. the pedal's 8' is the second bar", oa_sel == 1 && org_patch[OP_PB + 2] == 3);
            press(B_EDIT);
            bad += check("EDIT again: ADVANCED EDIT (a different picture)", oa_page == 8 && frame_hash() != h_lo);
            press(B_EDIT);
            bad += check("EDIT again: back to the upper manual (no pedal page)", oa_man == 0 && oa_page == 1 && oa_sel == 0);
            press(B_EDIT); press(B_EDIT); press(B_EDIT); press(B_HOME); press(B_EDIT);
            bad += check("  .. and from HOME it opens the upper manual", oa_man == 0 && oa_page == 1);
            org_set(OP_PB + 0, 0); org_set(OP_PB + 2, 0); org_set(OP_LB + 0, 0); org_set(OP_LB + 8, 0);
        }
        /* ADVANCED EDIT (0.9.4, 0.9.7): EDIT, EDIT, EDIT; SELECT picks a row, ALGORITHM changes it by one step, the knobs 1-4 go to rows 1-4.
         * 0.9.7: no TUNING and no TEMPERAMENT rows (60 Hz gear and A 440 are fixed): the first row is SPLIT */
        {
            uint32_t h_a, r;
            memset(org_patch + OP_TUNING, 0, ORG_PSIZE - OP_TUNING);
            press(B_HOME);
            press(B_EDIT); press(B_EDIT); press(B_EDIT);
            bad += check("ADVANCED EDIT: the page after DRAWBARS' lower manual", oa_page == 8);
            h_a = frame_hash();
            bad += check("  .. 29 rows, the first picked (SPLIT: no TUNING, no TEMPERAMENT row)", OA_NR[8] == 29 && oa_row[8] == 0 && !strcmp(OA_LB[8][0], "SPLIT"));
            turn(EN_ALGO, 1);
            bad += check("  .. ALGORITHM on the first row switches SPLIT on", org_patch[OP_SPLIT] == 1 && frame_hash() != h_a);
            turn(EN_ALGO, -1);
            bad += check("  .. and off again", org_patch[OP_SPLIT] == 0);
            turn(EN_SELECT, 1);
            bad += check("  .. SELECT picks the next row", oa_row[8] == 1);
            turn(EN_SELECT, -1);
            bad += check("  .. the tuning is fixed: the patch's TUNING byte is not read (60 Hz gear, A 440)", org_ci(OP_TUNING, 0) == 64 && !org.temp && org.wtab == ORG_WINC);
            turn(EN_K1, 1);
            bad += check("  .. knob 1 is SPLIT: OFF -> ON", org_patch[OP_SPLIT] == 1);
            turn(EN_K2, 3);
            bad += check("  .. knob 2 is SPLIT POINT: G3 -> G#3 .. (3 steps up: B3)", org_ci(OP_SPLIT_PT, org_patch[OP_SPLIT_PT]) == 22);
            turn(EN_K1, -1);
            turn(EN_K2, 3);
            bad += check("  .. SPLIT POINT waits while the split is off", org_ci(OP_SPLIT_PT, org_patch[OP_SPLIT_PT]) == 22);
            org_set(OP_SPLIT_PT, 0);
            for (i = 0; i < 40u; i++)
                turn(EN_SELECT, 1);
            bad += check("  .. SELECT stops at the last row (no wrap)", oa_row[8] == 28);
            for (i = 0; i < 40u; i++)
                turn(EN_SELECT, -1);
            bad += check("  .. and at the first", oa_row[8] == 0);
            for (r = 0; r < 29u; r++) {
                uint32_t op = OA_AOP[r], rows_ok;
                uint8_t before = org_patch[op];
                oa_row[8] = (uint8_t)r;
                oa_top[8] = 0;
                turn(EN_ALGO, 1);
                turn(EN_ALGO, -1);
                rows_ok = op == OP_XTALK || op == OP_SPLIT ? org_patch[op] == 0u : org_ci(op, org_patch[op]) == org_ci(op, before) || org_ci(op, before) == org_pmax(op);   /* (at the top the step up stays, the one down moves) */
                if (!rows_ok) printf("row %u op %u before %u after %u\n", r, op, before, org_patch[op]);
                bad += check("  .. every row: a step up and down is a round trip (or the end stop)", rows_ok);
                bad += org_patch[op] <= org_pmax(op) ? 0 : check("  .. every row stays in range", 0);
            }
            {
                oa_val_t v;
                memset(org_patch + OP_TUNING, 0, ORG_PSIZE - OP_TUNING);
                oa_val(8u, 22u, &v);
                bad += check("  .. PERC TRIGGER 1'", !strcmp(v.t, "1'"));
                oa_val(8u, 23u, &v);
                bad += check("  .. SCAN RATE 7.25 HZ", !strcmp(v.t, "7.25 HZ"));
                oa_val(8u, 12u, &v);
                bad += check("  .. WIRING -40 DB", !strcmp(v.t, "-40 DB"));
                oa_val(8u, 13u, &v);
                bad += check("  .. XT FLOOR -96 DB", !strcmp(v.t, "-96 DB"));
                oa_val(8u, 9u, &v);
                bad += check("  .. COMPARTMENT -40 DB", !strcmp(v.t, "-40 DB"));
                oa_val(8u, 14u, &v);
                bad += check("  .. XT MIN OFF", !strcmp(v.t, "OFF"));
                oa_val(8u, 15u, &v);
                bad += check("  .. PERC FAST 1.0 S", !strcmp(v.t, "1.0 S"));
                oa_val(8u, 19u, &v);
                bad += check("  .. PERC GAIN 11.0", !strcmp(v.t, "11.0"));
                oa_val(8u, 7u, &v);
                bad += check("  .. RELEASE MODEL CLICK (the default since 0.9.5)", !strcmp(v.t, "CLICK"));
                oa_val(8u, 2u, &v);
                bad += check("  .. ATTACK CLICK 100 % by default (0.9.5)", !strcmp(v.t, "100 %"));
                oa_val(8u, 3u, &v);
                bad += check("  .. RELEASE CLICK 50.0 % by default (0.9.5)", !strcmp(v.t, "50.0 %"));
                org_set(OP_ATK_LEVEL, org_bi(OP_ATK_LEVEL, 40));
                oa_val(8u, 2u, &v);
                bad += check("  .. ATTACK CLICK up to 200 %", !strcmp(v.t, "200 %"));
                oa_val(8u, 0u, &v);
                bad += check("  .. SPLIT OFF by default", !strcmp(v.t, "OFF") && !v.on);
                oa_val(8u, 1u, &v);
                bad += check("  .. SPLIT POINT G3 by default, dim while the split is off", !strcmp(v.t, "G3") && !v.on);
                org_set(OP_SPLIT, 1);
                oa_val(8u, 1u, &v);
                bad += check("  .. SPLIT POINT lit with the split on", v.on);
                org_set(OP_SPLIT_PT, org_bi(OP_SPLIT_PT, 60));
                oa_val(8u, 1u, &v);
                bad += check("  .. SPLIT POINT tops at C7 (96)", !strcmp(v.t, "C7"));
                org_set(OP_SPLIT_PT, org_bi(OP_SPLIT_PT, 1));
                oa_val(8u, 1u, &v);
                bad += check("  .. and the bottom C#2 (37) is the second", !strcmp(v.t, "C#2"));
                org_set(OP_XTALK, 0);
                oa_val(8u, 9u, &v);
                bad += check("  .. the crosstalk rows are dim with CROSSTALK off", !v.on);
                oa_row[8] = 9;
                turn(EN_ALGO, 1);
                bad += check("  .. and changing one switches CROSSTALK on", org_patch[OP_XTALK] == 1);
            }
            memset(org_patch + OP_TUNING, 0, ORG_PSIZE - OP_TUNING);
            press(B_EDIT);
            bad += check("  .. EDIT goes back to the upper manual", oa_page == 1 && oa_man == 0);
            press(B_HOME);
        }
        /* REVERB and DELAY (0.4.0): the send is the part's, the rest the instrument's globals */
        {
            uint32_t h_rv, h_dl;
            int16_t sz;
            TSEL->p[P_REV] = 0; TSEL->p[P_DLY] = 0;
            song.g[G_RTYPE] = 1; song.g[G_RSIZE] = 64; song.g[G_RDAMP] = 64;
            song.g[G_DTIME] = 0; song.g[G_DRREV] = 64; song.g[G_DFDBK] = 60; song.g[G_DCOLOR] = 64; song.g[G_DMIX] = 64;
            oa_page = 5;
            h_rv = frame_hash();
            turn(EN_K1, 1);
            bad += check("REVERB: KNOB 1 raises the send (8 a detent)", TSEL->p[P_REV] == 8);
            turn(EN_K1, -3);
            bad += check("  .. and stops at 0", TSEL->p[P_REV] == 0);
            for (i = 0; i < 20u; i++)
                turn(EN_K1, 1);
            bad += check("  .. and at 127", TSEL->p[P_REV] == 127);
            turn(EN_K2, 1);
            bad += check("  .. KNOB 2: the type", song.g[G_RTYPE] == 2);
            for (i = 0; i < 4u; i++)
                turn(EN_K2, 1);
            bad += check("  .. round the five types", song.g[G_RTYPE] == 1);
            turn(EN_K2, -1); turn(EN_K2, -1);
            bad += check("  .. and backwards round", song.g[G_RTYPE] == 4);
            sz = song.g[G_RSIZE];
            turn(EN_K3, 1);
            turn(EN_K4, -1);
            bad += check("  .. KNOB 3: size, KNOB 4: tone", song.g[G_RSIZE] == sz + 8 && song.g[G_RDAMP] == 56);
            bad += check("  .. a different picture", frame_hash() != h_rv);
            press(B_FX);
            bad += check("REVERB: FX goes on to DELAY", oa_page == 6);
            h_dl = frame_hash();
            turn(EN_K1, 2);
            turn(EN_K2, -1);
            turn(EN_K3, 1);
            turn(EN_K4, 1);
            for (i = 0; i < 4u; i++)
                turn(EN_SELECT, 1);
            turn(EN_ALGO, -1);
            turn(EN_SELECT, 1);
            turn(EN_ALGO, 1);
            bad += check("DELAY: send (two detents), type (wraps back to REVERSE), time, feedback; SELECT then ALGORITHM: colour, mix",
                         TSEL->p[P_DLY] == 16 && song.g[G_DTIME] == 6 && song.g[G_DRREV] == 72 && song.g[G_DFDBK] == 68 &&
                         song.g[G_DCOLOR] == 56 && song.g[G_DMIX] == 72 && oa_row[6] == 5);
            for (i = 0; i < 30u; i++)
                turn(EN_K4, 1);
            bad += check("  .. the feedback stops at 120", song.g[G_DFDBK] == 120);
            bad += check("  .. a different picture", frame_hash() != h_dl);
            oa_page = 4;
        }
        /* EQ (0.5.0): on, LOW, MID, HIGH in 1 dB steps (0 is flat), the MID's frequency on ALGORITHM */
        {
            uint32_t h_eq, i;
            org_set(OP_EQ, 0); org_set(OP_EQ_LOW, 0); org_set(OP_EQ_MID, 0); org_set(OP_EQ_HIGH, 0); org_set(OP_EQ_FREQ, 0);
            oa_page = 6;
            press(B_FX);
            bad += check("DELAY: FX goes on to the EQ page", oa_page == 7);
            h_eq = frame_hash();
            turn(EN_K1, 1);
            bad += check("EQ: KNOB 1 switches it on", org_patch[OP_EQ] == 1);
            turn(EN_K2, 1);
            bad += check("  .. KNOB 2: LOW up 1 dB (byte 1)", eq_db(org_patch[OP_EQ_LOW]) == 1 && org_patch[OP_EQ_LOW] == 1);
            turn(EN_K2, -1); turn(EN_K2, -1);
            bad += check("  .. and below flat: -1 dB is byte 24", eq_db(org_patch[OP_EQ_LOW]) == -1 && org_patch[OP_EQ_LOW] == 24);
            for (i = 0; i < 20u; i++)
                turn(EN_K2, -1);
            bad += check("  .. stops at -12 dB", eq_db(org_patch[OP_EQ_LOW]) == -12);
            for (i = 0; i < 30u; i++)
                turn(EN_K3, 1);
            bad += check("  .. KNOB 3: MID stops at +12 dB", eq_db(org_patch[OP_EQ_MID]) == 12);
            turn(EN_K4, 1);
            bad += check("  .. KNOB 4: HIGH up 1 dB", eq_db(org_patch[OP_EQ_HIGH]) == 1);
            for (i = 0; i < 4u; i++)
                turn(EN_SELECT, 1);
            bad += check("  .. SELECT picks the fifth row, the MID's frequency", oa_row[7] == 4);
            turn(EN_ALGO, 1);
            bad += check("  .. ALGORITHM: the MID's frequency 900 -> 1280 Hz", EQ_FREQ_HZ[eq_fidx(org_patch[OP_EQ_FREQ])] == 1280);
            for (i = 0; i < 20u; i++)
                turn(EN_ALGO, 1);
            bad += check("  .. up to 7.2 kHz and stops", EQ_FREQ_HZ[eq_fidx(org_patch[OP_EQ_FREQ])] == 7200);
            for (i = 0; i < 20u; i++)
                turn(EN_ALGO, -1);
            bad += check("  .. down to 160 Hz and stops", EQ_FREQ_HZ[eq_fidx(org_patch[OP_EQ_FREQ])] == 160);
            bad += check("  .. a different picture", frame_hash() != h_eq);
            org_set(OP_EQ, 0); org_set(OP_EQ_LOW, 0); org_set(OP_EQ_MID, 0); org_set(OP_EQ_HIGH, 0); org_set(OP_EQ_FREQ, 0);
            oa_page = 4;
        }
        if (shot_dir) {
            static const char *const N[10] = {"home", "drawbars", "perc", "rotary", "drive", "reverb", "delay", "eq", "lower", "advanced"};
            uint32_t pg;
            org_set(OP_ROT, 1);
            org_set(OP_DRV, 1);
            oa_msg_until = 0;
            oa_pp_until = 0;
            for (pg = 0; pg < 10u; pg++) {
                char path[200];
                FILE *f;
                if (pg == 7u) {                               /* (the EQ's shot shows something set) */
                    org_set(OP_EQ, 1); org_set(OP_EQ_LOW, eq_byte(5)); org_set(OP_EQ_MID, eq_byte(-3)); org_set(OP_EQ_FREQ, eq_fbyte(7));
                    org_set(OP_EQ_HIGH, eq_byte(2));
                }
                oa_page = (uint8_t)(pg == 9u ? 8u : pg == 8u ? 1u : pg);
                oa_man = (uint8_t)(pg == 8u);
                oa_sel = (uint8_t)(pg == 8u ? 3u : 0u);
                (void)frame_hash();
                snprintf(path, sizeof path, "%s/%s.ppm", shot_dir, N[pg]);
                f = fopen(path, "wb");
                if (f) {
                    fprintf(f, "P6\n240 240\n255\n");
                    for (i = 0; i < 240u * 240u; i++) {
                        uint16_t c = (uint16_t)((host_screen[i] >> 8) | (host_screen[i] << 8));     /* (host.h stores the LCD's byte order) */
                        unsigned char px[3] = {(unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31)};
                        fwrite(px, 1, 3, f);
                    }
                    fclose(f);
                }
            }
        }
        press(B_HOME);
    }

    printf("%s\n", bad ? "ORGAN APP TEST FAILED" : "organ app test passed");
    return bad != 0;
}
