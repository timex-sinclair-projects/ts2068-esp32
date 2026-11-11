#!/usr/bin/env python3
"""Check a TS2068 screen dump (tools/sendkeys.py {SHOT}) against the screen
redrawn independently from the dumped memory.

The dump holds the 2068's screen memory (HOME RAM 4000h-7FFFh), the SCLD
screen mode, and the VGA frame the firmware drew from it. This script
redraws the frame from the memory by FUSE's rules (display_render.c,
display.h, peripherals/scld.c), not by the firmware's code, and compares
the two pixel by pixel. It also writes a PNG of the firmware's frame.

  tools/check_screen.py shot-1.bin [--png shot-1.png]

Exit status 0 if every pixel matches.
"""

import argparse
import re
import sys
import zlib
import struct

W, H = 640, 240
TOP, LEFT = 24, 64          # border lines above, border pixels to the left


def parse(path):
    data = open(path, "rb").read()
    nl = data.index(b"\n")
    head = data[:nl].decode()
    m = re.match(r"#SHOT mode=([0-9a-f]+) hirescol=(\d+) flash=(\d) border=(\d+) colors=([0-9a-f]{32}) w=(\d+) h=(\d+)", head)
    if not m:
        sys.exit(f"bad header: {head}")
    info = {
        "mode": int(m.group(1), 16), "hirescol": int(m.group(2)), "flash": int(m.group(3)),
        "border": int(m.group(4)), "colors": bytes.fromhex(m.group(5)),
        "w": int(m.group(6)), "h": int(m.group(7)),
    }
    pos = nl + 1
    blocks = {}
    for tag in (b"#MEM ", b"#FB "):
        nl = data.index(b"\n", pos)
        n = int(data[pos + len(tag):nl])
        blocks[tag.strip().decode()] = unrle(data[nl + 1:nl + 1 + n])
        pos = nl + 1 + n
    return info, blocks["#MEM"], blocks["#FB"]


def unrle(b):
    out = bytearray()
    for i in range(0, len(b), 2):
        out += bytes([b[i + 1]]) * b[i]
    return bytes(out)


def bitmap_addr(y, x):
    # Spectrum screen layout: third, pixel line in the character, character row.
    return ((y >> 6) << 11) | ((y & 7) << 8) | (((y >> 3) & 7) << 5) | x


def expected_frame(info, mem):
    """Palette index (0-15, 8+ = bright) of every pixel, by FUSE's rules."""
    d1, d2 = mem[:0x2000], mem[0x2000:0x4000]
    mode = info["mode"] & 7
    hires = bool(mode & 4)
    # hires_convert_dec(): the hi-res colour field gives ink, its complement paper.
    hink = info["hirescol"] & 7
    hpaper = hink ^ 7
    border = hpaper if hires else info["border"]

    frame = [[border] * W for _ in range(H)]
    for y in range(192):
        for x in range(32):
            ba = bitmap_addr(y, x)
            aa = 0x1800 + (y >> 3) * 32 + x
            src = d2 if mode & 1 else d1            # display_get_addr: bit 0 adds ALTDFILE_OFFSET
            data = src[ba]
            row = frame[TOP + y]
            px = LEFT + x * 16
            if hires:
                if mode == 4:
                    data2 = d1[aa]
                elif mode == 5:
                    data2 = d2[aa]
                elif mode == 6:
                    data2 = d2[ba]
                else:
                    data2 = data
                word = (data << 8) | data2
                for i in range(16):
                    row[px + i] = hink if word & (0x8000 >> i) else hpaper
            else:
                if mode & 2:
                    attr = d2[ba]                    # display_get_attr_byte: b1 -> line start + ALTDFILE
                else:
                    attr = src[aa]
                ink, paper = attr & 7, (attr >> 3) & 7
                if attr & 0x80 and info["flash"]:
                    ink, paper = paper, ink
                bright = 8 if attr & 0x40 else 0
                for i in range(8):
                    c = (ink if data & (0x80 >> i) else paper) + bright
                    row[px + 2 * i] = row[px + 2 * i + 1] = c
    return frame


def png(path, fb):
    def rgb(v):
        # fabgl R2G2B2: bits 0-1 red, 2-3 green, 4-5 blue.
        return bytes(((v >> s) & 3) * 85 for s in (0, 2, 4))
    raw = b"".join(b"\0" + b"".join(rgb(fb[y * W + x]) for x in range(W)) for y in range(H))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("shot")
    ap.add_argument("--png", help="write the firmware's frame here")
    args = ap.parse_args()

    info, mem, fb = parse(args.shot)
    if len(mem) != 16384 or len(fb) != W * H:
        sys.exit(f"dump sizes wrong: mem {len(mem)}, frame {len(fb)}")
    if args.png:
        png(args.png, fb)

    colors = info["colors"]
    want = expected_frame(info, mem)
    bad = []
    for y in range(H):
        for x in range(W):
            if fb[y * W + x] != colors[want[y][x]]:
                bad.append((x, y))
    mode = info["mode"] & 7
    print(f"{args.shot}: mode {mode}, hi-res colour {info['hirescol']}, border {info['border']}, flash {info['flash']}")
    if bad:
        x, y = bad[0]
        print(f"  MISMATCH: {len(bad)} pixels differ; first at x={x} y={y}: "
              f"frame {fb[y * W + x]:02x}, expected {colors[want[y][x]]:02x} (palette {want[y][x]})")
        sys.exit(1)
    print(f"  all {W * H} pixels match")


if __name__ == "__main__":
    main()
