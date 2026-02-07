#!/usr/bin/env python3
import argparse
import sys
import time
import re

import serial

try:
    from pyfiglet import Figlet
    FIGLET = Figlet(font="slant")
except Exception:
    FIGLET = None


MAX_ANGER = 3
ALERT_TIMEOUT_MS = 10000
COOLDOWN_MS = 10000
ANGRY_CALM_MS = 15000
REWARD_MS = 8000

ANSI = {
    "reset": "\033[0m",
    "bold":  "\033[1m",
    "dim":   "\033[2m",

    "red":   "\033[31m",
    "green": "\033[32m",
    "yellow":"\033[33m",
    "cyan":  "\033[36m",
    "mag":   "\033[35m",
    "white": "\033[37m",
}

STATE_STYLE = {
    "IDLE":     ("green",  "Still / waiting"),
    "ALERT":    ("yellow", "Warning"),
    "ANGRY":    ("red",    "Escalating"),
    "COOLDOWN": ("yellow", "Cooling down"),
    "REWARD":   ("cyan",   "Reward"),
    "LOCKED":   ("mag",    "Lockout"),
}

STAT_RE = re.compile(r"^@STAT\s+state=(\w+)\s+anger=(\d+)\s+patienceMs=(\d+)\s*$")

def clear_screen():
    sys.stdout.write("\033[2J\033[H")
    sys.stdout.flush()

def paint(color_name: str, text: str, bold: bool = False, dim: bool = False) -> str:
    c = ANSI.get(color_name, "")
    b = ANSI["bold"] if bold else ""
    d = ANSI["dim"] if dim else ""
    return f"{b}{d}{c}{text}{ANSI['reset']}"

def big_text(s: str) -> str:
    if FIGLET:
        return FIGLET.renderText(s)
    return s + "\n"

def parse_bracket_line(line: str):
    # [STATE] message
    line = line.strip()
    if not (line.startswith("[") and "]" in line):
        return None, None
    st = line[1:line.index("]")].strip()
    msg = line[line.index("]")+1:].strip()
    return st, msg

def format_ms(ms: int) -> str:
    if ms < 0: ms = 0
    sec = ms // 1000
    return f"{sec}s"

def anger_bar(anger: int, max_anger: int = MAX_ANGER, width: int = 12) -> str:
    # Scale filled blocks to width
    if max_anger <= 0:
        return "[" + ("-" * width) + "]"
    filled = int(round((anger / max_anger) * width))
    filled = max(0, min(width, filled))
    return "[" + ("#" * filled) + ("-" * (width - filled)) + "]"

def state_countdown_ms(state: str, elapsed_ms: int, patience_ms: int) -> int | None:
    # Returns remaining ms (or None if no countdown)
    if state == "IDLE":
        return patience_ms - elapsed_ms
    if state == "ALERT":
        return ALERT_TIMEOUT_MS - elapsed_ms
    if state == "ANGRY":
        return ANGRY_CALM_MS - elapsed_ms
    if state == "COOLDOWN":
        return COOLDOWN_MS - elapsed_ms
    if state == "REWARD":
        return REWARD_MS - elapsed_ms
    # LOCKED has no timer
    return None

def main():
    ap = argparse.ArgumentParser(description="Sentinel HUD (big colored text + telemetry).")
    ap.add_argument("--port", default="/dev/ttyACM0", help="e.g. /dev/ttyACM0 or /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--no-figlet", action="store_true", help="Disable big ASCII text")
    ap.add_argument("--show-raw", action="store_true", help="Show last raw line at bottom")
    args = ap.parse_args()

    global FIGLET
    if args.no_figlet:
        FIGLET = None

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except Exception as e:
        print(f"Could not open {args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    # Arduino resets when serial opens
    time.sleep(1.2)
    ser.reset_input_buffer()

    # HUD state
    last_state = "IDLE"
    last_msg = ""
    last_raw = ""
    anger = 0
    patience_ms = 0

    # Track when the current state started (PC-side)
    state_started_at = time.time()

    def redraw():
        nonlocal last_state, last_msg, anger, patience_ms, state_started_at, last_raw

        color, subtitle = STATE_STYLE.get(last_state, ("white", ""))
        elapsed_ms = int((time.time() - state_started_at) * 1000)

        remaining = state_countdown_ms(last_state, elapsed_ms, patience_ms)

        clear_screen()
        print(paint(color, big_text(last_state), bold=True))

        if subtitle:
            print(paint("white", subtitle, dim=True))

        # Main message
        if last_msg:
            print()
            print(paint(color, last_msg, bold=True))
        else:
            print()
            print(paint("white", "(no message)", dim=True))

        # HUD line
        print("\n" + paint("white", "—", dim=True) * 30)

        # Anger meter
        bar = anger_bar(anger, MAX_ANGER)
        anger_line = f"Anger: {bar}  ({anger}/{MAX_ANGER})"
        anger_color = "green" if anger == 0 else ("yellow" if anger < MAX_ANGER else "red")
        print(paint(anger_color, anger_line, bold=True))

        # Patience / pot
        print(paint("cyan", f"Patience knob: {format_ms(patience_ms)} to earn REWARD in IDLE", bold=False))

        # Countdown
        if remaining is None:
            print(paint("white", "Countdown: (none)", dim=True))
        else:
            label = "Reward in" if last_state == "IDLE" else "Next change in"
            print(paint(color, f"{label}: {format_ms(remaining)}", bold=True))

        # raw line
        if args.show_raw and last_raw:
            print("\n" + paint("white", f"raw: {last_raw}", dim=True))

        sys.stdout.flush()

    redraw()

    try:
        while True:
            raw = ser.readline()
            if raw:
                line = raw.decode(errors="ignore").strip()
                if not line:
                    continue
                last_raw = line

                # Telemetry line
                m = STAT_RE.match(line)
                if m:
                    st = m.group(1)
                    anger = int(m.group(2))
                    patience_ms = int(m.group(3))

                    # If state changed, reset timer
                    if st != last_state:
                        last_state = st
                        state_started_at = time.time()
                    redraw()
                    continue

                # Human message line
                st, msg = parse_bracket_line(line)
                if st:
                    # If state changed, reset timer
                    if st != last_state:
                        last_state = st
                        state_started_at = time.time()
                    last_msg = msg
                    redraw()
                    continue

            # also refresh countdown smoothly even without new serial lines
            # (lightweight update ~10 fps)
            time.sleep(0.10)
            redraw()

    except KeyboardInterrupt:
        clear_screen()
        print("bye.")
    finally:
        ser.close()

if __name__ == "__main__":
    main()


