/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Guilherme Ruppert (FMini-1, organvave-1) */
/* Engine table. organvave-1 builds one engine, ORGAN (eng_organ.c). The numbers are stored (user presets, the
 * autosave, the editor protocol): append, never reorder. Felucca's engines 0..13 and FMini-1's FMUG (14) are gone;
 * their numbers are kept as silent reserved entries (ENG_GONE: no presets, never selectable, eng_ok) so that ORGAN
 * stays 15 and a stored sound of a retired engine reads (it loads as the organ). */
#include "../engines/src/dsp.c"   /* Felucca-Engines (MIT): the shared DSP blocks */

#include "eng_organ.c"            /* ORGAN: setBfree's tone generator in integer C */

/* a retired engine number: silent, no presets, never offered (a stored sound of one loads as ORGAN: eng_ok) */
static void eng_gone_note_on(struct track *t, voice_t *v) { (void)t; (void)v; }
static void eng_gone_render(struct track *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    (void)t; (void)v; (void)out; (void)n; (void)m;
}
#define GONE_DESC {"-", F_INT, 0, 0, 0, 0, 0}
static const engine_t ENG_GONE = {
    .name = "-",
    .page_title = {"-", "-"},
    .edit = {GONE_DESC, GONE_DESC, GONE_DESC, GONE_DESC, GONE_DESC, GONE_DESC, GONE_DESC, GONE_DESC},
    .note_on = eng_gone_note_on,
    .render = eng_gone_render,
    .knob = {P_E4, P_E5, P_E6, P_REL},
};

/* the editor protocol, user presets and the autosave store these indices: append, never reorder */
static const engine_t *const ENGINES[NENGINES] = {
    &ENG_GONE, &ENG_GONE, &ENG_GONE, &ENG_GONE, &ENG_GONE, &ENG_GONE, &ENG_GONE,   /* 0..6: Felucca's (retired) */
    &ENG_GONE, &ENG_GONE, &ENG_GONE, &ENG_GONE, &ENG_GONE, &ENG_GONE, &ENG_GONE,   /* 7..13 */
    &ENG_GONE,                   /* 14: FMini-1's FMUG (retired) */
    &ENG_ORGAN,                  /* 15 (ENGI_ORGAN) */
};

/* a track's engine number as an index (the audio paths: a compare, cheaper than % NENGINES; a bad number: ORGAN) */
static inline uint32_t eng_idx(uint32_t e) { return e < NENGINES ? e : ENGI_ORGAN; }

/* the engines one can pick, in the order they are shown (the editor's list): eng_ok(e), the n-th eng_vis(n), e's
 * place eng_rank(e), the next / previous eng_step(e, dir) (wraps) */
static const uint8_t ENGINE_ORDER[NENG_SHOWN] = {ENGI_ORGAN};
static int eng_ok(uint32_t e) { return e == ENGI_ORGAN; }
