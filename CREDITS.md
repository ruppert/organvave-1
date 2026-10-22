# Credits

Felucca is Leo Kuroshita's ([@kurogedelic](https://github.com/kurogedelic), [Hügelton Instruments](https://hugelton.com)). It uses and
thanks the work below; each keeps its own licence (see [LICENSE](LICENSE), [LICENSING.md](LICENSING.md) and [LICENSES/](LICENSES/)).
OrganVave-1 (Guilherme Ruppert) is built on FMini-1 and Felucca with Felucca's other engines, its sample sets and its
second core taken out; the credits for those parts stay below, marked "(Felucca: not in this firmware)".

- OrganVave-1's organ and rotary speaker (in development, ORGAN.md) are ported from [setBfree](https://github.com/pantherb/setBfree)
  by Fredrik Kilander, Robin Gareus, Ken Restivo and Will Panther (GPL-2.0-or-later): its tone generator, keys and buses,
  tapering and crosstalk, envelopes and key click, percussion, scanner vibrato / chorus, preamp curve and the rotary speaker
  `whirl.c`, rewritten in integer C with the same algorithms and data; the derived files keep its copyright notices. setBfree's
  rotary cites, and its constants come from: J. Herrera, C. Hanson, J. S. Abel, "Discrete Time Emulation of the Leslie
  Speaker", AES 127th Convention, 2009 (speeds and time constants); J. Smith, S. Serafin, J. Abel, D. Berners, "Doppler
  simulation and the Leslie", DAFx-02, 2002 (the horn's angle-dependent impulse responses, read off a figure by setBfree's
  author)
- The equaliser's filters (0.5.0) use the formulas of Robert Bristow-Johnson's "Cookbook formulae for audio EQ biquad filter
  coefficients" (a published recipe, no code taken), evaluated by `tools/gen_eq_tables.py`
- The splash picture is the owner's

- **[Hügelton Instruments](https://hugelton.com)** (Leo Kuroshita, [@kurogedelic](https://github.com/kurogedelic)):
  Felucca itself; (not in this firmware: the PHASE engine's waveforms (a C port of the oscillator of
  [CrispyZebra](https://github.com/hugelton/CrispyZebra), GPL-3.0); the DRUM voices and kits; the Hügelton Sample
  Pack, the drum samples, GPL-3.0-only); the [Fukiai](https://github.com/hugelton/Fukiai) icon
  font ([MIT](LICENSES/MIT-Fukiai.txt)); the shared DSP helpers of the ANALOG, LOFI, TRIO, WHEEL and NOISE engines
  ([Felucca-Engines](https://github.com/hugelton/Felucca-Engines), [MIT](LICENSES/MIT-Felucca-Engines.txt))
- Fonts: [Inter Tight](https://github.com/rsms/inter-tight) by The Inter Project Authors, [SIL OFL 1.1](LICENSES/OFL-InterTight.txt);
  the browser emulator's labels: [DotGothic16](https://github.com/fontworks-fonts/DotGothic16) by The DotGothic16 Project Authors, [SIL OFL 1.1](LICENSES/OFL-DotGothic16.txt)
- (Felucca: not in this firmware) Samples: [Versilian Studios](https://versilian-studios.com/) [VSCO-2 Community Edition](https://github.com/sgossner/VSCO-2-CE) and [VCSL](https://github.com/sgossner/VCSL), CC0 1.0: the SAMPLE sets, also SLICE's PIANO and PIANO HD
- (Felucca: not in this firmware) VOICE engine: after [klattsch](https://github.com/tgies/klattsch) by Tony Gies (MIT); formant data from Klatt (1980) and Hillenbrand et al. (1995)
- (Felucca: not in this firmware) PHYS engine: models ported from [DaisySP](https://github.com/electro-smith/DaisySP) by Electrosmith and Emilie Gillet (MIT) and from Emilie Gillet's [eurorack](https://github.com/pichenettes/eurorack) code (MIT)
- (Felucca: not in this firmware) FM6 engine: msfa from [Dexed](https://github.com/asb2m10/dexed) by Google Inc. and Pascal Gauthier (Apache-2.0)
- Guilherme Ruppert: FMini-1 (the FMUG engine, its app and editor: not in this firmware since 0.1.0) and organvave-1. FMUG filter (FMini-1 1.7.0): the Minimoog ladder's calibration (the singing pitch against the corner, the feedback lag pole, the critical loop gain,
  the cutoff and emphasis dials) read from the circuit notes of [CA-72](https://github.com/idlefoundry/ca-72) by idlefoundry (GPL-3.0-or-later); the
  integer implementation (tools/fmug_filter.py, firmware/src/eng_fmug.c) is written for this firmware
- (Felucca: not in this firmware) Second core: its start, mailbox and guards from [X0X](https://github.com/charlesvestal/fm1-x0x) by [charlesvestal](https://github.com/charlesvestal)
  with the dual-core findings of [Melodee](https://github.com/keremimo/melodee) by [keremimo](https://github.com/keremimo); its audio split,
  measurements and hardening after [Jangada](https://github.com/zednaked/jangada) by [zednaked](https://github.com/zednaked) (all GPL-3.0)
- Browser emulator: after [X0X](https://github.com/charlesvestal/fm1-x0x) by [charlesvestal](https://github.com/charlesvestal) (GPL-3.0), a Felucca fork whose browser build showed the way
- Package format and boot files: [JieLi AC79 SDK](https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK) ([Apache-2.0](LICENSES/Apache-2.0.txt); three of its files are in every package, none in this tree)
- Contributions: [keremimo](https://github.com/keremimo) (white-key scales, #2), [ChanceTheMaker](https://github.com/ChanceTheMaker)
  (TRS MIDI, bend, sustain and clock, palettes, favourites, editor display settings: #8, #10, #11, #12),
  [andreahaku](https://github.com/andreahaku) (sample recording and trim, #29; SLICE manual slices and tests, #27, #22),
  [spinkham](https://github.com/spinkham) (the boot fix for 0.9 projects, #111; the emulator's MIDI knobs, #182),
  [ugai](https://github.com/ugai) (the Try page's horizontal knob drag, #240),
  [zednaked](https://github.com/zednaked) (ratchets, #100),
  [jasonpersinger](https://github.com/jasonpersinger) (the ROOM reverb click fix, #121),
  [renebohne](https://github.com/renebohne) (MIDI Program Change, #179),
  [gospodindilly-ops](https://github.com/gospodindilly-ops) (the second core, #247)
