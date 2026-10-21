# Building OrganVave-1

The build makes three files in `build/`:

| File | What |
| --- | --- |
| `felucca.bin` | the firmware app |
| `loader/ota.bin` | the update loader |
| `felucca.fwsc` | the installable package (app + loader) |

## Get the source

Five engines are in a git submodule (`firmware/engines/`, [Felucca-Engines](https://github.com/hugelton/Felucca-Engines),
MIT), so clone with `--recurse-submodules`:

```
git clone --recurse-submodules https://github.com/hugelton/Felucca.git
```

In a clone made without it, run `git submodule update --init` once. GitHub's "Download ZIP" and the source archives
it adds to a release leave `firmware/engines/` empty; each release also has `felucca-X.Y.Z-source.tar.gz`, the whole
source with the submodule's files in it. The build stops with "firmware/engines is empty" when they are missing.

## Prerequisites (macOS)

- Python 3 with Pillow and fontTools: `pip3 install Pillow fonttools` (the UI font and icons are
  rasterised at build time)
- Docker Desktop. The JieLi toolchain is Linux x86-64 only; the build runs each tool in a
  `linux/amd64` `debian:bookworm-slim` container (Rosetta on Apple silicon). Keep the source
  tree in a folder Docker can share, e.g. under `/Users`.
- The JieLi Linux toolchain (clang 4.0.1 for pi32v2, from JieLi's package server):

  ```
  tools/get_toolchain.sh            # installs to ~/.jieli/toolchain
  ```

- The JieLi AC79 SDK (Apache-2.0). The package uses three of its files
  (`cpu/wl82/tools/uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`); they are not part of this tree.

  ```
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

- Node.js (optional, for the web tests).
- wasi-sdk 25 (optional, `WASI_SDK`: the browser emulator, `web/emu/`).

On Linux x86-64 the toolchain runs natively and Docker is not needed.

## Build

```
./build.sh
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations
(`~/.jieli/toolchain`, `~/fw-AC79_AIoT_SDK`).

`./build.sh --release 0.1` makes a release build: the package identity becomes `FM-1_801`
and the version string (`ORGANVAVE_VERSION`) `v0.1`; the package is `build/organvave-1-0.1.fwsc`, and
`build/release-0.1/` holds what a release ships: the package, the app
(`organvave-1-0.1-app.bin`), `SHA256SUMS`, `LICENSE`, `LICENSING.md` and
`LICENSES/` (the package contains Apache-2.0 SDK files, so the licence texts travel with it).

Build options (environment, `0` or `1`; defaults in `firmware/src/felucca.c` and `firmware/src/usb.c`):

| Flag | Default | |
| --- | --- | --- |
| `FELUCCA_OTA` | 1 | update entry and the editor's SysEx |
| `FELUCCA_UAC` | 1 | USB audio input (the master output, stereo, 44.1 or 48 kHz) |
| `FELUCCA_UAC_48K` | 1 | `0`: the USB audio input at 44.1 kHz only, as up to 1.0.5.2 (`firmware/src/usb.c`) |
| `FELUCCA_UAC_AS_FIRST` | 1 | the audio control header lists the audio streaming interface before the MIDI one, so the USB audio driver of older macOS (up to 15) finds it; `0`: 1.0 to 1.5's order, byte for byte (`firmware/src/usb.c`) |
| `FELUCCA_UART` | 1 | TRS MIDI IN |

The firmware runs on one core and has no USB serial console (Felucca's second core and CDC console, its other
engines and its sample sets are not in this tree).

## Browser emulator

```
./build.sh
WASI_SDK=/path/to/wasi-sdk-25.0 web/emu/build_wasi.sh     # -> docs/emu (OUT=build/emu for a scratch copy)
python3 -m http.server -d docs/emu 8790                   # open http://localhost:8790/
```

`web/emu/felucca_web.c` builds the firmware's sources in `src/felucca.c`'s order against a simulated FM-1, and
wasi-sdk compiles it into an import-free reactor module (`web/emu/wasi_shim/` stubs the few POSIX headers), with
`index.html`, `worklet.js` and the font in `fonts/`. The page keeps the flash's storage sectors (user presets, their
patches, the settings, the autosave) in the browser. Not emulated: USB (audio, the editor, updates) and TRS MIDI.

## Tests

```
tests/run_tests.sh
```

Runs the host tests and, with Node.js, the web page tests (with python playwright, the patch editor in Chromium;
with `WASI_SDK` set, the browser emulator's WebAssembly build). Run it after `./build.sh` (it uses `build/` and needs
`AC79_SDK` set as for the build). The suites cover the instrument (every screen, the factory patches, the filter, the
FX, the speaker EQ, overload protection), flash storage, user presets, the settings and the autosave, MIDI IN (USB,
TRS), the editor protocol, USB audio and descriptors, the update entry and loader, the command-line installer, the
keys and knobs, the text renderer. A failing section prints `SECTION FAILED: <name>`.

The regression suite (`tests/regress.c`) renders the 100 factory patches, the voice modes, the FX sends and the
sequencer and compares a hash of each render with `tests/golden.txt`; it also checks levels, voices and the CPU
cost (`tests/cpu_baseline.txt`, `tests/target_budget.txt`). After an intended change of the sound, run
`build/host/regress` against a scratch copy with `GOLDEN_UPDATE=1` and merge the lines you mean to change;
`BUDGET_UPDATE=1` does the same for the cost files.

## Install

Use the web installer in Chrome or Edge:
<https://hugelton.github.io/Felucca/webapp/installer/>. It installs the released package.

From the command line (needs `pip3 install mido python-rtmidi`):

```
python3 tools/fm1_install.py build/felucca.fwsc
python3 tools/fm1_install.py --info          # identity of the connected FM-1
```

Or, to install your own build from the web installer, make a local copy of the site and open it from `localhost`
(Web MIDI needs a secure context):

```
mkdir -p /tmp/site && python3 web/make_fmug_installer.py build/felucca.fwsc /tmp/site/index.html dev
cd /tmp/site && python3 -m http.server 8000
# open http://localhost:8000/
```

Installing firmware is at your own risk. If an install fails and the FM-1 no longer
starts, recovery needs [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
