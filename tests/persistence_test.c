/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (organvave-1) */
/* The settings and the autosave against a RAM NOR (firmware/src/settings.c, storage.c): written only when quiet and
 * changed, retried after a flash error, read back at power-on; the autosave after AS_IDLE_MS of quiet, at most once per
 * AS_GAP_MS, restored only after a clean boot; FMini-1's settings record read as it is, its autosave (a project) and a
 * damaged one ignored. */
#define CHECK_TAG "persistence"
#include "editor_host.h"

static void power_on(int clean)                 /* main.c's order: the settings, the defaults, then the autosave */
{
    memset(&song, 0, sizeof song); memset(trk, 0, sizeof trk); memset(&as, 0, sizeof as);
    memset(&persist_saved, 0, sizeof persist_saved); memset(&persist_flash, 0, sizeof persist_flash);
    memset(up_bank, 0, sizeof up_bank);
    persist_pending = 0; fx_lowcut = 0; settings_leds = 0; panel.magic = 0;
    persist_boot();
    host_tracks_init();
    track_defaults(&trk[0]);
    org_set_patch(ORG_FACTORY[0]);                  /* (the organ's patch as at power-on: nothing kept in RAM) */
    set_engine_of(&trk[0], ENGI_ORGAN);
    trk[0].engine = trk[0].eng_req;
    song.sel = 0;
    autosave_boot(clean);
}

static void idle(uint32_t ms)                   /* the main loop, nothing going on */
{
    uint32_t t;
    for (t = 0; t < ms; t += 50u) {
        fm1_ms += 50u;
        settings_poll();
        autosave_poll();
    }
}

int main(void)
{
    int bad = 0;
    uint32_t e0;
    memset(host_nor, 0xFF, sizeof host_nor);
    host_wdt_hook = host_editor_wdt;
    fm1_ms = 1000u;
    power_on(1);
    bad += check("a blank flash: the defaults, nothing written", !host_erases && !host_progs && TSEL->preset == 0);

    /* settings */
    fx_lowcut = 2; settings_leds = LEDS_DIM; set_bytes[7][3] = 42;
    host_notes = 0; fm1_in.notes = 1;
    settings_save();
    bad += check("a settings change with a key down: not written yet", !host_erases);
    fm1_in.notes = 0;
    idle(100);
    bad += check("  .. written once the keys are up", host_erases == 1u && !persist_pending);
    e0 = host_erases;
    settings_save(); idle(100);
    bad += check("saving unchanged settings: no erase", host_erases == e0);
    host_fail_erase = 1;
    fx_lowcut = 1; settings_save(); idle(100);
    bad += check("a flash error: kept pending", persist_pending == 2u);
    host_fail_erase = 0;
    idle(1200);
    bad += check("  .. written on the retry a second later", !persist_pending && persist_flash.lowcut == 1u);
    power_on(1);
    bad += check("power-on: LOW CUT, LEDS and the UI's bytes back",
                 fx_lowcut == 1u && settings_leds == LEDS_DIM && set_bytes[7][3] == 42);

    /* autosave */
    {
        uint8_t pk[ORG_PSIZE];
        apply_preset(2);
        memcpy(pk, org_patch, ORG_PSIZE); pk[OP_TEMP] = 1; pk[OP_TUNING] = 70; pk[OP_XT_WIRE] = 9; pk[OP_SC_V3] = 5;
        org_set_patch(pk);
        TSEL->p[P_LEVEL] = 77; song.g[G_BPM] = 133;
        e0 = host_erases;
        idle(AS_IDLE_MS - 1000u);
        bad += check("autosave: nothing before AS_IDLE_MS of quiet", host_erases == e0);
        idle(2000);
        bad += check("  .. then one write", host_erases == e0 + 1u);
        TSEL->p[P_LEVEL] = 70;
        idle(AS_IDLE_MS + 2000u);
        bad += check("  .. a second change waits for AS_GAP_MS after the first", host_erases == e0 + 1u);
        idle(AS_GAP_MS);
        bad += check("  .. then is written", host_erases == e0 + 2u);
        power_on(1);
        bad += check("power-on after a clean boot: the organ patch, LEVEL, the tempo and the preset back",
                     org_patch[OP_TEMP] == 1 && TSEL->p[P_LEVEL] == 70 && song.g[G_BPM] == 133 && TSEL->preset == 2);
        bad += check("  .. with the ADVANCED EDIT bytes (past the old 64)", org_patch[OP_TUNING] == 70 && org_patch[OP_XT_WIRE] == 9 && org_patch[OP_SC_V3] == 5);
        {   /* a 0.9.3 record (64 patch bytes, no ADVANCED EDIT ones): still loads, the new bytes are the defaults */
            as_rec_t a;
            as_capture(&a);
            memset(a.patch + OP_TUNING, 0, AS_PATCH0 - OP_TUNING);        /* (those bytes were reserved: 0) */
            st_save(OBJ_AUTOSAVE, &a, AS_OLD_SIZE);
            org_set_patch(pk);
            power_on(1);
            bad += check("a 0.9.3 autosave (shorter): loads, the ADVANCED EDIT bytes read as the defaults",
                         org_patch[OP_TEMP] == 1 && TSEL->preset == 2 && org_patch[OP_TUNING] == 0 && org_patch[OP_XT_WIRE] == 0 && org_patch[OP_SC_V3] == 0);
            st_save(OBJ_AUTOSAVE, &a, sizeof a);
            power_on(1);
            bad += check("  .. the full one again", org_patch[OP_TEMP] == 1);
        }
        power_on(0);
        bad += check("power-on after a crash: the defaults (the autosave is not loaded)",
                     TSEL->p[P_LEVEL] != 70 && TSEL->preset == 0);
    }

    /* FMini-1's records */
    {
        persist_t p;
        uint32_t junk[64];
        memset(host_nor, 0xFF, sizeof host_nor);
        memset(&p, 0, sizeof p);
        p.magic = PERSIST_MAGIC; p.lowcut = 2; p.palette = 3; p.bytes.factory[8][5] = 17; p.bytes.user = 0x1234;
        st_save(OBJ_SETTINGS, &p, sizeof p);
        memset(junk, 0x5A, sizeof junk); junk[0] = 0x364E5546u;   /* "FUN6": an FMini-1 / Felucca project */
        st_save(OBJ_AUTOSAVE, junk, sizeof junk);
        power_on(1);
        bad += check("FMini-1's settings record: LOW CUT and GLO's bytes read", fx_lowcut == 2u && set_bytes[8][5] == 17);
        bad += check("FMini-1's autosave (a project): ignored, the defaults", TSEL->preset == 0 && TSEL->p[P_LEVEL] == ORG_LEVEL[0]);
        fx_lowcut = 0; settings_save(); idle(100); power_on(1);
        bad += check("  .. a later save keeps the fields this firmware does not use",
                     persist_flash.palette == 3u && persist_flash.bytes.user == 0x1234u && fx_lowcut == 0u);
    }
    printf("%s\n", bad ? "PERSISTENCE TEST FAILED" : "persistence test passed");
    return bad != 0;
}
