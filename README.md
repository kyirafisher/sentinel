# sentinel
Sentinel — Interactive Embedded State Machine + Live PC HUD

Sentinel is a small Arduino-based device that reacts to physical disturbance with escalating feedback. It models behavior using a finite state machine (IDLE → ALERT → ANGRY → COOLDOWN → IDLE, with REWARD and LOCKED branches). To make the system reliable with noisy real-world sensors, tilt input is handled as an edge-based event with re-arming and state-dependent filtering.

The device streams both human-readable messages and machine-readable telemetry to a Python HUD, which displays large colored state text and live stats in real time.

Demo behaviors:

-Tilt once → ALERT (warning)

-Tilt again → ANGRY (blinking red + rapid beeps)

-Stay calm for a fixed time → COOLDOWN → IDLE

-Stay idle long enough (pot-controlled patience) → REWARD (compliment)

-Escalate too far → LOCKED (continuous alarm until a deliberate reset)

Hardware:

-Arduino Uno (or compatible)

-Tilt switch

-Pushbutton

-Piezo buzzer

-LEDs

-Resistors

-Trimmer potentiometer

-Breadboard + jumpers


Software & engineering highlights:

-Finite state machine architecture

-Edge-triggered tilt detection with re-arming to prevent “stuck” triggers

-State-dependent tilt sensitivity (“wake easy / rage harder”)

-Non-blocking timers with millis() (no delay-based state logic)

-Separation of embedded logic and UI rendering via serial telemetry


Dual-format serial output:

[STATE] message (human readable)

@STAT state=... anger=... patienceMs=... (machine readable for UI)

How to run:

-Upload sentinel.ino to the Arduino

-Install Python requirements:

  -pip install --user pyserial pyfiglet
  
Run the HUD:

-python3 sentinel_hud.py --port /dev/ttyACM0
