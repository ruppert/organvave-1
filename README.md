# OrganVave-1

![OrganVave-1](docs/organvave-splash.png)

A tonewheel-organ firmware for the M-VAVE FM-1, by Guilherme Ruppert: an organ in the style of the classic electromechanical
consoles (nine drawbars, percussion, scanner vibrato / chorus, a valve-style preamp) with a rotary speaker, ported from
[setBfree](https://github.com/pantherb/setBfree) to integer C for the FM-1's chip.

**Status: v0.1.0, the first organ.** The firmware now plays one instrument: the upper manual's nine drawbars on setBfree's
tone generator (the tonewheels, the key click, the crosstalk and the temperaments), with eight factory patches and the
drawbar page. Percussion, vibrato, the rotary, the lower manual and the pedals come in the milestones below. Nothing here
has been tested on a real FM-1 yet. Do not install it unless you know how to recover an FM-1 (see [BUILDING.md](BUILDING.md)).

## Plan

[ORGAN.md](ORGAN.md) is the approved plan: the map from setBfree to the firmware, the measured budget, the two rotary
models (CLASSIC: setBfree's `whirl.c`, reduced; LIGHT: a minimal physical model; chosen per preset), the interface (drawn
after the real console's panel), the presets, the tests and the milestones:

| version | contents |
| --- | --- |
| 0.0.x | the house: identity, names, docs; the slimming |
| 0.1.0 | tone generator, upper manual, click, the drawbar page, 8 presets (now) |
| 0.2.0 | percussion, vibrato / chorus |
| 0.3.0 | rotary (CLASSIC and LIGHT), drive |
| 0.3.1 | REVERB and DELAY pages (the engine's reverbs and delays, on the FX button) |
| 0.4.0 | lower manual and pedals over MIDI (channels 1 / 2 / 3, or GLO's channel N, N + 1, N + 2), the swell (CC 7 / 11), the drawbar CCs 70..78, the rotor speed (CC 1, CC 64), LOWER and PEDAL drawbar pages (EDIT), a bank of 16 factory presets (not yet on an FM-1). |
| 0.5.0 | the equaliser: LOW and HIGH shelves, a MID peak with twelve frequencies, -12 .. +12 dB (EQ page on the FX button, editor controls; not yet on an FM-1) |
| 0.9.0 | beta: overload fallback (when the CPU runs late: "CPU HIGH" on screen, the delay off for 2 s, the oldest held key let go; the rotary and the drive are never changed), all-keys stress tests, user manual (not yet on an FM-1) |
| 0.9.1 | fix: distortion with the rotary on (CLASSIC and LIGHT): the cabinet clipped its input at 1.0; a chord is several times that. Now it takes the mix down by 4 and brings it back after (not yet on an FM-1) |
| 1.0.0 | after it has been tested on the hardware |

## Building and developing

[BUILDING.md](BUILDING.md) sets up the toolchain and builds the package; [DEVELOPING.md](DEVELOPING.md) explains the
architecture, the method and the tests; [CLAUDE.md](CLAUDE.md) is the short list of rules.

## Origin and licence

OrganVave-1 is a fork of FMini-1 (Guilherme Ruppert), itself a fork of [Felucca](https://github.com/hugelton/Felucca) by
Leo Kuroshita (Hügelton Instruments). It is free software under the GNU General Public License version 3 only
([LICENSE](LICENSE)); the parts with other licences are listed in [LICENSING.md](LICENSING.md) and credited in
[CREDITS.md](CREDITS.md).

"M-VAVE" and "FM-1" are trademarks of their owners. OrganVave-1 is independent firmware, not affiliated with or endorsed by
them, and uses no other trademarks in its names.

## User manual (0.9.0)

- **Keys and MIDI**: channel 1 plays the upper manual, 2 the lower, 3 the pedals (with GLO on channel N: N, N + 1, N + 2). CC 7 / 11 is the swell, CC 70..78 the nine drawbars of the channel's manual, CC 1 the rotor speed (0..42 SLOW, 43..85 STOP, 86..127 FAST), CC 64 toggles SLOW / FAST, CC 123 / 120 release the notes.
- **Pages**: EDIT walks the drawbar pages (UPPER, LOWER, PEDAL). The FX button walks PERC & VIB, ROTARY, DRIVE, REVERB, DELAY, EQ and back home.
- **Overload**: if the CPU runs late the footer of every page reads "CPU HIGH", the delay is switched off for 2 s (re-armed while the overload lasts) and the oldest held key is let go, one per request, never the last. The rotary and the drive are not touched.
- Nothing here has been tested on a real FM-1.
