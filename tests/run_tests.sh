#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Guilherme Ruppert (FMini-1, organvave-1)
# Host tests of the firmware's sources (no hardware). Run from the repo root after ./build.sh:
#   tests/run_tests.sh                      (a failing section prints "SECTION FAILED: <name>")
# Most tests include tests/host.h (the firmware in src/felucca.c's order, with HAL stubs, a RAM NOR, a screen); the
# editor's, MIDI's and the stores' tests tests/editor_host.h on top of it (the SysEx plumbing, the editor, the app).
#
# regression (tests/regress.c; details at its top): golden renders (the 100 factory patches, the voice modes, the FX
#                   sends, the sequencer; one FNV-1a hash each, tests/golden.txt), health (NaN, clipping, DC, a voice
#                   that never ends, a hang), the voice budget, the host CPU per sample against tests/cpu_baseline.txt.
#                   GOLDEN_UPDATE=1 / BUDGET_UPDATE=1 rewrite them: merge only the lines you mean to change.
# target cost (tests/target_budget.py): the render loops in the pi32v2 disassembly against tests/target_budget.txt.
# the organ: tests/organ_test.c (the tone generator against numbers from setBfree's: tools/setbfree_ref/compare.py).
# the instrument: tests/organ_app_test.c (the screens, PRESETS, SAVE, GLO, the keys), fx_test.c (reverbs, delays,
#                   choruses, TUBE, OUTPUT), speaker_test.c (SPEAKER EQ),
#                   audio_test.c (overload protection, fades, DMA diagnostics).
# the stores: storage_test.c (A/B sectors, torn writes), upreset_test.c (user presets, their patches, other shapes),
#                   persistence_test.c (the settings and the autosave, FMini-1's records).
# MIDI and USB: midi_control_test.c (MIDI IN end to end), midi_uart_test.c (the TRS parser), editor_test.c (the editor
#                   protocol, USB and UART framing), uac_test.c (USB audio), usb_desc_test.c (the descriptors), the M-UPGRADE
#                   entry and the update loader (ota_test.c, ldr_test.c: need build/felucca.fwsc from the real SDK).
# the panel and the screen: input_test.c (debounce, the LED scan), theme_test.c, text_ref_test.c, text_spacing_test.py.
# Sanitizers (ASan + UBSan when the compiler has them; SANITIZE=0 skips): the stores, the protocols and a short fuzz of
#                   the editor (tests/fuzz_ed.c; longer: build/host/asan/fuzz_ed 300000 7).
# web: the installer's JS (web/test_web.mjs, web/test_backup.mjs, node), the browser emulator (web/emu: built with
#                   WASI_SDK set, its native twin always; node web/emu/emu_test.mjs). (The organ's patch editor and the site: web/test_editor_organ.mjs, web/test_site.mjs.)
set -e
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
cd "$(dirname "$0")/.."
OUT=build/host
mkdir -p "$OUT"
CC="${CC:-cc} -O1 -Wall -Wno-unused-function"
fail=0
run() { _n=$1; echo "== $_n"; shift; "$@" || { fail=1; echo "SECTION FAILED: $_n"; }; }
HC="$CC -w -Ibuild/gen -Ifirmware/src -Itests"

[ -f build/gen/felucca_tables.h ] || { echo "run ./build.sh first (build/gen)"; exit 1; }

$CC -o "$OUT/storage_test" tests/storage_test.c
run "flash storage (A/B, torn writes)" "$OUT/storage_test"
$CC -o "$OUT/input_test" tests/input_test.c
run "keys and buttons: fast press, long release, bouncy contacts (one note each), glitches, encoders, the LED scan" "$OUT/input_test"
$CC -o "$OUT/midi_uart_test" tests/midi_uart_test.c
run "TRS MIDI parser" "$OUT/midi_uart_test"

HALF=$(sed -n 's/^#define HALF_FRAMES \([0-9]*\).*/\1/p' firmware/src/core.h)
$CC -O2 -DT_CDC=0 -DHALF_FRAMES=$HALF -o "$OUT/uac_test" tests/uac_test.c -lm
run "USB audio input: descriptors, ring and packets, 44.1 -> 48 kHz resampler, 48 kHz stream" "$OUT/uac_test"
# USB descriptor layouts: UAC 48K AS-first (the default 1 1 1); 48K = 0, no UAC and MIDI first: 1.0's bytes where they apply
for v in 1.1.1 1.0.1 0.1.1 1.1.0; do
    IFS=. read -r t_uac t_48 t_asf <<EOF
$v
EOF
    $CC -DT_CDC=0 -DT_UAC="$t_uac" -DT_LAYOUT=0 -DT_ON=1 -DT_48K="$t_48" -DT_ASF="$t_asf" -o "$OUT/usb_desc_test" tests/usb_desc_test.c
    run "USB descriptors: UAC $t_uac (48 kHz $t_48, audio streaming first $t_asf)" "$OUT/usb_desc_test"
done

[ -f build/felucca.fwsc ] || { echo "run ./build.sh first (build/felucca.fwsc)"; exit 1; }
$CC -DOWN_PKG=1 -o "$OUT/ota_test" tests/ota_test.c
run "M-UPGRADE entry (own loader)" "$OUT/ota_test" build/felucca.fwsc
head -c 200000 build/felucca.bin > "$OUT/old_app.bin"
python3 tools/fm1pkg_make.py "$OUT/old_app.bin" build/loader/ota.bin "$OUT/old.fwsc" >/dev/null
$CC -o "$OUT/ldr_test" tests/ldr_test.c
run "update loader: other app -> this build" "$OUT/ldr_test" "$OUT/old.fwsc" build/felucca.fwsc

$HC -o "$OUT/organ_app_test" tests/organ_app_test.c -lm
run "the organ's app: HOME and DRAWBARS (the knobs, SELECT, a knob on HOME), PRESETS over the factory and the user presets, SAVE, GLO, octave, keys and MIDI to the organ" "$OUT/organ_app_test" "$OUT/oa"
$HC -O2 -o "$OUT/organ_test" tests/organ_test.c -lm
run "the organ's tone generator against setBfree: the temperaments' wheels, the footages, the drawbar law, nine cases' levels and attacks (clicks, crosstalk, key compression, swell), bounds, silence" "$OUT/organ_test"
$HC -O2 -o "$OUT/rotary_test" tests/rotary_test.c -lm
run "the rotary CLASSIC against setBfree's: the sidebands, stopped rotors, the speed ramp reaching its targets, bounds at full scale and at the filters' resonances, silence" "$OUT/rotary_test"
$HC -O2 -o "$OUT/drive_test" tests/drive_test.c -lm
run "the drive against setBfree's: the odd harmonics of sines at 110 Hz and 1 kHz, no even ones, block-size independence, bounds at the largest gains, silence, a DC step blocked" "$OUT/drive_test"
$HC -O2 -o "$OUT/rotary_light_test" tests/rotary_light_test.c -lm
run "the rotary LIGHT against CLASSIC: sidebands and levels (nine tones), bounds, silence, the model switch (a 20 ms fade, no jump, each model alone afterwards)" "$OUT/rotary_light_test"
$HC -O2 -o "$OUT/effects_test" tests/effects_test.c -lm
$HC -O1 -o "$OUT/midi_organ_test" tests/midi_organ_test.c -lm
run "the effects section end to end: the patch bytes through mix_block (the rotary: speeds, STOP, LIGHT; the drive; the fades; silence)" "$OUT/effects_test"
$HC -O1 -o "$OUT/eq_test" tests/eq_test.c -lm
run "the equaliser: the codings, flat, LOW / MID / HIGH against their design at the ends, the steps, bounds and the settle" "$OUT/eq_test"
run "the organ over MIDI: the manuals by channel (GLO's MIDI channel), the swell, the drawbar CCs, the rotor speed" "$OUT/midi_organ_test"
$HC -O2 -o "$OUT/fx_test" tests/fx_test.c -lm
run "FX: five stereo reverbs (decay, level, corners, type changes), seven delays, three choruses, TUBE and OUTPUT, bypass at 0" "$OUT/fx_test"
$HC -O2 -o "$OUT/speaker_test" tests/speaker_test.c -lm
run "SPEAKER EQ: FLAT / LOWCUT / BASS+ responses, BASS+ harmonics of the bass, the sub cut, no offset after, headroom on a full-scale kick (#180)" "$OUT/speaker_test"
$HC -o "$OUT/audio_test" tests/audio_test.c -lm
run "audio: overload protection, bounded fades and DMA diagnostics" "$OUT/audio_test"
$HC -o "$OUT/upreset_test" tests/upreset_test.c -lm
run "user presets: banks and organ patches across a power cycle, other shapes empty, mapped by count, a retired engine on the organ, failures" "$OUT/upreset_test"
$HC -o "$OUT/persistence_test" tests/persistence_test.c -lm
run "persistence: the settings (quiet, unchanged, retry), the autosave (idle, gap, clean boot), FMini-1's records" "$OUT/persistence_test"
$HC -o "$OUT/editor_test" tests/editor_test.c -lm
run "editor: the real C protocol (INFO, DUMP, presets, user presets, organ patches, live sync), USB and UART framing and recovery" "$OUT/editor_test"
$HC -o "$OUT/midi_control_test" tests/midi_control_test.c -lm
run "MIDI IN: notes, pedal per channel, bend ranges, CC1 / 120 / 121 / 123, the CC map, Program Change, filters, overflow" "$OUT/midi_control_test"
$HC -o "$OUT/theme_test" tests/theme_test.c -lm
run "themes: contrast, text blending and font metrics" "$OUT/theme_test"
$HC -o "$OUT/text_ref_test" tests/text_ref_test.c -lm
run "text: pens, kerning and pixels equal the reference renderer" "$OUT/text_ref_test"
if python3 -c "import PIL" 2>/dev/null; then
    run "text spacing: glyph gaps against the font's own (16x supersampled), S M <= 0.25 px, L <= 0.5 px" \
        python3 tests/text_spacing_test.py build/gen/ui_fonts.h
fi
$HC -O2 -o "$OUT/regress" tests/regress.c -lm
run "regression: golden renders, health, voices, CPU budget" "$OUT/regress" tests/golden.txt tests/cpu_baseline.txt

# ASan + UBSan: the stored-data and protocol paths, and a short fuzz run (a fixed seed: the same inputs every run)
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize=signed-integer-overflow,shift,bounds,object-size,pointer-overflow"
SAN="$SAN -fno-sanitize-recover=undefined -w"
san_ok() {
    printf 'int main(void){return 0;}\n' > "$OUT/san_probe.c" &&
        ${CC%% *} $SAN -o "$OUT/san_probe" "$OUT/san_probe.c" 2>/dev/null && "$OUT/san_probe"
}
if [ "${SANITIZE:-1}" = 0 ]; then
    echo "== skip the sanitizer runs (SANITIZE=0)"
elif ! san_ok; then
    echo "== skip the sanitizer runs (the compiler has no ASan / UBSan)"
else
    mkdir -p "$OUT/asan"
    A="$OUT/asan"
    SCC="${CC%% *} $SAN -Ibuild/gen -Ifirmware/src -Itests"
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
    $SCC -o "$A/ldr_test" tests/ldr_test.c
    run "ASan/UBSan: update loader (other app -> this build)" "$A/ldr_test" "$OUT/old.fwsc" build/felucca.fwsc
    $SCC -DOWN_PKG=1 -o "$A/ota_test" tests/ota_test.c
    run "ASan/UBSan: M-UPGRADE entry (own loader)" "$A/ota_test" build/felucca.fwsc
    for t in storage_test editor_test upreset_test persistence_test midi_control_test; do
        $SCC -o "$A/$t" tests/$t.c -lm
        run "ASan/UBSan: $t" "$A/$t"
    done
    $SCC -o "$A/fuzz_ed" tests/fuzz_ed.c -lm
    run "ASan/UBSan fuzz: editor SysEx and raw USB-MIDI packets (20000, seed 7)" "$A/fuzz_ed" 20000 7
fi

run "regression: target cost of the render loops (pi32v2 disassembly)" python3 tests/target_budget.py \
    build/felucca.dis tests/target_budget.txt

run "installer CLI (fm1_install.py) against a simulated FM-1" python3 tests/install_test.py

if command -v node >/dev/null 2>&1; then
    run "web: package builder, update protocol" node web/test_web.mjs
    run "web backup: capture, validation before writes, restore order" node web/test_backup.mjs
    $HC -o "$OUT/editor_bridge" tests/editor_bridge.c -lm
    run "organ patch editor against the firmware's editor: the INFO tag, ORG_GET / ORG_PUT, a refusal" node web/test_editor_organ.mjs "$OUT/editor_bridge"
    python3 web/make_site.py build/felucca.fwsc "$OUT/site" "$(sed -n 's/^#define ORGANVAVE_VERSION "v\(.*\)"/\1/p' firmware/src/core.h)" >/dev/null
    run "site: the installer page (markers, scripts, the embedded package)" node web/test_site.mjs "$OUT/site/index.html"
else
    echo "== skip web tests (no node)"
fi

cc -O2 -ffp-contract=off -w -Ibuild/gen -Ifirmware/src -o "$OUT/emu_native" web/emu/native_check.c -lm
run "browser emulator: the heavy song from the native build" "$OUT/emu_native" "$OUT/emu_native.f32"
if [ -n "${WASI_SDK:-}" ] && command -v node >/dev/null 2>&1; then
    run "browser emulator: the firmware to WebAssembly (web/emu/build_wasi.sh -> build/emu)" env OUT=build/emu sh web/emu/build_wasi.sh
    run "browser emulator: boot, keys, MIDI, screen, LEDs, PLAY, a save kept across instances, = native, cost" \
        node web/emu/emu_test.mjs build/emu/felucca.wasm "$OUT/emu_native.f32"
else
    echo "== skip the browser emulator's WebAssembly build (set WASI_SDK; needs node)"
fi

[ $fail -eq 0 ] && echo "ALL HOST TESTS PASSED" || { echo "HOST TESTS FAILED"; exit 1; }
