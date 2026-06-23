# UM FeatherS2 Neo + AW9110C Jog Breakout — PlatformIO demo

Reads the **Jog Breakout (revC2)** — an ALPS two-detent up/down jog lever with a
center push and an RGB indicator LED, all behind an **AW9110C** I²C GPIO
expander / LED driver — from an **Unexpected Maker FeatherS2 Neo** (ESP32-S2)
over I²C, and shows the navigation on the FeatherS2 Neo's **built-in 5×5 RGB
matrix**.

Each matrix column lights one dot at a row; it starts as a horizontal line at
**row 2**. Column 2's dot is the **central** dot, columns 0/1/3/4 are the
**outer** dots. The lever is a two-stage switch, so a light touch and a full
throw do different things:

- **Small push (light touch) up/down** — **on press**, previews a one-row move of
  the **central** dot **plus a randomly-chosen neighbour** (column 1 or 3),
  wrapping around the matrix edges, and retints the LED to that row's hue.
- **Release without a big push** — **cancels** the preview (both dots and colour revert).
- **Big push (full throw)** — **commits**: the previewed move is kept and the
  outer dots jump to the central dot's row (a horizontal line at the new spot).
- **Center push** — flips the board 90° (rows ↔ columns): the line toggles
  between horizontal and vertical, and the lever's motion axis flips with it.

Every matrix dot is coloured by **its own row's hue**, so an aligned line is one
solid colour and the previewing central dot pops out in a neighbouring hue while
it's off the line.

The jog LED sits at **50% brightness**, jumping to **full only while a big push
(full throw) is held** and dropping back on release. A small push keeps it dim
(it just changes the hue); the center push doesn't brighten it either.

So a small push previews on the centre column; push harder to commit it to the
grid, or just release to cancel. The jog LED shows that hue **steadily** (via
`DIM`). It does *not* breathe: the AW9110C's hardware breathing engines (BLINK and
SMART-FADE) both ramp every channel to `GCR` Imax, which is white for a mix — they
can't fade a true hue. (For a breathing hue you'd animate `DIM` in software.)

The driver still exposes those engines — `setBreath(JOG_BREATH_BLINK, mask)` and
`setBreath(JOG_BREATH_SMART)` — for single-colour breathing, even though this demo
drives the LED steadily.

## Wiring

The FeatherS2 Neo exposes I²C on its Feather header (**SDA = GPIO8, SCL =
GPIO9**). Connect the Jog Breakout's four I²C lines — from its JST-SH connector
(J1) or its 1×05 header (J2) — to the Feather:

| FeatherS2 Neo | Jog Breakout |
| ------------- | ------------ |
| 3V3           | 3V3          |
| GND           | GND          |
| SDA (GPIO8)   | SDA          |
| SCL (GPIO9)   | SCL          |

`Wire.begin()` with no arguments already targets GPIO8/GPIO9 on the
`um_feathers2_neo` board, so no pin numbers are hard-coded. The 5×5 matrix uses
the board's own `NEOPIXEL_MATRIX_DATA` (GPIO21) and `NEOPIXEL_MATRIX_PWR`
(GPIO4) — nothing to wire.

### Optional: interrupt mode

The AW9110C's active-low **INTN** is broken out on the Jog Breakout's 1×05
header (J2), not on the 4-pin connector. Over I²C alone the driver polls the
expander every `JOG_POLL_MS` (50 ms) — fine for a jog dial. For zero-poll,
interrupt-driven operation, run a wire from J2's INT pin to a free Feather GPIO
and set `PIN_JOG_INT` in [`src/main.cpp`](src/main.cpp) to that pin (e.g. `5`).

## Board map (from the revC2 netlist)

The AW9110C splits its 10 outputs into two register groups: **P0 = OUT4–9**,
**P1 = OUT0–3**.

| AW9110C | Net   | Function                                  | P0DI bit |
| ------- | ----- | ----------------------------------------- | -------- |
| OUT0    | RED_K | RGB LED red cathode (LED constant-current)| —        |
| OUT1    | GRN_K | RGB LED green cathode                     | —        |
| OUT2    | BLU_K | RGB LED blue cathode                      | —        |
| OUT4    | /P1   | up — light touch (detent 1, LED only)     | 0        |
| OUT5    | /P2   | up — full throw (detent 2, navigates)     | 1        |
| OUT6    | /PT   | center push (select / long-press enter)   | 2        |
| OUT7    | /P3   | down — full throw (detent 2, navigates)   | 3        |
| OUT8    | /P4   | down — light touch (detent 1, LED only)   | 4        |

Switch common is GND with 10 kΩ pull-ups, so the inputs are **active-low**.
**AD0 and AD1 are tied high → I²C address `0x5B`.** Chip ID register `0x10`
reads `0x23`.

## Build, upload, monitor

Requires [PlatformIO](https://platformio.org/) (`pip install platformio`, or the
VS Code extension).

```bash
pio run                 # build
pio run -t upload       # flash the FeatherS2 Neo (hold BOOT, tap RESET if it won't enter the bootloader)
pio device monitor      # 115200 baud
```

On boot the jog board's LED starts breathing and the matrix shows the cursor.
Work the lever and watch the cursor move (1 row on a light touch, 2 on a full
throw) and the hue rotate.

## Notes

- The driver in [`lib/AW9110JogDial`](lib/AW9110JogDial) is board-agnostic — only
  the demo `main.cpp` knows about the FeatherS2 Neo. Its diagnostics go through a
  `JOG_DEBUG` macro gated by the `JOG_DEBUG_SERIAL` build flag (set in
  [`platformio.ini`](platformio.ini)); remove that flag to silence the raw
  P0DI/event output.
- If the matrix bar moves the wrong way or sideways, flip `MATRIX_FLIP_Y` (or
  adjust `pixelIndex()`) in [`src/main.cpp`](src/main.cpp); pixel order is
  assumed row-major from the top.
- Tunables (LED brightness, long-press time, auto-repeat rate, poll interval)
  are `#define`s at the top of
  [`AW9110JogDial.h`](lib/AW9110JogDial/AW9110JogDial.h); override them with
  `-D` flags in `platformio.ini`.
- **Light vs full press** is decided on *release* of the light contact: a full
  throw closes both contacts, so if the full contact was reached during a press,
  no `*_LIGHT` event fires. The driver emits `JOG_EVENT_UP`/`DOWN` (full) and
  `JOG_EVENT_UP_LIGHT`/`DOWN_LIGHT` (light); map them however you like.
- **LED API:** `setRGB(r,g,b)` / `setHue(hue,value)` write the per-channel `DIM`
  (0x20/21/22). `setBreath(mode, mask)` selects the engine:
  - `JOG_BREATH_OFF` — steady `DIM`.
  - `JOG_BREATH_SMART` — SMART-FADE (`PATEN` 0x14, `P1DIR`=0 for SMART, `FDTMR`
    0x15, no `GO`). Call `breatheLoop()` each loop so the host flips the `P1DO`
    (0x03) direction bit every `JOG_SMART_HALF_MS` (must exceed the fade time).
  - `JOG_BREATH_BLINK` — autonomous BLINK (`P1DIR`=1, `FLTMR` 0x16, `GCR.GO`
    0x11). `mask` / `setBlinkMask()` pick channels; `GO` is one-shot per reset, so
    entering BLINK does a soft reset + reconfigure.
  - **Both** breathing engines ramp every channel to `GCR` Imax, **not** `DIM` —
    so they breathe white for a mix and can only do single colours / the 6
    saturated hues. A breathing *hue* has to be a software `DIM` animation; this
    demo just shows the hue steadily.
- **LED current & colour balance (FM-3510RGBA-SG):** the LED's absolute-max
  forward current is 25 mA, below the AW9110C's 0–40 mA default range, so the
  driver caps `GCR.GCC` to **0–20 mA** — safe even at `DIM=0xFF`. The channels
  also differ hugely in efficiency (typ Iv: R 445 mcd@15 mA, G 762 mcd@8 mA, B
  104 mcd@5 mA), so `setRGB`/`setHue` scale each by `JOG_BAL_R/G/B` (≈0.70 / 0.22
  / 1.0) to equalise luminance — otherwise white reads green. This balances the
  steady `DIM` colour this demo uses; the hardware breathing engines ignore `DIM`,
  so their colours are raw. Raise `JOG_BAL_G` for a brighter, greener look.
- Tune breath speed with `JOG_FADE_ON/OFF`, `JOG_FULL_ON/OFF`, `JOG_SMART_HALF_MS`
  in [`AW9110JogDial.h`](lib/AW9110JogDial/AW9110JogDial.h), and `LED_VALUE` /
  `HUE_STEP_*` in [`src/main.cpp`](src/main.cpp).
- Single instance only — the INTN ISR uses one shared flag.
- If a direction's detents feel inverted, swap its `_D1`/`_D2` bit defines; if
  up and down are swapped, swap the `UP_*` pair with the `DOWN_*` pair.
```
