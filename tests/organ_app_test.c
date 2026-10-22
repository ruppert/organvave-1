/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Guilherme Ruppert (organvave-1) */
/* The organ's screens and controls (firmware/src/organ_app.c) on the host: HOME and DRAWBARS drawn and told apart, the
 * drawbar knobs (the four marked bars, SELECT moves them, the range clamped), PRESET over the factory and the user
 * presets (wrapping), SAVE (the first free slot, the name, the patch kept), GLO (MIDI IN, MIDI CHANNEL, ABOUT closes),
 * the octave keys, PERC & VIB, ROTARY and DRIVE (the knobs, SCL; with a directory argument the pages are written as .ppm). */
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
        bad += check("  .. KNOB 1 switches the percussion on", org_patch[OP_PERC] == 1);
        turn(EN_K1, 1);
        bad += check("  .. and off again", org_patch[OP_PERC] == 0);
        turn(EN_K2, 1);
        bad += check("  .. KNOB 2: soft", org_patch[OP_PERC_SOFT] == 1);
        turn(EN_K3, 1);
        bad += check("  .. KNOB 3: fast", org_patch[OP_PERC_FAST] == 1);
        turn(EN_K4, 1);
        bad += check("  .. KNOB 4: the 2 2/3' third", org_patch[OP_PERC_SEL] == 1);
        turn(EN_ALGO, 1);
        bad += check("  .. ALGORITHM: VIB 1", org_patch[OP_VIB] == 1);
        turn(EN_ALGO, 9);
        bad += check("  .. the selector stops at CHO 3", org_patch[OP_VIB] == 6);
        {
            uint8_t before = org_patch[OP_UB + oa_win];
            turn(EN_SELECT, 1);
            bad += check("  .. SELECT moves no bar here (the page stays)", oa_page == 2 && org_patch[OP_UB + oa_win] == before);
        }
        press(B_FX);
        bad += check("FX again: the ROTARY page", oa_page == 3);
        press(B_FX);
        bad += check("FX again: the DRIVE page", oa_page == 4);
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
        turn(EN_K1, 1);
        bad += check("ROTARY: KNOB 1 switches it off", org_patch[OP_ROT] == 0);
        turn(EN_K1, 1);
        turn(EN_K2, 1);
        bad += check("  .. KNOB 2: LIGHT", org_patch[OP_ROT_MODEL] == 1);
        turn(EN_K3, -1);
        bad += check("  .. KNOB 3: the sharp horn", org_patch[OP_ROT_HORN] == 1);
        turn(EN_K4, 1);
        bad += check("  .. KNOB 4: FAST", org_patch[OP_ROT_SPEED] == 2);
        turn(EN_K4, 1);
        bad += check("  .. KNOB 4 stops at FAST", org_patch[OP_ROT_SPEED] == 2);
        turn(EN_ALGO, -1);
        bad += check("  .. ALGORITHM counter-clockwise narrows the image", org_patch[OP_ROT_MIC] == 1);
        for (i = 0; i < 8u; i++)
            turn(EN_ALGO, -1);
        bad += check("  .. and stops at mono", org_patch[OP_ROT_MIC] == 6);
        turn(EN_SELECT, 1);
        org_set(OP_ROT_BAL, 4);
        turn(EN_SELECT, -1);
        bad += check("  .. SELECT: the horn level", org_patch[OP_ROT_BAL] == 3);
        bad += check("  .. a different picture from the first", frame_hash() != h_r);
        press(B_FX);
        bad += check("FX: the DRIVE page", oa_page == 4);
        h_d = frame_hash();
        turn(EN_K1, 1);
        bad += check("DRIVE: KNOB 1 switches it on", org_patch[OP_DRV] == 1);
        org_set(OP_DRV_GAIN, 6);
        turn(EN_K2, 1);
        bad += check("  .. KNOB 2: the gain up a step", org_patch[OP_DRV_GAIN] == 7);
        for (i = 0; i < 14u; i++)
            turn(EN_K2, -1);
        bad += check("  .. and down to the bottom", org_patch[OP_DRV_GAIN] == 0);
        turn(EN_K3, 3);
        bad += check("  .. KNOB 3: the character", org_patch[OP_DRV_FAT] == 1);
        bad += check("  .. a different picture", frame_hash() != h_d);
        if (shot_dir) {
            static const char *const N[5] = {"home", "drawbars", "perc", "rotary", "drive"};
            uint32_t pg;
            org_set(OP_ROT, 1);
            org_set(OP_DRV, 1);
            oa_msg_until = 0;
            oa_pp_until = 0;
            for (pg = 0; pg < 5u; pg++) {
                char path[200];
                FILE *f;
                oa_page = (uint8_t)pg;
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
