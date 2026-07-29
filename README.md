# chip-logic-scope — a graphical 8-channel logic analyzer for Wokwi

[![build](https://github.com/giltal/Wokwi-graphical-logic-analyzer/actions/workflows/build.yml/badge.svg)](https://github.com/giltal/Wokwi-graphical-logic-analyzer/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/giltal/Wokwi-graphical-logic-analyzer)](https://github.com/giltal/Wokwi-graphical-logic-analyzer/releases/latest)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A Wokwi **custom chip** that behaves like a benchtop logic analyzer: eight digital inputs, a
480 × 320 screen **on the part itself**, live waveforms, a real trigger, cursors, automatic
measurements and protocol decoders for **UART, I2C and SPI**.

Wokwi's built-in `wokwi-logic-analyzer` only dumps a `.vcd` file when the run ends. This part
draws the signals **while the simulation is running**, inside the diagram, with no external
viewer.

```
+----------------------------------------------------------+
| RUN  100us/div  win 900us  T D7 rise edge 25%  ev 8421 …  |
+------+---------------------------------------------------+
| D0 1 |  ▔▔▔|___|▔▔▔▔▔▔▔|________|▔▔▔▔                     |
| D1 0 |  ...                                               |
| ...  |                                                    |
| D7 1 |                                                    |
+------+---------------------------------------------------+
| UART |  [48 'H'][65 'e'][6C 'l'][6C 'l'][6F 'o']          |
+------+---------------------------------------------------+
| A 25% B 75%  dt 450us  1/dt 2.22kHz   D3 H              |
| D0  f 5.00kHz  T 200us  duty 50%  min 100us  edges 42     |
+----------------------------------------------------------+
```

---

## Features

- **8 digital channels** (D0–D7), transition-based capture with a 32768-event ring buffer
- **Live waveform display** on the part — column-oriented rendering, correct even when
  zoomed far out (a column holding two or more transitions is flagged as a glitch)
- **16-step 1-2-5 timebase**, 100 ns/div … 10 ms/div
- **Trigger**: off (roll), edge, level, auto and single-shot, with an adjustable pre-trigger
- **Two time cursors** with Δt / 1÷Δt readout
- **Automatic measurements**: frequency, period, duty cycle, minimum pulse width, edge count
- **Protocol decoders**: UART, I2C, SPI — drawn as labelled boxes in a decode lane
- **Channel mask** with adaptive lane height, per-channel colors
- **On-screen settings menu**, so the whole feature set is reachable from four sliders
- Pure C11, no allocation after init, ~160 KB of wasm

---

## Quick start (this repository)

The repository is an ESP-IDF project that wires an ESP32-S3 to the chip and generates test
traffic on every channel.

```powershell
# 1. build the chip  (WASI SDK, see "Building the chip")
cd chip
.\build.ps1

# 2. build the demo firmware
cd ..
. "$env:USERPROFILE\esp\v5.4.1\esp-idf\export.ps1"
idf.py build

# 3. run it
#    F1 -> "Wokwi: Start Simulator"
```

---

## Pinout

| Pin | Direction | Purpose |
|---|---|---|
| `GND` | power | ground |
| `VCC` | power | supply (not required for operation) |
| `D0` … `D7` | input | digital channels, sampled on every transition |

In `diagram.json` the part type is **`chip-logic-scope`**.

---

## Controls

Wokwi renders one slider per control and offers no layout, grouping, buttons or mouse input,
so a slider per parameter does not scale. The panel is therefore **four sliders**, two of them
driving an on-screen menu:

| Slider | Range | Purpose |
|---|---|---|
| **Time/div** | 0–15 | timebase index |
| **Run** | 0–1 | 0 = stop (freeze the frame), 1 = run |
| **Setting** | 0–14 | selects a parameter |
| **Value** | 0–255 | edits the selected parameter |

Moving either menu slider pops a **settings overlay** on the screen for a few seconds, listing
every parameter with the selected row highlighted.

`Value` is scaled onto each parameter's own range and uses **catch-up**: after you move
`Setting`, the new slider position is adopted but *not* applied, so picking a parameter never
overwrites it — only actually moving `Value` writes.

### The settings menu

| # | Setting | Values |
|---|---|---|
| 0 | Cursor A | 0–100 % of the window (yellow) |
| 1 | Cursor B | 0–100 % of the window (green) |
| 2 | Trig mode | `off` · `edge` · `level` · `auto` · `single` |
| 3 | Trig ch | D0–D7 |
| 4 | Trig edge | `rise` · `fall` · `both` (in level mode: rise/both = high, fall = low) |
| 5 | Pre-trig | 0–100 %, how much of the window is drawn before the trigger |
| 6 | Chan mask | bitmask, bit *n* shows D*n*; 0 falls back to all eight |
| 7 | Measure | D0–D7, the channel summarised in the bottom row |
| 8 | Decoder | `off` · `UART` · `I2C` · `SPI` |
| 9 | Dec ch A | D0–D7 |
| 10 | Dec ch B | D0–D7 |
| 11 | Dec ch C | D0–D7 or `none` |
| 12 | Baud | `attr` (use the `uartBaud` attribute) or 300 … 230400 |
| 13 | SPI mode | 0–3 |
| 14 | SPI order | `LSB` · `MSB` |

---

## Timebase

`Time/div` indexes a 1-2-5 table. The plot is 9 divisions wide (50 px each), so the visible
window is `9 × time/div`:

| Index | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| **/div** | 100 ns | 200 ns | 500 ns | 1 µs | 2 µs | 5 µs | 10 µs | 20 µs |

| Index | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|
| **/div** | 50 µs | 100 µs | 200 µs | 500 µs | 1 ms | 2 ms | 5 ms | 10 ms |

Index **9** (100 µs/div → 900 µs window) is the default.

---

## Trigger

| Mode | Behaviour |
|---|---|
| `off` | roll mode — the window free-runs with the newest data at the right edge |
| `edge` | wait for the selected edge on the trigger channel, then capture one window |
| `level` | trigger while the channel is at the selected level |
| `auto` | like `edge`, but rolls freely if no trigger arrives within `max(2 × window, 100 ms)` |
| `single` | one shot: capture one window and freeze until the trigger config changes or Run is toggled |

The state machine runs `IDLE → ARMED → FILLING → HOLD`. The displayed window only advances
when a sweep *completes*, so you never see a half-captured frame. The status bar shows
`RUN` / `ROLL` / `ARM` / `HOLD` / `STOP`, and a marker on the plot shows the trigger point.

---

## Measurements

Everything is computed over the **visible window only**:

- **Cursors** — A (yellow) and B (green), with `Δt` and `1/Δt` in the info bar, plus the level
  of the measured channel at each cursor
- **Auto-measure** — for the channel selected by `Measure`: frequency, period (first to last
  rising edge), duty cycle (accumulated high time), minimum pulse width and edge count

---

## Protocol decoders

Select one with the `Decoder` setting. The decoder replays the capture over the visible window
every frame, so there is no separate capture path and nothing to arm — zoom or pan and the
annotations follow. Decoded items appear as labelled boxes in a lane at the bottom of the plot;
a box too narrow for its full label shows the longest prefix that fits (`48 'H'` → `48` → `4`),
collapsing to a coloured tick when it is only a pixel or two wide.

| Decoder | Dec ch A | Dec ch B | Dec ch C | Extra settings |
|---|---|---|---|---|
| **UART** | RX | — | — | `Baud`, `uartBits`, `uartParity`, `uartStop` |
| **I2C** | SDA | SCL | — | none (self-clocked) |
| **SPI** | SCK | MOSI | CS, or `none` | `SPI mode`, `SPI order`, word length via `uartBits` |

- **UART** locks onto every falling edge, samples each bit cell in the middle (LSB first) and
  checks parity and the stop bit. Failures render as `FRM` / `PAR` in red. A frame that would
  run past the right edge of the window is left undecoded until it is fully captured.
- **I2C** marks `S` (START) and `P` (STOP) where SDA moves while SCL is high, samples a bit on
  every SCL rising edge (MSB first) and reads ACK/NAK on the 9th clock. The first byte after a
  (repeated) START is drawn as `68 W` / `68 R`; a NAK turns the box red.
- **SPI** samples MOSI on a rising SCK when `CPOL == CPHA` (modes 0 and 3) and on a falling SCK
  otherwise. With a CS channel it counts clocks only while CS is low and drops a partial word on
  either CS edge, which resynchronises word boundaries automatically; with `Dec ch C = none` it
  decodes continuously.

When a decoder is active the status bar ends with a summary — `UART D7 9600 x6`,
`I2C D5/D6 x12`, `SPI D2/D3/D4 m0 x9` (decoder · channels · baud or SPI mode · annotation
count). It is the quickest way to tell a *misconfigured* decoder from a broken signal:
decoding the wrong channel usually shows up as `FRM` boxes or aliased bytes.

**Limitations:** one decoder at a time, three channels per decoder (so SPI MISO is not
decoded), and at most 192 annotations per frame.

---

## Attributes (`diagram.json`)

Every menu parameter keeps its own attribute, so a diagram can preconfigure the chip. Values
are strings, as always in `diagram.json`.

```jsonc
{
  "type": "chip-logic-scope",
  "id": "scope",
  "top": -20,
  "left": 200,
  "attrs": {
    "refreshHz": "20",
    "timebaseIndex": "9",
    "decoder": "1",
    "decoderCh0": "7",
    "uartBaud": "9600",
    "uartBits": "8"
  }
}
```

| Attribute | Default | Meaning |
|---|---|---|
| `timebaseIndex` | `9` | timebase, 0–15 |
| `running` | `1` | 1 = run, 0 = stop |
| `cursorPos` / `cursorPosB` | `25` / `75` | cursors A / B, 0–100 % |
| `triggerMode` | `0` | 0 off · 1 edge · 2 level · 3 auto · 4 single |
| `triggerChannel` | `0` | 0–7 |
| `triggerEdge` | `0` | 0 rising · 1 falling · 2 both |
| `preTriggerPercent` | `25` | 0–100 |
| `channelMask` | `255` | bit *n* shows D*n* |
| `measureChannel` | `0` | 0–7 |
| `decoder` | `0` | 0 none · 1 UART · 2 I2C · 3 SPI |
| `decoderCh0` / `decoderCh1` / `decoderCh2` | `0` / `1` / `8` | decoder lines A / B / C (`8` = none) |
| `uartBaudIndex` | `0` | 0 = use `uartBaud`, else 1–10 → 300 … 230400 |
| `uartBaud` | `115200` | bit rate used when `uartBaudIndex` is 0 |
| `uartBits` | `8` | UART data bits 5–9; also the SPI word length (4–16) |
| `uartParity` | `0` | 0 none · 1 even · 2 odd |
| `uartStop` | `1` | 1 or 2 |
| `spiMode` | `0` | 0–3 (CPOL = mode >> 1, CPHA = mode & 1) |
| `spiMsbFirst` | `1` | 0 = LSB first, 1 = MSB first |
| `refreshHz` | `20` | redraw rate, in **simulated** time |

`refreshHz` is a simulated-time rate: 20 means 20 frames per simulated second, which may be
many more or far fewer wall-clock frames depending on how fast the simulation runs.

---

## Screen layout (480 × 320)

| Region | Geometry | Contents |
|---|---|---|
| Status bar | `y 0–15` | run state, time/div, window, trigger, event count, buffer fill, decoder |
| Gutter | `x 0–27` | channel name and live level per lane |
| Plot | `x 28–477`, `y 20–291` | 9 divisions × 50 px; lanes share the height |
| Decode lane | bottom 26 px of the plot | annotation boxes (only when a decoder is active) |
| Info bar | `y 295` / `y 305` | cursors, Δt, 1/Δt · auto-measurements |

---

## Building the chip

Requires **WASI SDK** (tested with 33). On Windows it is expected in
`%LOCALAPPDATA%\wasi-sdk`; override with the `WASI_SDK_PATH` environment variable.

```powershell
cd chip
.\build.ps1          # -> chip/dist/logic-scope.chip.wasm + .chip.json + .chip.c
node tools\check-memory.js   # sanity check, see below
```

On Linux/macOS use the equivalent `make` in the same directory.

Three artifacts land in `chip/dist`:

| File | Used by |
|---|---|
| `logic-scope.chip.wasm` | VS Code extension, Wokwi CLI |
| `logic-scope.chip.json` | both — must sit next to the wasm with the same base name |
| `logic-scope.chip.c` | wokwi.com — the amalgamated single-file source |

The simulator imports the chip's linear memory with an **initial size of two pages (128 KB)**,
growable on demand. A module whose *declared* initial memory is larger fails to instantiate, so
all large buffers are `malloc`'d rather than static and the link uses
`-Wl,-z,stack-size=32768`. `tools/check-memory.js` instantiates the wasm against a two-page
memory and reports how much it grows to — run it after any change that adds state.

---

## Using the chip in your own project

Wokwi loads custom chips in two different ways, so there are two ways to install this one.

### On wokwi.com (the web editor)

The web editor compiles custom chips **from source, one C file per chip** — it cannot load a
prebuilt `.wasm`. That is why the build also produces `chip/dist/logic-scope.chip.c`, a single
file containing the whole chip (generated by `tools/amalgamate.js`).

1. Open your project on wokwi.com and click the blue **+** in the diagram editor →
   **Custom Chip**.
2. Name it **`logic-scope`** and pick **C**. Wokwi adds a `chip-logic-scope` part to the
   diagram and creates `logic-scope.chip.c` + `logic-scope.chip.json`.
3. Replace the contents of `logic-scope.chip.json` with
   [`chip/logic-scope.chip.json`](chip/logic-scope.chip.json).
4. Replace the contents of `logic-scope.chip.c` with
   [`chip/dist/logic-scope.chip.c`](chip/dist/logic-scope.chip.c) — that file is committed here
   and attached to every release, so no build is needed.
5. Wire your signals to `D0`–`D7` and press play.

The fastest way to share it with others is to publish a Wokwi project that already contains
the chip: anyone can open it and hit **Save a copy**, or copy the two files into a project of
their own.

### In VS Code or the Wokwi CLI

These load the compiled binary. Copy `logic-scope.chip.wasm` and `logic-scope.chip.json` (from
a release, or from `chip/dist` after a local build) into your project and register the chip in
`wokwi.toml`:

```toml
[[chip]]
name = 'logic-scope'
binary = 'chip/dist/logic-scope.chip.wasm'
```

The `.chip.json` must sit next to the `.wasm` with the same base name. Then add the part to
`diagram.json` as type `chip-logic-scope`.

---

## Demo firmware

`main/signal_gen.c` drives every channel of the demo diagram from an ESP32-S3:

| Channel | GPIO | Signal |
|---|---|---|
| D0, D1 | 4, 5 | binary counter, 100 µs tick (D0 = 5 kHz, D1 = 2.5 kHz) |
| D2 | 6 | SPI SCK, 200 kHz, mode 0 |
| D3 | 7 | SPI MOSI — sends `"SPI"` every 30 ms |
| D4 | 15 | SPI CS, active low |
| D5 | 16 | I2C SDA — 100 kHz WHO_AM_I read of the diagram's MPU6050 every 50 ms |
| D6 | 17 | I2C SCL |
| D7 | 18 | bit-banged UART, `"Hello "` at 9600 8N1 |

Suggested timebases:

| Signal | Time/div |
|---|---|
| UART byte (~1.04 ms) | `1ms` (index 12) |
| I2C transaction (~500 µs) | `100us` (index 9) |
| SPI burst (3 bytes ≈ 120 µs) | `20us`–`50us` (index 7–8) |

---

## Design notes

- **Capture is transition-based, not sampled**: one 16-byte event per change of the 8-bit input
  vector, so an idle bus costs nothing and edge timing is exact to the nanosecond.
- **Rendering is column-oriented**: for each of the 450 columns the capture is summarised into
  "was high / was low / had multiple transitions", so a zoomed-out view aliases visibly instead
  of silently dropping pulses.
- Only dirty row ranges are pushed to the framebuffer, and the whole frame is composed in a
  back buffer first.
- The chip cannot receive mouse or keyboard input — Wokwi delivers neither to custom chips — so
  the screen is output-only and all interaction goes through the four sliders.

---

## Roadmap

- [ ] 1-Wire decoder
- [ ] Auto-baud detection for UART
- [ ] VCD export parity with the built-in analyzer
- [ ] Analog / mixed-signal variant using `pin_adc_read()`
- [ ] Math channels (XOR / AND of two channels), FFT lane

---

## Contributing

The repository is an ESP-IDF project (so the Wokwi VS Code extension finds `wokwi.toml` and
`diagram.json` where it expects them) with the chip itself under `chip/`:

| Path | What it is |
|---|---|
| `chip/logic-scope.chip.json` | pinout, display size and the four sliders — source of truth |
| `chip/src/main.c` | `chip_init`, attributes, the settings menu, status bar, frame loop |
| `chip/src/capture.[ch]` | event ring buffer, trigger state machine, measurements, queries |
| `chip/src/render.[ch]`, `font.h` | framebuffer primitives and the 5×7 font |
| `chip/src/decoders/` | decoder interface, annotation list, UART / I2C / SPI |
| `chip/tools/check-memory.js` | instantiates the wasm against the simulator's 2-page memory |
| `chip/tools/amalgamate.js` | flattens `src/**/*.c` into the single file wokwi.com compiles |
| `main/signal_gen.c` | demo firmware driving all eight channels |
| `CLAUDE.md` | design notes, verified API facts, phase-by-phase history |

Useful things to know before changing the chip:

- **Adding a setting** is one row in `kSettings[]` in `chip/src/main.c` plus bumping the `max`
  of `settingIndex` in `chip/logic-scope.chip.json`. It automatically gets an attribute, a menu
  row and a slider mapping.
- **Adding a decoder** is a new file in `chip/src/decoders/`, a case in `decoder_run()`, an
  entry in `kDecoderNames[]`/`kDecoderLabels[]` and a root in `chip/tools/amalgamate.js`.
- The amalgamation makes every source one translation unit, so two files may not share a
  `static` name. Compile `chip/dist/logic-scope.chip.c` on its own after adding a file.
- Static data plus stack must stay under two 64 KB pages; allocate large buffers with `malloc`
  in `chip_init` and re-run `node chip/tools/check-memory.js`.
- `pin_init`, `attr_init`, `timer_init` and `framebuffer_init` may only be called from
  `chip_init()`, and each pin may have only one `pin_watch`.
- C11, `snake_case` in C, `camelCase` for chip attributes and control ids, and every change
  should leave `diagram.json` runnable.

CI (`.github/workflows/build.yml`) builds the wasm in Wokwi's own toolchain image, regenerates
the amalgamation, runs the memory check on every push, and publishes a release with all three
artifacts when a tag matching `v*` is pushed.

---

## License

MIT — see [LICENSE](LICENSE).
