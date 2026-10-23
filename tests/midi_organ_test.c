/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The organ over MIDI (0.4.0, setBfree's map) on the host, through the real event path (midi_in_q -> events_block):
 * 1. the channel picks the manual: 1 upper, 2 lower, 3 pedals (omni; the other channels play the upper); the key ranges
 *    (upper and lower 36..96, pedals 24..55) and note off.
 * 2. GLO's MIDI channel N: N, N + 1, N + 2 are the manuals (round the 16) and every other channel is ignored.
 * 3. sound: the lower manual and the pedals sound with their own drawbars (and are silent with those at 0).
 * 4. the swell (CC 7 and 11): 127 is the level of the patch, 64 is about half, 0 is silent.
 * 5. the drawbars (CC 70..78, inverted) of the channel's manual and the rotor speed (CC 1, CC 64), applied by the main loop.
 * 6. CC 123 releases the channel's manual only. */
#include "host.h"
#include "../firmware/src/organ_app.c"
#include <stdio.h>

static int bad;
static void check(const char *what, int ok)
{
    printf("midi: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
static void send(uint32_t st, uint32_t ch, uint32_t d1, uint32_t d2)
{
    uint32_t at = mi_w % MQ;
    midi_in_q[at] = (st >> 4) | ((st | ch) << 8) | (d1 << 16) | (d2 << 24);
    midi_in_source[at] = 1;
    mi_w++;
}
static void on(uint32_t ch, uint32_t n) { send(0x90, ch, n, 100); }
static void off(uint32_t ch, uint32_t n) { send(0x80, ch, n, 0); }
static void cc(uint32_t ch, uint32_t c, uint32_t v) { send(0xB0, ch, c, v); }
static double rms_run(uint32_t blocks)
{
    int32_t o[2 * CTL];
    double e = 0;
    uint32_t b, i;
    for (b = 0; b < blocks; b++) {
        mix_block(o, CTL);
        if (b >= blocks / 2u)
            for (i = 0; i < 2u * CTL; i++)
                e += (double)o[i] * o[i];
    }
    return sqrt(e / ((blocks - blocks / 2u) * 2.0 * CTL));
}
static void step(void) { int32_t o[2 * CTL]; mix_block(o, CTL); oa_input(); }
static uint32_t down(void)
{
    uint32_t k, n = 0;
    for (k = 0; k < ORG_NKEY; k++)
        n += org.keys[k] != 0;
    return n;
}
static void boot(void)
{
    uint32_t i;
    host_tracks_init();
    org_init();
    rot_w = dr_w = 0;
    fxo_seen = 0xFF;
    memset(&fx, 0, sizeof fx);
    rev_clear();
    host_preset(&trk[0], ENGI_ORGAN, 0);
    trk[0].p[P_AMODE] = 0;
    trk[0].p[P_DIST] = trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
    {
        uint8_t P[ORG_PSIZE];
        memset(P, 0, sizeof P);
        P[OP_UB + 2] = 8;
        P[OP_LB + 2] = 8;
        P[OP_PB + 0] = 8;
        org_set_patch(P);
    }
    mi_r = mi_w = 0;
    fm_midi_src = 3; fm_midi_base(0);
    fm_dbar_pend[0] = fm_dbar_pend[1] = fm_dbar_pend[2] = 0;
    fm_rot_req = 0;
    for (i = 0; i < 4u; i++)
        step();
}

int main(void)
{
    double a, b;
    boot();
    on(0, 60); on(1, 60); on(2, 36); on(5, 62);
    step();
    check("ch 1 upper (key 24), ch 2 lower (64 + 24), ch 3 pedals (128 + 12), ch 6 the upper again (26)",
          org.keys[24] && org.keys[64 + 24] && org.keys[128 + 12] && org.keys[26] && down() == 4u);
    off(0, 60); off(1, 60); off(2, 36); off(5, 62);
    step();
    check("the note offs release them", down() == 0u);
    on(0, 35); on(0, 97); on(1, 35); on(1, 97); on(2, 23); on(2, 56);
    step();
    check("outside the ranges (upper and lower 36..96, pedals 24..55) nothing sounds", down() == 0u);
    on(0, 36); on(0, 96); on(1, 36); on(1, 96); on(2, 24); on(2, 55);
    step();
    check("the first and last key of each manual", org.keys[0] && org.keys[60] && org.keys[64] && org.keys[124] && org.keys[128] && org.keys[159]);
    cc(1, 123, 0);
    step();
    check("CC 123 on the lower channel releases the lower manual only", !org.keys[64] && !org.keys[124] && org.keys[0] && org.keys[128]);
    cc(0, 123, 0); cc(2, 123, 0);
    step();
    check("  .. and on the others", down() == 0u);

    fm_midi_base(5);                                 /* an older setting: base channel 5 .. 7 */
    on(4, 60); on(5, 60); on(6, 36); on(0, 62); on(7, 62);
    step();
    check("GLO channel 5: 5 upper, 6 lower, 7 pedals; channels 1 and 8 ignored",
          org.keys[24] && org.keys[64 + 24] && org.keys[128 + 12] && !org.keys[26] && down() == 3u);
    fm_midi_base(15);                                /* round the 16: 15, 16, 1 */
    off(4, 60); off(5, 60); off(6, 36);
    on(14, 60); on(15, 60); on(0, 36);
    step();
    check("  .. channel 15 wraps: 15, 16, 1", org.keys[24] && org.keys[64 + 24] && org.keys[128 + 12]);
    off(14, 60); off(15, 60); off(0, 36);
    step();
    /* 0.9.5: a channel of its own for each manual (GLO), the lower and the pedals can be off */
    fm_ch_up = 4; fm_ch_lo = 9; fm_ch_pd = 12;
    on(3, 60); on(8, 60); on(11, 36); on(1, 62); on(2, 62);
    step();
    check("channels 4 / 9 / 12: each manual on its own channel, the others ignored",
          org.keys[24] && org.keys[64 + 24] && org.keys[128 + 12] && down() == 3u);
    off(3, 60); off(8, 60); off(11, 36);
    step();
    fm_ch_lo = 0; fm_ch_pd = 0;
    on(3, 60); on(1, 60); on(2, 36);
    step();
    check("  .. LOWER and PEDAL OFF: only the upper is heard", org.keys[24] && down() == 1u);
    off(3, 60);
    step();
    fm_ch_up = 0;
    on(5, 60); on(6, 64);
    step();
    check("  .. UPPER ALL takes every channel (the others being off)", org.keys[24] && org.keys[28] && down() == 2u);
    off(5, 60); off(6, 64);
    step();
    fm_midi_base(0);
    step();

    /* the split (0.9.5): the upper channel below the point plays the lower manual and the pedals as well */
    org_patch[OP_SPLIT] = 1;
    org_patch[OP_SPLIT_PT] = 0;                      /* the default byte: index 19, G3 (55) */
    on(0, 48); on(0, 60);
    step();
    check("SPLIT on: C3 (48) below G3 plays the lower manual and the pedals, not the upper",
          !org.keys[12] && org.keys[64 + 12] && org.keys[128 + 24] && org.keys[24] && down() == 3u);
    off(0, 48); off(0, 60);
    step();
    check("  .. the note-offs release them", down() == 0u);
    on(0, 55); on(0, 54);
    step();
    check("  .. G3 (55) is the upper's lowest, F#3 (54) goes below",
          org.keys[19] && !org.keys[18] && org.keys[64 + 18] && org.keys[128 + 30] && down() == 3u);
    org_patch[OP_SPLIT] = 0;
    off(0, 55); off(0, 54);
    step();
    check("  .. SPLIT turned off with the notes held: they still end", down() == 0u);
    on(0, 48);
    step();
    check("SPLIT off: the upper manual only", org.keys[12] && down() == 1u);
    off(0, 48);
    step();
    org_patch[OP_SPLIT] = 1;
    on(0, 48); on(0, 40);
    cc(0, 123, 0);
    step();
    check("  .. CC 123 on the upper channel also ends the split's notes", down() == 0u);
    org_patch[OP_SPLIT] = 0;
    cc(0, 120, 0);
    step();
    check("CC 120: every key off", down() == 0u);

    /* sound */
    on(1, 60);
    a = rms_run(120);
    check("the lower manual sounds (its drawbar 8')", a > 200.0);
    off(1, 60);
    (void)rms_run(60);
    on(2, 36);
    a = rms_run(120);
    check("the pedals sound (their 16')", a > 200.0);
    off(2, 36);
    (void)rms_run(400);                              /* (the release tail: the keys' envelope) */
    {
        uint8_t P[ORG_PSIZE];
        memcpy(P, org_patch, sizeof P);
        P[OP_LB + 2] = 0;
        P[OP_PB + 0] = 0;
        org_set_patch(P);
    }
    on(1, 60); on(2, 36);
    a = rms_run(120);
    check("with their drawbars at 0 the lower manual and the pedals are silent (but the key click, twice setBfree's since 0.9.5)", a < 8.0);
    off(1, 60); off(2, 36);
    (void)rms_run(30);

    /* the swell */
    on(0, 60);
    cc(0, 11, 127);
    a = rms_run(200);
    cc(0, 11, 64);
    b = rms_run(200);
    check("the swell: CC 11 at 64 is about half the level of 127 (0.4 .. 0.6)", b / a > 0.4 && b / a < 0.6);
    cc(0, 7, 0);
    b = rms_run(200);
    check("  .. CC 7 at 0: silent", b < 3.0);
    cc(0, 7, 127);
    b = rms_run(200);
    check("  .. CC 7 at 127: back to the full level", b / a > 0.95 && b / a < 1.05);
    off(0, 60);
    (void)rms_run(30);

    /* the drawbars and the speed */
    cc(0, 70, 127); cc(0, 71, 0); cc(0, 72, 64); cc(0, 78, 127);
    step();
    check("CC 70..78 on ch 1: the upper drawbars, inverted (127 is 0, 0 is 8, 64 is 4)",
          org_patch[OP_UB] == 0 && org_patch[OP_UB + 1] == 8 && org_patch[OP_UB + 2] == 4 && org_patch[OP_UB + 8] == 0);
    cc(1, 70, 0); cc(2, 76, 0);
    step();
    check("  .. ch 2 the lower and ch 3 the pedal drawbars", org_patch[OP_LB] == 8 && org_patch[OP_PB + 6] == 8 && org_patch[OP_UB] == 0);
    cc(0, 1, 0);
    step();
    check("CC 1 at 0: the rotor SLOW", org_patch[OP_ROT_SPEED] == 1);
    cc(0, 1, 60);
    step();
    check("  .. at 60: STOP", org_patch[OP_ROT_SPEED] == 0);
    cc(0, 1, 127);
    step();
    check("  .. at 127: FAST", org_patch[OP_ROT_SPEED] == 2);
    cc(0, 64, 127); step();
    check("CC 64 pressed: FAST goes to SLOW", org_patch[OP_ROT_SPEED] == 1);
    cc(0, 64, 0); step();
    check("  .. released: nothing", org_patch[OP_ROT_SPEED] == 1);
    cc(0, 64, 127); step();
    check("  .. pressed again: SLOW goes to FAST", org_patch[OP_ROT_SPEED] == 2);
    printf("%s\n", bad ? "MIDI ORGAN TEST FAILED" : "midi organ test passed");
    return bad != 0;
}
