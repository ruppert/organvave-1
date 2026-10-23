/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (organvave-1) */
/* MIDI IN end to end (firmware/src/midi_control.c through seq.c events_block): notes on any channel (the organ's keys),
 * the manuals by channel, bend by GLO > PITCH BEND, CC120 / 123 / 121, the CC map's queue, Program Change through the main
 * loop, the source and channel filters, realtime ignored, a lost message (overflow) releasing everything. Since 0.4.0 the
 * sustain pedal toggles the rotor and CC1 sets its speed (tests/midi_organ_test.c has the organ's map). */
#define CHECK_TAG "midi"
#include "editor_host.h"
#define CTL 32u

static void mreset(void)
{
    reset();
    memset(midi_ch, 0, sizeof midi_ch);
    midi_forget_all();
    memset((void *)fm_cc_rx, 0, sizeof fm_cc_rx); memset((void *)fm_cc_pend, 0, sizeof fm_cc_pend);
    fm_midi_src = 3; fm_midi_base(0); fm_bend_mode = 0; fm_bt = 0; midi_pc = 0;
    events_block(CTL);
}
static void q(uint32_t st, uint32_t d1, uint32_t d2, uint32_t source)
{
    midi_enqueue((st >= 0xF8u ? 0xFu : st >> 4) | st << 8 | d1 << 16 | d2 << 24, source);
    events_block(CTL);
}
/* the organ has no voices: a note's gate is its key (notes 36..96 are keys 0..60, org_note) */
static int gate(uint32_t note)
{
    return note >= 36u && note <= 96u && org.keys[note - 36u] != 0;
}

int main(void)
{
    int bad = 0;

    mreset();
    q(0x90, 60, 100, 1);
    bad += check("a USB note on: its key is down", gate(60));
    q(0x80, 60, 0, 1);
    bad += check("  .. its note off releases it", !gate(60));
    q(0x95, 62, 90, 2);
    bad += check("a TRS note on channel 6 (omni): its key is down", gate(62));
    q(0x95, 62, 0, 2);
    bad += check("  .. note on velocity 0 releases it", !gate(62));

    mreset();
    q(0xB0, 64, 127, 1); q(0x90, 64, 100, 1); q(0x80, 64, 0, 1);
    bad += check("the sustain pedal holds nothing (it toggles the rotor): a released note is released", !gate(64));
    mreset();
    q(0x90, 67, 100, 1); q(0x91, 67, 100, 1);
    bad += check("the same note on the upper and the lower channel: two keys down", gate(67) && org.keys[64 + 31]);
    q(0x80, 67, 0, 1);
    bad += check("  .. a note off on the upper channel leaves the lower one", !gate(67) && org.keys[64 + 31]);
    q(0x81, 67, 0, 1);
    bad += check("  .. the lower channel's note off releases it", !org.keys[64 + 31]);

    mreset();
    q(0xE0, 127, 127, 1);
    bad += check("PITCH BEND +-2: the top is +2 semitones", midi_bend_target[0] == 512);
    q(0xE0, 0, 0, 1);
    bad += check("  .. the bottom -2", midi_bend_target[0] == -512);
    fm_bend_mode = 3; q(0xE0, 0, 0, 1);
    bad += check("PITCH BEND +2 -24: the bottom -24", midi_bend_target[0] == -24 * 256);
    q(0xE0, 0, 64, 1);
    bad += check("  .. the centre 0", midi_bend_target[0] == 0);
    q(0xE0, 127, 127, 1); q(0xB0, 121, 0, 1);
    bad += check("CC121 resets the bend and the mod wheel", midi_bend_target[0] == 0 && TSEL->mw == 0);

    mreset();
    fm_rot_req = 0;
    q(0xB0, 1, 99, 1);
    bad += check("CC1: the rotor speed request (99: FAST)", fm_rot_req == 3);
    q(0xB3, 21, 33, 1);
    bad += check("another CC: noted for the CC map (the main loop applies it)",
                 fm_cc_rx[21] == 33 && (fm_cc_pend[21 >> 5] >> (21 & 31) & 1u));

    mreset();
    q(0x90, 60, 100, 1); q(0x90, 64, 100, 1); q(0x80, 60, 0, 1);
    q(0xB0, 123, 0, 1);
    bad += check("CC123 (All Notes Off) releases the channel's manual", !gate(60) && !gate(64));
    q(0x90, 60, 100, 1); q(0x91, 60, 100, 1);
    q(0xB0, 120, 0, 1);
    bad += check("CC120 (All Sound Off) ends everything", !gate(60) && !org.keys[64 + 24]);

    mreset();
    q(0xC0, 2, 0, 1);
    bad += check("Program Change: noted, not loaded in the audio path", midi_pc == (0x80 | 2) && TSEL->preset == 0);
    midi_pc_poll();
    bad += check("  .. the main loop loads factory patch 3", TSEL->preset == 2 && !midi_pc);
    q(0xC0, 120, 0, 1); midi_pc_poll();
    bad += check("  .. a program past the last is ignored (no wrap)", TSEL->preset == 2);

    mreset();
    fm_ch_up = 3; fm_ch_lo = fm_ch_pd = 0;
    q(0x90, 60, 100, 1);
    bad += check("MIDI CH 3: channel 1 is not heard", !gate(60));
    q(0x92, 60, 100, 1);
    bad += check("  .. channel 3 is (the upper manual)", gate(60));
    mreset();
    fm_midi_src = 0;
    q(0x90, 61, 100, 2);
    bad += check("MIDI IN USB: TRS is not heard", !gate(61));
    q(0x90, 61, 100, 1);
    bad += check("  .. USB is", gate(61));
    mreset();
    q(0x90, 62, 100, 3);
    bad += check("Bluetooth off: its source is not heard", !gate(62));
    q(0xF8, 0, 0, 1); q(0xFA, 0, 0, 1);
    bad += check("realtime (clock, start) is ignored: the transport stays stopped", !song.playing);

    mreset();
    q(0x90, 60, 100, 1); q(0x90, 64, 100, 1);
    midi_in_overflow = 1;
    events_block(CTL);
    bad += check("a lost message (overflow): every key released, the queue cleared",
                 !gate(60) && !gate(64) && mi_r == mi_w && !midi_in_overflow);

    printf("%s\n", bad ? "MIDI TEST FAILED" : "midi test passed");
    return bad != 0;
}
