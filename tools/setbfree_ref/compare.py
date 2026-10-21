#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Guilherme Ruppert (OrganVave-1)
"""Render the same cases with setBfree's tone generator (sbf_ref) and ORGAN's (build/host/organ_test render) and
compare them: rms of each (dB), the difference's level against the reference (SNR, dB), the peak.

    python3 tools/setbfree_ref/compare.py SBF_REF ORGAN_TEST [OUTDIR]

SBF_REF: the binary tools/setbfree_ref/build.sh makes; ORGAN_TEST: build/host/organ_test. The wheels of setBfree are
looped wavetables with a 1-LSB dither, ORGAN's DDS sines: the waveforms agree to the dither's level at first, then drift
apart slowly in phase (the loops' frequency error, under 0.001 sample a period), so the SNR is taken over the first
0.25 s and the levels over the whole render.
"""
import os
import subprocess
import sys

import numpy as np

CASES = [
    # name, bars, seconds, events, env
    ("one key 888000000", "888000000", 0.75, "0.0:+24,0.5:-24", {}),
    ("one key 888000000, no crosstalk", "888000000", 0.75, "0.0:+24,0.5:-24", {"NOXT": "1"}),
    ("C major 888888888", "888888888", 0.75, "0.0:+24,0.0:+28,0.0:+31,0.0:+36,0.5:-24,0.5:-28,0.5:-31,0.5:-36", {}),
    ("C major 838000000 equal", "838000000", 0.75, "0.0:+24,0.0:+28,0.0:+31,0.5:-24,0.5:-28,0.5:-31", {"TEMP": "0"}),
    ("C major 008080400 gear 50", "008080400", 0.75, "0.0:+24,0.0:+28,0.0:+31,0.5:-24,0.5:-28,0.5:-31", {"TEMP": "2"}),
    ("ten keys (key compression)", "888000000", 0.75, ",".join("0.0:+%d" % k for k in range(20, 40, 2)), {}),
    ("a run of overlapping keys", "888800000", 1.0,
     ",".join("%.3f:+%d,%.3f:-%d" % (0.05 * i, 24 + i, 0.05 * i + 0.12, 24 + i) for i in range(16)), {}),
    ("a key repeated fast", "008000000", 0.5, ",".join("%.3f:+33,%.3f:-33" % (0.02 * i, 0.02 * i + 0.01) for i in range(12)), {}),
    ("swell 0.25", "888000000", 0.5, "0.0:+24", {"SWELL": "0.25"}),
]


def render(binary, out, bars, secs, ev, env):
    e = dict(os.environ)
    for k in ("TEMP", "NOXT", "SWELL"):
        e.pop(k, None)
    e.update(env)
    subprocess.run([binary, "render", out, bars, str(secs), ev], check=True, env=e)
    return np.fromfile(out, np.float32).astype(np.float64)


def db(x):
    return 20 * np.log10(max(x, 1e-12))


def main():
    sbf, org = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else "/tmp"
    os.makedirs(out, exist_ok=True)
    print("%-36s %9s %9s %8s %8s %8s" % ("case", "ref rms", "org rms", "d dB", "SNR dB", "peak"))
    worst = 1e9
    for i, (name, bars, secs, ev, env) in enumerate(CASES):
        a = render(sbf, os.path.join(out, "ref%d.f32" % i), bars, secs, ev, env)
        b = render(org, os.path.join(out, "org%d.f32" % i), bars, secs, ev, env)
        n = min(len(a), len(b))
        a, b = a[:n], b[:n]
        q = min(n, 11025)
        ra, rb = np.sqrt(np.mean(a ** 2)), np.sqrt(np.mean(b ** 2))
        snr = db(np.sqrt(np.mean(a[:q] ** 2))) - db(np.sqrt(np.mean((a[:q] - b[:q]) ** 2)))
        worst = min(worst, snr)
        print("%-36s %9.4f %9.4f %8.3f %8.1f %8.3f" % (name, ra, rb, db(rb) - db(ra), snr, np.abs(b).max()))
    print("worst SNR over the first 0.25 s: %.1f dB" % worst)


if __name__ == "__main__":
    main()
