#pragma once

#include <Arduino.h>
#include <Wire.h>

// Debug hook. Define JOG_DEBUG_SERIAL=1 (e.g. in platformio.ini build_flags) to
// route the driver's diagnostics to Serial; otherwise they compile out.
#if defined(JOG_DEBUG_SERIAL) && JOG_DEBUG_SERIAL
  #define JOG_DEBUG(...) do { Serial.printf(__VA_ARGS__); Serial.println(); } while (0)
#else
  #define JOG_DEBUG(...) do {} while (0)
#endif

#define JOG_EVENT_NONE        0
#define JOG_EVENT_UP          1   // up full throw   (detent 2) -- "big push"
#define JOG_EVENT_DOWN        2   // down full throw (detent 2) -- "big push"
#define JOG_EVENT_SELECT      3   // short press of center
#define JOG_EVENT_ENTER       4   // long press of center
#define JOG_EVENT_UP_LIGHT    5   // up light touch PRESS   (detent 1 down) -- "small push"
#define JOG_EVENT_DOWN_LIGHT  6   // down light touch press (detent 1 down)
#define JOG_EVENT_UP_CANCEL   7   // up light released without a full throw -- small push undone
#define JOG_EVENT_DOWN_CANCEL 8   // down light released without a full throw

// Input-port (reg 0x00, P0DI) bit each jog actuation lands on. AW9110C drives
// the five switches on OUT4-8 -> P0DI bits 0-4 (active-low, 10k pull-up, common
// to GND). Each lever direction is a two-step (ALPS SLLB120300) switch and the
// driver fires, on the contact edges:
//   detent 1 press        -> JOG_EVENT_*_LIGHT   ("small push", immediately on press)
//   detent 2 press        -> JOG_EVENT_UP/DOWN   ("big push" / full throw)
//   detent 1 release w/o 2 -> JOG_EVENT_*_CANCEL  (a small push that never went big)
// A full throw passes through detent 1 first, so a big push emits *_LIGHT then
// UP/DOWN -- the small push "previews", the full throw "commits", and no CANCEL
// fires. A light-only tap emits *_LIGHT on press and *_CANCEL on release.
//
// Hardware-confirmed via debug P0DI dump: up reads 0x1E then 0x1C, down
// reads 0x0F then 0x07 -- the LIGHT contact closes bit 0 (up) / bit 4 (down),
// and the FULL throw adds bit 1 (up) / bit 3 (down); center is bit 2.
// If a direction's detents feel inverted, swap its _D1/_D2 bits; if up/down are
// inverted, swap the UP_* pair with the DOWN_* pair.
#define JOG_UP_D1_BIT     0   // OUT4 = /P1, up light touch  (detent 1)
#define JOG_UP_D2_BIT     1   // OUT5 = /P2, up full throw   (detent 2)
#define JOG_SELECT_BIT    2   // OUT6 = /PT, center
#define JOG_DOWN_D2_BIT   3   // OUT7 = /P3, down full throw (detent 2)
#define JOG_DOWN_D1_BIT   4   // OUT8 = /P4, down light touch (detent 1)

// Common-anode RGB LED on OUT0/1/2 (constant-current LED mode, DIM regs
// 0x20/21/22). Three ways to light it:
//   setRGB()/setHue()       - write per-channel DIM directly: steady, full hue.
//   setBreath(JOG_BREATH_SMART) - SMART-FADE: host flips one direction bit in
//                             breatheLoop() and the chip hardware-fades (64 steps)
//                             on/off. Like BLINK it ramps to GCR Imax, not DIM, so
//                             it breathes WHITE when several channels are on -- good
//                             for single-colour breathing, not a hue.
//   setBreath(JOG_BREATH_BLINK, mask) - autonomous BLINK; same Imax peak, so only
//                             the 6 saturated hues (one/two channels via the mask).
// NOTE: neither hardware engine can breathe a mixed colour (both peak at Imax). For
// a breathing *hue*, animate DIM in software (setHue) instead.

// LED current & colour balance for the FM-3510RGBA-SG (C727903). Its 25mA
// absolute-max forward current is below the AW9110C's 0~40mA default range, so
// the driver caps GCR.GCC to 0~20mA -- safe even at DIM=0xFF. Its channels also
// differ hugely in efficiency (typ Iv: R 445mcd@15mA, G 762mcd@8mA, B
// 104mcd@5mA), so setRGB()/setHue() scale each by JOG_BAL_* to equalise
// luminance (blue is dimmest -> reference); otherwise white looks green. This
// balances the DIM-based modes (steady / SMART-FADE) only -- BLINK ignores DIM,
// so its 6 hues stay raw. Raise JOG_BAL_G toward 255 for a brighter, greener look.
#ifndef JOG_BAL_R
#define JOG_BAL_R  179   // 0.70 x full
#endif
#ifndef JOG_BAL_G
#define JOG_BAL_G   56   // 0.22 x full
#endif
#ifndef JOG_BAL_B
#define JOG_BAL_B  255   // 1.00 x full
#endif

// Breathing fade/hold timers, as the AW9110C's 3-bit codes (FDTMR 0x15 / FLTMR 0x16):
//   000=0ms 001=315ms 010=630ms 011=1260ms 100=2520ms 101=5040ms
#ifndef JOG_FADE_ON
#define JOG_FADE_ON    0b010   // fade-in  time  (630 ms)
#endif
#ifndef JOG_FADE_OFF
#define JOG_FADE_OFF   0b010   // fade-out time  (630 ms)
#endif
#ifndef JOG_FULL_ON
#define JOG_FULL_ON    0b001   // BLINK hold-bright time (315 ms)
#endif
#ifndef JOG_FULL_OFF
#define JOG_FULL_OFF   0b001   // BLINK hold-dark time   (315 ms)
#endif

// SMART-FADE half-period: how long the host holds each direction before flipping
// the fade bit. Must exceed the fade time above (a '0' may only be written once
// the fade-on has reached full brightness), so keep this > the JOG_FADE_* ms.
#ifndef JOG_SMART_HALF_MS
#define JOG_SMART_HALF_MS   1000
#endif

#ifndef JOG_LED_BRIGHTNESS
#define JOG_LED_BRIGHTNESS  0x80   // built-in (non-breathing) feedback: detent 2 / center
#endif

#ifndef JOG_LED_DIM
#define JOG_LED_DIM         0x10   // built-in (non-breathing) feedback: detent 1
#endif

#ifndef JOG_REPEAT_DELAY_MS
#define JOG_REPEAT_DELAY_MS 500    // hold detent 2 this long before auto-repeat kicks in
#endif

#ifndef JOG_REPEAT_MS
#define JOG_REPEAT_MS       300    // medium-speed scroll interval once repeating
#endif

#ifndef JOG_POLL_MS
#define JOG_POLL_MS         50     // I2C poll interval when no INT pin is wired (PIN_JOG_INT < 0)
#endif

enum JogBreath { JOG_BREATH_OFF, JOG_BREATH_SMART, JOG_BREATH_BLINK };

typedef void (*JogEventHandler)(int event);

// AW9110C jog-dial reader: up / down / center-select off the I2C GPIO expander.
// Fully interrupt-driven: the chip's INTN line sets a flag, service() reads the
// port off the ISR and pushes decoded events to the handler. Falls back to I2C
// polling when no INT pin is wired (the STEMMA QT connector does not carry it).
// Single instance only (shared ISR flag).
class AW9110JogDial {
  TwoWire* _wire;
  uint8_t _addr;
  int8_t _int_pin;
  long _long_millis;
  JogEventHandler _handler;

  static const uint8_t NUM_INPUTS = 5;
  bool _stable_pressed[NUM_INPUTS];
  unsigned long _last_commit[NUM_INPUTS];

  unsigned long _sel_down_at;
  bool _full_up, _full_down;   // this direction's full throw (detent 2) was reached this press

  uint8_t _repeat_dir;   // JOG_EVENT_UP/DOWN while held at detent 2, else NONE
  unsigned long _repeat_next;
  unsigned long _last_poll;   // poll-mode (no INT pin) last I2C read
  uint8_t _last_v;            // last raw port value (to de-spam the debug log)

  uint8_t _r, _g, _b;    // last RGB levels written to the DIM registers
  bool _auto_led;        // true: driver shows lever feedback; false: app/breathing owns the LED
  JogBreath _breath;     // current breathing engine
  bool _smart_on;        // SMART-FADE: current direction-bit state
  unsigned long _smart_next;   // SMART-FADE: next direction flip

  bool readReg(uint8_t reg, uint8_t* val);
  void writeReg(uint8_t reg, uint8_t val);
  bool configurePins();  // (re)init: reset, set jog inputs + LED mode, seed input state
  void updateLED();      // built-in feedback: reflect live lever state on the LED (auto mode)

public:
  AW9110JogDial(uint8_t addr = 0x5B, long long_press_millis = 1000);
  bool begin(TwoWire& wire, int8_t int_pin = -1, JogEventHandler handler = NULL);
  void service();          // decode lever -> events (interrupt or poll); drives the LED in auto mode
  bool isPressed() const;      // center currently held
  bool fullThrowHeld() const;  // a full throw (big push, detent 2) currently held
  void prepareForSleep();  // LED off + drain the INT line to idle before the host sleeps

  // RGB LED on OUT0/1/2 (constant-current). setRGB drives all three channels at
  // once; setHue sweeps the colour wheel (hue/value 0..255, full saturation).
  void setRGB(uint8_t r, uint8_t g, uint8_t b);
  void setHue(uint8_t hue, uint8_t value = 255);

  // Breathing. setBreath selects the engine (OFF reverts to steady DIM). For
  // SMART-FADE, call breatheLoop() each loop() so the host can flip the fade
  // direction. For BLINK, blink_mask picks which channels breathe; setBlinkMask
  // changes that mask later without re-arming (handy to step the 6 hues).
  void setBreath(JogBreath mode, uint8_t blink_mask = 0x07);
  void setBlinkMask(uint8_t mask);
  void breatheLoop();
  void enableAutoLED(bool on);   // built-in lever feedback on/off (when app drives the LED)

  static void hsvToRgb(uint8_t h, uint8_t s, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b);
};
