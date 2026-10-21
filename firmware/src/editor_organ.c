/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* Editor protocol: the organ patch (EDITOR_PROTOCOL.md "Organ patches", cmds 85, 86). The patch is eng_organ.c's
 * ORG_PSIZE bytes (OP_*: the drawbars of the three manuals, the tuning, the crosstalk; the rest reserved), each 0..127.
 * Targets: 0 the instrument's own patch (index 0), 2 a factory patch (0..ORG_NFACTORY-1, read only), 3 a user preset's
 * patch (0..UP_SLOTS-1, read only; the preset must be an organ one: rc 2 otherwise; one stored without its own patch
 * gives the first factory patch, as loading it does).
 * 85 ORG_GET: target, index -> target, index, rc, [ORG_PSIZE bytes].
 * 86 ORG_PUT: target 0, offset, count, count bytes (count <= 48, offset + count <= ORG_PSIZE: a frame stays under 64 bytes) -> target,
 *    offset, rc. The bytes are clamped into their ranges. rc: 0 ok, 1 arguments, 2 not an organ (preset). */
enum { ED_ORG_GET = 85, ED_ORG_PUT };
enum { ED_ORG_INST = 0, ED_ORG_FACTORY = 2, ED_ORG_USER = 3 };

static int ed_organ_handle(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    uint32_t i, rc = 0;
    uint8_t pk[ORG_PSIZE];
    if (cmd == ED_ORG_PUT) {
        rc = n < 3u || a[0] != ED_ORG_INST || a[2] > 48u || n != 3u + a[2] || (uint32_t)a[1] + a[2] > ORG_PSIZE ? 1u : 0u;
        if (!rc && TSEL->eng_req != ENGI_ORGAN)
            rc = 2;
        if (!rc) {
            memcpy(pk, org_patch, ORG_PSIZE);
            for (i = 0; i < a[2]; i++)
                pk[a[1] + i] = a[3 + i];
            org_set_patch(pk);                               /* (sanitized into its ranges) */
            ui.force = 1;
        }
        ed_b(n ? a[0] : 127u); ed_b(n > 1u ? a[1] : 127u); ed_b(rc);
        return 1;
    }
    if (cmd != ED_ORG_GET)
        return 0;
    rc = n != 2u ? 1u : 0u;
    if (!rc && a[0] != ED_ORG_INST && a[0] != ED_ORG_FACTORY && a[0] != ED_ORG_USER)
        rc = 1;
    if (!rc && a[1] >= (a[0] == ED_ORG_FACTORY ? ORG_NFACTORY : a[0] == ED_ORG_USER ? (uint32_t)UP_SLOTS : 1u))
        rc = 1;
    if (!rc && a[0] == ED_ORG_USER && (!up_used(a[1]) || up_rec(a[1])->engine != ENGI_ORGAN))
        rc = 2;
    if (!rc && a[0] == ED_ORG_INST && TSEL->eng_req != ENGI_ORGAN)
        rc = 2;
    if (!rc) {
        if (a[0] == ED_ORG_USER) {
            uint8_t w[UPF_N];
            memcpy(pk, upf_get(a[1], w) ? ORG_FACTORY[0] : w, ORG_PSIZE);
        } else {
            memcpy(pk, a[0] == ED_ORG_FACTORY ? ORG_FACTORY[a[1]] : org_patch, ORG_PSIZE);
        }
    }
    ed_b(n ? a[0] : 127u); ed_b(n > 1u ? a[1] : 127u); ed_b(rc);
    if (!rc)
        for (i = 0; i < ORG_PSIZE; i++)
            ed_b(pk[i]);
    return 1;
}
