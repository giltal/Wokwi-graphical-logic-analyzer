# CLAUDE.md — Wokwi Graphical Logic Analyzer / Scope Chip

Working notes + plan for building **`chip-logic-scope`**: a custom Wokwi part with 8 digital
inputs and an on-part graphical screen showing live waveforms, later extended with protocol
decoders.

---

## 1. Goal

Build a reusable Wokwi Custom Chip that behaves like a benchtop logic analyzer:

- 8 digital input channels (`D0`–`D7`)
- A live waveform display rendered **on the part itself** (not an external viewer)
- Configurable timebase, trigger, and run/stop behaviour
- Later: protocol decoders (UART, I2C, SPI, 1-Wire) drawn as annotation lanes under the waveforms
- Distributable **two ways**: a prebuilt `.wasm` for the VS Code extension / wokwi-cli, and an
  amalgamated single `.c` for wokwi.com (see §11)

This complements the built-in `wokwi-logic-analyzer`, which only dumps a `.vcd` file at the end
of the run — it has **no live visualization**. That gap is exactly what we're filling.

---

## 2. Feasibility — verified API facts

All confirmed against docs.wokwi.com (2026-07) and `wokwi/inverter-chip`'s `src/wokwi-api.h`.

| Capability | API | Notes |
|---|---|---|
| Custom part with named pins | `<name>.chip.json` → `pins: []` | `""` = skipped pin slot |
| **On-part screen** | `.chip.json` → `"display": {"width","height"}` | This is the key enabler |
| Draw pixels | `framebuffer_init()`, `buffer_write()`, `buffer_read()` | **RGBA, 32 bpp**, size = `w*h*4` |
| Read inputs | `pin_init()`, `pin_read()` | |
| Edge events | `pin_watch()` + `pin_watch_config_t{edge, pin_change, user_data}` | `RISING`/`FALLING`/`BOTH` |
| Simulated time | `get_sim_nanos()` | ns resolution, virtual time |
| Periodic redraw | `timer_init()`, `timer_start(us, repeat)` | fires in **simulated** time |
| User params | `attr_init()`, `attr_init_float()`, `attr_read()` | set via `attrs` in `diagram.json` |
| Live sliders | `.chip.json` → `controls: []` | **only `"type": "range"` exists** |
| Debug output | `printf()` → "Chips Console" tab | needs trailing `\n` |
| Analog input (future) | `pin_adc_read()`, `ANALOG` mode | for a mixed-signal variant |

### Hard constraints (do not fight these)

1. `pin_init`, `attr_init`, `timer_init`, `framebuffer_init` **may only be called from `chip_init()`**.
2. Only **one `pin_watch` per pin** at a time.
3. Controls are sliders only — no buttons, dropdowns, or text. Enum-style options must be
   encoded as an integer range slider (e.g. `timebaseIndex` 0..11) or a `diagram.json` attribute.
4. There is **no mouse/click input** into a chip. All interaction = sliders + attributes.
   The screen is output-only. Design the UI accordingly (no cursors dragged by hand;
   use a slider-driven cursor position instead).
5. Chip is linked with `-Wl,--import-memory`; the simulator supplies linear memory with an
   **initial size of only 2 pages (128 KB)**, growable on demand. A module whose *declared*
   initial memory exceeds that fails to instantiate:
   `memory import has 2 pages which is smaller than the declared initial of N`.
   ⇒ **Large buffers must be `malloc`'d, not static** (static data + stack define the declared
   initial). Build with `-Wl,-z,stack-size=32768` to keep the declared initial at 1–2 pages.
   `malloc` works fine — it grows the imported memory via `memory.grow`.
   Verify with `node chip/tools/check-memory.js`.
6. Timers run on **simulated** time, so "30 fps" means 30 frames per simulated second, which
   may be far more or fewer wall-clock frames. Budget redraw cost carefully.

---

## 3. Architecture

```
                 D0..D7 (pin_watch, BOTH edges)
                        |
                        v
              +--------------------+        +---------------------+
              |  Capture engine    |  -->   |  Trigger state m/c  |
              |  ring of events    |        |  off/edge/level/auto|
              |  {ts_ns, mask u8}  |        +---------------------+
              +--------------------+
                        |
                        v  (timer @ render rate)
              +--------------------+        +---------------------+
              |  Decoder pipeline  |  -->   |  Annotation spans   |
              |  UART/I2C/SPI/1W   |        |  {t0,t1,kind,text}  |
              +--------------------+        +---------------------+
                        |
                        v
              +--------------------+
              |  Renderer          |  -->  framebuffer (RGBA)
              |  grid/lanes/labels |       buffer_write()
              +--------------------+
```

### 3.1 Capture engine

- **Event/transition based, not uniform sampling.** Store one entry per change of the 8-bit
  input vector: `struct { uint64_t ts_ns; uint8_t mask; }` (16 bytes packed, or 12 with a
  `uint32_t` delta encoding — start simple with the 16-byte version).
- Single `pin_change` callback shared by all 8 pins (`user_data` carries channel index).
  Callback rebuilds the current mask and appends an event if the mask changed.
- Fixed-size ring buffer, compile-time constant `SCOPE_MAX_EVENTS` (start at **32768**;
  = 512 KB). Overwrite oldest in roll mode.
- Keep the callback *tiny* — it runs on every edge and dominates simulation cost.

### 3.2 Trigger state machine

States: `IDLE → ARMED → TRIGGERED → FULL(hold)`.
Modes: `off` (roll / free-run), `edge`, `level`, `auto` (edge with timeout fallback), `single`.
Config: `triggerMode`, `triggerChannel`, `triggerEdge`, `preTriggerPercent`.
Mirrors the built-in `wokwi-logic-analyzer` semantics so users find it familiar.

### 3.3 Renderer

- Framebuffer is RGBA8888, row-major, stride `width*4`.
- Draw into a **static back buffer** in chip memory, then push to the framebuffer.
  Push **only dirty row ranges** via `buffer_write(fb, offset, ptr, len)` to avoid copying
  the whole frame every tick.
- Screen layout (default **480 × 320**):

  ```
  +----------------------------------------------------------+
  | status bar: RUN/STOP  |  2 us/div  |  trig D7 ↑  | 1234ev | 14 px
  +------+---------------------------------------------------+
  | D0   |  ▔▔▔|___|▔▔▔▔▔▔▔|________|▔▔▔▔                    |
  | D1   |  ...                                              | 8 lanes
  | ...  |                                                   | ~26 px each
  | D7   |                                                   |
  +------+---------------------------------------------------+
  | UART |  [ 'H' ][ 'e' ][ 'l' ][ 'l' ][ 'o' ]              | decode lane(s)
  +------+---------------------------------------------------+
  | cursors / measurements: Δt = 4.20 us   f = 238 kHz       | 16 px
  +----------------------------------------------------------+
  ```

- Text: embed a small **5×7 or 6×8 bitmap font** (`font.h`, ASCII 0x20–0x7E) — no libc font,
  no dynamic allocation. Add a 2× scale blitter for the status bar.
- Waveform drawing is **column-oriented**: for each x column compute the time window
  `[t0 + x*ns_per_px, t0 + (x+1)*ns_per_px)`, find the level(s) in that window from the event
  ring, and draw high/low/both (glitch → full-height vertical bar). This gives correct
  rendering even when zoomed way out.
- Per-channel colors, dimmed grid, dark background (classic scope look).

### 3.4 Decoders (phase 4+)

- Common interface so decoders are pluggable:
  ```c
  typedef struct {
    const char *name;
    void (*reset)(void *ctx);
    void (*feed)(void *ctx, uint64_t ts_ns, uint8_t mask);
    // emits into a shared annotation ring
  } decoder_t;
  ```
- Decoders consume the **same event stream** as the renderer (replayed over the visible
  window), so they need no extra capture path.
- Annotations: `{ uint64_t t0, t1; uint8_t kind; char text[12]; }` drawn as rounded boxes with
  centered text in the decode lane; boxes too narrow to fit text collapse to a colored tick.
- Order of implementation: **UART** (simplest, 1 channel) → **I2C** (2 ch, needs START/STOP/ACK)
  → **SPI** (3–4 ch) → **1-Wire**.
- Config via attributes: `decoder` (0=none,1=uart,2=i2c,3=spi,4=onewire), `decoderCh0..3`,
  `uartBaud`, `uartBits`, `uartParity`, `spiMode`, `spiMsbFirst`.

---

## 4. File layout

The workspace root is an **ESP-IDF project** (so the Wokwi VS Code extension finds `wokwi.toml`
and `diagram.json` where it expects them); the custom chip lives in `chip/`.

```
/
├─ CLAUDE.md                     ← this file
├─ README.md                     ← user-facing docs (pins, attrs, examples)
├─ .github/workflows/build.yml   ← CI: wasm + amalgamation + release on a v* tag
├─ wokwi.toml                    ← [[chip]] registration + firmware paths
├─ diagram.json                  ← demo circuit (ESP32-S3 + chip-logic-scope)
├─ CMakeLists.txt                ← ESP-IDF test app
├─ sdkconfig.defaults
├─ main/
│  ├─ CMakeLists.txt
│  └─ signal_gen.c               ← D0/D1 counter, D2-D4 SPI, D5/D6 I2C, D7 UART
└─ chip/
   ├─ logic-scope.chip.json      ← pinout + display + controls  (source of truth)
   ├─ build.ps1                  ← primary build on Windows (WASI SDK)
   ├─ Makefile                   ← same build for Linux/macOS/CI
   ├─ tools/
   │  ├─ check-memory.js         ← instantiates the wasm against a 2-page memory
   │  └─ amalgamate.js           ← src/**.c -> dist/logic-scope.chip.c (wokwi.com)
   ├─ src/
   │  ├─ wokwi-api.h             ← vendored from wokwi/inverter-chip (do not edit)
   │  ├─ main.c                  ← chip_init, pin watches, timers, attribute wiring
   │  ├─ capture.h / capture.c   ← ring buffer + trigger + measurements + queries
   │  ├─ render.h / render.c     ← framebuffer primitives + scope layout
   │  ├─ font.h                  ← 5×7 bitmap font table
   │  └─ decoders/
   │     ├─ decoder.h / decoder.c  ← interface + annotation list
   │     ├─ uart.c                 ← async serial
   │     ├─ i2c.c                  ← two-wire
   │     └─ spi.c                  ← (onewire.c to follow)
   └─ dist/                      ← build output (gitignored)
      ├─ logic-scope.chip.wasm   ← VS Code / wokwi-cli
      ├─ logic-scope.chip.json   ← copied from chip/ at build time
      └─ logic-scope.chip.c      ← amalgamated single file for wokwi.com
```

> **Naming rule:** `wokwi.toml` `[[chip]] name = 'logic-scope'` + `binary = 'chip/dist/logic-scope.chip.wasm'`
> requires a sibling `chip/dist/logic-scope.chip.json`. In `diagram.json` the part type is
> **`chip-logic-scope`**.

---

## 5. Toolchain / build

**Decision: local WASI SDK** (no Docker dependency, fastest iteration on Windows).
WASI SDK 33 installed at `%LOCALAPPDATA%\wasi-sdk`; override with `WASI_SDK_PATH`.

```powershell
cd chip
.\build.ps1            # -> chip/dist/logic-scope.chip.wasm + .chip.json
```

Underlying command (matches `wokwi/inverter-chip`, which is what Wokwi's own builder runs):

```
clang --target=wasm32-unknown-wasi --sysroot <wasi-sdk>/share/wasi-sysroot \
      -nostartfiles -Wl,--import-memory -Wl,--export-table -Wl,--no-entry \
      -Wall -Wextra -Werror -Os -o dist/logic-scope.chip.wasm src/*.c
```

**Test firmware: ESP32-S3 + ESP-IDF** (`board-esp32-s3-devkitc-1`). Board pin names in
`diagram.json` are bare GPIO numbers as strings (`"4"`, `"15"`, …).

```powershell
$env:PATH = "$env:USERPROFILE\.espressif\tools\idf-python\3.11.2;" + $env:PATH
. "$env:USERPROFILE\esp\v5.4.1\esp-idf\export.ps1"
idf.py set-target esp32s3
idf.py build
```

Run/iterate with the **Wokwi for VS Code** extension: `F1 → Wokwi: Start Simulator`.

---

## 6. Chip definition (initial draft)

`logic-scope.chip.json`:

```jsonc
{
  "name": "Logic Scope (8ch)",
  "author": "Gil Tal",
  "pins": ["GND", "VCC", "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7"],
  "display": { "width": 480, "height": 320 },
  "controls": [
    { "id": "timebaseIndex", "label": "Time/div",       "type": "range", "min": 0, "max": 15,  "step": 1 },
    { "id": "running",       "label": "Run 0stop 1run", "type": "range", "min": 0, "max": 1,   "step": 1 },
    { "id": "settingIndex",  "label": "Setting",        "type": "range", "min": 0, "max": 14,  "step": 1 },
    { "id": "settingValue",  "label": "Value",          "type": "range", "min": 0, "max": 255, "step": 1 }
  ]
}
```

Everything else is reached through the **settings menu** (see “Control panel” under Phase 3).
All menu parameters keep their own attributes, so `diagram.json` `attrs` still configures the
chip at startup: `timebaseIndex`, `running`, `cursorPos`, `cursorPosB`, `triggerMode`,
`triggerChannel`, `triggerEdge`, `preTriggerPercent`, `channelMask`, `measureChannel`,
`decoder`, `decoderCh0`, `decoderCh1`, `decoderCh2`, `uartBaudIndex`, `uartBaud`,
`uartBits`, `uartParity`, `uartStop`, `spiMode`, `spiMsbFirst`, `refreshHz`.

`timebaseIndex` maps into a 1-2-5 table: `10ns, 20, 50, 100, 200, 500, 1us … 100ms` per division.

---

## 7. Roadmap

### Phase 0 — Scaffold & smoke test
- [x] Vendor `chip/src/wokwi-api.h`; create `build.ps1` + `Makefile`; WASI SDK 33 installed.
- [x] Minimal `logic-scope.chip.json` with `display` + `main.c` that fills the framebuffer
      with a test pattern and prints a banner to the Chips Console.
- [x] `wokwi.toml` + `diagram.json` (ESP32-S3 + the chip) + ESP-IDF signal generator firmware.
- [x] Confirm the screen renders in VS Code. Byte order confirmed as **RGBA** —
      `RGBA(r,g,b)` in `render.h` is correct as written.
- **Exit criteria:** color bars + live channel blocks appear on the part during simulation. ✅

#### Environment gotchas found during Phase 0

| Symptom | Cause / fix |
|---|---|
| `argument '--target=wasm32-wasi' is deprecated` | wasi-sdk ≥ 25: use `--target=wasm32-wasip1` (same ABI) |
| `unused function 'timer_start_ns' / 'get_sim_nanos'` under `-Werror` | vendored `wokwi-api.h` declares statics → build with `-Wno-unused-function` |
| `raw.githubusercontent.com` unreachable via `curl.exe` | corporate network; `Invoke-WebRequest -UseBasicParsing` works |
| `dl.espressif.com` unreachable | blocked; install IDF python env with `IDF_PYTHON_CHECK_CONSTRAINTS=0` |
| `--interface_version: invalid choice: 3` from CMake | unconstrained PyPI installed `idf-component-manager` 3.x; pin `<3` (also `esptool<5`, `esp-idf-kconfig<3`, `esp-idf-size<2`) |
| `export.ps1` picks `idf5.4_py3.14_env` | system Python is 3.14; prepend `%USERPROFILE%\.espressif\tools\idf-python\3.11.2` to `PATH` first |

### Phase 1 — Live waveforms (the core deliverable)
- [x] `capture.c`: 8 × `pin_init(INPUT)`, shared `pin_change`, event ring buffer
      (`SCOPE_MAX_EVENTS` = 32768) + `capture_columns()` per-column summariser.
- [x] `render.c`: back buffer, `fb_fill_rect`/`hline`/`vline`/`pixel`, dirty-range flush.
- [x] `font.h` (5×7, ASCII 0x20–0x7E + ↑/↓ glyphs) + `fb_text` with integer scaling.
- [x] Column-oriented waveform renderer, 8 lanes + grid + channel labels + live level tabs.
- [x] Roll mode (free-running, newest data at right edge) + full 1-2-5 timebase table.
- [x] Verify against the ESP32-S3 counter firmware in the simulator.
- **Exit criteria:** blinking/PWM GPIOs on an ESP32 show as live square waves on the part. ✅

#### Screen layout (480 × 320)

| Region | Geometry |
|---|---|
| Status bar | `y 0..15` — RUN/STOP, time/div, window, event count, buffer fill |
| Gutter | `x 0..27` — channel name + live level per lane |
| Plot | `x 28..477`, `y 20..291` — 9 divisions × 50 px, lanes share the 272 px |
| Info bar | `y 295` cursors A/B + Δt + 1/Δt + level readout · `y 305` measurements |

`ns_per_px = ns_per_div / 50`; every timebase entry is a multiple of 50 so the
division is exact. A column containing a transition is drawn as a full-height
vertical bar; two or more transitions in one column (aliasing/glitch) switch the
bar to white.

### Phase 2 — Timebase & trigger
- [x] Timebase table + `timebaseIndex` slider, live rescale.
- [x] Trigger state machine (`off`/`edge`/`level`/`auto`/`single`) + pre-trigger buffer.
- [x] Run/Stop via the `running` control; frozen frame when stopped.
- [x] Status bar showing mode, time/div, trigger config, event count, buffer usage.
- **Exit criteria:** a single-shot capture on a rising edge of D7 stays frozen on screen. ✅

#### Phase 2 trigger semantics

| Control | Values |
|---|---|
| `triggerMode` | 0 off (roll) · 1 edge · 2 level · 3 auto · 4 single |
| `triggerChannel` | 0..7 → D0..D7 |
| `triggerEdge` | 0 rising · 1 falling · 2 both (in `level` mode: 0/2 = high, 1 = low) |
| `preTriggerPercent` | 0..100, share of the window drawn before the trigger |

States are `IDLE → ARMED → FILLING → HOLD`. Detection happens inside the
`pin_change` hot path (`trigger_on_change()`); everything else is evaluated once
per frame in `update_sweep()`. The displayed window is only advanced when a sweep
*completes*, so a partially captured frame is never shown. `edge`/`level`/`auto`
re-arm automatically from `HOLD`; `single` stays frozen until the trigger config
changes or `running` is toggled. `auto` rolls freely once armed for
`max(2 × window, 100 ms)` of simulated time without a trigger.

### Phase 3 — Measurements & UX polish
- [x] Slider-driven time cursors A/B + readout (Δt, 1/Δt).
- [x] Auto-measure per channel: frequency, period, duty cycle, min pulse, edge count.
- [x] Per-channel colors, channel enable mask, adaptive lane height for <8 channels.
- [x] Glitch/aliasing indication when a column contains multiple transitions.
- **Exit criteria:** Δt between two cursors and the measured frequency of a channel agree.

#### Phase 3 controls

| Control | Values |
|---|---|
| `cursorPos` / `cursorPosB` | 0..100 %, cursors A (yellow) and B (green) |
| `channelMask` | bitmask, bit *n* shows D*n*; 0 falls back to all 8. Lane height = `272 / count` |
| `measureChannel` | 0..7, channel summarised in the bottom info row |

Measurements (`capture_measure()`) are computed over the **visible window only**:
duty cycle from accumulated high time, period from the first/last rising edge,
`min` from the shortest pulse bounded by two transitions.

#### Control panel — the settings menu

`chip.json` accepts only `name`, `author`, `pins`, `controls`, `display` — there is no
appearance/SVG field, no way to place or group controls, and no mouse/touch input into a
chip. Wokwi renders every slider stacked in its own panel and sizes it with its own CSS, so
**panel size is a function of the label text and the number of `controls`** and nothing else.

One slider per parameter therefore does not scale. The panel is instead **four sliders**:

| Control | Purpose |
|---|---|
| `timebaseIndex` | time/div, used constantly |
| `running` | run/stop, used constantly |
| `settingIndex` | 0..14, selects a parameter |
| `settingValue` | 0..255, edits the selected parameter |

`settingValue` is scaled onto each parameter's own range
(`v = (raw * max + 127) / 255`) and applies **catch-up**: after `settingIndex` changes, the
new raw position is adopted but *not* applied, so selecting a parameter never clobbers it —
only actually moving `Value` writes. Moving either slider shows a transient overlay
(`draw_menu()`, `MENU_HOLD_NS` = 3 s of simulated time) listing all 12 settings in 2 columns
with the selected row highlighted and enum values rendered as text (`edge`, `UART`, `D7`, …).

| Index | Setting | Attribute | Range |
|---|---|---|---|
| 0 / 1 | Cursor A / B | `cursorPos` / `cursorPosB` | 0..100 % |
| 2 | Trig mode | `triggerMode` | off/edge/level/auto/single |
| 3 | Trig ch | `triggerChannel` | D0..D7 |
| 4 | Trig edge | `triggerEdge` | rise/fall/both |
| 5 | Pre-trig | `preTriggerPercent` | 0..100 % |
| 6 | Chan mask | `channelMask` | 0..255 |
| 7 | Measure | `measureChannel` | D0..D7 |
| 8 | Decoder | `decoder` | off/UART/I2C/SPI |
| 9 / 10 / 11 | Dec ch A / B / C | `decoderCh0` / `decoderCh1` / `decoderCh2` | D0..D7 (C also `none`) |
| 12 | Baud | `uartBaudIndex` | attr/300..230400 |
| 13 | SPI mode | `spiMode` | 0..3 |
| 14 | SPI order | `spiMsbFirst` | LSB/MSB |

The table lives in `kSettings[]` in `main.c` (`name`, `attr`, `max`, `def`, optional
`labels`); values live in `g_setting[]`, seeded once in `chip_init()` from
`attr_read(attr_init(...))` and clamped to `max`, so `diagram.json` `attrs` still configure
everything. **Adding a parameter = one row in `kSettings` + bumping `settingIndex`'s `max`
in `chip.json`.**

### Phase 4 — Protocol decoders
- [x] `decoder.h` interface + annotation list + annotation lane renderer.
- [x] UART decoder (baud, data bits, parity, stop bits; ASCII + hex display).
- [x] I2C decoder (START/STOP/ADDR+R/W/ACK/NAK/DATA).
- [x] SPI decoder (modes 0–3, MSB/LSB, optional CS).
- [ ] 1-Wire decoder.
- [ ] Auto-baud detection for UART (nice-to-have).

#### Decoder architecture

Decoders are **stateless replays over the visible window**: every frame
`decoder_run(id, capture, view_start, view_end, cfg, &g_annots)` rebuilds the
annotation list from the same event ring the renderer uses, so there is no
second capture path and no cross-frame state to keep in sync.

Capture exposes three queries for them (`capture.h`):
`capture_mask_at(t)`, `capture_next_edge(from, to, bit_mask, &ts, &mask_after)`
and `capture_next_level(from, to, bit, level, &ts)` — all binary-search based.

Annotations are `{t0, t1, kind, char text[12]}` in a fixed `annot_list_t`
(`ANNOT_MAX` = 192, static). When a decoder is active the plot gives up
`DECODE_H` = 26 px at the bottom; the waveform lanes share `PLOT_H - DECODE_H`
(`update_lanes(mask, area_h)`). Each annotation becomes a filled box with a
border and the **longest prefix of its label that fits** (`48 'H'` → `48` → `4`,
via `fb_text_width_n`), collapsing to a 1 px tick when narrower than 3 px.

| Attribute | Meaning |
|---|---|
| `decoder` | 0 none · 1 UART · 2 I2C · 3 SPI — **also a menu setting** |
| `decoderCh0..2` | channel per decoder line, all three in the menu; `decoderCh2` = 8 means *none* |
| `uartBaudIndex` | 0 = use `uartBaud`, else index into `kBaudTable` (5 = 9600, 9 = 115200) |
| `uartBaud` | bit rate used when `uartBaudIndex` is 0, default 115200 |
| `uartBits` | 5..9 data bits, default 8 — also the SPI word length (4..16) |
| `uartParity` | 0 none · 1 even · 2 odd |
| `uartStop` | 1 or 2 stop bits |
| `spiMode` | 0..3 — CPOL = `mode >> 1`, CPHA = `mode & 1` |
| `spiMsbFirst` | 0 LSB first · 1 MSB first (default) |

| Decoder | Lines |
|---|---|
| UART | `decoderCh0` = RX |
| I2C | `decoderCh0` = SDA, `decoderCh1` = SCL |
| SPI | `decoderCh0` = SCK, `decoderCh1` = MOSI, `decoderCh2` = CS (or *none*) |

The status bar ends with `UART D7 9600 x6` / `I2C D5/D6 x12` /
`SPI D2/D3/D4 m0 x9` (decoder · channels · baud or SPI mode · annotation count)
whenever a decoder is active — the fastest way to tell a misconfigured decoder
from a broken one. Decoding the wrong channel shows up as `FRM` boxes or aliased
bytes such as `55 'U'` / `4B 'K'`.

UART locks onto every falling edge, samples each bit cell in the middle
(LSB first) and checks parity + stop bit; failures render as `FRM`/`PAR` in the
error color. A frame that would extend past the right edge of the window is left
undecoded until the whole frame has been captured.

I2C is self-clocked, so it needs no rate: SDA moving while SCL is high is a
START (`S`) or STOP (`P`), every SCL rising edge samples a bit MSB first, and the
9th clock carries ACK/NAK. The first byte after a (repeated) START is drawn as
`68 W` / `68 R`, the rest via `annot_format_byte`; a NAK turns the box red.
START/STOP are single instants, so they are emitted with zero width and grown in
a final pass into the idle half of the bus — backwards for `S`, forwards for
`P`, by two of the shortest observed SCL periods — which keeps the letter
readable without covering the neighbouring byte.

SPI is clock-driven like I2C, so the only settings are the mode and the bit
order. CPOL/CPHA collapse to **sample on a rising SCK when `CPOL == CPHA`**
(modes 0 and 3) and on a falling SCK otherwise. Only MOSI is decoded. With a CS
channel the decoder counts clocks only while CS is low and drops a partial word
on either CS edge, which resynchronises the word boundary for free; with
`decoderCh2 = none` it decodes continuously.

The demo firmware bit-bangs `"Hello "` at **9600 8N1 on D7** (GPIO 18), runs a
**100 kHz I2C** WHO_AM_I read of the diagram's MPU6050 on **D5 = SDA (GPIO 16)**
and **D6 = SCL (GPIO 17)** every 50 ms, sends `"SPI"` over a **200 kHz mode-0
SPI** bus on **D2 = SCK (GPIO 6)**, **D3 = MOSI (GPIO 7)** and **D4 = CS
(GPIO 15)** every 30 ms, and drives a 2-bit binary counter on D0/D1.

| Signal | Best timebase |
|---|---|
| UART byte (~1.04 ms/frame) | `1ms/div` (index 12) — widest that still fits `48 'H'` |
| I2C transaction (~500 us) | `100us/div` (index 9, the default) |
| SPI burst (3 bytes ≈ 120 us) | `20us/div` (index 7) or `50us/div` (index 8) |

### Phase 5 — Distribution & extras
- [x] `README.md` with pinout, controls/menu, attributes, decoders, build + import steps
      (screenshots still to add).
- [x] `tools/amalgamate.js` → `dist/logic-scope.chip.c`, the single-file source wokwi.com needs.
- [x] GitHub Action (`.github/workflows/build.yml`) building the wasm in
      `wokwi/builder-clang-wasm`, running `check-memory.js`, and attaching
      wasm + json + amalgamated `.c` to a release on a `v*` tag.
- [ ] Push to GitHub, tag `v1.0.0`, publish a public wokwi.com project carrying the chip.
- [ ] Announce: Wokwi Discord `#custom-chips`, Wokwi blog/forum, `awesome-wokwi` lists.
- [ ] Stretch: VCD export parity, analog/mixed-signal variant using `pin_adc_read()`,
      FFT/spectrum lane, math channels (XOR/AND of two channels).

---

## 8. Performance budget & risks

| Risk | Mitigation |
|---|---|
| `pin_change` callback overhead at MHz-rate signals | Keep callback to a compare + 2 stores; no branching on channel; no printf |
| Full-frame `buffer_write` each tick (480×320×4 = 614 KB) | Dirty-row flush; render at ≤20 Hz sim time; skip redraw when no new events |
| Ring buffer memory under `--import-memory` | Static array, compile-time cap, no heap growth |
| Sim-time timers ≠ wall-clock frames | Derive refresh from `get_sim_nanos()` delta, clamp to a max frame rate |
| Zoomed-out rendering aliasing | Per-column min/max/any-transition summary, not point sampling |
| 64-bit math in wasm32 | Fine, but avoid `uint64_t` division in inner loops — precompute `ns_per_px` reciprocal |

---

## 9. Reference links

- Chips API index — https://docs.wokwi.com/chips-api/getting-started
- Chip JSON (`display`, `controls`) — https://docs.wokwi.com/chips-api/chip-json
- Framebuffer API — https://docs.wokwi.com/chips-api/framebuffer
- GPIO / `pin_watch` — https://docs.wokwi.com/chips-api/gpio
- Time / timers — https://docs.wokwi.com/chips-api/time
- Attributes — https://docs.wokwi.com/chips-api/attributes
- `wokwi.toml` `[[chip]]` — https://docs.wokwi.com/vscode/project-config
- Template repo + CI — https://github.com/wokwi/inverter-chip
- Built-in analyzer (semantics to mirror) — https://docs.wokwi.com/parts/wokwi-logic-analyzer
- Prior art: framebuffer example — https://wokwi.com/projects/330503863007183442
- Prior art: Dlloydev scope chip — https://github.com/Dlloydev/Wokwi-Chip-Scope

---

## 10. Conventions for this repo

- C11, no dynamic allocation after `chip_init`, no libc beyond `stdint`/`string`/`stdio`.
- All state lives in a single `scope_state_t`; pass it via `user_data`.
- `snake_case` for C, `camelCase` for chip attributes/controls (Wokwi convention).
- Colors as `0xAABBGGRR` packed `uint32_t` matching the RGBA byte order in the framebuffer
  (verify byte order empirically in Phase 0 — draw pure red and confirm).
- Every phase must leave the demo `diagram.json` in a runnable state.

---

## 11. Distribution — reaching wokwi.com users

Verified against docs.wokwi.com (2026-07). **There is no `dependencies` field in
`diagram.json`** — the format is only `version`, `author`, `editor`, `parts`, `connections`
(+ optional `serialMonitor`). A chip cannot be pulled into a project by URL, and the web
editor cannot load a prebuilt `.wasm`. So there are exactly two delivery paths:

| Target | Artifact | How it is loaded |
|---|---|---|
| wokwi.com (browser) | `dist/logic-scope.chip.c` + `logic-scope.chip.json` | **+** button → *Custom Chip* → name `logic-scope` → paste both files. Wokwi compiles the C **server-side, one file per chip**. |
| VS Code extension / `wokwi-cli` | `dist/logic-scope.chip.wasm` + `.chip.json` | `[[chip]]` in `wokwi.toml`, json sibling with the same base name |

### The amalgamation

`chip/tools/amalgamate.js` flattens `src/**/*.c` into one translation unit:

- roots in link order: `capture.c`, `render.c`, `decoders/{decoder,uart,i2c,spi}.c`, `main.c`
- quoted `#include`s are inlined once each (resolved relative to the including file);
  `<...>` includes are left alone
- `wokwi-api.h` is **kept as an `#include`** — the web compiler supplies it
- each file gets a `/* ==== path ==== */` banner

It is wired into both `build.ps1` and the `Makefile`, so `dist/` always holds all three
artifacts. Because everything ends up in one TU, a duplicate `static` name in two files
becomes a hard error — that is the check to run after adding a decoder:

```powershell
cd chip
node tools\amalgamate.js
# note: PowerShell needs -Wl,... flags quoted or it parses the comma as an array
$sdk = "$env:LOCALAPPDATA\wasi-sdk"
$a = @("--target=wasm32-wasip1","--sysroot=$sdk\share\wasi-sysroot","-Isrc","-nostartfiles",
       "-Wl,--import-memory","-Wl,--export-table","-Wl,--no-entry","-Wl,-z,stack-size=32768",
       "-Wall","-Wextra","-Wno-unused-function","-Werror","-Os",
       "-o","dist\amalgam-test.wasm","dist\logic-scope.chip.c")
& "$sdk\bin\clang.exe" @a
```

### Release + reach

- `.github/workflows/build.yml` builds in `wokwi/builder-clang-wasm`, runs `check-memory.js`,
  uploads artifacts on every push, and attaches wasm + json + `.c` to a GitHub release when a
  tag matching `v*` is pushed (same pattern as `wokwi/inverter-chip`).
- The real one-click channel for web users is a **published wokwi.com project** containing the
  chip and a demo circuit — others open it and press *Save a copy*.
- Announce in the Wokwi Discord `#custom-chips` channel (wokwi.com/discord), then the forum /
  `awesome-wokwi` style lists.
- `wokwi-cli` ≥ 0.20 can also compile chips without a local toolchain:
  `wokwi-cli chip compile src/*.c -o logic-scope.chip.wasm`.
