#!/usr/bin/env python3
"""Type on the TS2068 over the USB serial port (firmware built with
-D TS2068_SERIAL_KEYS; see include/SerialKeys.h).

Keys are 2068 keystrokes, not text: in K mode "p" is PRINT. Braces name
the keys that have no character:

  {ENTER} {BREAK} {DEL} {EXT}          ENTER, CAPS+SPACE, CAPS+0, CAPS+SYMBOL
  {LEFT} {DOWN} {UP} {RIGHT}           CAPS+5 / 6 / 7 / 8
  {SS}x  {CS}x                         SYMBOL SHIFT or CAPS SHIFT with key x
  {WAIT}                               pause one second
  {SHOT}                               save the screen (memory + VGA frame) as
                                       shot-N.bin in --shot-dir, for
                                       tools/check_screen.py

Examples:
  tools/sendkeys.py /dev/cu.wchusbserial10 'p"hello"{ENTER}'
  tools/sendkeys.py /dev/cu.wchusbserial10 --listen 20     # show serial output
  tools/sendkeys.py /dev/cu.wchusbserial10 --interactive   # type live; Ctrl-] quits

Opening the port resets the board (macOS pulses RTS/DTR on open), so each
run starts from a freshly booted 2068: the script waits for the boot to
finish before typing.
"""

import argparse
import re
import sys
import time

import serial

# The firmware takes 6 frames per key at 60 Hz: about 10 keys a second.
SECONDS_PER_KEY = 0.11
# Extra time after ENTER for the ROM to store the line or run the command.
ENTER_PAUSE = 0.6

NAMED = {
    "ENTER": b"\r",
    "BREAK": b"\x03",
    "DEL": b"\x7f",
    "EXT": b"\x05",
    "LEFT": b"\x025",
    "DOWN": b"\x026",
    "UP": b"\x027",
    "RIGHT": b"\x028",
    "SS": b"\x01",
    "CS": b"\x02",
}


def encode(text):
    """Yields (bytes, keys) pieces; WAIT yields (None, 1)."""
    for part in re.split(r"(\{[A-Z]+\})", text):
        if not part:
            continue
        m = re.fullmatch(r"\{([A-Z]+)\}", part)
        if not m:
            yield part.encode("ascii"), len(part)
        elif m.group(1) == "WAIT":
            yield None, 1
        elif m.group(1) == "SHOT":
            yield "SHOT", 0
        elif m.group(1) in NAMED:
            b = NAMED[m.group(1)]
            # SS/CS are prefixes for the next key: no key of their own.
            yield b, 0 if m.group(1) in ("SS", "CS") else 1
        else:
            sys.exit(f"unknown key {part}")


def open_port(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.05
    s.rts = False
    s.dtr = False
    s.open()
    return s


def wait_for_boot(s, settle, timeout=10.0):
    """Shows output until the emulator reports its boot time, then lets the
    ROM reach the copyright screen (settle seconds)."""
    seen = b""
    end = time.time() + timeout
    while time.time() < end:
        data = s.read(4096)
        if data:
            seen += data
            if b"Boot time" in seen:
                drain(s, settle)
                return
    print("(no boot seen; typing anyway)", file=sys.stderr)


def drain(s, seconds):
    end = time.time() + seconds
    while time.time() < end:
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()


def shot(s, path, timeout=60.0):
    """Asks the firmware for a screen dump and saves it, from #SHOT to #END."""
    s.write(b"\x16")
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        buf += s.read(65536)
        start = buf.find(b"#SHOT")
        if start >= 0 and b"#END" in buf[start:] and complete(buf[start:]):
            data = buf[start:buf.index(b"#END", start) + 4]
            with open(path, "wb") as f:
                f.write(data)
            print(f"[saved {path}, {len(data)} bytes]")
            return
    sys.exit("no screen dump came back")


def complete(buf):
    """True once both RLE blocks are in (their sizes are in the headers)."""
    pos = buf.find(b"\n") + 1
    for tag in (b"#MEM ", b"#FB "):
        if not buf.startswith(tag, pos):
            return False
        nl = buf.find(b"\n", pos)
        if nl < 0:
            return False
        pos = nl + 1 + int(buf[pos + len(tag):nl])
        if pos > len(buf):
            return False
    return buf.startswith(b"#END", pos)


def send(s, text, per_key=SECONDS_PER_KEY, shot_dir=".", shots=None):
    shots = shots if shots is not None else [0]
    for data, keys in encode(text):
        if data is None:
            drain(s, 1.0)
            continue
        if data == "SHOT":
            shots[0] += 1
            shot(s, f"{shot_dir}/shot-{shots[0]}.bin")
            continue
        s.write(data)
        drain(s, keys * per_key)
        if data.endswith(b"\r"):
            # The ROM ignores keys while it stores a line or runs a
            # command, and holds only one pending key: give it time.
            drain(s, ENTER_PAUSE)


def interactive(s):
    import termios
    import tty
    import select

    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    print("Typing to the 2068. Ctrl-] quits.\r")
    try:
        tty.setraw(fd)
        while True:
            r, _, _ = select.select([fd, s], [], [], 0.05)
            if fd in r:
                ch = sys.stdin.buffer.read1(64) if hasattr(sys.stdin.buffer, "read1") else sys.stdin.buffer.read(1)
                if b"\x1d" in ch:
                    break
                # Arrow keys arrive as ESC [ A..D.
                ch = (ch.replace(b"\x1b[D", NAMED["LEFT"]).replace(b"\x1b[B", NAMED["DOWN"])
                        .replace(b"\x1b[A", NAMED["UP"]).replace(b"\x1b[C", NAMED["RIGHT"]))
                s.write(ch)
            if s in r:
                data = s.read(4096)
                sys.stdout.write(data.decode("utf-8", "replace").replace("\n", "\r\n"))
                sys.stdout.flush()
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("keys", nargs="?", default="", help="keys to type")
    ap.add_argument("--listen", type=float, default=1.0, help="seconds to show serial output afterwards")
    ap.add_argument("--interactive", action="store_true", help="type live from this terminal")
    ap.add_argument("--boot-wait", type=float, default=1.5,
                    help="seconds after boot before typing (the TS-Pico ROM needs longer)")
    ap.add_argument("--key-time", type=float, default=SECONDS_PER_KEY, help="seconds per key")
    ap.add_argument("--shot-dir", default=".", help="where {SHOT} saves screen dumps")
    args = ap.parse_args()

    s = open_port(args.port)
    wait_for_boot(s, args.boot_wait)
    if args.interactive:
        interactive(s)
    else:
        send(s, args.keys, args.key_time, args.shot_dir)
        drain(s, args.listen)


if __name__ == "__main__":
    main()
