#include <Arduino.h>

/*
  SENTINEL 
  ------------------------------------------
  - Wake easy / Rage harder tilt behavior
  - Pot controls patience (IDLE -> REWARD delay)
  - ANGRY: blinking red + rapid beeps
  - LOCKED: solid red + continuous tone until unlocked


  Serial output formats:
    [STATE] message
    @STAT state=STATE anger=N patienceMs=MS
*/

const bool TILT_INVERTED = true;

// ================= PINS =========================
const uint8_t PIN_BUTTON = 13;
const uint8_t PIN_TILT   = 12;
const uint8_t PIN_PIEZO  = 6;
const uint8_t PIN_LED_R  = 2;
const uint8_t PIN_LED_Y  = 4;
const uint8_t PIN_LED_G  = 3;
const uint8_t PIN_POT    = A0;

// ================= STATES =======================
enum State : uint8_t { IDLE, ALERT, ANGRY, COOLDOWN, REWARD, LOCKED };
State state = IDLE;
uint8_t anger = 0;

// ================= TIMING =======================
const uint8_t  MAX_ANGER          = 3;
const uint16_t MIN_STATE_MS       = 200;

const uint16_t ALERT_TIMEOUT_MS   = 10000;
const uint16_t COOLDOWN_MS        = 10000;
const uint16_t ANGRY_CALM_MS      = 15000;
const uint16_t REWARD_MS          = 8000;

const uint16_t IDLE_REWARD_MIN_MS = 5000;
const uint16_t IDLE_REWARD_MAX_MS = 20000;

// Tilt sensitivity
const uint16_t TILT_DB_IDLE_MS    = 5;
const uint16_t TILT_DB_OTHER_MS   = 14;
const uint16_t TILT_REARM_MS      = 140;

// ANGRY effects
const uint16_t ANGRY_BLINK_MS     = 200;
const uint16_t ANGRY_BEEP_MS      = 120;
const uint16_t ANGRY_BEEP_DUR_MS  = 35;
const uint16_t ANGRY_BEEP_HZ      = 220;

// LOCKED effects
const uint16_t LOCKED_TONE_HZ     = 140;

// ================= TIMEKEEPING ==================
unsigned long enteredAt = 0;
unsigned long nextBlinkAt = 0;
unsigned long nextBeepAt  = 0;
bool angryRedOn = true;

// ================= UTIL =========================
static inline bool elapsed(unsigned long t0, unsigned long ms) {
  return (millis() - t0) >= ms;
}

static inline const char* sName(State s) {
  switch (s) {
    case IDLE: return "IDLE";
    case ALERT: return "ALERT";
    case ANGRY: return "ANGRY";
    case COOLDOWN: return "COOLDOWN";
    case REWARD: return "REWARD";
    default: return "LOCKED";
  }
}

static inline void leds(bool r, bool y, bool g) {
  digitalWrite(PIN_LED_R, r);
  digitalWrite(PIN_LED_Y, y);
  digitalWrite(PIN_LED_G, g);
}

static inline void showBaseLEDs() {
  switch (state) {
    case IDLE:     leds(false, false, true);  break;
    case ALERT:    leds(false, true,  false); break;
    case COOLDOWN: leds(false, true,  false); break;
    case REWARD:   leds(false, true,  true);  break;
    case ANGRY:    leds(true,  false, false); break;
    case LOCKED:   leds(true,  false, false); break;
  }
}

static inline void stopSound() { noTone(PIN_PIEZO); }

// ================= SERIAL OUTPUT =================
static inline void printStateMsg(const char* msg) {
  Serial.print('['); Serial.print(sName(state)); Serial.print("] ");
  Serial.println(msg);
}

static inline void printStatLine(unsigned long patienceMs) {
  Serial.print("@STAT state=");
  Serial.print(sName(state));
  Serial.print(" anger=");
  Serial.print(anger);
  Serial.print(" patienceMs=");
  Serial.println(patienceMs);
}

// ================= RNG ==========================
uint32_t rngState = 0xA5A5A5A5;
static inline uint32_t rng() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  return rngState;
}

static inline const char* pick3(const char* a, const char* b, const char* c) {
  switch (rng() % 3) { case 0: return a; case 1: return b; default: return c; }
}

static inline const char* pick4(const char* a, const char* b,
                                const char* c, const char* d) {
  switch (rng() % 4) { case 0: return a; case 1: return b; case 2: return c; default: return d; }
}

static inline const char* compliment() {
  return pick4(
    "Nice work — your patience is strong.",
    "You did great. Thanks for staying calm.",
    "That was impressive self-control.",
    "You're doing awesome. Keep it up."
  );
}

// ================= TILT EDGE ====================
struct TiltEdge {
  bool lastRawAct = false;
  unsigned long changedAt = 0;
  bool armed = true;
  unsigned long inactiveSince = 0;

  bool rawActive() const {
    bool raw = digitalRead(PIN_TILT);
    bool active = (raw == LOW);
    return TILT_INVERTED ? !active : active;
  }

  void reset() {
    armed = false;
    inactiveSince = 0;
    lastRawAct = rawActive();
    changedAt = millis();
  }

  bool event(State currentState) {
    bool rawAct = rawActive();
    if (rawAct != lastRawAct) {
      lastRawAct = rawAct;
      changedAt = millis();
    }

    const uint16_t needDb = (currentState == IDLE)
                              ? TILT_DB_IDLE_MS
                              : TILT_DB_OTHER_MS;
    if (!elapsed(changedAt, needDb)) return false;

    if (!rawAct) {
      if (inactiveSince == 0) inactiveSince = millis();
      if (!armed && elapsed(inactiveSince, TILT_REARM_MS)) armed = true;
    } else {
      inactiveSince = 0;
    }

    if (armed && rawAct) {
      armed = false;
      return true;
    }
    return false;
  }
};

TiltEdge tilt;

// ================= BUTTON =======================
bool buttonPressEvent() {
  static bool stable = HIGH, lastRaw = HIGH;
  static unsigned long changedAt = 0;

  bool raw = digitalRead(PIN_BUTTON);
  if (raw != lastRaw) {
    lastRaw = raw;
    changedAt = millis();
  }

  if (elapsed(changedAt, 25) && raw != stable) {
    stable = raw;
    return (stable == LOW);
  }
  return false;
}

static inline bool buttonHeld() {
  return digitalRead(PIN_BUTTON) == LOW;
}

// ================= STATE ENTRY ==================
static inline void enter(State next,
                         const char* msg,
                         int beepHz = 0,
                         int beepDur = 70,
                         unsigned long patienceMs = 0) {
  if (next != LOCKED) stopSound();

  state = next;
  enteredAt = millis();
  tilt.reset();

  angryRedOn = true;
  nextBlinkAt = millis();
  nextBeepAt  = millis();

  showBaseLEDs();

  if (next == LOCKED) {
    leds(true, false, false);
    tone(PIN_PIEZO, LOCKED_TONE_HZ);
  } else if (beepHz > 0) {
    tone(PIN_PIEZO, beepHz, beepDur);
  }

  printStateMsg(msg);
  printStatLine(patienceMs);
}

// ================= EFFECTS ======================
static inline void angryEffectsTick() {
  if (millis() >= nextBlinkAt) {
    nextBlinkAt = millis() + ANGRY_BLINK_MS;
    angryRedOn = !angryRedOn;
    digitalWrite(PIN_LED_R, angryRedOn);
  }
  if (millis() >= nextBeepAt) {
    nextBeepAt = millis() + ANGRY_BEEP_MS;
    tone(PIN_PIEZO, ANGRY_BEEP_HZ, ANGRY_BEEP_DUR_MS);
  }
}

// ================= SETUP ========================
void setup() {
  Serial.begin(9600);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_TILT,   INPUT_PULLUP);

  pinMode(PIN_PIEZO, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_Y, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);

  leds(false, false, false);

  rngState ^= (uint32_t)analogRead(PIN_POT) << 16;
  rngState ^= (uint32_t)analogRead(PIN_POT);

  anger = 0;
  enter(IDLE, "stillness acknowledged.", 880, 70, 0);
}

// ================= LOOP =========================
void loop() {
  if (!elapsed(enteredAt, MIN_STATE_MS)) return;

  const unsigned long patienceMs =
    map(analogRead(PIN_POT), 0, 1023,
        IDLE_REWARD_MIN_MS, IDLE_REWARD_MAX_MS);

  const bool t = tilt.event(state);
  const bool p = buttonPressEvent();

  if (state == ANGRY) angryEffectsTick();
  if (state == LOCKED) leds(true, false, false);

  switch (state) {
    case IDLE:
      if (t) {
        enter(ALERT, pick3("careful.", "i felt that.", "easy."),
              988, 60, patienceMs);
        break;
      }
      if (elapsed(enteredAt, patienceMs)) {
        enter(REWARD, compliment(), 1047, 60, patienceMs);
      }
      break;

    case ALERT:
      if (t) {
        if (anger < MAX_ANGER) anger++;
        enter(ANGRY, pick3("wrong move.", "again? unfortunate.", "you insist."),
              220, 90, patienceMs);
        break;
      }
      if (p) {
        enter(IDLE, pick3("noted.", "acknowledged.", "good."),
              880, 50, patienceMs);
        break;
      }
      if (elapsed(enteredAt, ALERT_TIMEOUT_MS)) {
        enter(IDLE, pick3("stand down.", "resetting.", "idle."),
              880, 50, patienceMs);
      }
      break;

    case ANGRY:
      if (t) {
        if (anger < MAX_ANGER) anger++;
        printStateMsg(pick3("stop.", "enough.", "do not."));
      }
      if (anger >= MAX_ANGER && elapsed(enteredAt, 1200)) {
        enter(LOCKED, pick3("access revoked.", "you are done.", "locked."),
              0, 0, patienceMs);
        break;
      }
      if (elapsed(enteredAt, ANGRY_CALM_MS)) {
        if (anger) anger--;
        enter(COOLDOWN, pick3("cool down.", "steady.", "watch yourself."),
              660, 70, patienceMs);
      }
      break;

    case COOLDOWN:
      if (t) {
        if (anger < MAX_ANGER) anger++;
        enter(ANGRY, pick3("no.", "not again.", "wrong."),
              220, 90, patienceMs);
        break;
      }
      if (elapsed(enteredAt, COOLDOWN_MS)) {
        enter(IDLE, pick3("returning.", "idle.", "reset."),
              880, 50, patienceMs);
      }
      break;

    case REWARD:
      if (t) {
        enter(ALERT, pick3("easy.", "careful.", "i noticed."),
              988, 60, patienceMs);
        break;
      }
      if (elapsed(enteredAt, REWARD_MS)) {
        enter(IDLE, pick3("cycle complete.", "done.", "idle."),
              880, 50, patienceMs);
      }
      break;

    case LOCKED: {
      static unsigned long holdAt = 0;
      if (buttonHeld()) {
        if (!holdAt) holdAt = millis();
        if (elapsed(holdAt, 2000)) {
          holdAt = 0;
          anger = 0;
          enter(IDLE, pick3("mercy granted.", "one more chance.", "reset."),
                988, 70, patienceMs);
        }
      } else {
        holdAt = 0;
      }
      break;
    }
  }
}
