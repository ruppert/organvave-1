#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Guilherme Ruppert (OrganVave-1)
"""The reference numbers of tests/organ_adv_test.c: setBfree's tone generator (tools/setbfree_ref/sbf_ref.c, `adv`) run with
the ADVANCED EDIT settings below; per key the contributions' count and sums, and the eight attack / release envelopes.

    sh tools/setbfree_ref/build.sh && python3 tools/gen_organ_adv_ref.py     -> tests/organ_adv_ref.txt

A set is {patch byte: index}: the index is the step the panel shows (eng_organ.c ORG_ADV: N steps, default D); the byte the
patch holds is (index - D) mod N, and the setBfree value follows from the index."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(HERE, "..", "build", "sbf_ref", "sbf_ref")
OUT = os.path.join(HERE, "..", "tests", "organ_adv_ref.txt")

# op: (N, D)  (eng_organ.c ORG_ADV)
ADV = {58: (4, 0), 59: (41, 20), 60: (41, 5), 61: (41, 25), 62: (4, 2), 63: (41, 20),
       64: (52, 31), 65: (52, 0), 66: (52, 31), 67: (52, 31), 68: (52, 3), 69: (52, 0)}


def ladder(idx):
    return 0.0 if idx == 0 else 10 ** (-(51 - idx) * 2 / 20.0)


# name: {op: index}; the unset ones are the defaults
SETS = {
    "xt_more": {64: 37, 65: 21, 66: 41, 67: 41},                     # compartment -28 dB, transformer -50, strip -20, wiring -20
    "xt_parts": {64: 0, 66: 11, 67: 21, 68: 11, 69: 6},              # no compartment, strip -80 dB, wiring -60, floor -80 dB, min -90 dB
    "xt_full": {64: 46, 65: 46, 66: 46, 67: 46},                     # -10 dB everywhere: more than the RAM table holds
    "env_click": {59: 40, 60: 2, 61: 12, 62: 0, 63: 24},             # attack click 200 %, bursts 6..38 samples, release click 60 %
    "env_shelf": {58: 3, 62: 3, 60: 3, 61: 15},                      # shelf attack, shelf release
    "env_cosine": {58: 1, 62: 1},
    "env_linear": {58: 2, 62: 0, 63: 40},                            # linear attack, click release at 100 %
}


def run(idx):
    env = dict(os.environ)
    g = lambda op: idx.get(op, ADV[op][1])
    env.update({
        "ATKMODEL": str(g(58)), "ATKLEVEL": repr(g(59) / 20.0), "ATKMIN": repr(g(60) / 40.0), "ATKMAX": repr(g(61) / 40.0),
        "RELMODEL": str(g(62)), "RELLEVEL": repr(g(63) / 40.0),
        "COMP": repr(ladder(g(64))), "TRANS": repr(ladder(g(65))), "TSTRIP": repr(ladder(g(66))), "WIRE": repr(ladder(g(67))),
        "FLOOR": repr(ladder(g(68))), "MIN": repr(ladder(g(69))),
    })
    tmp = "/tmp/organ_adv_ref_run.txt"
    subprocess.check_call([REF, "adv", tmp], env=env)
    return open(tmp).read().splitlines()


def main():
    out = ["# tests/organ_adv_ref.txt: setBfree's tonegen.c with the ADVANCED EDIT settings (tools/gen_organ_adv_ref.py). Do not edit.",
           "# set NAME bytes(58..69); key K n sum(level) sum(wheel*level) sum(bus*level)  [level Q18]; attack / release B Q15 x 128"]
    for name, idx in SETS.items():
        lines = run(idx)
        byt = [(idx.get(op, ADV[op][1]) - ADV[op][1]) % ADV[op][0] for op in range(58, 70)]
        out.append("set %s %s" % (name, " ".join(map(str, byt))))
        keys = {}
        env = {}
        for l in lines:
            t = l.split()
            if not t or t[0].startswith("#"):
                continue
            if t[0] == "contrib":
                k, w, b, v = int(t[1]), int(t[2]), int(t[3]), float(t[4])
                kk = k if k < 64 else 64 + (k - 128)
                q = int(round(v * (1 << 18)))
                a = keys.setdefault(kk, [0, 0, 0, 0])
                a[0] += 1
                a[1] += q
                a[2] += w * q
                a[3] += b * q
            elif t[0] in ("attack", "release"):
                env[(t[0], int(t[1]))] = [min(32768, int(round(float(x) * 32768))) for x in t[2:]]
        for kk in sorted(keys):
            out.append("key %d %d %d %d %d" % ((kk,) + tuple(keys[kk])))
        for b in range(8):
            out.append("attack %d %s" % (b, " ".join(map(str, env[("attack", b)]))))
        for b in range(8):
            out.append("release %d %s" % (b, " ".join(map(str, env[("release", b)]))))
    open(OUT, "w").write("\n".join(out) + "\n")
    print("wrote", os.path.relpath(OUT), os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
