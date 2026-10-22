# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Guilherme Ruppert (OrganVave-1)
"""OrganVave-1's factory bank, in physical units (CLAUDE.md: the bank is written in units, the generator converts):
tools/gen_organ_presets.py -> firmware/src/eng_organ_presets.h.

A preset: its name (no trademarks, no artists), the upper drawbars as the console writes them ("88 8000 000": 16' 5 1/3'
8' | 4' 2 2/3' 2' 1 3/5' | 1 1/3' 1'), the lower and the pedal drawbars (0.4.0 plays them; stored now), the tuning
("gear60", "gear50", "equal"), the crosstalk (setBfree's model on or off) and the reverb send (0..127). The level is not
written here: the generator sets it so that every preset's C major chord (C4 E4 G4) is as loud as JAZZ's at 0 dB.

From 0.3.0 a preset may carry a last field, a dict of effects (absent: both off):
  "rot": (model, speed, horn, width, horn_db)   model "classic" | "light"; speed "stop" | "slow" | "fast"; horn "normal" |
         "sharp"; width in degrees (180 widest .. 0 narrowest, 30 a step); horn_db the horn's level against the drum (1.5 dB steps)
  "drv": (gain_db, character)   gain_db re setBfree's default (2 dB steps, -12 .. +12); character 0..8 (0: setBfree's default)
The rotary or the drive switched on in the panel starts from the neutral values the generator writes for a preset without it
(width 180, horn level even, gain 0 dB). The rotary costs CPU: CLASSIC is the heavy model (about 1575 instructions a sample),
LIGHT the cheap one, so most presets use LIGHT until the instrument's CPU meter says otherwise.
"""

BANK = [
    # name,          upper,         lower,         pedal,      tuning,   crosstalk, reverb, effects (optional)
    ("FULL ORGAN",   "88 8888 888", "00 8888 000", "80 8000 000", "gear60", True, 30, {"rot": ("classic", "slow", "normal", 180, 0)}),
    ("GOSPEL",       "88 8800 000", "00 8880 000", "80 6000 000", "gear60", True, 30, {"rot": ("classic", "fast", "normal", 180, 0), "drv": (0, 2)}),
    ("JAZZ",         "88 8000 000", "00 8800 000", "60 4000 000", "gear60", True, 24, {"rot": ("light", "slow", "normal", 180, 0)}),
    ("BALLAD",       "68 8600 000", "00 6600 000", "60 4000 000", "gear60", True, 36, {"rot": ("light", "slow", "normal", 120, 0)}),
    ("WHISTLE",      "80 0000 008", "00 8000 000", "60 4000 000", "gear60", True, 30),
    ("CLARINET",     "00 8040 000", "00 6000 000", "50 3000 000", "gear60", True, 36),
    ("REED",         "00 6876 540", "00 4440 000", "60 4000 000", "gear60", True, 36, {"rot": ("light", "slow", "sharp", 180, 0)}),
    ("SWEET",        "87 6543 211", "00 7600 000", "70 5000 000", "gear60", True, 36, {"rot": ("light", "slow", "normal", 180, 0), "drv": (-4, 0)}),
    ("BLUES",        "80 8800 000", "00 8600 000", "60 4000 000", "gear60", True, 30, {"rot": ("light", "slow", "normal", 150, 0), "drv": (2, 1)}),
    ("ROCK",         "88 8800 008", "00 8800 000", "80 6000 000", "gear60", True, 24, {"rot": ("classic", "fast", "sharp", 180, 0), "drv": (4, 4)}),
    ("SOUL",         "80 8860 000", "00 7600 000", "60 4000 000", "gear60", True, 30, {"rot": ("light", "slow", "normal", 120, 0)}),
    ("CHURCH",       "60 8640 220", "00 6420 000", "80 6000 000", "gear50", True, 48),
    ("STRINGS",      "00 4568 654", "00 4440 000", "50 3000 000", "equal",  False, 40, {"rot": ("light", "slow", "normal", 180, 0)}),
    ("FLUTE",        "00 8400 000", "00 5200 000", "40 2000 000", "gear60", True, 36),
    ("TREMOLO",      "00 8800 000", "00 6600 000", "60 4000 000", "gear60", True, 30, {"rot": ("classic", "fast", "normal", 180, 0)}),
    ("BRIGHT",       "00 8888 800", "00 8800 000", "60 4000 000", "gear60", True, 30, {"rot": ("light", "fast", "sharp", 180, 0), "drv": (0, 0)}),
]
