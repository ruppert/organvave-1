#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Guilherme Ruppert (OrganVave-1)
"""Generates firmware/src/eng_drive_tables.h: the drive's shaping curves and gain tables (integer, for the firmware).

The drive (0.9.24, two types since 0.9.30) is a short chain: an input high-pass (one pole), a pre-gain (the GAIN knob,
0.0 .. 10.0 in tenths; 0 is the bypass), a waveshaper table (tanh-like curves on -8..8, 512 steps), an optional second
stage (DISTORTION), a tone (dark low-pass / bright tilt) and an output trim.

  TUBE DRIVE    a warm overdrive, made here: a mild, slightly asymmetric soft clip. Its GAIN follows the harmonic
                distortion, not the pre-gain (0.9.30, the owner's law): a 441 Hz sine at the organ's level comes out with
                THD(g) = 25.2 (e^(0.085 g) - 1) % (0.9.31: the top 25 % above 0.9.30's 27 %, a little less exponential):
                0.21 % at 0.1, 1.1 % at 0.5, 4.7 % at 2.0, 13.4 % at 5.0, 33.75 % at 10.0.
                The pre-gain of every step is solved here for that THD (bisection on the float model).
  DISTORTION    a high-gain two-stage clip, Marshall JCM900 style (the second stage clipping the first's output): 3 dB
                of pre-gain a GAIN unit, as since 0.9.24.

The organ's level at the drive's input (fx.c takes the mix down by 12 dB first, so that a chord's peaks fit): a five-note
chord at velocity 100 has RMS 0.73 (1.0 = 8192 in Q13; measured on the host). The THD and the trims are measured at it.
The trims: the output at the dry level, rising 1 % a GAIN unit (+10 % at 10.0), with the output's peak held under its
clamp (4.0: the output is Q15 up to 4.0 since 0.9.30).

TUBE SCREAM and TUBE PREAMP (0.9.24 .. 0.9.29, inspired by TS-808-Ultra and ValveLord) were removed in 0.9.30 (owner).
The floats are here only: the firmware is integer.
Run: python3 tools/gen_drive_tables.py
"""
import math, os

FS = 44100.0
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "firmware", "src", "eng_drive_tables.h")
NWS = 512                                   # table steps over -8 .. +8 (1/32 each)
NG = 101                                    # the GAIN knob: 0.0 .. 10.0 in tenths (index 0 is the bypass)
OP_RMS = 0.73                               # the organ at the drive's input (above)
OUT_CLAMP = 4.0                             # the output's clamp (Q15, +-131071)
PRE_Q = 1024                                # the pre-gain's unit (Q10)

# (name, HP cutoff Hz, [stage shapes: (A, B, C)], second stage gain)
TYPES = [
    ("TUBE DRIVE", 60.0, [(0.90, 0.75, 0.10)], 0.0),
    ("DISTORTION", 180.0, [(0.95, 1.80, 0.20), (0.90, 1.50, -0.05)], 2.0),   # s2: the gain into stage 2
]

def shape(a, b, c, x):
    return a * (math.tanh(b * x + c) - math.tanh(c))

def table(a, b, c):
    t = []
    for i in range(NWS):
        x = -8.0 + i * 16.0 / NWS                     # the firmware indexes (x + 8) * 32: step 1/32, index 256 is x = 0
        t.append(max(-32767, min(32767, int(round(shape(a, b, c, x) * 32767)))))
    return t

def hp_coef(fc):
    return int(round(math.exp(-2 * math.pi * fc / FS) * 32768))

def lp_coef(fc):
    return int(round((1 - math.exp(-2 * math.pi * fc / FS)) * 32768))

def thd_target(g):
    """the TUBE DRIVE law (owner, 0.9.30; 0.9.31): THD in % at GAIN g (units)"""
    return 25.2 * (math.exp(0.085 * g) - 1.0)          # 0.9.31 (0.9.30: 14.29 (e^(0.1061 g) - 1), 27 % at 10.0)

def dist_pre_db(g):
    """DISTORTION: its 0.9.24 law (-10 dB + 3 dB a unit), +12 dB for the input taken down by 12 dB in 0.9.30"""
    return 2.0 + 3.0 * g

def chain(ty, G, x):
    """the float model: HP, pre-gain G, the stages (the firmware's clamps: the shaper's input at +-8)"""
    name, fc, stages, s2 = ty
    a = math.exp(-2 * math.pi * fc / FS)
    y = 0.0; xp = 0.0; out = []
    for v in x:
        y = a * (y + v - xp); xp = v
        s = max(-8.0, min(8.0, y * G))
        for si, (A, B, C) in enumerate(stages):
            s = shape(A, B, C, s)
            if si == 0 and s2:
                s = max(-8.0, min(8.0, s * s2))
        out.append(s)
    return out

SETTLE, NTHD = 2000, 4000                           # a 441 Hz sine: 100 samples a period, 40 periods measured
SINE = [OP_RMS * math.sqrt(2) * math.sin(2 * math.pi * 441.0 * i / FS) for i in range(SETTLE + NTHD)]

def harm(y, k):
    re = im = 0.0
    w = 2 * math.pi * 441.0 * k / FS
    for i, v in enumerate(y):
        re += v * math.cos(w * i); im += v * math.sin(w * i)
    return 2 * math.hypot(re, im) / len(y)

def thd(ty, G):
    y = chain(ty, G, SINE)[SETTLE:]
    f1 = harm(y, 1)
    return 100.0 * math.sqrt(sum(harm(y, k) ** 2 for k in range(2, 10))) / f1

def solve_pre(ty, target):
    lo, hi = math.log(0.005), math.log(40.0)           # the THD rises with the pre-gain: bisection on log G
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        if thd(ty, math.exp(mid)) < target:
            lo = mid
        else:
            hi = mid
    return math.exp(0.5 * (lo + hi))

def rms(v):
    return math.sqrt(sum(t * t for t in v) / len(v))

def main():
    pre = []
    for ty in TYPES:
        row = [1.0]
        for gi in range(1, NG):
            g = gi / 10.0
            row.append(solve_pre(ty, thd_target(g)) if ty[0] == "TUBE DRIVE" else 10 ** (dist_pre_db(g) / 20.0))
        pre.append(row)
    # the trims, on an organ-like chord (bass fundamentals, the drawbars' partials) at the organ's level
    n = int(FS * 0.5)
    freqs = [110.0, 165.0, 220.0, 330.0, 440.0, 660.0, 880.0]
    a = OP_RMS * math.sqrt(2) / math.sqrt(len(freqs))
    sig = [a * sum(math.sin(2 * math.pi * f * i / FS) for f in freqs) for i in range(n)]
    ref = rms(sig[n // 2:])
    trims = []
    for ti, ty in enumerate(TYPES):
        row = [1.0]
        for gi in range(1, NG):
            g = gi / 10.0
            y = chain(ty, pre[ti][gi], sig)[n // 2:]
            target = ref * (1.0 + 0.01 * g)
            pk = max(abs(v) for v in y)
            row.append(min(target / rms(y), 0.95 * OUT_CLAMP / pk))
        trims.append(row)
    cut = [1500.0 * 2 ** (t * 0.6) for t in range(4)] + [1500.0] * 5   # dark steps 0..3; 4..8 use the bright emphasis
    lines = []
    lines.append("/* GENERATED by tools/gen_drive_tables.py: do not edit (0.9.30). The drive's curves, gains and trims. */")
    lines.append("#ifndef ENG_DRIVE_TABLES_H\n#define ENG_DRIVE_TABLES_H")
    lines.append("#include <stdint.h>")
    lines.append("#define DR_NWS 512")
    lines.append("#define DR_TYPES 2                             /* 0 TUBE DRIVE, 1 DISTORTION (the patch byte: 2, 3) */")
    lines.append("#define DR_NG 101                             /* the GAIN knob: 0.0 .. 10.0 in tenths */")
    lines.append("/* the waveshapers: 512 steps over -8 .. +8, Q15 out: 0 TUBE DRIVE, 1 DISTORTION stage 1, 2 DISTORTION stage 2 */")
    lines.append("static const int16_t DR_WS[3][DR_NWS] = {")
    for (ti, si) in [(0, 0), (1, 0), (1, 1)]:
        A, B, C = TYPES[ti][2][si]
        lines.append("  {" + ",".join(str(v) for v in table(A, B, C)) + "},")
    lines.append("};")
    lines.append("static const uint16_t DR_HP[2] = {%s};" % ",".join(str(hp_coef(ty[1])) for ty in TYPES))
    lines.append("/* the pre-gain, Q10, per type and GAIN (tenths 0..100; index 0 is the bypass, unused). TUBE DRIVE: solved for its THD law */")
    lines.append("static const uint16_t DR_PRE[2][DR_NG] = {")
    for row in pre:
        lines.append("  {" + ",".join(str(max(1, min(65535, int(round(G * PRE_Q))))) for G in row) + "},")
    lines.append("};")
    lines.append("/* the output trim, Q8, per type and GAIN (tenths): the dry level at the organ's level, +1 % a unit (+10 % at 10.0) */")
    lines.append("static const uint16_t DR_TRIM[2][DR_NG] = {")
    for row in trims:
        lines.append("  {" + ",".join(str(min(65535, int(round(t * 256)))) for t in row) + "},")
    lines.append("};")
    lines.append("/* the tone: the dark cutoff (Hz) per TONE step 0..8 (the low-pass), and the bright emphasis's own low-pass (Hz) */")
    lines.append("static const uint16_t DR_TONE_LP[9] = {%s};" % ",".join(str(lp_coef(c)) for c in cut))
    lines.append("#define DR_LP_BRIGHT %d" % lp_coef(2000.0))   # the bright emphasis pivots at 2 kHz
    lines.append("#endif")
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", os.path.normpath(OUT))
    for ti, ty in enumerate(TYPES):
        print(ty[0], "pre-gain", ["%.3f" % pre[ti][g] for g in (1, 5, 10, 20, 50, 100)])
        print(ty[0], "trims", ["%.2f" % trims[ti][g] for g in (1, 5, 10, 20, 50, 100)])
    t = TYPES[0]
    print("TUBE DRIVE THD check", ["%.1f:%.2f%%" % (g / 10, thd(t, pre[0][g])) for g in (1, 5, 20, 50, 100)])

if __name__ == "__main__":
    main()
