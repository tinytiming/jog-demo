#include "AW9110JogDial.h"

// AW9110C registers (P0 group = OUT4-9, P1 group = OUT0-3)
#define REG_INPUT0      0x00   // P0DI: OUT4-9 input state
#define REG_P1DO        0x03   // OUT0-3 output state; in SMART-FADE: fade-on(1)/fade-off(0)
#define REG_CONFIG0     0x04   // P0DIR: 1 = input
#define REG_P1DIR       0x05   // OUT0-3 direction; in breathing: BLINK(1) vs SMART-FADE(0)
#define REG_INT0        0x06   // P0MSK: 0 = interrupt enabled
#define REG_INT1        0x07   // P1MSK
#define REG_ID          0x10
#define REG_GCR         0x11   // Global control: D7 = GO (start BLINK), D[1:0] = current range
#define REG_LEDMODE0    0x12   // P0WKMD: 1 = GPIO mode (0 = LED mode); reset default 0xFF
#define REG_LEDMODE1    0x13   // P1WKMD
#define REG_PATEN       0x14   // per-channel breathing enable
#define REG_FDTMR       0x15   // fade-on / fade-off time (BLINK & SMART-FADE)
#define REG_FLTMR       0x16   // hold-bright / hold-dark time (BLINK only)
#define REG_SOFTRESET   0x7F

// DIM (LED constant-current) registers: OUT0/1/2 = R/G/B cathodes.
#define REG_DIM_RED     0x20   // OUT0
#define REG_DIM_GREEN   0x21   // OUT1
#define REG_DIM_BLUE    0x22   // OUT2
#define LED_P1_MASK     0x07   // OUT0/1/2

#define GCR_GO          0x80   // GCR D7: arm BLINK breathing
#define GCR_RANGE       0x02   // GCR D[1:0]=10 -> 0~20mA range (FM-3510 IF max is 25mA)
#define FDTMR_BITS      (((JOG_FADE_OFF & 0x07) << 3) | (JOG_FADE_ON & 0x07))
#define FLTMR_BITS      (((JOG_FULL_OFF & 0x07) << 3) | (JOG_FULL_ON & 0x07))

#define AW9110_ID       0x23
#define INPUT_MASK      ((1 << JOG_UP_D2_BIT) | (1 << JOG_UP_D1_BIT) | (1 << JOG_SELECT_BIT) | \
                         (1 << JOG_DOWN_D1_BIT) | (1 << JOG_DOWN_D2_BIT))

#define DEBOUNCE_MS     20

// Light (detent 1) before full (detent 2) for each direction, so that when both
// close in one I2C frame the small-push event is emitted before the big-push one.
static const uint8_t INPUT_BITS[] = {
  JOG_UP_D1_BIT, JOG_UP_D2_BIT, JOG_SELECT_BIT, JOG_DOWN_D1_BIT, JOG_DOWN_D2_BIT
};
static const uint8_t IDX_UP_D1 = 0, IDX_UP_D2 = 1, IDX_SELECT = 2, IDX_DOWN_D1 = 3, IDX_DOWN_D2 = 4;

static volatile bool _aw9110_int_flag = false;
static void aw9110_isr() { _aw9110_int_flag = true; }

AW9110JogDial::AW9110JogDial(uint8_t addr, long long_press_millis) {
  _wire = NULL;
  _addr = addr;
  _int_pin = -1;
  _long_millis = long_press_millis;
  _handler = NULL;
  for (uint8_t i = 0; i < NUM_INPUTS; i++) {
    _stable_pressed[i] = false;
    _last_commit[i] = 0;
  }
  _sel_down_at = 0;
  _full_up = false;
  _full_down = false;
  _repeat_dir = JOG_EVENT_NONE;
  _repeat_next = 0;
  _last_poll = 0;
  _last_v = 0xFF;
  _r = _g = _b = 0;
  _auto_led = true;
  _breath = JOG_BREATH_OFF;
  _smart_on = false;
  _smart_next = 0;
}

bool AW9110JogDial::readReg(uint8_t reg, uint8_t* val) {
  _wire->beginTransmission(_addr);
  _wire->write(reg);
  if (_wire->endTransmission() != 0) return false;
  if (_wire->requestFrom(_addr, (uint8_t)1) != 1) return false;
  *val = _wire->read();
  return true;
}

void AW9110JogDial::writeReg(uint8_t reg, uint8_t val) {
  _wire->beginTransmission(_addr);
  _wire->write(reg);
  _wire->write(val);
  _wire->endTransmission();
}

// Soft-reset and (re)configure: jog switches on OUT4-8 as interrupting inputs,
// OUT0/1/2 as LED-mode outputs, LED off. Also clears any armed BLINK GO. Ends by
// seeding the debounced input state. Used by begin() and on entering BLINK mode
// (where a reset is the only way to re-arm GO).
bool AW9110JogDial::configurePins() {
  writeReg(REG_SOFTRESET, 0x00);
  delay(5);                                 // let the internal-circuit reset settle
  writeReg(REG_LEDMODE0, INPUT_MASK);       // our pins as GPIO (rest stay LED mode)
  writeReg(REG_CONFIG0, INPUT_MASK);        // our pins as inputs
  writeReg(REG_INT0, (uint8_t)~INPUT_MASK); // interrupt only on our pins
  writeReg(REG_INT1, 0xFF);                 // no P1 interrupts
  writeReg(REG_LEDMODE1, (uint8_t)~LED_P1_MASK); // P1_0/1/2 -> LED mode (0), rest GPIO
  writeReg(REG_GCR, GCR_RANGE);                  // cap LED current to 0~20mA (< FM-3510 25mA max)
  writeReg(REG_DIM_RED, 0x00);                   // all channels off to start
  writeReg(REG_DIM_GREEN, 0x00);
  writeReg(REG_DIM_BLUE, 0x00);
  _r = _g = _b = 0;
  _breath = JOG_BREATH_OFF;

  uint8_t v = 0;
  if (!readReg(REG_INPUT0, &v)) return false;   // seed + clear any pending INT (arms INTN)
  for (uint8_t i = 0; i < NUM_INPUTS; i++) {
    _stable_pressed[i] = (v & (1 << INPUT_BITS[i])) == 0;
  }
  _last_v = v;
  return true;
}

bool AW9110JogDial::begin(TwoWire& wire, int8_t int_pin, JogEventHandler handler) {
  _wire = &wire;
  _int_pin = int_pin;
  _handler = handler;
  _repeat_dir = JOG_EVENT_NONE;
  _full_up = _full_down = false;

  uint8_t id = 0;
  bool id_ok = readReg(REG_ID, &id);
  JOG_DEBUG("AW9110 jog: ID reg=0x%02x (expect 0x%02x) @addr 0x%02x", id, AW9110_ID, _addr);
  if (!id_ok || id != AW9110_ID) {
    JOG_DEBUG("AW9110 jog: chip not found, begin() failed");
    return false;
  }

  // Arm the ISR before the seed read: a press in the gap just sets the flag
  // (one redundant read later). The other order wedges the input forever --
  // a press after the read but before attach holds INTN low with no edge left
  // to fire, and nothing would ever read the port to release it.
  if (_int_pin >= 0) {
    pinMode(_int_pin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(_int_pin), aw9110_isr, FALLING);
  }

  if (!configurePins()) {
    JOG_DEBUG("AW9110 jog: seed read failed, begin() failed");
    if (_int_pin >= 0) detachInterrupt(digitalPinToInterrupt(_int_pin));
    return false;
  }
  JOG_DEBUG("AW9110 jog: begin ok, int_pin=%d, P0=0x%02x", (int)_int_pin, _last_v);
  return true;
}

void AW9110JogDial::service() {
  if (_wire == NULL) return;

  // auto-repeat while a detent 2 is held: no further interrupts come, so this
  // runs on a millis() timer (no I2C). First repeat fires JOG_REPEAT_DELAY_MS
  // after the press (set below), then every JOG_REPEAT_MS.
  if (_repeat_dir != JOG_EVENT_NONE && (long)(millis() - _repeat_next) >= 0) {
    if (_handler) _handler(_repeat_dir);
    _repeat_next = millis() + JOG_REPEAT_MS;
  }

  bool do_read = false;
  if (_aw9110_int_flag) {                  // ISR fired
    _aw9110_int_flag = false;
    do_read = true;
  } else if (_int_pin >= 0) {              // interrupt mode: recover a missed/stuck-low INTN
    do_read = (digitalRead(_int_pin) == LOW);
  } else {                                 // no INT pin wired: poll the port over I2C
    if ((unsigned long)(millis() - _last_poll) >= JOG_POLL_MS) {
      _last_poll = millis();
      do_read = true;
    }
  }
  if (!do_read) return;

  uint8_t v = 0;
  if (!readReg(REG_INPUT0, &v)) {  // reading de-asserts INTN, re-arms the falling edge
    // skip the frame: 0xFF would decode as "all released" and fake a SELECT.
    // INTN is still low, so the recovery check above retries next loop.
    JOG_DEBUG("AW9110 jog: input read failed");
    return;
  }
  unsigned long now = millis();
  if (v != _last_v) {   // only log when the port actually changes (poll mode reads at JOG_POLL_MS)
    JOG_DEBUG("AW9110 jog: P0=0x%02x", v);
    _last_v = v;
  }

  for (uint8_t i = 0; i < NUM_INPUTS; i++) {
    bool pressed = (v & (1 << INPUT_BITS[i])) == 0;  // active-low
    if (pressed == _stable_pressed[i]) continue;
    if ((unsigned long)(now - _last_commit[i]) < DEBOUNCE_MS) continue;  // bounce suppression

    _stable_pressed[i] = pressed;
    _last_commit[i] = now;
    JOG_DEBUG("AW9110 jog: P0_%d %s", INPUT_BITS[i], pressed ? "pressed" : "released");

    switch (i) {
      case IDX_UP_D1:   // light contact: small push fires immediately on press
        if (pressed) {
          _full_up = false;                              // new press cycle
          if (_handler) _handler(JOG_EVENT_UP_LIGHT);
        } else {
          if (!_full_up && _handler) _handler(JOG_EVENT_UP_CANCEL);  // never went big -> undo
          _full_up = false;
        }
        break;

      case IDX_UP_D2:   // full throw: big push, then auto-repeat if held
        if (pressed) {
          _full_up = true;
          if (_handler) _handler(JOG_EVENT_UP);
          _repeat_dir = JOG_EVENT_UP;
          _repeat_next = now + JOG_REPEAT_DELAY_MS;
        } else if (_repeat_dir == JOG_EVENT_UP) {
          _repeat_dir = JOG_EVENT_NONE;
        }
        break;

      case IDX_DOWN_D1:
        if (pressed) {
          _full_down = false;
          if (_handler) _handler(JOG_EVENT_DOWN_LIGHT);
        } else {
          if (!_full_down && _handler) _handler(JOG_EVENT_DOWN_CANCEL);
          _full_down = false;
        }
        break;

      case IDX_DOWN_D2:
        if (pressed) {
          _full_down = true;
          if (_handler) _handler(JOG_EVENT_DOWN);
          _repeat_dir = JOG_EVENT_DOWN;
          _repeat_next = now + JOG_REPEAT_DELAY_MS;
        } else if (_repeat_dir == JOG_EVENT_DOWN) {
          _repeat_dir = JOG_EVENT_NONE;
        }
        break;

      case IDX_SELECT:  // short vs long press of center
        if (pressed) {
          _sel_down_at = now;
        } else if (_handler) {
          _handler((unsigned long)(now - _sel_down_at) >= (unsigned long)_long_millis
                     ? JOG_EVENT_ENTER : JOG_EVENT_SELECT);
        }
        break;
    }
  }

  updateLED();   // reflect the new lever state on the LED (no-op when the app/breathing owns it)
}

bool AW9110JogDial::isPressed() const {
  return _stable_pressed[IDX_SELECT];
}

bool AW9110JogDial::fullThrowHeld() const {
  return _stable_pressed[IDX_UP_D2] || _stable_pressed[IDX_DOWN_D2];
}

void AW9110JogDial::setRGB(uint8_t r, uint8_t g, uint8_t b) {
  if (_wire == NULL) return;
  uint8_t dr = (uint16_t)r * JOG_BAL_R / 255;   // per-channel luminance balance (FM-3510)
  uint8_t dg = (uint16_t)g * JOG_BAL_G / 255;
  uint8_t db = (uint16_t)b * JOG_BAL_B / 255;
  if (dr != _r) { writeReg(REG_DIM_RED, dr);   _r = dr; }   // only push the channels that changed
  if (dg != _g) { writeReg(REG_DIM_GREEN, dg); _g = dg; }
  if (db != _b) { writeReg(REG_DIM_BLUE, db);  _b = db; }
}

void AW9110JogDial::setHue(uint8_t hue, uint8_t value) {
  uint8_t r, g, b;
  hsvToRgb(hue, 255, value, r, g, b);
  setRGB(r, g, b);
}

// Select the breathing engine for OUT0/1/2.
//  OFF   : steady DIM output (setRGB/setHue control the colour directly).
//  SMART : SMART-FADE -- each channel fades to its own DIM (full hue). Drive the
//          fade direction from breatheLoop(); no GO needed.
//  BLINK : autonomous BLINK -- every channel breathes to the same peak (GCR
//          Imax), so blink_mask selects which channels are on -> one of 6 hues.
//          BLINK's GO can only be armed once per reset, so re-init the chip here.
void AW9110JogDial::setBreath(JogBreath mode, uint8_t blink_mask) {
  if (_wire == NULL) return;
  switch (mode) {
    case JOG_BREATH_OFF:
      writeReg(REG_PATEN, 0x00);   // stop breathing -> steady DIM output
      writeReg(REG_P1DO, 0x00);
      break;

    case JOG_BREATH_SMART:
      writeReg(REG_FDTMR, FDTMR_BITS);
      writeReg(REG_P1DIR, 0x00);          // OUT0/1/2 -> SMART-FADE (the default)
      writeReg(REG_PATEN, LED_P1_MASK);   // breathing on OUT0/1/2
      writeReg(REG_P1DO, 0x00);           // start dark; breatheLoop() fades it up next tick
      _smart_on = false;
      _smart_next = millis();
      break;

    case JOG_BREATH_BLINK:
      configurePins();                    // reset so GO can be armed cleanly
      writeReg(REG_FDTMR, FDTMR_BITS);
      writeReg(REG_FLTMR, FLTMR_BITS);
      writeReg(REG_P1DIR, LED_P1_MASK);   // OUT0/1/2 -> BLINK
      writeReg(REG_PATEN, blink_mask & LED_P1_MASK);
      writeReg(REG_GCR, GCR_GO | GCR_RANGE);   // GO last; keep the 0~20mA cap
      break;
  }
  _breath = mode;
}

// Change which channels breathe in BLINK mode without re-arming GO (lets the
// demo step through the 6 hues). No effect unless already in BLINK mode.
void AW9110JogDial::setBlinkMask(uint8_t mask) {
  if (_wire == NULL || _breath != JOG_BREATH_BLINK) return;
  writeReg(REG_PATEN, mask & LED_P1_MASK);
}

// SMART-FADE only: flip the fade direction every JOG_SMART_HALF_MS. Writing the
// P1DO bits 1 fades the enabled channels up to their DIM (the hue); 0 fades them
// down to dark. The 64-step ramp between is the chip's; the host only toggles.
void AW9110JogDial::breatheLoop() {
  if (_breath != JOG_BREATH_SMART) return;
  if ((long)(millis() - _smart_next) < 0) return;
  _smart_on = !_smart_on;
  writeReg(REG_P1DO, _smart_on ? LED_P1_MASK : 0x00);
  _smart_next = millis() + JOG_SMART_HALF_MS;
}

void AW9110JogDial::enableAutoLED(bool on) { _auto_led = on; }

// 8-bit HSV -> RGB. h/s/v and the outputs are all 0..255.
void AW9110JogDial::hsvToRgb(uint8_t h, uint8_t s, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t region = h / 43;                 // six 43-wide sectors of the wheel
  uint8_t rem = (h - region * 43) * 6;     // position within the sector, 0..255
  uint8_t p = (uint16_t)v * (255 - s) / 255;
  uint8_t q = (uint16_t)v * (255 - ((uint16_t)s * rem) / 255) / 255;
  uint8_t t = (uint16_t)v * (255 - ((uint16_t)s * (255 - rem)) / 255) / 255;
  switch (region) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
}

// Built-in live feedback (auto mode only, i.e. not breathing): detent 2 = bright,
// detent 1 = dim, center = red. Direction colour: up = green, down = blue.
void AW9110JogDial::updateLED() {
  if (!_auto_led) return;       // app / breathing engine owns the LED
  if (_stable_pressed[IDX_UP_D2])        setRGB(0, JOG_LED_BRIGHTNESS, 0);
  else if (_stable_pressed[IDX_DOWN_D2]) setRGB(0, 0, JOG_LED_BRIGHTNESS);
  else if (_stable_pressed[IDX_UP_D1])   setRGB(0, JOG_LED_DIM, 0);
  else if (_stable_pressed[IDX_DOWN_D1]) setRGB(0, 0, JOG_LED_DIM);
  else if (_stable_pressed[IDX_SELECT])  setRGB(JOG_LED_BRIGHTNESS, 0, 0);
  else                                   setRGB(0, 0, 0);
}

void AW9110JogDial::prepareForSleep() {
  if (_wire == NULL) return;

  setBreath(JOG_BREATH_OFF, 0);
  setRGB(0, 0, 0);             // don't leave the LED lit through sleep

  // Drain to idle before the host arms an INT-line wake. INTN pulses low on
  // *any* input change -- including releasing the key that triggered sleep -- so
  // wait until every input reads released (idle high), then one final read to
  // de-assert INTN. Bounded so a stuck input can't wedge shutdown.
  uint8_t v = 0;
  unsigned long t0 = millis();
  while ((millis() - t0) < 2000) {
    if (!readReg(REG_INPUT0, &v)) return;
    if ((v & INPUT_MASK) == INPUT_MASK) break;   // all released
    delay(10);
  }
  readReg(REG_INPUT0, &v);   // final read clears INTN -> line idles high
}
