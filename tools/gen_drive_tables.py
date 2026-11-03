#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Guilherme Ruppert (OrganVave-1)
"""Generates firmware/src/eng_drive_tables.h: the drive's shaping curves and gain tables (integer, for the firmware).

The drive (0.9.24; two types since 0.9.30, six since 0.9.41) is a short chain. The first two types (dr_chain):
an input high-pass (one pole), a pre-gain (the GAIN knob, 0.0 .. 10.0 in tenths; 0 is the bypass), a waveshaper table (curves
on -8..8, 512 steps), an optional second stage (DISTORTION), a DC blocker, the tone (dark low-pass / bright tilt), an output trim.

  TUBE DRIVE    (byte 2; 0 and 1 too) a warm overdrive: a mild, slightly asymmetric soft clip (tanh with an offset: even
                harmonics). Its GAIN follows the harmonic distortion, not the pre-gain (0.9.30, the owner's law): a 441 Hz sine
                at the organ's level comes out with THD(g) = 25.2 (e^(0.085 g) - 1) % (0.9.31): 0.21 % at 0.1, 4.7 % at 2.0,
                13.4 % at 5.0, 33.75 % at 10.0. The pre-gain of every step is solved here for that THD (bisection on the model).
  DISTORTION    (byte 3) a high-gain two-stage clip (the second stage clipping the first's output): 3 dB of pre-gain a GAIN
                unit, as since 0.9.24.

The four of 0.9.41 (owner; dr_chain2: the same steps plus a cathode shelf, a dry share into the clipper, the grid's bias shift,
a DC blocker of the type's own corner (the output coupling capacitor) and a fixed low-pass). Each follows the TUBE DRIVE law's
exponential scaled to its own top (the THD at 10.0):
  SOFT TUBE     (byte 4) a gentle valve: a triode-like curve, tanh(k x) / k with k 1.0 above zero and 0.55 below (the positive
                half bends first): mostly the second harmonic; 80 Hz in, a fixed low-pass at 6 kHz; 15 % at 10.0.
  MID DRIVE     (byte 5) the classic green mid-boost pedal without its clipping diodes: its op-amp stage has unity gain for the
                lows and the GAIN for what is above 720 Hz (the 4.7 k / 47 nF leg: x + G hp720(x)), and the op-amp's rails clip
                the sum (x / (1 + x^4)^(1/4), symmetric: odd harmonics, rails at 4 units); a fixed low-pass at 4 kHz; 30 %.
  HARD CLIP     (byte 6) a flat clip at 0.9 (a square at the top), from 80 Hz up, a fixed low-pass at 5 kHz; 40 %.
  TIGHT TUBE    (byte 7) a circuit model of one 12AX7 common-cathode stage, voiced tight (TUBE DRIVE without its weight in the
                bass). The plate's transfer curve is solved here from Koren's triode equations (mu 100, ex 1.4, kG1 1060, kp
                600, kvb 300) on the load line (B+ 250 V, plate resistor 100 k), self-biased by a 1.5 k cathode resistor
                (-1.2 V, 170 V at the plate, a gain of 60); the grid stops near +0.7 V (the grid stopper against the grid
                current). Around it, the circuit's frequency-dependent parts:
                  - the input coupling capacitor: a high-pass at 120 Hz;
                  - the cathode's partial bypass: the lows see 30 % of the gain (the cathode resistor's degeneration), the
                    highs all of it, the corner at 400 Hz (a small bypass capacitor);
                  - the grid's bias shift: peaks that drive the grid positive charge the coupling capacitor through the
                    grid current (1.5 ms) and pull the bias down; it leaks back through the 1 M grid resistor (22 ms);
                  - the output coupling capacitor into the next stage: a high-pass at 100 Hz.
                The low notes saturate less and the intermodulation's difference tones (f2 - f1, the low rumble under
                TUBE DRIVE's chords) are mostly taken off by the output coupling: on a C2 + C3 triad organ chord, 7 .. 8 dB less
                new low content than TUBE DRIVE at the same THD (the printout below). 25 % at 10.0.

The organ's level at the drive's input (fx.c takes the mix down by 12 dB first, so that a chord's peaks fit): a five-note
chord at velocity 100 has RMS 0.73 (1.0 = 8192 in Q13; measured on the host). The THD and the trims are measured at it.
The trims: the output at the dry level, rising 1 % a GAIN unit (+10 % at 10.0), with the output's peak held under its
clamp (4.0: the output is Q15 up to 4.0 since 0.9.30).

TUBE SCREAM and TUBE PREAMP (0.9.24 .. 0.9.29) were removed in 0.9.30 (owner); their bytes 0 and 1 play as TUBE DRIVE.
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
DCB_10HZ = 32721                            # dr_chain's DC blocker (10 Hz), Q15

# ---- the 12AX7 stage (Koren's model) ----
K_MU, K_EX, K_KG1, K_KP, K_KVB = 100.0, 1.4, 1060.0, 600.0, 300.0
K_BP, K_RA, K_RK = 250.0, 100e3, 1500.0
K_VS = 0.5                                  # volts at the grid per unit of the shaper's input
K_VN = 125.0                                # volts at the plate per unit of the shaper's output
K_VGL = 0.7                                 # the grid's positive limit (V): the stopper against the grid current

def koren_ip(vgk, vpk):
    if vpk <= 0:
        return 0.0
    t = K_KP * (1.0 / K_MU + vgk / math.sqrt(K_KVB + vpk * vpk))
    e1 = vpk / K_KP * (t + math.log1p(math.exp(-t)) if t > 30 else math.log1p(math.exp(t)))
    return 0.0 if e1 <= 0 else 2.0 * e1 ** K_EX / K_KG1

def koren_vp(vgk):
    """the plate voltage on the load line: vp = B+ - Ra ip(vgk, vp) (bisection)"""
    lo, hi = 0.0, K_BP
    for _ in range(60):
        m = 0.5 * (lo + hi)
        if m - K_BP + K_RA * koren_ip(vgk, m) > 0:
            hi = m
        else:
            lo = m
    return 0.5 * (lo + hi)

def koren_bias():
    """the self-bias: vgk = -Rk ip"""
    lo, hi = -5.0, 0.0
    for _ in range(60):
        m = 0.5 * (lo + hi)
        if m + K_RK * koren_ip(m, koren_vp(m)) > 0:
            hi = m
        else:
            lo = m
    return 0.5 * (lo + hi)

K_VGK0 = koren_bias()
K_VP0 = koren_vp(K_VGK0)

def tube_12ax7(x):
    vgk = K_VGK0 + x * K_VS
    if vgk > 0:
        vgk = K_VGL * math.tanh(vgk / K_VGL)
    return (K_VP0 - koren_vp(vgk)) / K_VN      # inverted: a rising grid brings the plate down

# ---- the curves (DR_WS rows) ----
def tanh_abc(a, b, c):
    return lambda x: a * (math.tanh(b * x + c) - math.tanh(c))

def triode(a, kp, kn):
    return lambda x: a * (math.tanh(kp * x) / kp if x >= 0 else math.tanh(kn * x) / kn)

def rails(a, r, p):
    return lambda x: a * (x / r) / (1.0 + abs(x / r) ** p) ** (1.0 / p)

def hard(a):
    return lambda x: a * max(-1.0, min(1.0, x))

CURVES = [
    ("TUBE DRIVE", tanh_abc(0.90, 0.75, 0.10)),
    ("DISTORTION stage 1", tanh_abc(0.95, 1.80, 0.20)),
    ("DISTORTION stage 2", tanh_abc(0.90, 1.50, -0.05)),
    ("SOFT TUBE", triode(0.55, 1.0, 0.55)),
    ("MID DRIVE", rails(0.95, 4.0, 4.0)),
    ("HARD CLIP", hard(0.90)),
    ("TIGHT TUBE", tube_12ax7),
]

# ---- the types, in the firmware's order (dr_t.type) ----
# name, byte, HP Hz, curve rows, stage-2 gain, THD top % (None: DISTORTION's dB law), and the chain2 parts:
#   shelf (Hz, the lows' share) or None, dry share into the clipper, bias shift, DC blocker / output coupling Hz, fixed LP Hz
TYPES = [
    dict(name="TUBE DRIVE", byte=2, hp=60.0, rows=[0], s2=0.0, top=33.76),
    dict(name="DISTORTION", byte=3, hp=180.0, rows=[1, 2], s2=2.0, top=None),
    dict(name="SOFT TUBE", byte=4, hp=80.0, rows=[3], s2=0.0, top=15.0, shelf=None, dry=0.0, bias=False, dc=10.0, lp=6000.0),
    dict(name="MID DRIVE", byte=5, hp=720.0, rows=[4], s2=0.0, top=30.0, shelf=None, dry=1.0, bias=False, dc=10.0, lp=4000.0),
    dict(name="HARD CLIP", byte=6, hp=80.0, rows=[5], s2=0.0, top=40.0, shelf=None, dry=0.0, bias=False, dc=10.0, lp=5000.0),
    dict(name="TIGHT TUBE", byte=7, hp=120.0, rows=[6], s2=0.0, top=25.0, shelf=(400.0, 0.30), dry=0.0, bias=True, dc=100.0,
         lp=None),
]
LAW_K = 0.085                                # the law's exponent a GAIN unit (0.9.31)
BIAS_TC, BIAS_REC = 0.0015, 0.022            # the grid current's charge, the grid resistor's leak (s)

def table(f):
    t = []
    for i in range(NWS):
        x = -8.0 + i * 16.0 / NWS                     # the firmware indexes (x + 8) * 32: step 1/32, index 256 is x = 0
        t.append(max(-32767, min(32767, int(round(f(x) * 32767)))))
    return t

WS = [table(f) for _, f in CURVES]

def ws(row, s):
    """the firmware's lookup: linear interpolation on the table (Q15 out, as a float)"""
    u = (s + 8.0) * 32.0
    i = int(math.floor(u))
    if i < 0:
        return WS[row][0] / 32767.0
    if i >= NWS - 1:
        return WS[row][NWS - 1] / 32767.0
    f = u - i
    return (WS[row][i] + (WS[row][i + 1] - WS[row][i]) * f) / 32767.0

def hp_coef(fc):
    return int(round(math.exp(-2 * math.pi * fc / FS) * 32768))

def lp_coef(fc):
    return int(round((1 - math.exp(-2 * math.pi * fc / FS)) * 32768))

def thd_target(ty, g):
    """the law (owner, 0.9.30; 0.9.31; 0.9.41 for the new types): THD in % at GAIN g (units), on the type's own top"""
    if ty["name"] == "TUBE DRIVE":
        return 25.2 * (math.exp(LAW_K * g) - 1.0)
    return ty["top"] * (math.exp(LAW_K * g) - 1.0) / (math.exp(LAW_K * 10.0) - 1.0)

def dist_pre_db(g):
    """DISTORTION: its 0.9.24 law (-10 dB + 3 dB a unit), +12 dB for the input taken down by 12 dB in 0.9.30"""
    return 2.0 + 3.0 * g

def chain(ty, G, x, trim=1.0):
    """the float model. The first two types as since 0.9.30 (HP, pre-gain, the stages: the solver's model, unchanged); the
    chain2 types with every part of the firmware's dr_chain2 but the tone (flat)"""
    a = math.exp(-2 * math.pi * ty["hp"] / FS)
    y = 0.0; xp = 0.0; out = []
    if "shelf" not in ty:
        for v in x:
            y = a * (y + v - xp); xp = v
            s = max(-8.0, min(8.0, y * G))
            for si, row in enumerate(ty["rows"]):
                s = CURVES[row][1](s)
                if si == 0 and ty["s2"]:
                    s = max(-8.0, min(8.0, s * ty["s2"]))
            out.append(s * trim)
        return out
    sh = ty["shelf"]
    b = math.exp(-2 * math.pi * sh[0] / FS) if sh else 0.0
    kl = sh[1] if sh else 1.0
    kc = 1 - math.exp(-1 / (FS * BIAS_TC)); kr = 1 - math.exp(-1 / (FS * BIAS_REC))
    xc = -K_VGK0 / K_VS
    dcp = hp_coef(ty["dc"]) / 32768.0
    pc = lp_coef(ty["lp"]) / 32768.0 if ty["lp"] else 1.0
    row = ty["rows"][0]
    h2 = yp = bs = bx = by = pl = 0.0
    for v in x:
        y = a * (y + v - xp); xp = v
        d = y
        if sh:
            h2 = b * (h2 + y - yp); yp = y
            d = h2 + kl * (y - h2)
        e = max(-8.0, min(8.0, d * G + v * ty["dry"]))
        if ty["bias"]:
            e += bs
            if e > xc:
                bs -= (e - xc) * kc
            bs -= bs * kr
        s = ws(row, e)
        by = s - bx + dcp * by; bx = s
        pl += (by - pl) * pc
        out.append(pl * trim)
    return out

SETTLE, NTHD = 2000, 4000                           # a 441 Hz sine: 100 samples a period, 40 periods measured
SINE = [OP_RMS * math.sqrt(2) * math.sin(2 * math.pi * 441.0 * i / FS) for i in range(SETTLE + NTHD)]
COS_SIN = {}

def harm(y, k):
    if k not in COS_SIN:
        w = 2 * math.pi * 441.0 * k / FS
        COS_SIN[k] = ([math.cos(w * i) for i in range(NTHD)], [math.sin(w * i) for i in range(NTHD)])
    c, s = COS_SIN[k]
    re = sum(v * cc for v, cc in zip(y, c)); im = sum(v * ss for v, ss in zip(y, s))
    return 2 * math.hypot(re, im) / len(y)

def thd(ty, G):
    y = chain(ty, G, SINE)[SETTLE:]
    f1 = harm(y, 1)
    return 100.0 * math.sqrt(sum(harm(y, k) ** 2 for k in range(2, 10))) / f1

def solve_pre(ty, target):
    lo, hi = math.log(0.005), math.log(63.9)           # the THD rises with the pre-gain: bisection on log G (Q10: under 64)
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        if thd(ty, math.exp(mid)) < target:
            lo = mid
        else:
            hi = mid
    return math.exp(0.5 * (lo + hi))

def rms(v):
    return math.sqrt(sum(t * t for t in v) / len(v))

def mud(ty, G):
    """the new low content: the output's energy from 20 to 250 Hz away from the input's partials, re the whole output (dB),
    on a C2 + C3 E3 G3 organ chord (8', 4', 2 2/3' each) at the organ's level"""
    import numpy as np
    fr0 = [65.41, 130.81, 164.81, 196.0]
    freqs = [f * k for f in fr0 for k in (1, 2, 3)]
    n = int(FS)
    t = np.arange(n) / FS
    sig = sum(np.sin(2 * np.pi * f * t) for f in freqs)
    sig = sig * OP_RMS / np.sqrt(np.mean(sig ** 2))
    y = np.array(chain(ty, G, list(sig))[n // 4:])
    Y = np.abs(np.fft.rfft(y * np.hanning(len(y)))) ** 2
    fr = np.fft.rfftfreq(len(y), 1 / FS)
    near = np.zeros(len(fr), bool)
    for f in freqs:
        near |= np.abs(fr - f) < 3
    low = (fr > 20) & (fr < 250)
    return 10 * math.log10(Y[low & ~near].sum() / Y.sum()), 10 * math.log10(Y[low].sum() / Y.sum())

def main():
    pre = []
    for ty in TYPES:
        row = [1.0]
        for gi in range(1, NG):
            g = gi / 10.0
            row.append(solve_pre(ty, thd_target(ty, g)) if ty["top"] else 10 ** (dist_pre_db(g) / 20.0))
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
    nt = len(TYPES)
    L = []
    L.append("/* GENERATED by tools/gen_drive_tables.py: do not edit (0.9.41). The drive's curves, gains and trims. */")
    L.append("#ifndef ENG_DRIVE_TABLES_H\n#define ENG_DRIVE_TABLES_H")
    L.append("#include <stdint.h>")
    L.append("#define DR_NWS 512")
    L.append("#define DR_TYPES %d                             /* %s */" % (nt, ", ".join("%d %s (byte %d)" % (i, t["name"], t["byte"]) for i, t in enumerate(TYPES))))
    L.append("#define DR_NG 101                             /* the GAIN knob: 0.0 .. 10.0 in tenths */")
    L.append("/* the waveshapers: 512 steps over -8 .. +8, Q15 out: %s */" % ", ".join("%d %s" % (i, c[0]) for i, c in enumerate(CURVES)))
    L.append("static const int16_t DR_WS[%d][DR_NWS] = {" % len(CURVES))
    for t in WS:
        L.append("  {" + ",".join(str(v) for v in t) + "},")
    L.append("};")
    L.append("static const uint16_t DR_HP[DR_TYPES] = {%s};          /* the input high-pass, Q15 */" % ",".join(str(hp_coef(t["hp"])) for t in TYPES))
    L.append("static const uint8_t DR_ROW[DR_TYPES] = {%s};          /* the type's (first) curve */" % ",".join(str(t["rows"][0]) for t in TYPES))
    L.append("/* the chain2 types' parts (0 for the first two): the shelf's high-pass (Q15) and the lows' share (Q15), the dry share into the clipper (Q10),")
    L.append(" * the grid's conduction point (Q12; 0: no bias shift), the DC blocker / output coupling pole (Q15), the fixed low-pass (Q15; 0: none) */")
    L.append("static const uint16_t DR_SH[DR_TYPES] = {%s};" % ",".join(str(hp_coef(t["shelf"][0]) if t.get("shelf") else 0) for t in TYPES))
    L.append("static const uint16_t DR_KL[DR_TYPES] = {%s};" % ",".join(str(int(round(t["shelf"][1] * 32768)) if t.get("shelf") else 32768) for t in TYPES))
    L.append("static const uint16_t DR_DRY[DR_TYPES] = {%s};" % ",".join(str(int(round(t.get("dry", 0.0) * 1024))) for t in TYPES))
    L.append("static const uint16_t DR_XC[DR_TYPES] = {%s};" % ",".join(str(int(round(-K_VGK0 / K_VS * 4096)) if t.get("bias") else 0) for t in TYPES))
    L.append("static const uint16_t DR_DCP[DR_TYPES] = {%s};" % ",".join(str(hp_coef(t["dc"]) if "dc" in t else DCB_10HZ) for t in TYPES))
    L.append("static const uint16_t DR_PLP[DR_TYPES] = {%s};" % ",".join(str(lp_coef(t["lp"]) if t.get("lp") else 0) for t in TYPES))
    L.append("#define DR_BIAS_KC %d                          /* the grid current's charge a sample (%.1f ms), Q15 */" % (int(round((1 - math.exp(-1 / (FS * BIAS_TC))) * 32768)), BIAS_TC * 1e3))
    L.append("#define DR_BIAS_KR %d                            /* its leak a sample (%.0f ms), Q15 */" % (int(round((1 - math.exp(-1 / (FS * BIAS_REC))) * 32768)), BIAS_REC * 1e3))
    L.append("/* the pre-gain, Q10, per type and GAIN (tenths 0..100; index 0 is the bypass, unused). Solved for each type's THD law (DISTORTION: dB) */")
    L.append("static const uint16_t DR_PRE[DR_TYPES][DR_NG] = {")
    for row in pre:
        L.append("  {" + ",".join(str(max(1, min(65535, int(round(G * PRE_Q))))) for G in row) + "},")
    L.append("};")
    L.append("/* the output trim, Q8, per type and GAIN (tenths): the dry level at the organ's level, +1 % a unit (+10 % at 10.0) */")
    L.append("static const uint16_t DR_TRIM[DR_TYPES][DR_NG] = {")
    for row in trims:
        L.append("  {" + ",".join(str(min(65535, int(round(t * 256)))) for t in row) + "},")
    L.append("};")
    L.append("/* the tone: the dark cutoff (Hz) per TONE step 0..8 (the low-pass), and the bright emphasis's own low-pass (Hz) */")
    L.append("static const uint16_t DR_TONE_LP[9] = {%s};" % ",".join(str(lp_coef(c)) for c in cut))
    L.append("#define DR_LP_BRIGHT %d" % lp_coef(2000.0))   # the bright emphasis pivots at 2 kHz
    L.append("#endif")
    with open(OUT, "w") as f:
        f.write("\n".join(L) + "\n")
    print("wrote", os.path.normpath(OUT))
    print("12AX7: bias %.3f V, plate %.1f V, ip %.3f mA" % (K_VGK0, K_VP0, 1e3 * koren_ip(K_VGK0, K_VP0)))
    for ti, ty in enumerate(TYPES):
        print("%-11s pre-gain" % ty["name"], ["%.3f" % pre[ti][g] for g in (1, 5, 20, 50, 100)],
              "trims", ["%.2f" % trims[ti][g] for g in (1, 5, 20, 50, 100)])
        if ty["top"]:
            print("            THD", ["%.1f:%.2f%%" % (g / 10, thd(ty, pre[ti][g])) for g in (1, 5, 20, 50, 100)])
    try:
        for g in (20, 50, 100):
            print("bass mud at GAIN %.1f (new lows 20..250 Hz, lows' share; dB):" % (g / 10),
                  ", ".join("%s %.1f / %.1f" % ((t["name"],) + mud(t, pre[i][g])) for i, t in enumerate(TYPES) if t["top"]))
    except ImportError:
        pass

if __name__ == "__main__":
    main()
