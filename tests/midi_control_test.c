/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (organvave-1) */
/* MIDI IN end to end (firmware/src/midi_control.c through seq.c events_block and the voices): notes on any channel,
 * the sustain pedal per channel, a note held on two channels, bend by GLO > PITCH BEND, CC1 the mod wheel, CC120 /
 * 123 / 121, the CC map's queue, Program Change through the main loop, the source and channel filters, realtime
 * ignored, a lost message (overflow) releasing everything. */
#define CHECK_TAG "midi"
#include "editor_host.h"
#define CTL 32u

static void mreset(void)
{
    reset();
    memset(midi_ch, 0, sizeof midi_ch);
    midi_forget_all();
    memset((void *)fm_cc_rx, 0, sizeof fm_cc_rx); memset((void *)fm_cc_pend, 0, sizeof fm_cc_pend);
    fm_midi_src = 3; fm_midi_ch = 0; fm_bend_mode = 0; fm_bt = 0; midi_pc = 0;
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
    bad += check("pedal down: a released note keeps its gate", gate(64));
    q(0xB1, 64, 0, 1);
    bad += check("  .. another channel's pedal up does not release it", gate(64));
    q(0xB0, 64, 0, 1);
    bad += check("  .. its own channel's pedal up does", !gate(64));

    mreset();
    q(0x90, 67, 100, 1); q(0x91, 67, 100, 1); q(0x80, 67, 0, 1);
    bad += check("a note held on two channels: one note off keeps it", gate(67));
    q(0x81, 67, 0, 1);
    bad += check("  .. the second releases it", !gate(67) && !midi_owners);

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
    q(0xB0, 1, 99, 1);
    bad += check("CC1: the mod wheel", TSEL->mw == 99);
    q(0xB3, 74, 33, 1);
    bad += check("another CC: noted for the CC map (the main loop applies it)",
                 fm_cc_rx[74] == 33 && (fm_cc_pend[74 >> 5] >> (74 & 31) & 1u));

    mreset();
    q(0xB0, 64, 127, 1); q(0x90, 60, 100, 1); q(0x90, 64, 100, 1); q(0x80, 60, 0, 1);
    q(0xB0, 123, 0, 1);
    bad += check("CC123 (All Notes Off) keeps what the pedal holds", gate(60) && gate(64));
    q(0xB0, 120, 0, 1);
    bad += check("CC120 (All Sound Off) ends everything, the pedal too", !gate(60) && !gate(64) && !midi_owners);

    mreset();
    q(0xC0, 2, 0, 1);
    bad += check("Program Change: noted, not loaded in the audio path", midi_pc == (0x80 | 2) && TSEL->preset == 0);
    midi_pc_poll();
    bad += check("  .. the main loop loads factory patch 3", TSEL->preset == 2 && !midi_pc);
    q(0xC0, 120, 0, 1); midi_pc_poll();
    bad += check("  .. a program past the last is ignored (no wrap)", TSEL->preset == 2);

    mreset();
    fm_midi_ch = 3;
    q(0x90, 60, 100, 1);
    bad += check("MIDI CH 3: channel 1 is not heard", !gate(60));
    q(0x92, 60, 100, 1);
    bad += check("  .. channel 3 is", gate(60));
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
    bad += check("a lost message (overflow): every MIDI note released, the queue cleared",
                 !gate(60) && !gate(64) && !midi_owners && mi_r == mi_w && !midi_in_overflow);

    printf("%s\n", bad ? "MIDI TEST FAILED" : "midi test passed");
    return bad != 0;
}
