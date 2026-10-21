# ORGAN.md: the organvave-1 plan

organvave-1 turns the M-VAVE FM-1 into a tonewheel organ: one instrument, no sequencer, no arpeggiator, no other
engines. The organ (tone generator, keys and buses, percussion, scanner vibrato / chorus, preamp) and the rotary speaker
are ported from setBfree (<https://github.com/pantherb/setBfree>, GPL-2.0-or-later; read at commit 2f41983b5b): rewritten
in integer C for the firmware, with the same algorithms and data; the floating-point original is the reference on the
host (`tools/`). Felucca's organ engine (`eng_wheel.c`) is not used. Approved by the owner on 2026-10-20, with the
amendments marked **(owner)**.

**Revision 2026-10-21 (after the slimming).** The budget below is measured on the slimmed tree; the rotary section is
revised: two rotary models, selectable on the screen and stored in every preset **(owner)**, and a better reduced
whirl that the freed SRAM allows (1024 angles, interpolated coefficients: approved by the owner on 2026-10-21).

Method (DEVELOPING.md section 10): float model, then fixed point, then host tests against the reference, then content,
then the UI.

## Measured budget (slimmed tree, Oct 2026, JieLi clang)

| | before the slimming | now | limit | free now (after the build's reserves) |
| --- | --- | --- | --- | --- |
| image (flash) | 533,648 | 257,944 | 581,564 | 323,620 |
| RAM .data + .bss | 65,896 | 16,248 | 73,728 | 53,384 (4 KiB reserve) |
| pool | 348,096 | 249,764 | 368,640 | 110,684 (8 KiB reserve) |

About 164 KB of SRAM and 316 KB of flash are free, before FMUG goes (0.1.0: about 85 KB of flash, of which the splash
57.6 KB comes back as the organ's splash). Pool users today: the delay line 131 KB (FMini-1's seven delays: 1.49 s,
mono int16), the UI canvas 60 KB, the reverb 35 KB, the user presets and their patches 10 KB, the chorus 4 KB. The delay line
is the one big lever left: half of it (0.74 s) frees 65 KB if the organ ever needs it.

**(owner)** The firmware keeps only the organ: every other engine, the SAMPLE data, the sequencer, the arpeggiator and
everything tied to them go, to free RAM and flash (done in the slimming; the sequencer and the arpeggiator stay until
0.1.0 replaces FMUG's UI). FMUG stays as a scaffold (sound and UI) until the organ engine replaces it in 0.1.0.

### Memory plan

| block | SRAM | flash | notes |
| --- | --- | --- | --- |
| tonewheels, keys, buses (3 manuals) | ~10 KB | ~6 KB | 91 wheel states, per-wheel bus levels int16 per manual, the tapering / crosstalk rules |
| envelopes and click | - | ~4 KB | generated tables (attack / release, noise burst, fixed seed) |
| scanner vibrato / chorus | ~1 KB | ~1 KB | |
| preamp | ~1 KB | ~1 KB | |
| rotary CLASSIC (improved whirl, below) | ~53 KB | ~37 KB | 37 KB of angle tables copied to SRAM at boot, four 1024-sample lines |
| rotary LIGHT (physical model, below) | ~3 KB | - | both models stay allocated: switching never reallocates |
| total new | ~68 KB | ~49 KB | of 164 KB / 316 KB free |

Static loop-instruction cost (method of `tests/target_budget.py`, probe kernels compiled with the chip's clang):

| block | instructions per sample |
| --- | --- |
| one tonewheel, one bus (DDS + interpolated sine) | 15 |
| one tonewheel, three buses with ramps (a key changing) | 32 |
| scanner vibrato / chorus | 46 |
| preamp (setBfree's structure) | 262 |
| rotary, setBfree whirl reduced (256-angle tables, the first plan) | 767 |
| rotary CLASSIC: whirl, 1024 angles, interpolated coefficients (below) | 951 |
| rotary CLASSIC, economy variant: 1024 angles, nearest coefficients | ~767 |
| rotary LIGHT: minimal physical model (stereo), + HORN tone filter | 210 + ~10 |

Active tonewheels measured in setBfree: 1 key 17 (9 without crosstalk), 4 keys 41, 10 keys 49 (33), all 61 keys 91 (the
ceiling: the cost depends on wheels, not keys, so polyphony is every key). Typical total with every block on about 2,000,
worst case about 2,900 (FMUG at 12 voices: 2,724; Felucca's ANALOG at 8 voices: 4,912), before the rotary: with CLASSIC
about 3,850 worst case, with LIGHT about 3,110 (the FX buses come on top in every case, as they did for FMUG). Static
counts, not cycles: only the device's CPU meter confirms them. If CLASSIC is too heavy on the device with every key down
and the drive on, the economy variant (same sidebands, about 19 dB more residual) is the first fallback; the second core
(removed in the slimming, in git history: X0X / Jangada) could carry the rotary if both fall short.

## Rotary speaker: two models, selectable **(owner)**

The ROTARY pop-up has a MODEL field: **CLASSIC** (the owner's option 2: setBfree's `whirl.c`, reduced) and **LIGHT** (the
owner's option 3: the minimal physical model). **(owner)** MODEL is part of every preset: loading a preset sets it, like
the rest of the rotary's values; there is no global override. Switching fades the output over about 20 ms and starts the
other model from its current rotor angles and speeds, so the rotation goes on without a jump. Both models are always
allocated.

**One set of parameters for both models**, so a preset's values mean the same whichever MODEL it names: horn and drum
speeds (slow, fast), acceleration and deceleration of each, STOP (the rotors brake and halt), HORN / DRUM balance, width
(the mic angle), leakage (the direct signal around the cabinet) and HORN character (default, mellow, sharp). In LIGHT the
HORN character is a tone filter on the horn path (about 10 instructions a sample) fitted to CLASSIC's three filter sets;
LIGHT's own internals (crossover frequency, AM and Doppler depths, reflection level) are factory tuning against CLASSIC,
not on the screen. CLASSIC's geometry (horn and drum radius, mic distance) is fixed: its angle tables are generated from
it in `tools/`.

### CLASSIC: the improved reduced whirl

Same structure as before: the two horn filters, six horn taps with the angle-dependent 5-tap FIR and de-interpolating
writes, the two reflection filters, six drum taps, the drum output filters, the speed laws, leakage and the mic angle.
What changes, measured on setBfree's own code (float, 44.1 kHz, sines at 300 Hz, 2 kHz and 5 kHz, slow and fast,
against the full 16384-angle `whirl.c`):

- **The first plan's error was not the resolution but how the small tables were built.** Built at 256 angles by
  `whirl.c`'s own code, the tables move the reverse-direction entries by one step and quantize the angle FIR's control
  points: the sidebands come out up to 3.1 dB (2 kHz) and 9.6 dB (5 kHz) off at slow speed, and the waveform differs at
  15 to 23 dB SNR at 2 to 5 kHz. Interpolating the coefficients alone does not fix it (it only removes the spurs).
- **The fix: build the full-resolution tables in `tools/` and sample them**, keeping the tap phases exact, then
  interpolate both the displacement and the FIR coefficients between angles. Measured: 256 angles 54 dB SNR, 512
  angles 64 dB, **1024 angles 71 dB** at 2 to 5 kHz (81 to 96 dB at 300 Hz), sidebands within 0.03 dB, spurs as the full
  one. With nearest coefficients instead (the economy variant): 52 dB at 1024 angles, 58 dB at 2048, sidebands within
  0.03 dB.
- **Chosen (owner, 2026-10-21): 1024 angles with interpolated coefficients.** Tables: forward and backward displacement for horn and drum
  (int32 Q8) and the forward and backward angle FIRs (int16 Q14): 37 KB, generated in `tools/` from the full model and
  copied to SRAM at boot (no flash reads in the audio interrupt). Lines of 1024 samples (the default geometry needs
  about 360; the headroom allows other mic distances and radii), int32: 16 KB. Cost 951 instructions a sample (static,
  chip compiler), against 767 for the first plan.
- **The horn character**: setBfree's three horn filter sets (default, mellow, sharp: `cfg/whirl-*.cfg`) as a HORN field
  in the pop-up; their biquad coefficients are generated in `tools/`, so the switch costs nothing per sample.
- Not taken: the full 16384-angle tables (about 330 KB even as int16, for no measurable gain over 1024 interpolated);
  setBfree's horn comb filter (disabled in its own build).

Reference measurements (setBfree, 2 kHz and 150 Hz sines): rotation horn 0.673 / 7.050 Hz, drum 0.599 / 5.951 Hz
(published, Leslie 147: horn 48 / 400 RPM, drum 40 / 342 RPM); horn FM at fast rms 33 to 36 cents (geometry with a 19.2
cm radius: +-43 cents peak far field); horn AM 33 to 35 dB peak to peak (left), 12.7 dB (right); drum time constants
measured at the output 4.10 s up (parameter 4.127), 1.48 s down (1.371). The drum model makes strong phase modulation
(-331 .. +28 cents at 150 Hz, fast); the literature calls the drum mostly AM: noted, to revisit with measurements. The
horn's angle coefficients in setBfree were read off a figure, not measured.

### LIGHT: the minimal physical model

A crossover (one low-pass biquad near 800 Hz, high = input - low), the horn with a direct and a reflected read tap per
mic (linear interpolation, delays ramped per block from the rotor angle), angle-dependent gains ramped per block; the
drum with one read tap and a gain per mic; one shared input line per rotor (256 samples). 210 instructions a sample,
about 3 KB. It has no reference of its own, so it is tuned against CLASSIC with the same measurements (rotation rates and
their time constants, horn and drum AM, FM in cents, the first sideband pairs, the left / right difference): the two
models then share speeds, acceleration and balance, and LIGHT is the cheaper approximation of CLASSIC, not a different
speaker.

### Alternatives studied (first plan)

The full `whirl.c` needs about 950 KB of tables and lines (the whole SRAM is 442 KB). Herrera / Hanson / Abel 2009
(time-varying FIR from measured impulse responses that are not published as data), Smith / Serafin / Abel / Berners 2002
(setBfree's horn already interprets its principal components), Pekonen et al. 2011 (spectral delay filters: cheap, no
measurements in the paper, not geometric Doppler), Calf's rotary (LGPL-2.1+), Surge XT / sst-effects (GPL-3.0), LeslieSim
(no licence: ideas only), mda Leslie (GPL-2+ or MIT, not read). None found in Guitarix (a Univibe only), the Faust
standard libraries, Pd's standard objects or Csound's opcodes.

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
- **Rotary**: above (CLASSIC from `whirl.c`, LIGHT our own).

## Effects section **(owner)**

REVERB, ROTARY (MODEL CLASSIC / LIGHT, speeds and acceleration, HORN / ROTOR balance, HORN character), DRIVE (gain, overdrive types, tone), DELAY and EQ, each
edited in a pop-up as FMini-1's FX page did.

## Interface

27 keys (F3..G5) with OCT- / OCT+; the upper manual on the keys and MIDI, the lower manual and the pedals on MIDI.
Pages: DRAWBARS (the nine bars drawn; SELECT moves a window of four, KNOB 1..4 set 0..8 with detents), PERC / VIB, the
effects (above), VOICE (click, crosstalk, tuning 60 / 50 Hz / equal, level), LOWER / PEDAL. A button switches the rotor
FAST / SLOW. MIDI CC as setBfree: upper drawbars 70..78 (inverted), swell CC 7 / 11, sustain toggles the rotor. Editor: new
commands from 85 (ORG_GET / ORG_PUT / ORG_PRESET), an INFO tag, a drawbar value format.

## Presets

A bank in physical units (registration "888000000", percussion, vibrato, rotary with its MODEL, drive in dB, click, level), 16 to 32
classic registrations with descriptive names (no trademarks, no artists), levels from `tools/preset_lab.py` on a chord.

## Tests (measure before listening, against setBfree)

Levels per drawbar and key; gear tuning (A4 = 439.99 Hz); percussion envelope and trigger; vibrato depth; rotary speeds,
time constants, AM, FM, sidebands, for both models (CLASSIC against setBfree's full `whirl.c`, LIGHT against CLASSIC),
a model switch without a click or a jump in the rotation; clipping with 61 keys at 888888888; 91 wheels within the budget; no click but the
designed one; the static budget of the new functions.

## Milestones

| version | contents |
| --- | --- |
| 0.0.x | the house: identity (`FM-1_8XY`), names, README / CREDITS, docs, the workflow manual only; the slimming |
| 0.1.0 | tone generator, upper manual, click, the drawbar page, about 8 presets; FMUG removed |
| 0.2.0 | percussion, vibrato / chorus |
| 0.3.0 | rotary (CLASSIC and LIGHT, the MODEL setting), drive, the effects section |
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
