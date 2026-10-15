"""Read the board's UART log for a fixed time and print a line whenever the
idle-timeout counter crosses a threshold. Used to watch the sleep path live."""
import argparse
import re
import sys
import time

import serial

RULES = [(20, "panel should be DARK now (20s)"), (60, "panel should be DARK now (60s)")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--reboot", action="store_true", help="pulse DTR/RTS first so boot log is caught")
    args = ap.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=0.2)
    if args.reboot:
        ser.dtr = False
        ser.rts = True      # EN low = reset
        time.sleep(0.15)
        ser.rts = False     # release -> boot
        time.sleep(0.2)
    start = time.time()
    buf = b""
    fired = set()
    while time.time() - start < args.seconds:
        buf += ser.read(256)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip()
            print(f"{time.time() - start:6.1f}s {text}", flush=True)
        idle = re.search(rb"idle (\d+)s", buf)
        if idle:
            n = int(idle.group(1))
            for limit, msg in RULES:
                if n >= limit and limit not in fired:
                    fired.add(limit)
                    print(f"{n}s elapsed -> {msg}", flush=True)
    ser.close()


if __name__ == "__main__":
    sys.exit(main())
