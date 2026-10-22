/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Guilherme Ruppert (FMini-1, organvave-1) */
/* Keyboard, arpeggiator, sequencer and transport (Felucca's seq.c, cut down to what the one instrument uses: the SEQ
 * pop-up's 16 steps, the ARP pop-up, the keys and MIDI IN).
 * Everything here runs in the audio ISR, once per CTL-sample block, and ends in trk_note_on / trk_note_off: engines
 * never see where a note came from. The keys and MIDI IN play track 1 (organ_app.c GLO: the MIDI source and channel).
 * The SEQ pop-up records its steps itself (fa_seq_note); the sequencer plays them on the track's LEN / DIV / SWING /
 * GATE. A step: up to 4 notes, NOTE / TIE / REST, accent and slide (acid style), as Felucca's. */
#define KB_SILENT 255u
static uint32_t kb_prev;
static volatile uint8_t kb_boot_hold;    /* #137: set from power-on to the end of the splash (main.c): the keys sound nothing,
                                          * and one held across its end stays silent until it is let go and pressed again */
static uint8_t kb_note[27];              /* per key: the note it started */
static uint8_t in_key;                   /* the key (+ 1) whose note goes into input_on now (key_on), 0: none (MIDI IN) */
#define KB_VEL 100u                      /* the keys' velocity */
/* #191: where each held[i] came from, so the arp maps it again as it plays (arp_list: the octave buttons as they were):
 * the key + 1 (bits 0..4) and the octave then + 3 (bits 5..7); 0: MIDI IN, held[i] plays as it is. Out of track_t */
static uint8_t held_key[16];
static volatile uint8_t transport_req;   /* 1 start, 2 stop, 3 restart (from the UI) */
static volatile uint8_t panic_req;       /* bit 0: release every sounding note (preset / engine change) */

static uint32_t trk_index(const track_t *t) { return (uint32_t)(t - trk); }

/* key k's note with the octave buttons at oct (the 27 keys from F; key 7 is C4 at octave 0), TRN on top */
static uint32_t kb_pitch(const track_t *t, uint32_t k, int32_t oct)
{
    return (uint32_t)clamp(53 + (int32_t)k + 12 * oct + t->p[P_TRANS], 0, 127);
}
static uint32_t kb_map(const track_t *t, uint32_t k) { return kb_pitch(t, k, song.octave); }

/* ------------------------------------------------------------- arp --- */
static void arp_add(track_t *t, uint32_t note)
{
    uint32_t i;
    if (t->p[P_AHOLD] && t->arp_phys == 0u)
        t->nheld = 0;                               /* new chord replaces the latched one */
    t->arp_phys++;                                  /* every key-down: arp_remove counts every key-up */
    for (i = 0; i < t->nheld; i++)
        if (t->held[i] == note)
            return;                                 /* repeated note-on: not a new note */
    if (t->nheld < 16u) {
        held_key[t->nheld] = in_key ? (uint8_t)(in_key | (uint32_t)(song.octave + 3) << 5) : 0u;
        t->held[t->nheld++] = (uint8_t)note;
    }
    if (t->nheld == 1u) {
        t->arp_pos = 0xFFFFFFF;                     /* fire on this block */
        t->arp_idx = 0xFFFFFFFFu;
    }
}

static void arp_remove(track_t *t, uint32_t note)
{
    uint32_t i, k = 0;
    if (t->arp_phys)
        t->arp_phys--;
    if (t->p[P_AHOLD])
        return;
    for (i = 0; i < t->nheld; i++)
        if (t->held[i] != note) {
            held_key[k] = held_key[i];
            t->held[k++] = t->held[i];
        }
    t->nheld = (uint8_t)k;
}

/* the ARP's modes (P_AMODE). Append-only: a stored value keeps its meaning. ORD plays the held notes in the order they
 * were pressed. Over the note list (the held notes low to high, or as played, across OCT octaves), C E G held: DNUP
 * G E C E; UP+8 C E G C' (the first note again an octave above the top); CONV C G E (outside in); DIVG E G C (inside
 * out); PINKY C G E G (the top note between the others); THUMB C E C G (the bottom one); WALK a random step up or down
 * the list (bouncing off its ends); CHORD all the held notes together (the first 4), an octave higher each step
 * through OCT */
enum { AM_OFF, AM_UP, AM_DN, AM_UPDN, AM_RND, AM_ORD, AM_REPEAT, AM_DNUP, AM_UP8, AM_CONV, AM_DIVG, AM_PINKY,
       AM_THUMB, AM_WALK, AM_CHORD };

static void arp_forget(track_t *t)               /* (its notes ended otherwise: trk_all_off) */
{
    t->arp_note = t->arp_ch[0] = t->arp_ch[1] = t->arp_ch[2] = 0;
}

/* CHORD's notes. Not inlined: they run once an arp step, and kept out of the audio ISR's loop
 * (tests/target_budget.py) */
static __attribute__((noinline)) void arp_chord_off(track_t *t)   /* the other notes end */
{
    uint32_t i;
    for (i = 0; i < 3u; i++)
        if (t->arp_ch[i]) {
            trk_note_off(t, t->arp_ch[i]);
            t->arp_ch[i] = 0;
        }
}

static __attribute__((noinline)) void arp_chord(track_t *t, const uint32_t *list, uint32_t n)
{
    uint32_t i, nt, top = 0, o = t->arp_idx % (uint32_t)(t->p[P_AOCT] > 0 ? t->p[P_AOCT] : 1);
    if (n > 4u)
        n = 4u;
    for (i = 0; i < n; i++) {                       /* the held notes (sorted or as played), o octaves up */
        nt = (uint32_t)clamp((int32_t)list[i] + 12 * (int32_t)o, 0, 127);
        if (nt == 127u && top++)
            continue;                               /* (notes clamped onto the top one: once) */
        if (!t->arp_note)
            t->arp_note = (uint8_t)nt;
        else
            t->arp_ch[i - 1u] = (uint8_t)nt;
        trk_note_on(t, nt, 100);
    }
}

static void arp_off(track_t *t)                  /* the sounding arp note(s) end */
{
    if (t->arp_ch[0] | t->arp_ch[1] | t->arp_ch[2])
        arp_chord_off(t);
    trk_note_off(t, t->arp_note);
    t->arp_note = 0;
}

/* the place in the note list (len notes) the step arp_idx plays (not inlined, as arp_chord); UP+8 adds its note */
static __attribute__((noinline)) uint32_t arp_pick(track_t *t, uint32_t *list, uint32_t len)
{
    uint32_t i = t->arp_idx, m = (uint32_t)t->p[P_AMODE], cyc, k, w;
    if (m == AM_UP8 && len < 64u)                   /* UP+8: the first note again over the top */
        list[len++] = (uint32_t)clamp((int32_t)list[0] + 12 * t->p[P_AOCT], 0, 127);
    cyc = len > 1u ? 2u * len - 2u : 1u;
    k = i % cyc;
    switch (m) {
    case AM_DN:
        return len - 1u - i % len;
    case AM_UPDN:
        return k < len ? k : cyc - k;
    case AM_DNUP:
        return k < len ? len - 1u - k : k - (len - 1u);
    case AM_RND:
        return rng() % len;
    case AM_CONV:
    case AM_DIVG:
        k = i % len;
        if (m == AM_DIVG)
            k = len - 1u - k;
        return (k & 1u) ? len - 1u - k / 2u : k / 2u;
    case AM_PINKY:
        return len < 2u ? 0u : (k & 1u) ? len - 1u : k / 2u;
    case AM_THUMB:
        return len < 2u ? 0u : (k & 1u) ? 1u + k / 2u : 0u;
    case AM_WALK:
        w = t->arp_walk;
        if (!i)
            w = 0;                                  /* (from the bottom) */
        else if (w >= len)
            w = len - 1u;                           /* (notes let go) */
        else if (len > 1u)
            w = !w ? 1u : w == len - 1u ? w - 1u : (rng() >> 7) & 1u ? w + 1u : w - 1u;
        t->arp_walk = (uint8_t)w;
        return w;
    default:
        return i % len;
    }
}

static volatile uint32_t beat_pos, beat_n;      /* samples into the beat, the beat of the bar (0..3) */

/* the arp's note list (list, its length returned; 0: nothing to play): the held notes (sorted or as played) over OCT
 * octaves, *cnt of them. #191: a key's note mapped again (TRN now, the octave it was pressed in), the rest (MIDI IN)
 * as they came; each once */
static uint32_t arp_list(const track_t *t, uint32_t *list, uint32_t *cnt)
{
    uint32_t i, j, n = 0, x, s, last = 0, len = 0, o;
    for (i = 0; i < t->nheld; i++) {
        x = t->held[i];
        if ((s = held_key[i]) != 0u)
            x = kb_pitch(t, (s & 31u) - 1u, (int32_t)(s >> 5) - 3);
        last = x;
        for (j = 0; j < n && list[j] != x; j++)
            ;
        if (j == n)
            list[n++] = x;
    }
    *cnt = n;
    if (!t->p[P_AORDER] && t->p[P_AMODE] != AM_ORD)   /* (ORD: as pressed, held[] keeps that order) */
        for (i = 1; i < n; i++)
            for (j = i; j > 0 && list[j - 1] > list[j]; j--) {
                x = list[j];
                list[j] = list[j - 1];
                list[j - 1] = x;
            }
    for (o = 0; o < (uint32_t)t->p[P_AOCT]; o++)
        for (i = 0; i < n && len < 64u; i++)
            list[len++] = clamp((int32_t)list[i] + 12 * (int32_t)o, 0, 127);
    if (n && t->p[P_AMODE] == AM_REPEAT) { list[0] = last; len = 1; } /* REPEAT: last played note, no octave traversal */
    return len;
}

/* an arp step starts (period: its length): the next note of the list. Not inlined: once a step, kept out of the audio
 * ISR's loop (tests/target_budget.py) */
static __attribute__((noinline)) void arp_fire(track_t *t, uint32_t period)
{
    uint32_t cnt, list[64], len, j;
    if (!(len = arp_list(t, list, &cnt))) {
        if (t->arp_note)
            arp_off(t);
        return;
    }
    t->arp_idx++;
    j = t->p[P_AMODE] == AM_CHORD ? 0u : arp_pick(t, list, len);
    if (t->arp_note)
        arp_off(t);
    if ((uint32_t)(rng() & 127u) <= (uint32_t)t->p[P_APROB]) {
        t->arp_off = period * (uint32_t)t->p[P_AGATE] / 128u;
        if (t->p[P_AMODE] == AM_CHORD) {            /* CHORD: the first 4 notes, an octave up each step */
            arp_chord(t, list, cnt);
            return;
        }
        t->arp_note = (uint8_t)list[j];
        trk_note_on(t, t->arp_note, 100);
    }
}

static void arp_step(track_t *t, uint32_t n)
{
    uint32_t period;
    int32_t sw;
    if (t->arp_note) {
        if (t->arp_off <= n)
            arp_off(t);
        else
            t->arp_off -= n;
    }
    if (!t->p[P_AMODE] || !t->nheld) {
        if (!t->nheld && t->arp_note)
            arp_off(t);
        return;
    }
    period = div_samples((uint32_t)t->p[P_ARATE]);
    sw = t->p[P_ASWING] * (int32_t)period / 250;
    t->arp_pos += n;
    if (t->arp_pos < period + (uint32_t)((t->arp_idx & 1u) ? sw : -sw) && t->arp_pos != 0xFFFFFFF + n)
        return;
    t->arp_pos = 0;
    arp_fire(t, period);
}

/* -------------------------------------------------------- note input --- */
static void input_on(track_t *t, uint32_t note, uint32_t vel)
{
    if (t->p[P_AMODE])
        arp_add(t, note);
    else
        trk_note_on(t, note, vel);
}

static int midi_note_held(const track_t *t, uint32_t note);
static void input_off(track_t *t, uint32_t note)
{
    if (midi_note_held(t, note))
        return;
    arp_remove(t, note);                            /* both: the note may have started in the */
    trk_note_off(t, note);                          /* other mode (ARP switched while held) */
}

/* a key held sounds note */
static int midi_local_held(uint32_t note)
{
    uint32_t k;
    for (k = 0; k < 27u; k++)
        if (((fm1_in.notes >> k) & 1u) && kb_note[k] == note)
            return 1;
    return 0;
}

/* key k plays kb_note[k] on track t. A note another key or MIDI holds already sounds: it is not started again (nor
 * sent to MIDI OUT) */
static void key_on(uint32_t k, track_t *t)
{
    uint32_t x = kb_note[k], j;
    for (j = 0; j < 27u; j++)
        if (j != k && ((fm1_in.notes >> j) & 1u) && kb_note[j] == x)
            return;
    if (midi_note_held(t, x))
        return;
    in_key = (uint8_t)(k + 1u);                     /* (the arp keeps the key: arp_add) */
    input_on(t, x, KB_VEL);
    in_key = 0;
    midi_out_event(0x09u | 0x90u << 8 | x << 16 | KB_VEL << 24);
}

/* key k is up: its note ends, but not while another key holds it */
static void key_off(uint32_t k, track_t *t)
{
    uint32_t x = kb_note[k];
    if (midi_local_held(x))
        return;
    input_off(t, x);
    midi_out_event(0x08u | 0x80u << 8 | x << 16);
}


static void keyboard_block(void)
{
    uint32_t cur = fm1_in.notes, ch, k;
    ch = cur ^ kb_prev;
    if (!ch)
        return;
    if (kb_boot_hold) {                           /* #137: under the splash; their release is silent too */
        for (k = 0; k < 27u; k++)
            if ((ch & cur) >> k & 1u)
                kb_note[k] = KB_SILENT;
        kb_prev = cur;
        return;
    }
    for (k = 0; k < 27u; k++) {
        if (!((ch >> k) & 1u))
            continue;
        if ((cur >> k) & 1u) {
            kb_note[k] = (uint8_t)kb_map(TSEL, k);
            key_on(k, TSEL);
        } else {
            if (kb_note[k] == KB_SILENT)
                continue;
            key_off(k, TSEL);
        }
    }
    kb_prev = cur;
}

/* -------------------------------------------------------- sequencer --- */
static void seq_start(void)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++) {                   /* every track from its step 0, together */
        track_t *t = &trk[i];
        t->seq_idx = (uint16_t)(t->p[P_SLEN] - 1);
        t->seq_pos = 0x7FFFFFFF;                   /* step 0 fires on the first block */
        if (t->p[P_LTRIG] && t->p[P_LSYNC])        /* LFO 2 FREE + SYNC: in time with the bars from PLAY */
            t->lfo_ph = (uint32_t)t->p[P_LPHASE] << 25;
    }
    song.tick = 0;
    beat_pos = 0; beat_n = 0;                      /* the ARP LED's beat from the top too */
    song.playing = 1;
}

static void seq_release(track_t *t)
{
    uint32_t i;
    for (i = 0; i < t->seq_n; i++)
        trk_note_off(t, t->seq_notes[i]);
    t->seq_n = 0;
    t->seq_hold = 0;
    t->slide_glide = 0;                             /* live MONO / LEG keys must not glide after it */
}

static void seq_stop(void)
{
    uint32_t i;
    song.playing = 0;
    for (i = 0; i < NTRK; i++)
        seq_release(&trk[i]);
}

/* play one step: TIE extends, REST releases, NOTE (re)triggers its notes; a SLIDE on the previous step makes this one
 * legato with a glide (acid style) */
static __attribute__((noinline)) void seq_step(track_t *t, const step_t *s, uint32_t idx, uint32_t period)
{
    uint32_t i, j, gate = period * (uint32_t)t->p[P_SGATE] / 128u;
    uint32_t vel = (s->flags & SF_ACCENT) ? 127u : (s->vel ? s->vel : 96u);
    uint32_t slide_in = t->seq_hold && t->seq_n;
    uint32_t len = t->p[P_SLEN] ? (uint32_t)t->p[P_SLEN] : 1u;
    uint32_t next_tie = t->step[(idx + 1u) % len].time == ST_TIE;
    uint32_t m = s->n < 4u ? s->n : 4u;
    if (s->time == ST_TIE) {
        if (t->seq_n) {
            t->seq_off = gate + period / 2u;
            t->seq_hold = (s->flags & SF_SLIDE) != 0 || next_tie;   /* chains hold at any GATE / swing */
        }
        return;
    }
    if (s->time == ST_REST || !m) {
        seq_release(t);
        return;
    }
    t->slide_glide = (uint8_t)slide_in;
    if (!slide_in)
        seq_release(t);
    for (i = 0; i < m; i++)
        trk_note_on(t, s->note[i], vel);
    if (slide_in)                                   /* release what is not held over */
        for (i = 0; i < t->seq_n; i++) {
            for (j = 0; j < m && s->note[j] != t->seq_notes[i]; j++)
                ;
            if (j == m)
                trk_note_off(t, t->seq_notes[i]);
        }
    t->seq_n = 0;
    for (i = 0; i < m; i++)
        t->seq_notes[t->seq_n++] = s->note[i];
    t->seq_off = gate;
    t->seq_hold = (s->flags & SF_SLIDE) != 0 || next_tie;   /* next step a TIE: keep the notes to it */
}

static void seq_tick(track_t *t, uint32_t n)
{
    uint32_t period, len;
    if (t->seq_n && !t->seq_hold) {
        if (t->seq_off <= n)
            seq_release(t);
        else
            t->seq_off -= n;
    }
    if (!song.playing)
        return;
    period = div_samples((uint32_t)t->p[P_SDIV]);
    len = (uint32_t)t->p[P_SLEN];
    len = len ? len : 1u;
    t->seq_pos += n;
    for (;;) {
        uint32_t cur_len = swing_step_len(t, period, t->seq_idx);
        if (t->seq_pos < cur_len && t->seq_pos != 0x7FFFFFFFu + n)
            break;
        t->seq_pos = t->seq_pos >= 0x7FFFFFFFu ? 0u : t->seq_pos - cur_len;   /* (PLAY: its first step) */
        t->seq_idx = (uint16_t)((t->seq_idx + 1u) % len);
        seq_step(t, &t->step[t->seq_idx], t->seq_idx, period);
    }
}

/* MIDI IN (organ_app.c, GLO): the one instrument is track 1 on every channel the setting accepts; the sources are 1 USB,
 * 2 MIDI (TRS), 3 Bluetooth (no radio driver in this firmware yet: nothing arrives with that number). The CC map is the
 * app's own: the ISR only notes the last value of each CC (fm_cc_rx, bit cc of fm_cc_pend); the main loop applies it */
static volatile uint8_t fm_midi_src = 3;                 /* 0 USB, 1 MIDI, 2 BLUETOOTH, 3 ALL */
static volatile uint8_t fm_midi_ch;                      /* 0 ALL (omni), 1..16 */
static volatile uint8_t fm_bend_mode;                    /* GLO > PITCH BEND: 0 +-2, 1 +-12, 2 +2 -12, 3 +2 -24 */
static volatile uint8_t fm_bt;                           /* GLO > BLUETOOTH ON/OFF: the Bluetooth MIDI source is heard only when ON */
static volatile uint8_t fm_cc_rx[128];
static volatile uint32_t fm_cc_pend[4];
/* The organ's manuals by MIDI channel, as setBfree's defaults: the upper on channel 1, the lower on 2, the pedals on 3 (any other
 * channel plays the upper, as before 0.4.0). GLO > MIDI IN's channel N moves the three to N, N + 1, N + 2 (round the 16) and then
 * ignores every other channel. Returns 0 upper, 1 lower, 2 pedals, 3 none */
static uint32_t __attribute__((noinline)) org_manual_of(uint32_t ch)
{
    uint32_t d;
    if (!fm_midi_ch)
        return ch == 1u ? 1u : ch == 2u ? 2u : 0u;
    d = (ch + 16u - (fm_midi_ch - 1u)) & 15u;
    return d < 3u ? d : 3u;
}
static int __attribute__((noinline)) fm_midi_ok(uint32_t source, uint32_t ch)
{
    if (source == 3u && !fm_bt)
        return 0;
    return (fm_midi_src == 3u || fm_midi_src + 1u == source) && org_manual_of(ch) < 3u;
}

#include "midi_control.c"

/* everything that happens between two rendered blocks */
static void events_block(uint32_t n)
{
    uint32_t i, pr;
    if (transport_req == 1u) {
        seq_start();
        transport_req = 0;
    } else if (transport_req == 2u) {
        seq_stop();
        transport_req = 0;
    } else if (transport_req == 3u) {                 /* from the top without stopping */
        seq_stop();
        seq_start();
        transport_req = 0;
    }
    pr = panic_req;
    panic_req = 0;
    if (midi_in_overflow) {                            /* a lost note-off must never leave a held note */
        mi_r = mi_w;
        midi_forget_all();
        for (i = 0; i < NTRK; i++)
            trk[i].seq_n = trk[i].seq_hold = trk[i].slide_glide = 0;
        pr |= (1u << NTRK) - 1u;
        RING_PUBLISH();
        midi_in_overflow = 0;
    }
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        if ((pr >> i) & 1u) {
            midi_forget_all();
            trk_all_off(t);
            t->nheld = 0;
            t->arp_phys = 0;
            arp_forget(t);
        }
        engine_block(t);                              /* engine switch: fade, then switch (voice.c) */
        /* ARP turned off, or HOLD released with no key down: drop the latched chord */
        if ((t->armp && !t->p[P_AMODE]) || (t->aholdp && !t->p[P_AHOLD] && !t->arp_phys)) {
            t->nheld = 0;
            if (!t->p[P_AMODE])
                t->arp_phys = 0;
            if (t->arp_note)
                arp_off(t);
        }
        t->armp = t->p[P_AMODE];
        t->aholdp = t->p[P_AHOLD];
    }
    keyboard_block();
    while (mi_r != mi_w) {                            /* USB-MIDI (and TRS) in */
        uint32_t at = mi_r % MQ, pkt = midi_in_q[at], status = (pkt >> 8) & 0xFFu;
        uint32_t st = status & 0xF0u, ch = status & 0x0Fu;
        uint32_t d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        uint32_t source = midi_in_source[at] ? midi_in_source[at] : 1u;
        mi_r++;
        if (status < 0xF8u && fm_midi_ok(source, ch))   /* (realtime: no external clock) */
            midi_event(st, ch, d1, d2);
    }
    for (i = 0; i < NTRK; i++)
        seq_tick(&trk[i], n);
    for (i = 0; i < NTRK; i++)
        arp_step(&trk[i], n);
    beat_pos += n;                                    /* the beat the ARP LED flashes on */
    if (beat_pos >= beat_samples()) {
        beat_pos -= beat_samples();
        beat_pos = beat_pos < beat_samples() ? beat_pos : 0u;
        beat_n = (beat_n + 1u) & 3u;
    }
    if (song.playing)
        song.tick++;
}
