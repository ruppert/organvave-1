# CLAUDE.md

OrganVave-1: a tonewheel-organ firmware for the M-VAVE FM-1 by Guilherme Ruppert, seeded from his FMini-1 (a fork of
[Felucca](https://github.com/hugelton/Felucca), GPL-3.0-only, by Leo Kuroshita). Repository: <https://github.com/ruppert/organvave-1>
(the repository and file names are lower case; the product name is written OrganVave-1). The plan is [ORGAN.md](ORGAN.md): the
organ and the rotary are ported from setBfree; Felucca's `eng_wheel.c` is not used. Until 0.1.0 the firmware still runs
FMini-1's instrument (FMUG) as a scaffold; since 0.1.0 FMUG is gone. Package identity `FM-1_800` (releases `FM-1_8XY`). GitHub
Pages is on, source GitHub Actions: the Installer workflow publishes the site (installer page with the package, patch editor,
emulator, splash) from `main`.

The long guide is [DEVELOPING.md](DEVELOPING.md) (architecture, the filter, presets, editor, tests, release, and how to start a
new firmware from this base). Read it before changing the engine, the UI or the tests. [BUILDING.md](BUILDING.md) is the
toolchain setup.

## Working with the owner

- The owner writes Portuguese (pt-BR): answer in Portuguese. Code, comments, commit messages and documents are English.
- Nothing here has been tested on a real FM-1 unless the owner says so. Say it in the closing message of any firmware change.
- When something ships, remind the owner: the site publishes the firmware package the Installer workflow builds, so a push
  to `main` puts the new package on the site; GitHub Releases are created by the owner by hand (the session's API returns 403).
- Commit trailers, on every commit: the `Co-Authored-By` and `Claude-Session` lines the session's reminder gives.
- organvave-1 has one branch, `main`: `git push origin main`. A stop hook refuses to end a session with unpushed commits.
- Do not change the project when the owner only asks a question.

## Commands (from the repository root)

```
export JIELI_TOOLCHAIN=<dir with pi32v2/bin/clang>   AC79_SDK=<AC79 SDK checkout with cpu/wl82/tools/uboot.boot>
./build.sh                                  # build/felucca.bin, build/loader/ota.bin, build/felucca.fwsc (~30 s)
sh tests/run_tests.sh > /tmp/tests.log 2>&1 # the whole host suite, a few minutes; run it in the background
python3 tools/gen_organ_tables.py           # regenerate firmware/src/eng_organ_tables.h (from tools/setbfree_ref)
python3 tools/gen_organ_presets.py          # regenerate firmware/src/eng_organ_presets.h from tools/organ_bank.py
WASI_SDK=<wasi-sdk-25.0 dir> sh web/emu/build_wasi.sh                    # docs/emu (the browser emulator)
python3 web/make_site.py build/felucca.fwsc /tmp/site X.Y.Z  # the site (the installer page, editor/, emu/ copied by the workflow; only with a real SDK's package)
python3 tools/gen_splash.py                 # firmware/src/organvave_splash.h from docs/organvave-splash.png
```

A single host test is one `cc` line (they are in `tests/run_tests.sh`). The sandbox is ephemeral: the toolchain and the SDK may have to be fetched again (`tools/get_toolchain.sh`;
BUILDING.md has the SDK clone). If a download is refused by the network allowlist, say so and ask for the files.

## Rules that are easy to break

- The firmware is integer-only and one compilation unit (`firmware/src/felucca.c` includes the rest; **the order matters**).
  No floating point, no 64-bit multiplies in a per-sample loop (int64 only per block). Floating point lives in `tools/`.
- `firmware/engines/` is a git submodule (MIT, another repository): never edit it. Value formats the engine headers do not
  have go in `firmware/src/core.h`.
- Engine numbers (`ENGINES[]` in `engines.c`) are stored in projects, presets and the editor protocol: append, never reorder.
- Generated files are checked in and are not edited by hand: `eng_fmug_filter.h`, `eng_fmug_presets.h` (and `build/gen`, which is
  not checked in). Change the generator, rerun it, commit both.
- The factory bank (`tools/fmug_bank.py`) is written in physical units (Hz, ms, cents). A change of a law (the filter's) needs
  a conversion of the bank (`tools/fmug_retune.py` is the pattern) and new levels (`tools/fmug_levels.json`).
- Raw CUTOFF / EMPHASIS bytes changed meaning in 1.7.0: a stored patch, user preset, sound file or library entry from 1.6.x sounds
  different. Any change of a stored value's meaning needs a note in `web/EDITOR_PROTOCOL.md` and, if possible, a migration.
- The version is `ORGANVAVE_VERSION` in `firmware/src/core.h`; the workflow, the emulator and `build.py --release` use it.
  (`FELUCCA_VERSION` in `felucca.c` is Felucca's and stays: the editor INFO.) After a version bump rebuild `docs/emu`.
- The interface is drawn after the real console's panel (owner): drawbars, rocker tabs, the vibrato selector, wood and chrome.
- No trademarks in product, patch or screen names.
- New source files carry `SPDX-License-Identifier: GPL-3.0-only`. Credit outside work in `CREDITS.md`.
- New tests are registered in `tests/run_tests.sh`. `tests/target_budget.txt` (static loop-instruction cost of the render
  functions) and `tests/golden.txt` (render hashes) change only on purpose: `BUDGET_UPDATE=1 python3 tests/target_budget.py
  build/felucca.dis tests/target_budget.txt`; for goldens run `build/host/regress` against a scratch copy with `GOLDEN_UPDATE=1`
  and merge only the lines you mean to change.

## The test suite today (after the Oct 2026 slimming): what fails and is not yours

The tree holds only what the instrument uses (one FMUG part, 16 steps; Felucca's other engines, samples, UI, projects,
second core and serial console are gone; DEVELOPING.md section 1). The suite takes a few minutes. With a real AC79 SDK
everything should pass. With the measuring SDK stand-in (no real `uboot.boot`) four sections fail and are not yours: the
M-UPGRADE entry and the update loader, plain and under ASan (they check the package itself). A failing section prints
`SECTION FAILED: <name>`. A test that prints nothing and fails is a `return 1` with no message: run the binary alone.
`web/test_editor_organ.mjs` and `web/test_site.mjs` need node; the installer's JS and the site are checked by `web/test_web.mjs`. A new failure outside
this list is yours.

## Before saying a change is done

Build, run the tests that touch the change (and the whole suite for anything shared), regenerate the emulator and the installer
if the firmware or the site changed, commit with the trailers, push `main`, and give the owner the `.fwsc`
(`SendUserFile`) with the standing reminders above.
