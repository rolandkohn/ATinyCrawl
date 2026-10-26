/*
  ATinyCrawl - RC speed limiter, ATtiny84A @ 8 MHz internal
  ------------------------------------------------
  Throttle channel in -> PA0 (pin 13)
  AUX channel in      -> PA1 (pin 12)   knob/dial (stepless) or 2/3-position switch
  ESC signal out      <- PA5 / OC1B (pin 8)
  Heartbeat LED       <- PA3 (pin 10), optional, via 470R..1k to GND, blinks 2x/s

  Forward throttle is scaled by the AUX channel (LIMIT_MIN_PCT..100%).
  Reverse is limited too, but only LIMIT_REVERSE_STRENGTH % as strongly.

  Timer1: prescaler 8 -> 1 tick = 1 us, Fast PWM mode 14, TOP = 19999 -> 50 Hz
  The same timer timestamps the input pulses, so clock tolerance of the
  internal oscillator cancels out (measured N ticks -> output N ticks).

  Board: ATTinyCore, "ATtiny24/44/84(a) (No bootloader)", 8 MHz internal,
         millis()/micros() disabled (we kill Timer0 IRQ anyway).
*/

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/atomic.h>

#if F_CPU != 8000000L
#error "Set clock to 8 MHz internal and run 'Burn Bootloader' once to set fuses."
#endif

// ================= CONFIG =================
const bool     FORWARD_IS_HIGH     = true;   // forward = pulse ABOVE neutral? flip if limiter acts on reverse
const bool     LIMIT_CH_REVERSED   = false;  // flip if the AUX channel works the wrong way round
const uint8_t  LIMIT_MIN_PCT       = 20;    // limit at AUX minimum (0 = car can't move forward at all)
const uint8_t  LIMIT_MAX_PCT       = 100;    // limit at AUX maximum
const uint8_t  LIMIT_REVERSE_STRENGTH = 50; // how strongly AUX limits REVERSE, in % of the forward effect:
                                             // 0 = reverse never limited, 100 = same limit as forward,
                                             // 50 = half the effect (forward 20 % -> reverse 60 %)
const uint8_t  LIMIT_FAILSAFE_PCT  = 30;     // used if the AUX signal is lost

const uint16_t LIMIT_IN_LOW        = 1050;   // AUX pulse at min (us), slightly inside 1000
const uint16_t LIMIT_IN_HIGH       = 1950;   // AUX pulse at max (us), slightly inside 2000
                                             // -> end values are reached even if the radio falls a bit short

const uint16_t NEUTRAL_DEFAULT     = 1500;   // placeholder, replaced by calibration before first use
const uint16_t NEUTRAL_CAL_MIN     = 1400;   // plausible window for auto-calibrated neutral
const uint16_t NEUTRAL_CAL_MAX     = 1600;

const uint16_t PULSE_VALID_MIN     = 800;    // anything outside = glitch, ignored
const uint16_t PULSE_VALID_MAX     = 2200;
const uint16_t OUT_MIN             = 900;    // hard clamp of output
const uint16_t OUT_MAX             = 2100;

const bool     HEARTBEAT           = true;   // LED on pin 10. false = pin 10 stays an input (safe if bridged)

const uint8_t  TIMEOUT_FRAMES      = 5;      // 5 x 20 ms = 100 ms without signal -> failsafe
const uint16_t FRAME_US            = 20000;  // 50 Hz output
// ==========================================

volatile uint16_t thrRise, limRise;
volatile uint16_t thrPulse, limPulse;
volatile bool     thrNew, limNew;
volatile uint8_t  lastPA;
volatile uint8_t  frames;                    // increments every 20 ms (Timer1 overflow)

static inline uint16_t elapsed(uint16_t now, uint16_t start) {
  // TCNT1 counts 0..FRAME_US-1 and wraps
  return (now >= start) ? (now - start) : (uint16_t)(now + FRAME_US - start);
}

ISR(PCINT0_vect) {
  uint16_t now = TCNT1;
  uint8_t pa = PINA;
  uint8_t changed = pa ^ lastPA;
  lastPA = pa;

  if (changed & _BV(PA0)) {
    if (pa & _BV(PA0)) thrRise = now;
    else { thrPulse = elapsed(now, thrRise); thrNew = true; }
  }
  if (changed & _BV(PA1)) {
    if (pa & _BV(PA1)) limRise = now;
    else { limPulse = elapsed(now, limRise); limNew = true; }
  }
}

ISR(TIM1_OVF_vect) {
  frames++;
}

static void setOutput(uint16_t us) {
  if (us < OUT_MIN) us = OUT_MIN;
  if (us > OUT_MAX) us = OUT_MAX;
  // OCR1B is a 16-bit register sharing TEMP with TCNT1 reads in the ISR -> must be atomic.
  // Fast PWM: OCR1B is double-buffered, takes effect at next TOP (glitch-free).
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { OCR1B = us - 1; }
}

// The ESC gets pulses ONLY while the receiver delivers a throttle signal.
// No signal -> line stays LOW, exactly like a receiver without link.
// So the ESC sees the same thing as if it were plugged directly into the receiver,
// in any power-up order. On signal loss the ESC runs its own failsafe (motor off).
static bool outOn = false;

static void outputEnable(bool on) {
  if (on == outOn) return;
  if (on) {
    // connect OC1B right after a timer overflow, so the first pulse is complete
    uint8_t f0;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { f0 = frames; }
    for (;;) { uint8_t f1; ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { f1 = frames; } if (f1 != f0) break; }
    TCCR1A |= _BV(COM1B1);
  } else {
    TCCR1A &= ~_BV(COM1B1);                    // pin falls back to PORTA bit = LOW
  }
  outOn = on;
}

static bool pulseValid(uint16_t p) {
  return p >= PULSE_VALID_MIN && p <= PULSE_VALID_MAX;
}

static uint16_t neutral = NEUTRAL_DEFAULT;
static bool     calibrated = false;

// Neutral calibration runs inside loop(), so it does not depend on power-up order.
// Until it is done, the ESC gets NO pulses at all (like a receiver that is not yet
// bound). Its first pulse is therefore already the correctly limited signal.
// Neutral is taken from CAL_SAMPLES consecutive pulses that stay within CAL_SPREAD us
// of each other (trigger at rest) and inside the plausible window. Otherwise: retry.
const uint8_t  CAL_SAMPLES = 10;             // 10 pulses = 0.2 s at 50 Hz
const uint8_t  CAL_SPREAD  = 12;             // max. min/max spread in us while sampling

static void calibrateStep(uint16_t t) {
  static uint32_t sum = 0;
  static uint16_t lo = 0xFFFF, hi = 0;
  static uint8_t  n = 0;
  if (t < lo) lo = t;
  if (t > hi) hi = t;
  sum += t; n++;
  if (hi - lo > CAL_SPREAD || t < NEUTRAL_CAL_MIN || t > NEUTRAL_CAL_MAX) {
    sum = 0; n = 0; lo = 0xFFFF; hi = 0;      // trigger moving or implausible -> start over
    return;
  }
  if (n >= CAL_SAMPLES) {
    neutral = sum / n;
    calibrated = true;
  }
}

void setup() {
  cli();
  TIMSK0 = 0;                                  // kill Timer0 IRQ (millis) -> less input jitter

  PORTA &= ~_BV(PA5);                          // ESC line LOW = no pulses ...
  DDRA  |=  _BV(PA5);                          // ... until a throttle signal arrives (OC1B, pin 8)
  if (HEARTBEAT) DDRA |= _BV(PA3);             // heartbeat LED (pin 10)
  DDRA  &= ~(_BV(PA0) | _BV(PA1));             // inputs
  PORTA &= ~(_BV(PA0) | _BV(PA1));             // no pull-ups (external pull-downs optional)

  // Timer1: Fast PWM mode 14 (TOP = ICR1), clk/8 -> 1 us. OC1B stays DISCONNECTED here;
  // it is connected only while a valid throttle signal is present (see outputEnable).
  TCCR1A = _BV(WGM11);
  TCCR1B = _BV(WGM13) | _BV(WGM12) | _BV(CS11);
  ICR1   = FRAME_US - 1;
  OCR1B  = NEUTRAL_DEFAULT - 1;
  TIMSK1 = _BV(TOIE1);

  // Pin change interrupts on PA0, PA1
  PCMSK0 = _BV(PCINT0) | _BV(PCINT1);
  GIMSK |= _BV(PCIE0);
  lastPA = PINA;
  sei();
}

void loop() {
  static uint16_t thr = 0, lim = 0;
  static uint8_t  thrSeen = 0, limSeen = 0;
  static bool     thrEver = false, limEver = false;

  uint8_t f; bool tn, ln; uint16_t t, l;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    f  = frames;
    tn = thrNew; thrNew = false; t = thrPulse;
    ln = limNew; limNew = false; l = limPulse;
  }

  // --- heartbeat: 2 Hz, 240 ms on / 260 ms off (one frame = 20 ms) ---
  static uint8_t lastF = 0, beat = 0;
  beat += (uint8_t)(f - lastF);                 // frames since last loop
  lastF = f;
  while (beat >= 25) beat -= 25;                // 25 frames = 500 ms
  if (HEARTBEAT) { if (beat < 12) PORTA |= _BV(PA3); else PORTA &= ~_BV(PA3); }

  if (tn && pulseValid(t)) {
    thr = t; thrSeen = f; thrEver = true;
    if (!calibrated) calibrateStep(t);        // one sample per received pulse
  }
  if (ln && pulseValid(l)) { lim = l; limSeen = f; limEver = true; }

  bool thrOk = thrEver && (uint8_t)(f - thrSeen) < TIMEOUT_FRAMES;
  bool limOk = limEver && (uint8_t)(f - limSeen) < TIMEOUT_FRAMES;
  // Forget stale values once timed out. Otherwise the 8-bit frame counter wraps
  // after 256 frames (5.12 s) and an old pulse would look "fresh" again briefly.
  // A new valid pulse sets the flag again -> automatic recovery.
  if (!thrOk) thrEver = false;
  if (!limOk) limEver = false;

  // --- limit percentage from AUX channel ---
  uint8_t pct;
  if (limOk) {
    uint16_t span = LIMIT_IN_HIGH - LIMIT_IN_LOW;
    uint16_t c = lim;
    if (c < LIMIT_IN_LOW)  c = LIMIT_IN_LOW;
    if (c > LIMIT_IN_HIGH) c = LIMIT_IN_HIGH;
    uint32_t frac = c - LIMIT_IN_LOW;                       // 0..span
    if (LIMIT_CH_REVERSED) frac = span - frac;
    pct = LIMIT_MIN_PCT + (uint8_t)((frac * (LIMIT_MAX_PCT - LIMIT_MIN_PCT)) / span);
  } else {
    pct = LIMIT_FAILSAFE_PCT;
  }

  // --- output ---
  uint16_t out;
  if (!thrOk) {
    outputEnable(false);                                    // no throttle signal -> no pulses
    return;
  } else if (!calibrated) {
    outputEnable(false);                                    // not calibrated yet -> no pulses
    return;
  } else {
    int16_t d = (int16_t)thr - (int16_t)neutral;
    bool forward = FORWARD_IS_HIGH ? (d > 0) : (d < 0);
    if (forward) {
      d = (int16_t)(((int32_t)d * pct) / 100);
    } else if (d != 0) {
      uint8_t rpct = 100 - (uint8_t)(((uint16_t)(100 - pct) * LIMIT_REVERSE_STRENGTH) / 100);
      d = (int16_t)(((int32_t)d * rpct) / 100);
    }
    out = (uint16_t)((int16_t)neutral + d);
  }
  setOutput(out);
  outputEnable(true);
}
