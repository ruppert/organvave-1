# Developing OrganVave-1

OrganVave-1 (repository `organvave-1`) is a tonewheel-organ firmware for the M-VAVE FM-1. [ORGAN.md](ORGAN.md) is its plan
(the map from setBfree, the budget, the milestones); this guide is how the code is laid out and how to change it. It was
seeded from FMini-1 (Guilherme Ruppert), itself a fork of Felucca (Leo Kuroshita); what is left of those is named in
[CREDITS.md](CREDITS.md). Since 0.1.0 the instrument is the organ alone: FMini-1's synthesizer engine, its UI and its
editor are gone, and so are the sequencer, the arpeggiator and Felucca's other engines.

## 1. What runs on the chip

The FM-1 has a JieLi AC79 (pi32v2 core, clang 4.0.1), an I2S codec, a 240 x 240 RGB565 LCD, a key matrix with 14 labelled
buttons (`panel.c`), seven encoders (SELECT, ALGO, PRESET, K1..K4), 27 keys, a MASTER pot, USB (MIDI, audio) and TRS MIDI IN.
The sample rate is 44.1 kHz. The audio interrupt renders one I2S half (128 frames) in blocks of `CTL` = 32 samples.

Budget (`./build.sh` prints these and fails when they do not fit): image 277,640 B (limit 581,564); RAM `.data` + `.bss`
24,744 B (limit 73,728); the `.pool` section 249,764 B (limit 368,640).

## 2. Repository map

| Path | What |
| --- | --- |
| `firmware/src/` | the firmware, one compilation unit: `felucca.c` includes everything, in an order that matters |
| `firmware/src/eng_organ.c` | the organ: the tonewheel tone generator (keys, buses, contributions, click, swell, compression), the patch, `org_track_render` |
| `firmware/src/eng_organ_tables.h` | **generated** from setBfree's own numbers (`tools/gen_organ_tables.py`); do not edit |
| `firmware/src/eng_organ_presets.h` | **generated** from `tools/organ_bank.py` (`tools/gen_organ_presets.py`); do not edit |
| `firmware/src/organ_app.c` | the screens (HOME, DRAWBARS), the presets, SAVE, GLO, the LEDs, the splash |
| `firmware/src/editor_organ.c` | the editor's organ patch commands (85, 86) |
| `firmware/src/engines.c` | `ENGINES[]`: 0..14 retired (`"-"`), 15 the organ |
| `firmware/src/voice.c`, `fx.c`, `audio.c`, `seq.c` | the track render (the organ is handed over in `track_render`), the FX buses and the master, the audio ISR, the note input |
| `firmware/src/editor.c`, `upreset.c`, `settings.c`, `storage.c` | the editor protocol; user presets; the settings and the autosave; flash |
| `firmware/src/organvave_splash.h` | **generated** from `docs/organvave-splash.png` (`tools/gen_splash.py`); do not edit |
| `firmware/engines/` | **git submodule** (MIT, Felucca-Engines). Only its shared `dsp.c` is built (`sine_i`). Never edited here |
| `firmware/hal/`, `firmware/loader/` | register access (only here); the update loader |
| `tools/` | Python generators and the measuring tools (floating point lives here); `build.py` is what `build.sh` runs |
| `tools/setbfree_ref/` | setBfree's tone generator as the reference: `sbf_ref.c`, the dump the tables come from, `compare.py` |
| `tests/` | host tests (`run_tests.sh`), the harnesses `host.h` and `editor_host.h`, goldens, budgets |
| `web/` | the site (`site_organ.html`, `make_site.py`), the installer's JavaScript, the patch editor (`editor_organ.html`), `EDITOR_PROTOCOL.md`, `emu/` (the browser emulator) |
| `docs/` | what the site serves that is checked in: `index.html` (generated), `emu/` (generated wasm), the splash |
| `.github/workflows/installer.yml` | builds the firmware and publishes the site to GitHub Pages on a push to `main` |

## 3. How the firmware is put together

**One translation unit.** `felucca.c` includes the HAL (header-only), then the sources in dependency order; `engines.c`
includes `eng_organ.c` and defines `ENGINES[]`; `organ_app.c` is included near the end, before `main.c`. The host tests and the
browser build include the same files (`tests/host.h`, `tests/editor_host.h`, `web/emu/felucca_web.c`), so what runs on the host
is what runs on the chip, compiled by a different compiler. `./build.sh` generates `build/gen/*.h` (sine, envelopes, fonts,
icons): host tests need them, so run `./build.sh` first.

**Integer only.** No floating point and no 64-bit multiply in a per-sample loop (int64 only per block). The organ's tables carry
the setBfree numbers already converted; `tools/` does the floating point.

**Audio.** `audio.c` (the I2S interrupt) calls `mix_block` (`fx.c`) per block: events, then each track (`mix_part`, which calls
`track_render`), the sends into the buses, the master (a peak limiter at `LIM_T` 18000, then the soft clip), and the USB audio tap.
`track_render` hands the organ to `org_track_render` (its keys are `org_key`, from `org_note`): the organ renders its whole part
into the buffer and the track's level, pan and sends apply after it, as for any part. The main loop runs the UI and the MIDI; the
interrupt never waits for it. Anything the main loop changes that the interrupt reads goes through a publish step:
`org_set_patch` copies the patch, then bumps `org_pgen`; the interrupt takes it at the start of the next block (`org_block`).

**The organ's tone generator** (`eng_organ.c`, the setBfree port). Each of the 91 wheels is a 32-bit phase accumulator on one
shared sine (`sine_i`), with the three tunings of setBfree as increments (`ORG_WINC`: gear 60 Hz, gear 50 Hz, equal). A key
is a list of (wheel, bus, level) contributions (`ORG_KC`, with crosstalk; `ORG_KCX`, without); the key's messages (`org_key`) go into a
queue that `org_fragment` applies at each 128-frame fragment, as setBfree's `oscGenerateFragment` does: the attack's click
(`ORG_ATK`), the release (`ORG_REL`), the key compression (`ORG_KEYCOMP`), the swell (`ORG_SWELL_TC`). The drawbars enter as
Q12 bus levels (`/8` of the drawbar's 0..8). `org_render` sums the wheels per bus and mixes the result into the part.

**Numbers.** Q15 signals; the wheel levels Q18 packed with the wheel and bus into one word (`wheel << 25 | bus << 20 | level`).
`tools/setbfree_ref/compare.py` shows the port against the reference: levels within 0.002 dB, the attack within 0.04 dB,
waveforms with 43 dB SNR over 0.25 s (the wheels are pure sines here; setBfree's wavetables carry a dither).

## 4. Changing the organ

- **The tables** come from setBfree. To refresh them, build the reference (`sh tools/setbfree_ref/build.sh DIR`, which clones
  setBfree at 2f41983 if needed), dump with `DIR/sbf_ref dump tools/setbfree_ref/tonegen_dump.txt`, then
  `python3 tools/gen_organ_tables.py`. The dump is checked in; glibc's `rand()` decides the click envelopes, so the dump is made
  with the same libc.
- **The factory patches** are in `tools/organ_bank.py` (drawbars as `upper "88 8000 000"` strings, the tuning, the crosstalk,
  the reverb send). `python3 tools/gen_organ_presets.py` writes `eng_organ_presets.h`, the LEVEL of each preset (about the same
  loudness as the JAZZ patch) included. Change the bank, rerun, commit both.
- **A stored value's meaning** never changes silently. The patch bytes, the autosave and the user presets are stored: a new
  meaning needs a new version (`AS_MAGIC`, `UP_VER`, the patch's `ORG_PSIZE`), a note in `web/EDITOR_PROTOCOL.md`, and a rule for
  older records. `ENGINES[]` entries are appended, never reordered.
- **The rotary, the vibrato, the percussion, the lower manual and the pedals** are the next milestones in ORGAN.md. They are
  ports of setBfree's `whirl.c`, `vibrato.c`, `percussion` and the lower and pedal keys; the reduced rotary (1024 angles,
  interpolated coefficients) is the plan of the owner's approved revision.

## 5. Tests

`sh tests/run_tests.sh > /tmp/tests.log 2>&1` after `./build.sh` (the package sections need `AC79_SDK`; the whole suite takes
several minutes: run it in the background and read the log). A section that fails prints `SECTION FAILED: <name>`. A test
that prints nothing and fails is a `return 1` with no message: run the binary alone.

| Test | What it guards |
| --- | --- |
| `organ_adv_test.c` | ADVANCED EDIT: the builders (`org_env_build`, `org_xt_build`, `org_tuning_build`) against setBfree's `tonegen.c` (`tests/organ_adv_ref.txt`, made by `tools/gen_organ_adv_ref.py` from `sbf_ref adv`): the zero patch equals the flash tables, the crosstalk settings' per-key sums, the click envelopes, the RAM table's key accounting, the tuning, the percussion and the scanner bytes |
| `organ_test.c` | the organ against setBfree: the temperaments' wheels, the footages, the drawbar law, nine cases of levels and attacks (clicks, crosstalk, key compression, swell), bounds, silence. `render OUT.f32` writes the port's output for `tools/setbfree_ref/compare.py` |
| `organ_app_test.c` | the screens and the controls on the host: HOME and DRAWBARS drawn and told apart, the drawbar knobs and the SELECT window, PRESET over the factory and user presets (wrapping), SAVE, GLO (MIDI IN, channel, ABOUT), the octave keys |
| `editor_test.c` | the editor's handler: INFO's tags, NAMES, PRESET, DUMP, user presets, organ patches (ORG_GET / ORG_PUT), live sync, USB and UART framing |
| `upreset_test.c`, `persistence_test.c` | user presets and the organ patch across a power cycle; the settings and the autosave; FMini-1's records read as they are |
| `midi_control_test.c`, `midi_uart_test.c`, `uac_test.c`, `usb_desc_test.c` | MIDI IN (notes to the organ's keys, the pedal, the channels), TRS MIDI, USB audio and descriptors |
| `audio_test.c` | the real audio interrupt with a simulated DMA: overload, fades, the USB level, the keys under the splash |
| `fx_test.c`, `speaker_test.c` | the FX buses (reverbs, delays, choruses, TUBE, bypass) and the SPEAKER EQ |
| `regress.c` + `golden.txt`, `cpu_baseline.txt`, `target_budget.*` | render hashes of the 8 organ presets, the FX sends and the sequencer's pattern; the key budget, a held key under pressure, no hanging key after the note-offs; CPU; the static instruction cost of the render loops |
| `ota_test.c`, `ldr_test.c`, `install_test.py`, `storage_test.c` | the update entry and loader, the installer CLI, flash |
| `web/test_web.mjs`, `web/test_backup.mjs`, `web/emu/emu_test.mjs` | the installer's JavaScript and the browser emulator against its native twin (the last two need node; the emulator needs `WASI_SDK`) |

Regenerating the goldens and the budgets is on purpose only: `GOLDEN_UPDATE=1` for `golden.txt` (review the diff; it rewrites the
whole file from the current renders), `BUDGET_UPDATE=1 python3 tests/target_budget.py build/felucca.dis tests/target_budget.txt`
for the loops. `cpu_baseline.txt` is measured on macOS, where the kernel counts instructions: `BUDGET_UPDATE=1` there.

**Host compiler.** `tests/host.h` includes `usb.c`, whose registers are reached through `hal/fm1_usb.h`, and that header carries
the pi32 `csync` instruction as inline asm. A host `cc` (GNU as) rejects it. On the owner's toolchain that is the case to
check first; in the Linux sandbox the suite ran through an assembler wrapper that drops `csync` / `ssync` lines, outside the
repository.

## 6. Site, installer, emulator, release

- The site is `web/make_site.py build/felucca.fwsc OUTDIR VERSION`: the installer page (`web/site_organ.html`, with the package
  embedded and the installer's JavaScript from `fm1pkg.js` and `fm1ota.js`), the package beside it, and the patch editor
  (`web/editor_organ.html`, copied to `editor/`), the emulator (`docs/emu/`, copied to `emu/`) and the splash. The Installer
  workflow builds it on every push that touches what the site is made of and publishes it (Settings > Pages: Source = GitHub
  Actions). Its tests: `web/test_site.mjs` and `web/test_editor_organ.mjs` (the editor against the firmware, through
  `tests/editor_bridge.c`).
- The browser emulator: `web/emu/felucca_web.c` compiles the firmware's sources against a simulated FM-1;
  `web/emu/build_wasi.sh` (wasi-sdk 25, no Emscripten) writes `docs/emu/`. Rebuild it after every version bump.
- **Release checklist:** bump `ORGANVAVE_VERSION` (`core.h`); build; run the tests; regenerate `docs/emu` (and `docs/index.html`
  when the installer exists); update the protocol and this guide; commit; push `main`; give the owner the `.fwsc` from a build
  with the real AC79 SDK. The owner creates the GitHub Release by hand (the session's token cannot). Nothing here has been tested on
  a real FM-1 unless the owner says so.
- Installing is at the owner's risk; recovery of a dead FM-1 is in BUILDING.md.

## 7. Starting another firmware from this base

1. **A new repository seeded from this one** (keep the history and `LICENSE`: the code is GPL-3.0-only and the authorship in the
   headers stays; add new work to `CREDITS.md`). Use a product name without a trademark.
2. **Identity**: `FELUCCA_ID` in `firmware/src/felucca.c` is the package identity (`FM-1_800` here, release builds `FM-1_8XY`).
   The update loader answers `ota-FM-1_900` and the installers compare only the model (the text before `_`), so a package installs
   over any other FM-1 package by design. Then the product name, `ORGANVAVE_VERSION`, the splash (`tools/gen_splash.py`), the
   loader's USB name (`usb.c`) and the port filters (`web/fm1ota.js`, `tools/fm1_install.py`), the site, the README and the workflow.
3. **The engine**: a new `eng_<name>.c` included by `engines.c`, a new entry appended to `ENGINES[]` with a new `ENGI_` number, and
   the hooks `track_render` and `trk_note_on` route to it (as the organ does). `eng_organ.c` shows the patch bytes, the publish step
   and the generated header.
4. **The UI**: the drawing is in `organ_app.c`; the layout follows the real console's panel (owner's rule: the screens are drawn
   after the Hammond panel: drawbars, rocker tabs, the vibrato selector, wood and chrome).
5. **The bank**: physical units in a Python bank, a generator that checks its own output, the generated header checked in.
6. **Editor and protocol**: new commands take the next free numbers in `EDITOR_PROTOCOL.md`, with an INFO tag so the editor can
   tell old firmware from new.
7. **Tests with the code**, and the static budget for the new render functions (`tests/target_budget.py` FUNCS).
8. **Site, installer, emulator** last: they are generated from the same sources.

**The organ's source.** The organ and the rotary are ported from setBfree (GPL-2.0-or-later, compatible with this GPL-3.0-only tree;
its notices stay in the derived files). Felucca's organ engine (`firmware/engines/src/eng_wheel.c`) is not used, not even as a
starting point (the owner's decision). Files without a licence header in setBfree are not copied. Method: a float reference
from setBfree, the fixed-point port, host tests against the reference, then the content, then the UI.

## 8. Gotchas collected

- `build/` is not checked in; `./build.sh` makes `build/gen`. A host test fails to compile without it.
- `tests/run_tests.sh` compiles with `-w`: warnings are hidden. `./build.sh` uses the chip's clang and shows the warnings that matter.
- The render loops are measured, not guessed: `org_render` cost 410 in its 4-way unrolled form and 235 as the plain loop, so the
  plain loop stayed (`tests/target_budget.txt`).
- The sandbox for Claude sessions is reclaimed after a while: files in `/tmp` and the toolchain directories disappear. Keep
  scratch work in the session's scratchpad directory and commit what matters.
- The GitHub token of a session cannot create Releases (403). The owner does it.

## 9. Overload fallback (0.9.0)

The organ has no voices to shed. `shed_voice()` (audio.c) therefore calls `fx_cpu_high()` (fx.c): it arms `cpu_high` (2756 blocks, 2 s; `mix_block` counts it down) and `org_shed_req`. While `cpu_high` is set the delay bus does not run (`dly_act` is false; the buffer reads as silence when it comes back) and the footer of every organ page reads "CPU HIGH". At its next fragment `org_fragment` (eng_organ.c) releases the oldest held key (`org.kage`), never the last one. The rotary and the drive are never changed. `tests/effects_test.c` sections 8 and 9 cover this and an all-keys stress run; `tests/audio_test.c` checks the hook. Modules added since 0.3: eng_eq.c (noinline `eq_block`), the REVERB / DELAY / EQ pages in organ_app.c, the organ MIDI in midi_control.c.

## 10. Drive, rotary and page model (0.9.2)

- **Drive**: GAIN, TYPE, TONE, OUTPUT (patch bytes 41, 48, 49, 50). TYPE 0 is setBfree's preamp (DISTORTION, `dr_run`, untouched); TYPE 1 is TUBE (`dr_tube` in eng_drive.c: an asymmetric soft cubic with a DC tracker, as loud as DISTORTION at the default gain). `dr_run_ex` runs either curve and then `dr_post` (a tilt around 1 kHz and an output gain, 2 dB a step). The old CHARACTER byte (42) is ignored.
- **Rotary**: bytes 51..56 scale the horn and drum speeds (1.12^n a step) and the ramp times (1.4^n a step). The coding is "zero is the default": `org_c9` / `org_b9` (eng_organ.c). The ramp steps are the table `ROT_KT` (fx.c), because `rot_k` divides in 64 bits and the interrupt cannot link `__udivdi3`; effects_test.c checks every entry against `rot_k`.
- **Rotary input**: the reverb's return holds a faint constant, which the turning rotors would make a tone: `mix_block` runs a 7 Hz high-pass (`rot_dc`) in front of the cabinet and feeds zeros once the input has been quiet for 1.1 s. The host tests (regress.c, audio_test.c) now initialise the effects like main.c's `felucca_init`; before, the rotors never turned in them.
- **Pages**: pages 2..7 are row lists (`oa_val`, `oa_adj`, `oa_rows` in organ_app.c): SELECT picks a row (highlighted), ALGORITHM changes it, K1..K4 act on rows 1..4. The drawbar page is UPPER or LOWER+PEDAL (EDIT toggles): the Hammond has 2 pedal drawbars (16' and 8'), shown at the left of the 9 lower ones.
- **Editor**: `ORG_PUT` carries at most 48 bytes, so the editor sends the 57 patch bytes in two frames.

## 11. ADVANCED EDIT (0.9.4)

- **The page**: EDIT walks UPPER, LOWER + PEDAL, ADVANCED (page 8 of organ_app.c: 27 rows, `OA_AOP` maps a row to its patch byte, `oa_val_adv` / `oa_adj_adv`). FX never visits it.
- **The bytes**: `ORG_PSIZE` 96; bytes 57..81 are `OP_TUNING` .. `OP_SC_V3`, table `ORG_ADV[]` = {op, N, D}: a byte b is the step (b + D) mod N (`org_ci`, `org_bi`; conditional subtractions, no division), so zero is setBfree's default and an older patch sounds as before. The meaning of each is in `web/EDITOR_PROTOCOL.md`. The autosave record keeps its old 64-byte patch and appends `patch_ext`; the shorter 0.9.3 record is still accepted (`AS_OLD_SIZE`).
- **Tables built in the main loop** (`org_adv_poll`, called from main.c): the click envelopes (`org_env_build`, setBfree's `initEnvelopes` with glibc's `rand()` stream reproduced from the same position, so the defaults equal `ORG_ATK` / `ORG_REL` within 2 LSB), the scaled wheel increments (`org_tuning_build`) and the crosstalk table (`org_xt_build`: setBfree's `applyDefaultConfiguration` + `compilePlayMatrix` on `ORG_KCX` with the neighbour tables `ORG_XT_PAIR/TRE/TRW/TSE/TSW`). A zero patch keeps pointing at the flash tables; the RAM ones are used only for a setting that leaves the defaults. Pool: about 50 KB of RAM (`ORG_XT_CAP` = 10752 entries x 4 B); when a setting asks for more entries the floor is raised until it fits (the faintest entries go first).
- **Key accounting**: a key remembers the crosstalk table it went down with (`org.keys[k]` 1 flash, 3 RAM; `q[]` bits 1..2). The RAM table is rebuilt only while `org.nram` is 0 (keys down on it, plus releases not yet applied by the interrupt, which decrements in `org_fragment`); until then new keys keep the table that is there. Nothing in the interrupt allocates, divides in 64 bits or multiplies in 64 bits per sample.
- **Reference**: `sh tools/setbfree_ref/build.sh` then `python3 tools/gen_organ_adv_ref.py` rewrites `tests/organ_adv_ref.txt`; `tools/gen_organ_tables.py` writes the tables the builders use (`ORG_RAND_RING`, the neighbour tables, `ORG_PERC_DEC`, `ORG_XT_G`, `ORG_PG`, `ORG_SCAN_INC`) from `tools/setbfree_ref/tonegen_dump.txt`.
- **0.9.5**: the page has 29 rows (SPLIT and SPLIT POINT after TEMPERAMENT; the crosstalk rows are dimmed with CROSSTALK off and switch it on when changed). The click envelopes are `int16_t` (`ORG_ATK[8][128]`, signed so a click above 1.0 fits): the attack level is i / 20 (default step 20: 1.0, top 2.0), the release i / 40 (default step 20: 0.5). `tools/setbfree_ref/sbf_ref.c` builds its reference with the same 1.0 / 0.5.
- **0.9.6**: the page has 31 rows (ATTACK GAIN, RELEASE GAIN last; bytes 84 and 85). The envelopes are Q14 (`ORG_ATK` / `ORG_REL` and RAM, 16384 = 1.0; the render multiplies with `>> 14`; the generator's `q14s`). The release model defaults to CLICK (`OP_REL_MODEL` step 0; the reference and the rand() check in `gen_organ_tables.py` consume the release's draws). The attack and release gains (bytes 84 `OP_CLICK_GAIN` and 85 `OP_REL_GAIN`, i / 20 each) act in `org_env_burst` on their own burst: above 100 % a bump of 4 e (1 - e) (the attack up, the release's gate deeper so it is louder), below 100 % a scale with an 8-sample fade. tests/organ_adv_test.c section 7 checks it. The split is in `midi_control.c` `org_midi_note` (a bit mask `org_split_held` remembers the notes that went to the lower manual and the pedals so their note-off finds them); the onboard keyboard does not follow it. GLO (organ_app.c) holds the three channels `fm_ch_up/lo/pd` (seq.c `org_manual_of`; `fm_midi_base` reads the older one-channel setting) and the MIDI CC list: `oa_ccmap[patch byte]` = the CC, learned from the next free CC that arrives while the list is open (`ml_free_cc` excludes the ones with a meaning of their own), applied by `oa_cc_poll`/`oa_ctl_cc`. Stored in `set_bytes` row 7 (marker 0xC5 in byte 4, lower and pedal channels in 5 and 6) and rows 8..10.
- **0.9.7**: no TUNING and no TEMPERAMENT rows: the tuning is fixed (A 440, the 60 Hz gear). The firmware no longer reads OP_TEMP (27) or OP_TUNING (57): the bytes stay in the patch, storage and the editor unchanged, and the factory CHURCH and STRINGS are the 60 Hz gear. The ADVANCED page has 29 rows (SPLIT first; knobs 1-2 go to SPLIT and SPLIT POINT). The screens' rows are AF_M text with a level bar under the label, a scroll bar on the right, and the lists stop at their ends (no wrap), GLOBAL and MIDI CC too.
- **0.9.8**: HOME has the nine drawbars larger (22 px wide, 8 px a step), the lamps in AF_M, no temperament lamp and no hint line (the CPU HIGH warning stays). GLOBAL is a full screen like the FX pages (`scr_global`, not a pop-up); the MIDI CC list stays a pop-up over it.
- **Editor**: `web/editor_organ.html` builds its Advanced sliders from the table `ADV_GROUPS` (the same N and D); it also works with a 0.9.3 device (INFO tag 64: the section is hidden and 57 bytes are sent).
