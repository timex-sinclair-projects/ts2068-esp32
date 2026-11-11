#!/usr/bin/env python3
"""
ESP2068 -- TS2068 port of ESPectrum

test/dock/dck_inspect.py -- structural inspector for .DCK files. Prints
the bank/chunk layout without needing to know or care what's inside a
chunk's image data; useful for sanity-checking a real cartridge dump
someone drops in for testing, or a synthetic fixture from
make_synthetic_dck.py, without this tool itself containing or requiring
any copyrighted content (see test/dock/README.md).

Usage:
    python3 test/dock/dck_inspect.py path/to/file.dck
"""

import sys
import hashlib

from dck_format import parse, bank_name, chunk_type_name, CHUNK_SIZE


def main(argv):
    if len(argv) != 2:
        print(f"usage: {argv[0]} <file.dck>", file=sys.stderr)
        return 2

    path = argv[1]
    with open(path, "rb") as f:
        raw = f.read()

    try:
        banks = parse(raw)
    except ValueError as e:
        print(f"INVALID: {e}", file=sys.stderr)
        return 1

    print(f"{path}: {len(raw)} bytes, {len(banks)} bank(s)")
    for bi, bank in enumerate(banks):
        print(f"\nbank {bi}: id={bank.bank_id} ({bank_name(bank.bank_id)})")
        for c in bank.chunks:
            addr_lo = c.index * CHUNK_SIZE
            addr_hi = addr_lo + CHUNK_SIZE - 1
            line = f"  chunk {c.index} [0x{addr_lo:04X}-0x{addr_hi:04X}]: {chunk_type_name(c.type_byte)}"
            if c.present:
                digest = hashlib.sha256(c.data).hexdigest()[:12]
                line += f"  sha256={digest}"
            print(line)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
