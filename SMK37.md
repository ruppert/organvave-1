# OrganVave-1 on the SMK-37 (beta)

OrganVave-1 also builds for the M-Vave SMK-37, a 37-key controller with the same chip as the FM-1. The FM-1 build is
unchanged (byte for byte); everything for the SMK-37 is behind `BOARD_SMK37`. The port follows
[genkigenki/fm1-to-smk37](https://github.com/genkigenki/fm1-to-smk37) and carries its board layer (the SMK-37 target of
genkigenki/Felucca); the decisions for each control are in [smk37-port.yaml](smk37-port.yaml).

**Not yet tested on a real SMK-37.** The board layer was verified on an SMK-37 Pro by its author; the organ's mapping
on top of it has only been tested on the host (`tests/smk_organ_test.c`).

## Supported models

- **SMK-37 Pro**: the default build (package identity `SMK-37 Pro_800`). Also for the Starrykey 37 Play.
- **SMK-37 Elite**: `MODEL=elite` (identity `SMK-37 Elite_800`).
- **MKE-P37**: `MODEL=mke` (identity `MKE-P37_800`).

All of them are the same hardware. Each build carries its model's name, so the stock updater still takes that unit's
stock package later: install the package for your model. The installers take a package only for exactly the connected
model; the unit's own stock package is the same model, so the way back to stock is always open.

## Controls

The FM-1's manual still applies; the SMK-37's controls map onto the FM-1's like this.

| SMK-37 | OrganVave-1 |
| --- | --- |
| Keys F3..G5 (the middle 27) | the FM-1's 27 keys (DRAWBAR PRESET MODE's keys too) |
| Keys C3..E3 and G#5..C6 (the outer 10) | more notes for the organ, past the 27: with the octave buttons and the manuals' split (silent in DRAWBAR PRESET MODE) |
| Pads 1-8 (top row) | the upper manual's drawbar sets, as a Hammond's preset keys: 800000000, 828000000, 008000000, 800000008, 888000000, 888800000, 888000007, 800008888 (DRAWBAR PRESET MODE's first eight); the set playing is lit bright |
| Pads 9-16 (bottom row) | LESLIE SLOW / FAST, the upper manual's VIB / CHORUS, PERCUSSION, SOFT, FAST, 3RD, OVERDRIVE (TUBE DRIVE at 2.5), REVERB, each lit while it is on |
| Faders 1..4 | drawbars of the manual KNOB BANK picks, in the bank FADER BANK picks: A 16' 5 1/3' 8' 4', B 2 2/3' 2' 1 3/5' 1 1/3', C 1', EXPR, DRIVE GAIN, REVERB SEND (PEDAL: its 16' and 8' on A). A fader takes over only once it moves |
| KNOB BANK | the faders' manual: UPPER, LOWER, PEDAL (lit off UPPER) |
| FADER BANK | the faders' bank: A, B, C (lit off A) |
| < / > | the previous / next preset |
| KNOB BANK + FADER BANK together | the diagnostics page (keys, pads, faders, wheels, pedal, buttons, the ADC, live); again to close |
| Mod wheel | the Leslie: up past 2/3 FAST, down under 1/3 SLOW |
| Pitch wheel | nothing (an organ has no bend) |
| Pedal (a footswitch) | the Leslie SLOW / FAST |
| K1 | MASTER (volume) |
| K2 / K3 / K4 | PRESETS / SELECT / ALGORITHM |
| K5..K8 | KNOB 1..4 |
| FX, SCALE, CHORD, NOTE REPEAT, PARA, GLOBE | FX, SCL, ENV, LFO, EDIT, GLO |
| PATCH / BT | HOME / SAVE |
| ARP / SEQ | ARP (DRAWBAR PRESET MODE) / SEQ |
| SEQ PLAY / SEQ REC | PLAY / REC |
| OCTAVE- / OCTAVE+ | OCT- / OCT+ |
| OCTAVE- + OCTAVE+ held at power-on | HARDWARE CALIBRATION: teach the buttons and knobs again |

The SMK-37's TRS jack is a MIDI output and this build does not use it; there is no TRS MIDI IN. MIDI works over USB.

## Build

See [BUILDING.md](BUILDING.md) for the toolchain.

```sh
BOARD=smk37 ./build.sh                 # build/organvave-1-smk37.fwsc (SMK-37 Pro)
BOARD=smk37 MODEL=elite ./build.sh     # build/organvave-1-smk37-elite.fwsc
BOARD=smk37 MODEL=mke ./build.sh       # build/organvave-1-smk37-mke.fwsc
./build.sh                             # the FM-1 build again before tests/run_tests.sh (the tests use its files)
```

## Install

**First a full flash backup** of the keyboard (the kit's
[02-update-and-recovery.md](https://github.com/genkigenki/fm1-to-smk37/blob/main/docs/02-update-and-recovery.md),
section 3): part of the flash is unique to each unit (its calibration and key) and cannot be downloaded again.

Then connect the keyboard by USB and run, with the package for your model:

```sh
python3 tools/fm1_install.py build/organvave-1-smk37.fwsc
```

`fm1_install.py` needs `mido` and `python-rtmidi` (`pip3 install mido python-rtmidi`). It checks that the package is
for the connected model. (The site's web installer publishes the FM-1 package only.)

## Going back to the stock firmware

Install the manufacturer's firmware package for your model with the manufacturer's updater. It works directly, because
the installed package keeps the keyboard's model name. `tools/fm1_install.py` can also install the stock SMK-37 Pro
V016 package.

## Safety

- Back up first (above), and have the stock package for your model and the manufacturer's updater ready.
- Keep the USB cable connected until the install has finished and the keyboard has restarted.
- The first start clears the stock firmware's settings, and any other firmware's data, in the shared data area (the
  store's owner tag is `SMK37DATAORGVAVE`).
- The firmware never writes the keyboard's calibration, Bluetooth pairing or key areas; the flash map refuses them at
  compile time and again in the flash driver.
- Installing other firmware is at your own risk.
