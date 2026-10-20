# ORGAN.md: the organvave-1 plan

organvave-1 turns the M-VAVE FM-1 into a tonewheel organ: one instrument, no sequencer, no arpeggiator, no other
engines. The organ (tone generator, keys and buses, percussion, scanner vibrato / chorus, preamp) and the rotary speaker
are ported from setBfree (<https://github.com/pantherb/setBfree>, GPL-2.0-or-later; read at commit 2f41983b5b): rewritten
in integer C for the firmware, with the same algorithms and data; the floating-point original is the reference on the
host (`tools/`). Felucca's organ engine (`eng_wheel.c`) is not used. Approved by the owner on 2026-10-20, with the
amendments marked **(owner)**.

Method (DEVELOPING.md section 10): float model, then fixed point, then host tests against the reference, then content,
then the UI.

## Measured budget (FMini-1 1.7.0 tree, JieLi clang, before the slimming)

| | used | limit | free (after the build's reserves) |
| --- | --- | --- | --- |
| image (flash) | 533,648 | 581,564 | 47,916 |
| RAM .data + .bss | 65,896 | 73,728 | 3,736 (4 KiB reserve) |
| pool | 348,096 | 368,640 | 12,352 (8 KiB reserve) |

Largest flash users: the SAMPLE engine's samples (129 KB), fonts (92 KB), FMUG (85 KB, of which the splash 57.6 KB).
Largest pool users: the delay line (131 KB), the UI canvas (60 KB), the parts' engine memory (52 KB), the SLICER (33 KB),
the reverb (33 KB).

**(owner)** The firmware keeps only the organ: every other engine, the SAMPLE data, the sequencer, the arpeggiator and
everything tied to them go, to free RAM and flash. FMUG stays as a scaffold (sound and UI) until the organ engine replaces
it in 0.1.0.

Static loop-instruction cost (method of `tests/target_budget.py`, probe kernels compiled with the chip's clang):

| block | instructions per sample |
| --- | --- |
| one tonewheel, one bus (DDS + interpolated sine) | 15 |
| one tonewheel, three buses with ramps (a key changing) | 32 |
| scanner vibrato / chorus | 46 |
| preamp (setBfree's structure) | 262 |
| rotary, setBfree whirl reduced (256-angle tables) | 767 |
| rotary, minimal physical model (stereo) | 210 |

Active tonewheels measured in setBfree: 1 key 17 (9 without crosstalk), 4 keys 41, 10 keys 49 (33), all 61 keys 91 (the
ceiling: the cost depends on wheels, not keys, so polyphony is every key). Typical total with every block on about 2,000,
worst case about 2,900 (FMUG at 12 voices: 2,724; Felucca's ANALOG at 8 voices: 4,912). Static counts, not cycles: only
the device's CPU meter confirms them.

## Rotary speaker: decision

**(owner)** setBfree's `whirl.c`, reduced: the same structure (horn filters, six horn taps with the angle-dependent 5-tap
FIR and de-interpolating writes, two reflection filters, six drum taps, the drum output filters, the speed laws), tables of
256 angles with interpolation (also of the FIR coefficients) instead of 16384, 512-sample lines.

Why: the full `whirl.c` needs about 950 KB of tables and lines (the whole SRAM is 442 KB); the reduced float version
measures within 1.9 dB of AM, 6 cents of FM and 0.5 dB on the first three sideband pairs of the full one, at 767
instructions a sample and 4 to 8 KB of RAM. Alternatives studied: a minimal physical model (210 instructions, no reference
of its own), Herrera / Hanson / Abel 2009 (time-varying FIR from measured impulse responses that are not published as
data), Smith / Serafin / Abel / Berners 2002 (setBfree's horn already interprets its principal components), Pekonen et al.
2011 (spectral delay filters: cheap, no measurements in the paper, not geometric Doppler), Calf's rotary (LGPL-2.1+),
Surge XT / sst-effects (GPL-3.0), LeslieSim (no licence: ideas only), mda Leslie (GPL-2+ or MIT, not read). None found in
Guitarix (a Univibe only), the Faust standard libraries, Pd's standard objects or Csound's opcodes.

Reference measurements (setBfree, 2 kHz and 150 Hz sines): rotation horn 0.673 / 7.050 Hz, drum 0.599 / 5.951 Hz
(published, Leslie 147: horn 48 / 400 RPM, drum 40 / 342 RPM); horn FM at fast rms 33 to 36 cents (geometry with a 19.2
cm radius: +-43 cents peak far field); horn AM 33 to 35 dB peak to peak (left), 12.7 dB (right); drum time constants
measured at the output 4.10 s up (parameter 4.127), 1.48 s down (1.371). The drum model makes strong phase modulation
(-331 .. +28 cents at 150 Hz, fast); the literature calls the drum mostly AM: noted, to revisit with measurements. The
horn's angle coefficients in setBfree were read off a figure, not measured.

## Map of setBfree to the firmware

- **Generators and gears** (`tonegen.c` `initOscillators`, `gears60ratios` / `gears50ratios`): a 32-bit DDS per wheel on
  the shared sine table, increments for 60 Hz, 50 Hz and equal temperament generated in `tools/`. The looped wavetables
  (up to 4096 floats per wheel), the 1-LSB dither and the per-wheel harmonics (default: none) are cut.
- **Keys, buses, tapering, crosstalk** (`applyManualDefaults`, `taperingModel`, `applyDefaultCrosstalk`,
  `applyDefaultConfiguration`, `compilePlayMatrix`): 9,801 contributions per manual with crosstalk (194 at most per key,
  9,128 below -60 dB): computed at the key event from rules kept in flash (tapering per key and bus, terminal mixes, wiring
  crosstalk), per-wheel bus levels in RAM (int16: 1.6 KB per manual). Pruning only if a test shows it inaudible.
- **Envelopes and click** (`initEnvelopes`): the attack and release envelopes over 128 samples with the noise burst,
  generated in `tools/` with a fixed seed; each wheel keeps its place across four 32-sample blocks.
- **Percussion**: from the 4' or 2 2/3' bus contact whatever the drawbar, 1' muted while on, single trigger, 1 s / 4 s to
  -60 dB, soft -6 dB, normal lowers the swell (0.605); HIPASS_PERCUSSION and KEYCOMPRESSION as setBfree builds them.
- **Drawbar law**: **(owner)** linear, as setBfree (8 -> 4 = -6 dB).
- **Scanner vibrato / chorus** (`vibrato.c`): write-side interpolated variable delay, 7.25 Hz, depths 3 / 6 / 9 samples;
  chorus = the same mixed with the dry signal at 0.707. (Not the original's 16-stage LC line.)
- **Preamp / overdrive** (`overdrive.c`): an effective 9-tap input FIR, the biased 1/(1+x^2) curve with sag and feedback
  poles, a 33-tap output FIR (no real oversampling). `overdrive.c` has no licence header (generated tables): the FIRs are
  designed anew in `tools/`, the curve follows `ovt_biased.c` (GPL-2.0-or-later).
- **Reverb** (`reverb.c`, about 39 KB of lines): not ported; the firmware's own reverb bus is used.
- **Rotary**: above.

## Effects section **(owner)**

REVERB, ROTARY (speeds and acceleration, HORN / ROTOR balance), DRIVE (gain, overdrive types, tone), DELAY and EQ, each
edited in a pop-up as FMini-1's FX page did.

## Interface

27 keys (F3..G5) with OCT- / OCT+; the upper manual on the keys and MIDI, the lower manual and the pedals on MIDI.
Pages: DRAWBARS (the nine bars drawn; SELECT moves a window of four, KNOB 1..4 set 0..8 with detents), PERC / VIB, the
effects (above), VOICE (click, crosstalk, tuning 60 / 50 Hz / equal, level), LOWER / PEDAL. A button switches the rotor
FAST / SLOW. MIDI CC as setBfree: upper drawbars 70..78 (inverted), swell CC 7 / 11, sustain toggles the rotor. Editor: new
commands from 85 (ORG_GET / ORG_PUT / ORG_PRESET), an INFO tag, a drawbar value format.

## Presets

A bank in physical units (registration "888000000", percussion, vibrato, rotary, drive in dB, click, level), 16 to 32
classic registrations with descriptive names (no trademarks, no artists), levels from `tools/preset_lab.py` on a chord.

## Tests (measure before listening, against setBfree)

Levels per drawbar and key; gear tuning (A4 = 439.99 Hz); percussion envelope and trigger; vibrato depth; rotary speeds,
time constants, AM, FM, sidebands; clipping with 61 keys at 888888888; 91 wheels within the budget; no click but the
designed one; the static budget of the new functions.

## Milestones

| version | contents |
| --- | --- |
| 0.0.x | the house: identity (`FM-1_8XY`), names, README / CREDITS, docs, the workflow manual only; the slimming |
| 0.1.0 | tone generator, upper manual, click, the drawbar page, about 8 presets; FMUG removed |
| 0.2.0 | percussion, vibrato / chorus |
| 0.3.0 | rotary, drive, the effects section |
| 0.4.0 | lower manual and pedals over MIDI, swell, MIDI CC, the full bank |
| 0.5.0 | editor, site, installer, emulator, GitHub Pages on |
| 0.9.x | beta |
| 1.0.0 | after the owner tests it on the hardware |

## Phase 1 findings

- Identity: the loader always answers `ota-FM-1_900`; the installers compare only the model (text before `_`), never the
  number, so any FM-1 package installs over any other, by design. organvave-1 takes `FM-1_8XY` and its installer accepts
  only `FM-1_8\d\d`.
- The real overlap is the flash store: projects and user presets live in the same sectors with the same formats. New
  magics make each firmware see the other's data as empty (no corruption), but saving in one overwrites the other's.
- The Installer workflow has never run (0 runs); GitHub Pages is off. It is kept to `workflow_dispatch` until the site
  exists.
