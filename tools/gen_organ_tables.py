#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Guilherme Ruppert (OrganVave-1)
"""OrganVave-1's tone generator tables, from setBfree's own (ORGAN.md).

    python3 tools/gen_organ_tables.py      -> firmware/src/eng_organ_tables.h

The data come from tools/setbfree_ref/tonegen_dump.txt: setBfree's tone generator (src/tonegen.c, GPL-2.0-or-later)
initialised as it is built (sbf_ref.c dump, srand(1)). To refresh it: sh tools/setbfree_ref/build.sh DIR, then
DIR/sbf_ref dump tools/setbfree_ref/tonegen_dump.txt (glibc's rand(): the click envelopes depend on it).

The tables (FS 44100, the firmware's and the reference's rate):
  ORG_WINC[3][91]    the wheels' 32-bit DDS increments: [0] gear 60 Hz, [1] gear 50 Hz, [2] equal temperament
  ORG_KC, ORG_KC_OFF the keys' contributions, crosstalk on (setBfree's default model): for key k the entries
                     ORG_KC[ORG_KC_OFF[k] .. ORG_KC_OFF[k + 1] - 1]; keys 0..63 the upper manual (the lower manual is
                     the same on buses + 9), 64..95 the pedals (setBfree's keys 128..159)
  ORG_KCX, ORG_KCX_OFF the same with the crosstalk off (compartment, terminal strip and wiring at 0)
                     an entry: wheel (1..91) << 25 | bus (0..26) << 20 | level (Q18, under 4.0)
  ORG_ATK[8][128], ORG_REL[8][128]  the attack (the contact click) and release envelopes, signed Q15 (32767 = 1.0)
  ORG_KEYCOMP[128]   the key compression's gain per keys down, Q15
  ORG_SWELL_TC       the swell's one-pole smoothing per sample, Q16
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DUMP = os.path.join(HERE, "setbfree_ref", "tonegen_dump.txt")
OUT = os.path.join(HERE, "..", "firmware", "src", "eng_organ_tables.h")
FS = 44100
NFRAG = 128


def load(path):
    d = {"wheel": {}, "contrib": [], "contribnx": [], "attack": {}, "release": {}, "keycomp": {}, "lower": False, "topo": {}}
    for line in open(path):
        t = line.split()
        if not t or t[0].startswith("#"):
            if t and "SampleRate" in line:
                assert "44100.0" in line and "BUFFER_SIZE_SAMPLES 128" in line, line
            continue
        if t[0] == "wheel":
            d["wheel"][int(t[1])] = tuple(float(x) for x in t[2:6])
        elif t[0] in ("contrib", "contribnx"):
            d[t[0]].append((int(t[1]), int(t[2]), int(t[3]), float(t[4])))
        elif t[0] in ("attack", "release"):
            d[t[0]][int(t[1])] = [float(x) for x in t[2:]]
        elif t[0] == "keycomp":
            d["keycomp"][int(t[1])] = float(t[2])
        elif t[0] == "topo":
            d["topo"][int(t[1])] = tuple(int(x) for x in t[2:8])
        elif t[0] == "gaintc":
            d["gaintc"] = float(t[1])
        elif t[0] == "lower":
            d["lower"] = True
        elif t[0] in ("harmonic", "wheelharmonics"):
            sys.exit("gen_organ_tables: the dump has wheel harmonics; the firmware's wheels are pure sines")
    assert d["lower"], "the dump must say that the lower manual is the upper one on buses + 9"
    assert sorted(d["wheel"]) == list(range(1, 92))
    assert sorted(d["topo"]) == list(range(1, 92)), "the dump must carry the terminals' neighbours (sbf_ref dump: topo)"
    return d


def glibc_ring(seed, pos):
    """glibc's rand() (TYPE_3 additive feedback, srand(seed)) after `pos` outputs: the 31 words of its state (the raw 32-bit
    values; an output is a word >> 1). The firmware continues the recurrence r[i] = r[i - 31] + r[i - 3]."""
    r = [seed]
    for i in range(1, 31):
        hi, lo = divmod(r[i - 1], 127773)
        w = 16807 * lo - 2836 * hi
        r.append(w + 2147483647 if w < 0 else w)
    a = r[:31]
    for i in range(31, 34):
        a.append(a[i - 31])
    n = 344 + pos
    for i in range(34, n):
        a.append((a[i - 31] + a[i - 3]) & 0xFFFFFFFF)
    return a[n - 31:n], a


RAND_POS = 154561            # rand() outputs consumed before setBfree's initEnvelopes (srand(1): the wavetables' dither first)


def check_envelopes(d):
    """the rand() position above must give the dump's attack envelopes (default settings): a failure means the dump changed"""
    ring, a = glibc_ring(1, RAND_POS + 4000)
    out = [x >> 1 for x in a[344 + RAND_POS:]]
    p = 0
    for b in range(8):
        burst = 16 + out[p] % 64
        start = out[p + 1] % (128 - burst)
        p += 2
        x = [0.0] * start
        for _ in range(burst):
            x.append(1.0 - 1.0 * out[p] / 2147483647.0)         # (the attack click level 1.0: sbf_ref make())
            p += 1
        x += [1.0] * (128 - len(x))
        y = [x[0] / 2]
        for i in range(1, 128):
            y.append((y[-1] + x[i]) / 2)
        assert max(abs(u - v) for u, v in zip(y, d["attack"][b])) < 1e-5, "RAND_POS does not give the dump's attack envelope %d" % b


def pack(contrib):
    """setBfree key numbers 0..63 and 128..159 -> table keys 0..95; returns (entries, offsets)"""
    by = [[] for _ in range(96)]
    for k, w, b, v in contrib:
        if k < 64:
            kk = k
        elif 128 <= k < 160:
            kk = 64 + (k - 128)
        else:
            raise ValueError(k)
        assert 1 <= w <= 91 and 0 <= b < 27
        q = int(round(v * (1 << 18)))
        assert 0 < q < (1 << 20), (k, w, b, v)
        by[kk].append((w << 25) | (b << 20) | q)
    ent, off = [], [0]
    for lst in by:
        ent += lst
        off.append(len(ent))
    return ent, off


def q15u(x):
    """a signed Q15 (int16): 1.0 is 32767 (the click levels above 1 make negative values)"""
    q = int(round(x * 32768))
    assert -32768 <= q <= 32768
    return min(q, 32767)


def rows(name, ctype, vals, per=12, fmt="{}"):
    out = []
    for i in range(0, len(vals), per):
        out.append("    " + ", ".join(fmt.format(v) for v in vals[i:i + per]) + ",")
    return out


def main():
    d = load(DUMP)
    L = ["/* SPDX-License-Identifier: GPL-3.0-only",
         " * Generated by tools/gen_organ_tables.py from tools/setbfree_ref/tonegen_dump.txt (setBfree's tone generator,",
         " * src/tonegen.c, GPL-2.0-or-later: Fredrik Kilander, Robin Gareus, Ken Restivo, Will Panther). Do not edit. */",
         "#ifndef ENG_ORGAN_TABLES_H", "#define ENG_ORGAN_TABLES_H", ""]
    L.append("/* the wheels' DDS increments (f * 2^32 / FS): [0] gear 60 Hz, [1] gear 50 Hz, [2] equal; wheel w at [w - 1] */")
    L.append("static const uint32_t ORG_WINC[3][91] = {")
    for col in (0, 1, 2):
        inc = [int(round(d["wheel"][w][col] * 4294967296.0 / FS)) for w in range(1, 92)]
        L.append("  {")
        L += rows("", "", inc, 8, "{}u")
        L.append("  },")
    L.append("};")
    for w in range(1, 92):
        assert abs(d["wheel"][w][3] - 1.0) < 1e-9, "wheel attenuation is not 1: the firmware assumes setBfree's default"
    for tag, key in (("ORG_KC", "contrib"), ("ORG_KCX", "contribnx")):
        ent, off = pack(d[key])
        L.append("")
        L.append("/* %s: %d contributions; an entry: wheel << 25 | bus << 20 | level Q18 */" %
                 ("crosstalk on" if tag == "ORG_KC" else "crosstalk off", len(ent)))
        L.append("static const uint16_t %s_OFF[97] = {" % tag)
        L += rows("", "", off, 12)
        L.append("};")
        L.append("static const uint32_t %s[%d] = {" % (tag, len(ent)))
        L += rows("", "", ent, 6, "{}u")         # decimal: a hex word in 0x02xxxxxx reads as the XIP window to tools/build.py
        L.append("};")
    for tag, key in (("ORG_ATK", "attack"), ("ORG_REL", "release")):
        L.append("")
        L.append("/* %s envelopes, signed Q15 (32767 = 1.0), one per active-list slot & 7 */" % key)
        L.append("static const int16_t %s[8][%d] = {" % (tag, NFRAG))
        for b in range(8):
            e = d[key][b]
            assert len(e) == NFRAG
            L.append("  {")
            L += rows("", "", [q15u(x) for x in e], 16)
            L.append("  },")
        L.append("};")
    check_envelopes(d)
    ring, _ = glibc_ring(1, RAND_POS)
    L.append("")
    L.append("/* glibc's rand() state (srand(1)) at the point setBfree's initEnvelopes starts: the click envelopes of any setting are")
    L.append(" * drawn from here (eng_organ.c org_env_build); an output is a word >> 1 */")
    L.append("static const uint32_t ORG_RAND_RING[31] = {")
    L += rows("", "", ring, 6, "{}u")
    L.append("};")
    L.append("")
    L.append("/* the terminals' neighbours (the crosstalk model, setBfree's tonegen.c wheelPairs / transformers / terminalStrip), by wheel:")
    L.append(" * the compartment's pair, the transformer row's two neighbours (wheels 44..91), the terminal strip's two neighbours; 0 none */")
    for nm, col in (("ORG_XT_PAIR", 0), ("ORG_XT_TRE", 1), ("ORG_XT_TRW", 2), ("ORG_XT_TSE", 3), ("ORG_XT_TSW", 4)):
        L.append("static const uint8_t %s[92] = {" % nm)
        L += rows("", "", [0] + [d["topo"][w][col] for w in range(1, 92)], 23)
        L.append("};")
    L.append("")
    L.append("/* the percussion's decay per block (32 samples), Q16, for 60 dB in 0.1 s .. 10.0 s (entry n: n * 0.1 s; n = 0 is 0.1 s) */")
    L.append("static const uint16_t ORG_PERC_DEC[100] = {")
    dec = [int(round(65536 * 10 ** (-96.0 / (FS * 0.1 * (n + 1))))) for n in range(100)]
    assert dec[9] == 65208 and dec[39] == 65454, (dec[9], dec[39])
    L += rows("", "", dec, 12)
    L.append("};")
    L.append("")
    L.append("/* the crosstalk ladder (ADVANCED EDIT): index 0 off, then -100 .. 0 dB in 2 dB steps (index 51 is 1.0), Q24 */")
    L.append("static const uint32_t ORG_XT_G[52] = {")
    L += rows("", "", [0] + [int(round((1 << 24) * 10 ** (-(51 - j) * 2 / 20.0))) for j in range(1, 52)], 8, "{}u")
    L.append("};")
    L.append("")
    L.append("/* the percussion's level steps, 0 .. -12 dB in 1 dB steps: gain * 2^23, so that (osc.perc.gain * 2) * entry is the envelope's start, Q24 */")
    L.append("static const uint32_t ORG_PG[13] = {")
    L += rows("", "", [int(round((1 << 23) * 10 ** (-j / 20.0))) for j in range(13)], 7, "{}u")
    L.append("};")
    L.append("")
    L.append("/* the scanner's phase step for 4.0 .. 22.0 Hz in 0.25 Hz steps (ORG_VIB_INC * f / 7.25) */")
    scan = [int(round(706122.0 * (4.0 + 0.25 * j) / 7.25)) for j in range(73)]      # (706122: the firmware's 7.25 Hz, eng_organ.c ORG_VIB_INC)
    assert scan[13] == 706122
    L.append("static const uint32_t ORG_SCAN_INC[73] = {")
    L += rows("", "", scan, 8, "{}u")
    L.append("};")
    L.append("")
    L.append("/* the key compression's gain for 0..127 keys down, Q15 */")
    L.append("static const uint16_t ORG_KEYCOMP[128] = {")
    L += rows("", "", [q15u(d["keycomp"][i]) for i in range(128)], 16)
    L.append("};")
    L.append("")
    L.append("#define ORG_SWELL_TC %du                /* the swell's smoothing per sample, Q16 (setBfree's %.9f) */" %
             (int(round(d["gaintc"] * 65536)), d["gaintc"]))
    L.append("")
    L.append("#endif")
    open(OUT, "w").write("\n".join(L) + "\n")
    print("wrote", os.path.relpath(OUT))


if __name__ == "__main__":
    main()
