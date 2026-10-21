/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Guilherme Ruppert (organvave-1) */
/* The user presets against a RAM NOR (firmware/src/upreset.c, storage.c): the banks and the patches across a power
 * cycle, a bank or record of another shape read as empty, an older record mapped by count, a retired engine's record
 * playing on the organ, the patch kept through a rename and dropped when the record changes, a failed write changing
 * nothing. */
#define CHECK_TAG "upreset"
#include "editor_host.h"

static void power_cycle(void)
{
    memset(up_bank, 0, sizeof up_bank); memset(&upf, 0, sizeof upf);
    persist_boot();                              /* (up_boot) */
}

int main(void)
{
    int bad = 0;
    uint8_t pk[UPF_N];                           /* (upf_get writes a whole entry) */
    int16_t v[P_COUNT];
    up_rec_t r;
    reset();
    bad += check("the banks at 0xDC000..0xDFFFF, the patches in their own object",
                 st_sector(OBJ_UPRESET0, 0) == 0xDC000u && st_sector(OBJ_UPRESET0 + 1, 1) == 0xDF000u &&
                 st_sector(OBJ_UPFM6, 0) != 0xFFFFFFFFu && sizeof(up_bank_t) <= ST_PAYLOAD_MAX);
    bad += check("the retired object numbers have no sector",
                 st_sector(1, 0) == 0xFFFFFFFFu && st_sector(4, 0) == 0xFFFFFFFFu && st_sector(7, 0) == 0xFFFFFFFFu &&
                 st_save(3, &r, 16) != 0);

    apply_preset(20);
    memcpy(pk, org_patch, ORG_PSIZE); pk[OP_TEMP] = 2;     /* (equal temperament: not the factory's) */
    org_set_patch(pk);
    TSEL->p[P_LEVEL] = 81; song.g[G_BPM] = 141; song.octave = -2;
    bad += check("up_store U18: rc 0", up_store(17, "ORGAN ONE") == 0 && up_used(17));
    power_cycle();
    bad += check("power cycle: U18 back with its name, values and extras",
                 up_used(17) && !memcmp(up_rec(17)->name, "ORGAN ONE", 9) && up_value(up_rec(17), P_LEVEL) == 81 &&
                 up_rec(17)->packed[UP_BPM_AT] == 141 && up_rec(17)->packed[UP_OCT_AT] == 2 && !up_used(16) && !up_used(0));
    apply_preset(0); song.g[G_BPM] = 100; song.octave = 0;
    bad += check("up_load U18: the patch, LEVEL, the tempo and the octave",
                 up_load(17) == 0 && org_patch[OP_TEMP] == 2 && TSEL->p[P_LEVEL] == 81 && song.g[G_BPM] == 141 &&
                 song.octave == -2 && TSEL->user == 18);

    up_set_name(up_rec(17), 17, "RENAMED");
    bad += check("a rename keeps the preset's patch", !upf_get(17, pk) && pk[OP_TEMP] == 2);
    up_set_value(up_rec(17), P_LEVEL, 50);
    bad += check("a change of the record drops it (the factory patch its SLOT names then)", upf_get(17, pk) != 0);

    /* other shapes */
    r = *up_rec(17);
    up_bank[1].rsize = 190;
    st_save(OBJ_UPRESET0 + 1, &up_bank[1], sizeof up_bank[1]);
    power_cycle();
    bad += check("a bank of another record size: empty", !up_used(17));
    reset();
    up_put(17, &r);
    up_rec(17)->ver = 6;
    bad += check("a record of an unknown version: empty", !up_used(17));
    up_rec(17)->ver = UP_VER;
    up_rec(17)->packed[3] = 200;
    bad += check("a version 4 record with a byte past its range: empty", !up_used(17));

    /* an older record (fewer common parameters): mapped by count */
    reset();
    memset(&r, 0, sizeof r);
    r.used = UP_USED; r.ver = UP_VER; r.engine = ENGI_ORGAN; r.np = 40; memcpy(r.name, "OLD", 3);
    {
        uint32_t i;
        for (i = 0; i < 32u; i++) up_set_value(&r, i, TP[i].def);
        up_set_value(&r, P_LEVEL, 66);
        for (i = 0; i < 8u; i++) up_set_value(&r, 32u + i, (int16_t)(i + 1u));
    }
    up_values(&r, v);
    {
        uint32_t i, ok = v[P_LEVEL] == 66;
        for (i = 0; i < 8u; i++) ok &= v[P_E0 + i] == (int16_t)param_fit(param_desc_of(ENGI_ORGAN, P_E0 + i), (int32_t)(i + 1u));
        for (i = 32; i < P_E0; i++) ok &= v[i] == (int16_t)param_fit(param_desc_of(ENGI_ORGAN, i), param_desc_of(ENGI_ORGAN, i)->def);
        bad += check("np 40: the first 32 as stored, E0..E7 at P_E0, the rest their defaults", ok);
    }
    r.engine = 3;                                /* (a Felucca engine this firmware no longer has) */
    up_put(5, &r);
    bad += check("a retired engine's record: used, it plays on the organ", up_used(5) && up_engine(5) == ENGI_ORGAN &&
                 up_load(5) == 0 && TSEL->eng_req == ENGI_ORGAN);

    /* failures */
    reset();
    host_fail_erase = 1;
    bad += check("a flash error: rc 2, the slot unchanged", up_store(3, "X") == 2 && !up_used(3));
    host_fail_erase = 0;
    song.playing = 1;
    bad += check("the transport running: rc 2, nothing written", up_store(3, "X") == 2 && !up_used(3) && !host_erases);
    song.playing = 0;
    bad += check("an erase: rc 0, empty after a power cycle", up_store(3, "X") == 0 && up_put(3, 0) == 0 &&
                 (power_cycle(), !up_used(3)));
    printf("%s\n", bad ? "UPRESET TEST FAILED" : "upreset test passed");
    return bad != 0;
}
