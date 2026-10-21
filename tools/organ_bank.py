# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Guilherme Ruppert (OrganVave-1)
"""OrganVave-1's factory bank, in physical units (CLAUDE.md: the bank is written in units, the generator converts):
tools/gen_organ_presets.py -> firmware/src/eng_organ_presets.h.

A preset: its name (no trademarks, no artists), the upper drawbars as the console writes them ("88 8000 000": 16' 5 1/3'
8' | 4' 2 2/3' 2' 1 3/5' | 1 1/3' 1'), the lower and the pedal drawbars (0.4.0 plays them; stored now), the tuning
("gear60", "gear50", "equal"), the crosstalk (setBfree's model on or off) and the reverb send (0..127). The level is not
written here: the generator sets it so that every preset's C major chord (C4 E4 G4) is as loud as JAZZ's at 0 dB.
"""

BANK = [
    # name,          upper,         lower,         pedal,      tuning,   crosstalk, reverb
    ("FULL ORGAN",   "88 8888 888", "00 8888 000", "80 8000 000", "gear60", True, 30),
    ("GOSPEL",       "88 8800 000", "00 8880 000", "80 6000 000", "gear60", True, 30),
    ("JAZZ",         "88 8000 000", "00 8800 000", "60 4000 000", "gear60", True, 24),
    ("BALLAD",       "68 8600 000", "00 6600 000", "60 4000 000", "gear60", True, 36),
    ("WHISTLE",      "80 0000 008", "00 8000 000", "60 4000 000", "gear60", True, 30),
    ("CLARINET",     "00 8040 000", "00 6000 000", "50 3000 000", "gear60", True, 36),
    ("REED",         "00 6876 540", "00 4440 000", "60 4000 000", "gear60", True, 36),
    ("SWEET",        "87 6543 211", "00 7600 000", "70 5000 000", "gear60", True, 36),
]
