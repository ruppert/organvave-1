# setBfree reference

OrganVave-1's organ is ported from [setBfree](https://github.com/pantherb/setBfree) (GPL-2.0-or-later), read at commit
2f41983. This directory builds setBfree's own tone generator on the host, as the floating-point reference the firmware
is measured against (ORGAN.md, DEVELOPING.md section 10). Nothing of setBfree is copied here: `build.sh` clones it.

- `sbf_ref.c`: includes setBfree's `src/tonegen.c` and drives it. `sbf_ref dump FILE` writes the data the firmware's
  tables come from; `sbf_ref render OUT.f32 BARS SECS EVENTS` renders keys (environment: `TEMP` 1 gear 60 Hz, 2 gear
  50 Hz, 0 equal; `NOXT` crosstalk off; `SWELL`). `srand(1)` first: the click envelopes are the same every run, and
  every temperament takes the 60 Hz ones (setBfree draws them after the wavetables' dither).
- `stubs.c`: the config hooks the tone generator's init references and the reference does not use.
- `build.sh [DIR]`: clones setBfree at 2f41983 into DIR (default `build/sbf_ref`) and builds `DIR/sbf_ref`.
- `tonegen_dump.txt`: `sbf_ref dump`'s output (glibc's `rand()`), checked in: `tools/gen_organ_tables.py` reads it.
- `compare.py SBF_REF ORGAN_TEST [DIR]`: renders the same cases with setBfree and with the firmware's tone generator
  (`build/host/organ_test render`) and prints the levels and the SNR. The numbers in `tests/organ_test.c` come from it.

Measured (Oct 2026): the levels agree within 0.002 dB, the attack fragments within 0.04 dB, the waveforms at 56 to 68 dB
SNR over the first 2048 samples (then they drift apart slowly in phase: setBfree's looped wavetables against the
firmware's DDS).
