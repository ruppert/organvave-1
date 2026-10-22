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

Since 0.1.0 FMUG is gone and the organ's splash takes its place in flash (the current figures are in DEVELOPING.md,
section 1). Pool users today: the delay line 131 KB (FMini-1's seven delays: 1.49 s,
mono int16), the UI canvas 60 KB, the reverb 35 KB, the user presets and their patches 10 KB, the chorus 4 KB. The delay line
is the one big lever left: half of it (0.74 s) frees 65 KB if the organ ever needs it.

**(owner)** The firmware keeps only the organ: every other engine, the SAMPLE data, the sequencer, the arpeggiator and
everything tied to them go, to free RAM and flash (done in the slimming). FMUG, the FMini-1 synth, went in 0.1.0 with its
UI: the organ's panel and screens replaced it.

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
  (int32 Q12 samples: a delay is a phase, Q8 cost 8 dB) and the forward and backward angle FIRs (int16 Q15): 37 KB, generated in `tools/` from the full model and
  copied to SRAM at boot (no flash reads in the audio interrupt). Lines of 1024 samples (the default geometry needs
  about 360; the headroom allows other mic distances and radii), int32: 16 KB. Cost (static, chip compiler, measured on the built
  firmware) 843 loop instructions plus six calls of the 122 of a horn tap: about 1,575 a sample, against the plan's 951 (the
  guard bits, the Q15 FIR summed before one rounding, the drum shelf's three-part multiply). Measured against setBfree's own
  `whirl.c` (waveform SNR, left / right): horn 400 rpm 79 / 69 dB, both rotors 77 / 73, slow 79 / 69, drum alone 74 / 73, a
  chord 83 / 83; the sidebands within 0.05 dB. What counted: the displacement at the angle's fraction (setBfree's is a float),
  the delays in Q12, the speed ramp carrying its fraction, the drum shelf's headroom (its state is 45 times the input).
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
drum with one read tap and a gain per mic; one shared input line per rotor (512 samples). Built: 337 loop instructions
a sample plus 144 a block, 4 KB, against the plan's 210 (its tone biquad, a Q14 direct form I crossover and the model's six
ramps are in it). It has no reference of its own, so it is tuned against CLASSIC with the same measurements (rotation rates and
their time constants, horn and drum AM, FM in cents, the first sideband pairs, the left / right difference): the two
models then share speeds, acceleration and balance, and LIGHT is the cheaper approximation of CLASSIC, not a different
speaker. **Measured** (`tests/rotary_light_test.c`, `tools/rotary_light/`): the sidebands of nine tones within 5 to 10 dB rms of
CLASSIC's (mean 6.4), the levels within 1.4 to 4.1 dB (6.7 for a 500 Hz tone on the drum: the crossover's region), the same
at slow and fast speeds; a character match (rates, depth of the modulation, left / right difference), not a sideband-by-sideband
one. The horn's tone is one biquad fitted to CLASSIC's horn filters above 500 Hz: 1.0 dB rms (default), 4 dB (sharp).

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
  **(owner)** No extras beyond setBfree here: no always-on leakage (the crosstalk above is the only bleed, heard when a
  contact closes) and no click tone / noise-frequency control (the click stays setBfree's contact bounce).
- **Percussion**: from the 4' or 2 2/3' bus contact whatever the drawbar, 1' muted while on, single trigger, 1 s / 4 s to
  -60 dB, soft -6 dB, normal lowers the swell (0.605); HIPASS_PERCUSSION and KEYCOMPRESSION as setBfree builds them.
- **Drawbar law**: **(owner)** linear, as setBfree (8 -> 4 = -6 dB).
- **Scanner vibrato / chorus** (`vibrato.c`): write-side interpolated variable delay, 7.25 Hz, depths 3 / 6 / 9 samples;
  chorus = the same mixed with the dry signal at 0.707. (Not the original's 16-stage LC line.)
- **Preamp / overdrive** (`overdrive.c`, ported in `src/eng_drive.c`, 0.3.0): an effective 9-tap input FIR, the biased curve
  with sag and feedback poles, a 33-tap output FIR (no real oversampling: setBfree sums the four polyphase rows of its
  interpolation filter for every sample and filters the curve's output at the same rate). The FIRs are not redesigned: the
  firmware takes setBfree's own taps (the sum of the four rows, and the output filter, which has only odd taps and the centre),
  dumped by `tools/setbfree_ref/drive_render --fir` into `tools/setbfree_ref/drive_fir.txt` and turned into
  `src/eng_drive_tables.h` by `tools/gen_drive_tables.py`. The curve of `ovt_biased.c` is odd, v(u) = sign(u) (D(|u| + bias) -
  D(bias)) with D(x) = x^2 / (1 + x^2), so one 1025-entry table (1/64 steps up to 16, 2 KB) serves it, and the post-differentiator
  (pole 0.999) is written as a low-pass with a 28-bit state (a 16-bit one stalls at its tiny coefficient). The input is Q13 (1.0 = 8192, up to 4.0: a loud chord is several times 1.0), the output Q15. The sag, the
  feedback poles and the global feedback are setBfree's defaults (gain 3.5675 in, 0.8795 out, fb 0.5821, fb2 0.999, gfb 0.6214,
  bias 0.5347, sag 0.991 / 0.0094). Measured against `overdrive.c` itself (sines at 110 Hz and 1 kHz, 0.1 to 1.0 full
  scale): the odd harmonics within 0.11 dB, no even ones (below -99 dB, as the reference), waveform SNR 48 to 62 dB; the cost
  is 201 loop instructions a block (about 150 a sample). In the instrument the output gain is setBfree's plus 3 dB (`DRV_OG` in `fx.c`): the curve saturates, and with a few keys the driven mix comes out 2 to 16 dB below the dry one. Limits: the pre-differentiator's state is clamped (its gain is
  1 / (1 - fb), 1000 at fb 0.999, outside the usable range); the input of the stage after the drive must be clamped (the
  largest gains put the output at 7 times full scale). Nothing here is tested on a real FM-1.
- **Reverb** (`reverb.c`, about 39 KB of lines): not ported; the firmware's own reverb bus is used.
- **Rotary**: above (CLASSIC from `whirl.c`, LIGHT our own).

## Effects section **(owner)**

REVERB, ROTARY (MODEL CLASSIC / LIGHT, speeds and acceleration, HORN / ROTOR balance, HORN character), DRIVE (gain, overdrive types, tone), DELAY and EQ, each
edited in a pop-up as FMini-1's FX page did.

## Interface

**(owner, 2026-10-21)** The whole interface is drawn after the real console's panel: its look and its controls (drawbars
with their brown / black / white caps and engraved footages, the rocker tabs of percussion and vibrato, the scanner's
rotary selector, the wood and the chrome trim, the half-moon rotor switch), not FMini-1's synth panel. The screens are
mocked up first with the firmware's own drawing code, for the owner's approval, then built in 0.1.0. The owner's references (2026-10-21):
the drawbars as on the console (a dark stem with light rails and the numbers 8 .. 1, a block cap with a lit top face and a
finger notch), the toggles as the console's tall cream rocker tabs (upper word, function, lower word), the rotary speed as
a slide switch with a glossy black ball on an engraved grey plate (SLOW / STOP / FAST). `tools/ui_mockups.c` draws them;
`docs/mockups/` holds the current set. Approved for now by the owner (2026-10-21): the base of the 0.1.0 interface.

27 keys (F3..G5) with OCT- / OCT+; the upper manual on the keys and MIDI, the lower manual and the pedals on MIDI.
Pages: DRAWBARS (the nine bars drawn; SELECT moves a window of four, KNOB 1..4 set 0..8 with detents), PERC / VIB, the
effects (above), VOICE (click, crosstalk, tuning 60 / 50 Hz / equal, level), LOWER / PEDAL. A button switches the rotor
FAST / SLOW. MIDI CC as setBfree: upper drawbars 70..78 (inverted), swell CC 7 / 11, sustain toggles the rotor. Editor: new
commands from 85 (ORG_GET / ORG_PUT / ORG_PRESET), an INFO tag, a drawbar value format.

### MIDI (0.4.0)

setBfree's map. The channel picks the manual: 1 the upper (MIDI 36..96 are its 61 keys), 2 the lower (36..96), 3 the pedals
(24..55, 32 keys); with GLO > MIDI CHANNEL set to N the three are N, N + 1, N + 2 (round the 16) and other channels are
ignored; with ALL every other channel plays the upper, as before 0.4.0. CC 7 and 11 are the swell (127: the level the factory
presets are balanced at, 0: silent; setBfree's default gain for no CC is not reproduced). CC 70..78 set the nine drawbars of
the channel's manual, inverted (127: 0, 0: 8). CC 1 sets the rotor speed (0..42 SLOW, 43..85 STOP, 86..127 FAST) and CC 64
(the sustain pedal, on press) toggles SLOW and FAST. CC 123 releases the channel's manual, CC 120 everything. The panel's
27 keys play the upper manual only; the LOWER and PEDAL drawbars are edited on the second and third press of EDIT.
The drawbar and speed CCs are noted in the audio ISR and applied by the main loop (organ_app.c `oa_cc_poll`); the notes and
the swell go straight to the audio path (midi_control.c). Not mapped: setBfree's percussion volume (CC 81), the transposes
and the keyboard split.

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
| 0.0.x | the house: identity (`FM-1_8XY`), names, README / CREDITS, docs, the workflow manual only; the slimming; the UI mock-ups |
| 0.1.0 (now) | tone generator, upper manual, click, the drawbar page, 8 factory patches; FMUG removed |
| 0.2.0 | percussion, vibrato / chorus (done in the firmware; PERC & VIB page, editor controls; not yet on an FM-1) |
| 0.3.0 | rotary (CLASSIC and LIGHT, the MODEL setting), drive (done in the firmware; ROTARY and DRIVE pages, editor controls; not yet on an FM-1) |
| 0.3.1 | REVERB and DELAY pages: send, type, size and tone; delay send, type, time, feedback, colour, mix (UI over the existing engine; the sends are saved with the preset, the rest with a user preset; not yet on an FM-1). The EQ came in 0.5.0 |
| 0.4.0 | lower manual and pedals over MIDI (channels 1 / 2 / 3, or GLO's channel N, N + 1, N + 2), the swell (CC 7 / 11), the drawbar CCs 70..78, the rotor speed (CC 1, CC 64), LOWER and PEDAL drawbar pages (EDIT), a bank of 16 factory presets (not yet on an FM-1). |
| 0.5.0 | the equaliser: LOW (120 Hz shelf), MID (a peak, Q 0.8, twelve frequencies 160 Hz .. 7.2 kHz) and HIGH (4 kHz shelf), each -12 .. +12 dB in 1 dB steps, after MASTER and before the limiter; EQ page (FX button), editor controls, patch bytes 43..47 (zero is flat) (not yet on an FM-1). The editor, site, installer, emulator and GitHub Pages came early, with 0.1.0, at the owner's request |
| 0.9.0 | beta: overload fallback (`fx_cpu_high`, called by the shedding: "CPU HIGH" in the footer of every page, the delay off for 2 s, the oldest held key released at the next fragment; the rotary and the drive are left alone), stress test with all 96 keys of three manuals and every effect on (bounded, falls silent) (not yet on an FM-1) |
| 0.9.1 | fix: the rotary cabinet (both models) hard-clipped its input at +-1.0 (Q15) while the dry path hands a chord, several times that, to the soft limiter; the input is now taken down by 4 and the output brought back by 4 before MASTER (tests/effects_test.c section 10: the output follows the swell linearly) (not yet on an FM-1) |
| 0.9.2 | the panel (owner): SELECT picks an item and ALGORITHM changes it on every FX page and on DRAWBARS; DRIVE: TYPE (DISTORTION, the former drive; TUBE, a cubic valve curve with a triode's asymmetry, no latency), GAIN, TONE (a tilt around 1 kHz), OUTPUT, the character is gone; ROTARY: the Leslie's horn and drum speeds and ramps; LOWER and PEDAL on one page, the pedal with its two real bars (patch bytes 48..56; (not yet on an FM-1) |
| 0.9.x | further betas |
| 1.0.0 | after the owner tests it on the hardware |

## Phase 1 findings

- Identity: the loader always answers `ota-FM-1_900`; the installers compare only the model (text before `_`), never the
  number, so any FM-1 package installs over any other, by design. organvave-1 takes `FM-1_8XY` and its installer accepts
  only `FM-1_8\d\d`.
- The real overlap is the flash store: projects and user presets live in the same sectors with the same formats. New
  magics make each firmware see the other's data as empty (no corruption), but saving in one overwrites the other's.
- The Installer workflow has never run (0 runs); GitHub Pages is off. It is kept to `workflow_dispatch` until the site
  exists.
