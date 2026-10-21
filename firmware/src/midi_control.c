/* SPDX-License-Identifier: GPL-3.0-only
 * Adapted from MIDI control contribution by ChanceTheMaker (2026).
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Guilherme Ruppert (FMini-1, organvave-1) */
/* MIDI IN's channel messages for the one instrument (track 1), included by seq.c after its input helpers. USB and TRS
 * share the channels' state. Each channel keeps its bend, its sustain pedal and the notes it holds (a bit per note, and
 * a bit per note held only by the pedal); the bend of the channel that played last is the instrument's (GLO > PITCH
 * BEND sets the range, RPN 0 does not). CC1 is the mod wheel (FMUG's MOD depth, eng_fmug.c t->mw); every other CC goes
 * to the UI's CC map (fm_cc_rx, applied by the main loop: fmug_app.c fa_cc_poll). */
typedef struct {
    int16_t bend;                           /* signed 14-bit value, zero = centre */
    uint8_t pedal;
} midi_channel_t;
static midi_channel_t midi_ch[16];
static uint32_t midi_held[16][4];            /* bit note: the channel holds the note (its key down or pedal-held) */
static uint32_t midi_pedal_held[16][4];      /* .. only the pedal holds it (its key is up) */
static uint16_t midi_owners;                 /* notes held by MIDI over every channel (CC123: no rescan when 0) */
#define MBIT(a, n) ((a)[(n) >> 5] >> ((n) & 31u) & 1u)

/* the bend of channel ch as the instrument's pitch (voice.c midi_bend_target, 1/256 semitone) */
static void midi_expression_channel(uint32_t ch)
{
    static const uint8_t UP[4] = {2, 12, 2, 2}, DN[4] = {2, 12, 12, 24};
    int32_t b = midi_ch[ch].bend, r = (int32_t)(b < 0 ? DN : UP)[fm_bend_mode & 3u] * 256;
    midi_bend_target[0] = b * r / (b < 0 ? 8192 : 8191);
}

/* a MIDI channel holds note on track t (one track: every channel's) */
static int midi_note_held(const track_t *t, uint32_t note)
{
    uint32_t ch;
    (void)t;
    if (!midi_owners)
        return 0;
    for (ch = 0; ch < 16u; ch++)
        if (MBIT(midi_held[ch], note))
            return 1;
    return 0;
}

static void midi_release(uint32_t ch, uint32_t note)
{
    if (!MBIT(midi_held[ch], note))
        return;
    midi_held[ch][note >> 5] &= ~(1u << (note & 31u));
    midi_pedal_held[ch][note >> 5] &= ~(1u << (note & 31u));
    midi_owners--;
    if (!midi_local_held(note))
        input_off(TSEL, note);                    /* input_off also checks the other channels */
}

static void midi_note_event(uint32_t ch, uint32_t note, uint32_t vel)
{
    if (vel) {
        if (MBIT(midi_held[ch], note))            /* a repeated note replaces the previous press, a pedal-held one too */
            midi_release(ch, note);
        midi_expression_channel(ch);
        if (!midi_note_held(TSEL, note) && !midi_local_held(note))
            input_on(TSEL, note, vel);
        midi_held[ch][note >> 5] |= 1u << (note & 31u);
        midi_owners++;
    } else if (MBIT(midi_held[ch], note)) {
        if (midi_ch[ch].pedal)
            midi_pedal_held[ch][note >> 5] |= 1u << (note & 31u);
        else
            midi_release(ch, note);
    }
}

static void midi_pedal_up(uint32_t ch)
{
    uint32_t note;
    midi_ch[ch].pedal = 0;
    for (note = 0; note < 128u; note++)
        if (MBIT(midi_pedal_held[ch], note))
            midi_release(ch, note);
}

/* a panic (a preset load) and CC120: what MIDI held is forgotten, so a later pedal-up or note-off cannot release a
 * note started since on another sound */
static void __attribute__((noinline)) midi_forget_all(void)
{
    memset(midi_held, 0, sizeof midi_held);
    memset(midi_pedal_held, 0, sizeof midi_pedal_held);
    midi_owners = 0;
    midi_bend_q8[0] = midi_bend_target[0] = 0;
}

static void midi_control(uint32_t ch, uint32_t cc, uint32_t value)
{
    midi_channel_t *c = &midi_ch[ch];
    uint32_t i;
    switch (cc) {
    case 1:
        TSEL->mw = (uint8_t)value;                 /* the mod wheel */
        break;
    case 120:                                      /* All Sound Off: ignores the pedal */
        trk_all_off(TSEL);
        TSEL->nheld = TSEL->arp_phys = 0;
        arp_forget(TSEL);
        TSEL->seq_n = TSEL->seq_hold = TSEL->slide_glide = 0;
        for (i = 0; i < NVOICE; i++)
            if (TSEL->v[i].active)
                voice_kill(&TSEL->v[i]);           /* one-block fade, regardless of RELEASE */
        midi_forget_all();
        break;
    case 123:                                      /* All Notes Off: normal releases, honours the pedal */
        for (i = 0; i < 128u; i++)
            if (MBIT(midi_held[ch], i))
                midi_note_event(ch, i, 0);
        if (!midi_owners && !fm1_in.notes) {
            trk_all_off(TSEL);
            TSEL->nheld = TSEL->arp_phys = 0;
            arp_forget(TSEL);
        }
        break;
    case 121:                                      /* Reset All Controllers */
        c->bend = 0;
        TSEL->mw = 0;
        midi_expression_channel(ch);
        midi_pedal_up(ch);
        break;
    case 64:
        if (value >= 64u)
            c->pedal = 1;
        else
            midi_pedal_up(ch);
        break;
    default:
        fm_cc_rx[cc & 127u] = (uint8_t)value;      /* GLO > MIDI CC's map, applied by the main loop */
        fm_cc_pend[(cc & 127u) >> 5] |= 1u << (cc & 31u);
        break;
    }
}

/* the CCs the CC map may take (not the ones with a meaning of their own here, nor the RPN / NRPN and bank selects) */
static int ml_free_cc(uint32_t cc)
{
    return cc && cc < 120u && cc != 1u && cc != 6u && cc != 11u && cc != 32u && cc != 38u && cc != 64u &&
           (cc < 96u || cc > 101u);
}

/* a CC sets parameter id of track t as a knob would: 0..127 over its range */
static void midi_cc_set(track_t *t, uint32_t id, uint32_t value)
{
    const param_desc_t *d = param_desc_of(eng_idx(t->eng_req), id);
    int32_t v = d->min + ((int32_t)value * (d->max - d->min) + 63) / 127;
    if (d->max > d->min)
        t->p[id] = (int16_t)v;
}

/* Program Change: noted here (0x80 | program; a later one before the main loop wins), loaded by the main loop
 * (ui.c midi_pc_poll): a sound load is never the audio ISR's work */
static volatile uint8_t midi_pc;

/* a channel voice message (realtime never gets here, SysEx neither) */
static void __attribute__((noinline)) midi_event(uint32_t st, uint32_t ch, uint32_t d1, uint32_t d2)
{
    if (st == 0x90u || st == 0x80u)
        midi_note_event(ch, d1, st == 0x90u ? d2 : 0);
    else if (st == 0xE0u) {
        midi_ch[ch].bend = (int16_t)((int32_t)(d1 | (d2 << 7)) - 8192);
        midi_expression_channel(ch);
    } else if (st == 0xB0u)
        midi_control(ch, d1, d2);
    else if (st == 0xC0u)
        midi_pc = (uint8_t)(0x80u | d1);
}
