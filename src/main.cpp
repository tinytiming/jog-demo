#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include "AW9110JogDial.h"

// ---- FeatherS2 Neo 5x5 RGB matrix ------------------------------------------
// Pins come from the board variant: data = NEOPIXEL_MATRIX_DATA (GPIO21),
// power-enable = NEOPIXEL_MATRIX_PWR (GPIO4). The matrix is 5x5 = 25 pixels.
#define MATRIX_W      5
#define MATRIX_H      5
#define NUM_PIXELS    (MATRIX_W * MATRIX_H)
#define CENTRAL_COL   2      // column 2 holds the "central" dot; columns 0,1,3,4 are "outer"

static Adafruit_NeoPixel matrix(NUM_PIXELS, NEOPIXEL_MATRIX_DATA, NEO_GRB + NEO_KHZ800);

// Pixel order is assumed row-major from the top: index = y*5 + x. The dots move
// vertically (up/down with the lever); flip this if up/down come out inverted.
#define MATRIX_FLIP_Y 0
static inline uint16_t pixelIndex(uint8_t x, uint8_t y) {
  if (MATRIX_FLIP_Y) y = (MATRIX_H - 1) - y;
  return y * MATRIX_W + x;
}

// ---- Jog Breakout ----------------------------------------------------------
// The Jog Breakout's INT line is on its 1x05 header (J2), not on the 4-pin
// STEMMA QT connector. Wired by I2C only, leave this -1 and the driver polls
// the expander. Wire J2's INT to a free Feather GPIO for interrupt mode.
#define PIN_JOG_INT   -1      // e.g. 5 if you wire INT -> GPIO5

// AD0 + AD1 are tied high on the revC2 board -> I2C address 0x5B.
static AW9110JogDial jog(0x5B);

#define LED_IDLE      128      // jog-LED brightness when idle / small push (50%)
#define LED_PUSHED    255      // jog-LED brightness while a big push is held (full)

// Each column lights one dot at row rowPos[col]. Starts as row 2 in every column
// (a horizontal line). Small push previews a move of the central dot (column 2)
// and one neighbour; big push commits it and snaps the outer dots to the central
// dot's row; center push flips the board 90 degrees.
static uint8_t rowPos[MATRIX_W] = { 2, 2, 2, 2, 2 };
static bool transposed = false;   // center push flips the board 90 deg (rows <-> cols)

static uint8_t hueForRow(uint8_t row) { return row * 51; }   // row 0..4 -> hue 0,51,102,153,204
static uint8_t wrapRow(int r) { return (r % MATRIX_H + MATRIX_H) % MATRIX_H; }   // wrap top<->bottom

// Every dot is coloured by its own row's hue: when all dots share a row they are
// the same colour (a clean line); when the central dot steps to a different row
// (a preview) it takes that row's hue and stands out.
static void render() {
  matrix.clear();
  for (int col = 0; col < MATRIX_W; col++) {
    uint8_t row = rowPos[col];
    uint8_t r, g, b;
    AW9110JogDial::hsvToRgb(hueForRow(row), 255, 255, r, g, b);
    uint8_t x = transposed ? row : col;   // flip swaps rows and columns
    uint8_t y = transposed ? col : row;
    matrix.setPixelColor(pixelIndex(x, y), matrix.Color(r, g, b));
  }
  matrix.show();
}

// The central dot's row sets the LED hue, shown STEADILY: the AW9110C's hardware
// breathing only reaches full Imax per channel (== white for a mix), so it can't
// breathe a real hue -- steady DIM keeps the colour correct.
// Each small push moves the central dot (column 2) plus a randomly-chosen
// neighbour (column 1 or 3); remember both rows so a cancel can revert them.
static uint8_t prevCentralRow  = 2;
static uint8_t movedNeighbor   = CENTRAL_COL - 1;
static uint8_t prevNeighborRow = 2;

static void beginStep() {
  movedNeighbor   = (esp_random() & 1) ? (CENTRAL_COL + 1) : (CENTRAL_COL - 1);
  prevCentralRow  = rowPos[CENTRAL_COL];
  prevNeighborRow = rowPos[movedNeighbor];
}

// Drive the jog LED to the central dot's hue: full brightness only while a big
// push (full throw) is held, 50% otherwise (idle or a small push).
static void applyLed() {
  jog.setHue(hueForRow(rowPos[CENTRAL_COL]), jog.fullThrowHeld() ? LED_PUSHED : LED_IDLE);
}

static void refreshCentral(const char* what) {
  applyLed();
  render();
  Serial.printf("%s: central row=%d hue=%u\n", what, rowPos[CENTRAL_COL], hueForRow(rowPos[CENTRAL_COL]));
}

static void equalizeOuter() {   // big push commit: outer dots jump to the central dot's row
  for (int col = 0; col < MATRIX_W; col++) rowPos[col] = rowPos[CENTRAL_COL];
  render();
  Serial.printf("equalize -> all row=%d\n", rowPos[CENTRAL_COL]);
}

static void onJog(int event) {
  switch (event) {
    // Small push fires on PRESS: preview a move of the central dot + a random
    // neighbour (they start aligned, so they move together), and retint the LED.
    case JOG_EVENT_UP_LIGHT:
      beginStep();
      rowPos[CENTRAL_COL]   = wrapRow(rowPos[CENTRAL_COL]   - 1);
      rowPos[movedNeighbor] = wrapRow(rowPos[movedNeighbor] - 1);
      refreshCentral("up");
      break;
    case JOG_EVENT_DOWN_LIGHT:
      beginStep();
      rowPos[CENTRAL_COL]   = wrapRow(rowPos[CENTRAL_COL]   + 1);
      rowPos[movedNeighbor] = wrapRow(rowPos[movedNeighbor] + 1);
      refreshCentral("down");
      break;

    // Small push released without going big: undo the preview (both dots + colour).
    case JOG_EVENT_UP_CANCEL:
    case JOG_EVENT_DOWN_CANCEL:
      rowPos[CENTRAL_COL]   = prevCentralRow;
      rowPos[movedNeighbor] = prevNeighborRow;
      refreshCentral("cancel");
      break;

    // Big push: keep the previewed central dot and commit it to the whole grid.
    case JOG_EVENT_UP:
    case JOG_EVENT_DOWN:
      equalizeOuter();
      break;

    // Center push: flip the board 90 degrees (rows <-> columns).
    case JOG_EVENT_SELECT:
      transposed = !transposed;
      render();
      Serial.printf("flip -> %s\n", transposed ? "transposed" : "normal");
      break;
    case JOG_EVENT_ENTER:
      Serial.println(F("ENTER"));
      break;
  }
}

static bool ready = false;

static void tryBegin() {
  // Wire.begin() with no args uses the FeatherS2 Neo defaults: SDA=GPIO8, SCL=GPIO9.
  ready = jog.begin(Wire, PIN_JOG_INT, onJog);
  if (ready) {
    jog.enableAutoLED(false);                                  // the demo owns the jog LED
    applyLed();                                                // steady hue, 50% while idle
    Serial.println(F("jog ready: small push previews a move (on press), release cancels,"));
    Serial.println(F("           big push commits/equalizes, center push flips the board"));
  } else {
    Serial.println(F("jog not found at 0x5B (check I2C wiring); retrying"));
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("UM FeatherS2 Neo + AW9110C jog demo"));

  pinMode(NEOPIXEL_MATRIX_PWR, OUTPUT);
  digitalWrite(NEOPIXEL_MATRIX_PWR, HIGH);   // power the 5x5 matrix
  matrix.begin();
  matrix.setBrightness(20);                  // half of the previous 40
  render();                                  // row 2 lit in every column

  Wire.begin();
  Wire.setClock(400000);                     // AW9110C supports 400 kHz fast-mode I2C

  tryBegin();
}

void loop() {
  if (!ready) {
    static unsigned long next_try = 0;
    if (millis() >= next_try) {
      next_try = millis() + 1000;
      tryBegin();
    }
    return;
  }

  jog.service();      // decode lever -> events (interrupt or poll)

  // Brightness: full only while a big push (full throw) is held, else 50%. The
  // big-push press/release fire no hue event, so re-apply on the state change.
  static bool lastBright = false;
  bool bright = jog.fullThrowHeld();
  if (bright != lastBright) {
    lastBright = bright;
    applyLed();
  }
}
