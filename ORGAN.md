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
| rotary LIGHT (physical model; removed in 0.9.23) | ~3 KB | - | - |
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
| rotary LIGHT: minimal physical model (stereo), + HORN tone filter (removed in 0.9.23) | 210 + ~10 |

**0.9.28, measured on the host (callgrind, x86 instructions a sample, 10 keys, every upper bar out, vibrato C3, percussion,
drive, HALL, delay, CLASSIC 4 FAST):** the tonewheels were 62 % of the render, the rotary 26 %, the reverb 5 %, the rest 7 %. A wheel
of a manual on the scanner computed its sine twice (the dry sum and the scanner's share, taken out again after the scanner);
`org_wheel_sc` / `org_wheel_scp` now compute it once and add the dry share, the scanner's share and the percussion's in one loop:
the total fell from 8,233 to 6,511 (10 keys) and from 8,634 to 6,735 (every wheel), the goldens unchanged. HOME's CPU meter shows
the render and, after a plus, TIMER5's time nested in it ("CPU 72+5%"): the overload check counts both against 93 %.

**0.9.36, a much reduced crosstalk (owner).** The crosstalk cost the render, not the key events: each key woke about 10 more
wheels (its neighbours, at -40 .. -60 dB of its main contribution) and gave the wheels of the drawbars at 0 a faint gain, so they
were all rendered (10 keys, 80 8800 000: 83 wheels against 23 without it, 2,330 against 911 x86 instructions a sample). Two
changes: (A) a key keeps only its entries within 40 dB of its strongest (the flash table from `tools/gen_organ_tables.py`, 10,687
entries -> 2,267, and the same rule in `org_xt_build` for ADVANCED EDIT's RAM table; `org_xt_prune` 0 builds setBfree's full model,
which the tests still compare exactly); (D) a wheel under -30 dB of a full one (in practice the leakage) takes the sine without its
interpolation (`ORG_FAINT`): under -80 dB of a full wheel on a crosstalk chord. Measured on the host: 1 key 627 -> 316 (no crosstalk:
312), 3 keys 1,148 -> 541, 10 keys 2,330 -> 1,254, changing chords -35 .. -42 %; with 30 keys or more every wheel sounds anyway
(little or no change). The nine cases against setBfree: within 0.24 dB rms, 0.31 dB on the attack.

**0.9.38, the crack of a high chord (owner: preset 001, the percussion off, a C major triad high up).** Two stages bent it. The
Leslie's input took the mix / 4 and clipped at 1.0, four full wheels: a high triad (its notes share wheels, which add in phase) went
past it in thousands of samples (C6: 14.6 % of added distortion against an all-linear chain). It now takes the mix / 16 and gives
the 12 dB back after the cabinet. The master's limiter followed every peak (attack ~0.1 ms): on a chord its gain moved with the
notes' beats (about 5 %), and a tanh over the whole range bent the signal it held at 0.55 of full scale. Now the attack is ~6 ms and
the tanh is a knee, linear up to 0.8 of full scale and bending towards 0.9 above (it takes the start of a note the slower attack
lets through; the health checks' 30000 stays free): C4 / C5 / C6 triads 6.1 / 4.8 / 14.6 % -> 2.0 / 1.8 / 1.4 %.

**0.9.39, still cracking (owner: the same chord, the Leslie on, and more with it off).** The limiter of 0.9.38 still lagged the
peaks: without the Leslie 1.7 .. 2.4 % of the samples went over 0.8 of full scale into the knee, a clip at audio rate. The limiter
now looks ahead 32 samples (0.73 ms of latency): the gain comes down over about 16 samples to what the loudest sample in the
window needs (LIM_T 24000, 0.73 of full scale) before it plays, holds 300 ms (the chord's beats do not move it) and returns over
~370 ms. On preset 001's C4 .. C6 triads, the Leslie on and off, no sample reaches the knee (the energy left at new frequencies is
0.2 .. 0.5 %, the gain's own movement, against 0.5 .. 2.8 % in 0.9.38); the output is 2 .. 3 dB quieter at the same MASTER, and
MASTER turned up only compresses more. The gain is applied exactly (lim_scale: a truncated >> 4 made a -1 remainder a DC of -16).

**0.9.40, the level moved by itself (owner: "a compressor acting in the sound").** It was the limiter: the organ reached the master far
above LIM_T, so the limiter compressed all the time (preset 001, the percussion off: at MASTER 2048 a triad lost 6 dB, eight notes
10 dB; at the top 8 .. 19 dB), and its 300 ms hold and ~370 ms return made the level breathe as chords changed. Gain staging: the
organ now enters the master 14 dB lower (ORG_TO_MASTER, fx.c; MASTER, the Leslie path, the dry path and USB alike). At MASTER 2048
nothing is limited; at the top a note, a triad or a four-note chord with its bass pass untouched, two hands (eight notes) lose about
2 dB, a high triad without the Leslie about 5 dB. The output is quieter at the same MASTER (turn it up); at the top the chords are
about as loud as before (they were held at LIM_T), single notes about 6 dB quieter (the compression removed). setBfree's key
compression (up to -5.7 dB with many keys) stays: it is the instrument's own.

Active tonewheels measured in setBfree: 1 key 17 (9 without crosstalk), 4 keys 41, 10 keys 49 (33), all 61 keys 91 (the
ceiling: the cost depends on wheels, not keys, so polyphony is every key). Typical total with every block on about 2,000,
worst case about 2,900 (FMUG at 12 voices: 2,724; Felucca's ANALOG at 8 voices: 4,912), before the rotary: with CLASSIC
about 3,850 worst case, with LIGHT about 3,110 (the FX buses come on top in every case, as they did for FMUG). Static
counts, not cycles: only the device's CPU meter confirms them. If CLASSIC is too heavy on the device with every key down
and the drive on, the economy variant (same sidebands, about 19 dB more residual) is the first fallback; the second core
(removed in the slimming, in git history: X0X / Jangada) could carry the rotary if both fall short.

## Rotary speaker: two models, selectable **(owner)**

The ROTARY pop-up has a MODEL field: **CLASSIC** (the owner's option 2: setBfree's `whirl.c`, reduced) and its variants CLASSIC 2 to 5 (0.9.23; LIGHT was removed). **(owner)** MODEL is part of every preset: loading a preset sets it, like
the rest of the rotary's values; there is no global override. Switching fades the output over about 20 ms and starts the
other model from its current rotor angles and speeds, so the rotation goes on without a jump. Both models are always
allocated.

**One set of parameters for both models**, so a preset's values mean the same whichever MODEL it names: horn and drum
speeds (slow, fast), acceleration and deceleration of each, STOP (the rotors brake and halt), HORN / DRUM balance, width
(the mic angle), leakage (the direct signal around the cabinet) and HORN character (default, mellow, sharp). (Before 0.9.23, LIGHT's HORN character was a tone filter on the horn path, about 10 instructions a sample, fitted to CLASSIC's three filter sets;
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

### The CLASSIC models (0.9.23)

LIGHT was removed in 0.9.23 (and the 256-angle model). The rotary's models are all the CLASSIC engine (setBfree's
`whirl.c`, the tables at 1024 angles) with a variant: CLASSIC 2 (the horn's FIR and delay every 8 samples, ramped), CLASSIC 3
(four horn taps), CLASSIC 4 (the drum every 4 samples and four horn taps) and CLASSIC 5 (the horn's geometry every 4
samples). Their levels are matched to CLASSIC's in dB (`ROT_VG`, measured by `tests/rotary_test.c`), and each stays
within the bounds that test checks (30 dB SNR for CLASSIC 2 and 5, 10 to 25 dB for CLASSIC 3 and 4).

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
- **Drive** (0.9.24, rewritten from scratch; two types since 0.9.30; `src/eng_drive.c`, the tables from `tools/gen_drive_tables.py`):
  the setBfree port of 0.3.0 is gone. Each type is a short integer chain (input high-pass, pre-gain, waveshaper table, a 10 Hz DC
  blocker, tone, output trim): **TUBE DRIVE** (made here: a warm, slightly asymmetric soft clip) and **DISTORTION** (a JCM900-style
  two-stage clip). TUBE SCREAM and TUBE PREAMP (0.9.24 .. 0.9.29) were removed (owner); their type bytes play as TUBE DRIVE.
  GAIN 0.0..10.0 in steps of 0.1, 0 bypasses the drive. **(owner, 0.9.30)** TUBE DRIVE's GAIN follows its harmonic distortion on an
  exponential law, THD(g) = 25.2 (e^(0.085 g) - 1) % for a sine at the organ's level (0.9.31: the top 25 % above 0.9.30's 27 %, a
  little less exponential): 0.2 % at 0.1 (inaudible), 1.1 % at 0.5, 4.7 % at 2.0, 13.4 % at 5.0, 33.75 % at 10.0; the pre-gain of every tenth is solved for it in the generator, and the firmware's chain
  follows it (tests/drive_test.c). DISTORTION keeps 3 dB of pre-gain a unit. The organ comes into the drive 12 dB down (fx.c: a
  five-note chord at velocity 100 is RMS 0.73 there, peaks under the input's 4.0) and the output, Q15 up to 4.0, is brought back;
  the trims hold the dry level on an organ-like chord at that level, +1 % a unit (+10 % at 10.0). Every shift rounds to the nearest:
  with TUBE DRIVE's trim at 39x at GAIN 0.1, truncation was a DC of 9 %. TONE: dark (a low-pass, 1.5 kHz to 5.2 kHz) or bright (a
  2 kHz-pivot treble lift); OUTPUT +-8 dB. Cost: 90 loop instructions a block (78 before 0.9.30: the rounding and the DC blocker). Known: well under the organ's level (a single
  drawbar's note) DISTORTION, which compresses, is about 10 dB louder than TUBE DRIVE. Nothing here is tested on a real FM-1, and the
  sound has not been listened to.
  **(owner, 0.9.41) Four more types**, on `dr_chain2` (the same steps plus a cathode shelf, a dry share into the clipper, the grid's
  bias shift, the DC blocker at the type's own corner and a fixed low-pass; 142 loop instructions), each on TUBE DRIVE's
  exponential law scaled to its own top: **SOFT TUBE** (byte 4: a gentle triode-like curve, the positive half bending first, mostly
  the second harmonic; 15 % at 10.0), **MID DRIVE** (byte 5: the green mid-boost pedal without its diodes, x + G hp720(x) clipped by
  the op-amp's rails; 30 %), **HARD CLIP** (byte 6: a flat clip; 40 %) and **TIGHT TUBE** (byte 7: a circuit model of one 12AX7
  common-cathode stage: Koren's triode equations solved on the load line, B+ 250 V, 100 k plate, 1.5 k cathode, -1.2 V bias and a
  gain of 60; the input coupling capacitor at 120 Hz, the cathode partly bypassed (the lows at 30 % of the gain below 400 Hz), the
  grid current's bias shift (1.5 ms charge, 22 ms leak) and the output coupling capacitor into the next stage at 100 Hz; 25 %).
  The owner asked for TIGHT TUBE as TUBE DRIVE without its weight in the bass: on a C2 + C3 triad organ chord it leaves 8 .. 9 dB
  less new low content (20 .. 250 Hz away from the chord's partials: the intermodulation's difference tones) than TUBE DRIVE at the
  same GAIN (tests/drive_test.c: -26.8 against -18.1 dB re the output at 5.0, -22.0 against -13.8 at 10.0). TUBE DRIVE and
  DISTORTION keep their curves and tables. In both chains the DC blocker and the low-passes carry their rounding remainders now
  (error feedback): rounded to the nearest, the DC blocker stopped at |y| < 349 and left a DC of about -40 dB after the sound.
  **(owner, 0.9.42) More gain at the top**: SOFT TUBE 30 % at 10.0 (twice; its top pre-gain 2.0 -> 5.6), TIGHT TUBE 50 % (twice;
  5.8 -> 11.0, near the stage's most: 50.3 % at 12, past it the THD falls, so the generator searches the rising side only), MID
  DRIVE 40 % (50 % more was asked, 45 %: a symmetric clip cannot pass 42.9 % on this measure, a perfect square's harmonics 2 .. 9;
  its clipper input and rails taken down by 4 for the room, the pre-gain at the top about 4x 0.9.41's, its fixed low-pass opened
  from 4 to 6 kHz). At the same THD TIGHT TUBE still leaves 6 .. 7 dB less new low content than TUBE DRIVE.
  **(owner, 0.9.43) TIGHT TUBE 40 % more gain at 10.0**: its THD is at the stage's most there and cannot rise, so past 5.0 its
  pre-gain goes on rising above its law, to 1.4x at 10.0 (11.0 -> 15.3): more saturation and compression; the THD measured eases
  from the peak to about 48 % at 10.0. At the same THD it still leaves about 6 dB less new low content than TUBE DRIVE.
- **Reverb** (`reverb.c`, about 39 KB of lines): not ported; the firmware's own reverb bus is used.
- **Rotary**: above (CLASSIC from `whirl.c`, and its variants; LIGHT, our own model, was removed in 0.9.23).

## Effects section **(owner)**

REVERB, ROTARY (MODEL CLASSIC 1 to 5, speeds and acceleration, HORN / ROTOR balance, HORN character), DRIVE (gain, overdrive types, tone), DELAY and EQ, each
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
(24..55, 32 keys). Since 0.9.5 GLO sets each channel: the upper is ALL (every channel the other two do not take) or 1..16, the
lower and the pedals are OFF or 1..16 (defaults 2 and 3, as setBfree). With SPLIT on (ADVANCED EDIT), the upper channel's notes below
SPLIT POINT also play the lower manual and the pedals. CC 7 and 11 are the swell (127: the level the factory
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
| 0.9.3 | the Leslie defaults (owner): horn FAST 390 rpm, horn ramp 1.5 s (deceleration twice that); the sliders and the ROTARY page scale around them (not yet on an FM-1) |
| 0.9.4 | ADVANCED EDIT (owner): EDIT walks UPPER, LOWER + PEDAL, then a page of 27 rows with setBfree's tone-generator settings (key click, crosstalk, tuning, percussion, scanner); the patch grows to 96 bytes (57..81), the crosstalk and click tables are built in RAM while no key holds them (not yet on an FM-1) |
| 0.9.6 | the release model CLICK by default (setBfree's is LINEAR: the release level was not heard before), ATTACK GAIN (byte 84) and RELEASE GAIN (byte 85), 0 .. 200 % each, the CLICK models, the envelopes Q14 (16384 = 1.0, so the gain reaches 2.0) (not yet on an FM-1) |
| 0.9.7 | TUNING and TEMPERAMENT fixed (A 440, 60 Hz gear; their bytes not read); the screens' text larger, a scroll bar, lists without wrap |
| 0.9.8 | HOME larger (drawbars, text), no temperament or hint line; GLOBAL a full screen |
| 0.9.9 | factory: JAZZ B3 first (888 upper, Leslie slow, percussion third), FULL ORGAN second; the reverb's type and size per preset |
| 0.9.10 | power-on: the first factory preset always (the autosave not restored) |
| 0.9.5 | the key click twice setBfree's by default (attack level step 20 = 1.0, release 0.5; the envelopes signed so ADVANCED EDIT reaches 2.0), GLO with an upper (ALL), a lower and a pedal MIDI channel and MIDI CC / MIDI Learn (86 controls, the CCs in `set_bytes` rows 8..10), ADVANCED EDIT SPLIT / SPLIT POINT (patch bytes 82, 83), the crosstalk rows switch CROSSTALK on; the crosstalk engine was checked again (audible at the extremes, tests/organ_adv_test.c section 6) (not yet on an FM-1) |
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
