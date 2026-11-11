#!/usr/bin/env python3
"""
ESP2068 -- TS2068 port of ESPectrum

test/dock/test_dck_format.py -- round-trip and error-handling checks for
dck_format.py. Run: python3 test/dock/test_dck_format.py
"""

import sys

from dck_format import (
    parse, write, Chunk, Bank, CHUNK_SIZE, BANK_DOCK, BANK_EXROM, BANK_HOME,
)

failures = 0


def check(cond, what):
    global failures
    status = "PASS" if cond else "FAIL"
    print(f"  {status}  {what}")
    if not cond:
        failures += 1


def make_bank(bank_id, present_chunks):
    """present_chunks: dict {index: (writable, data_byte_fill)}"""
    chunks = []
    for i in range(8):
        if i in present_chunks:
            writable, fill = present_chunks[i]
            type_byte = 0x02 | (0x01 if writable else 0x00)
            data = bytes([fill]) * CHUNK_SIZE
            chunks.append(Chunk(i, type_byte, data))
        else:
            chunks.append(Chunk(i, 0x00, None))
    return Bank(bank_id, chunks)


def main():
    print("dck_format round-trip and error-handling checks")
    print("=================================================")

    # ---- Round trip: single bank, mixed present/absent/RAM/ROM chunks ----
    print("\n-- single-bank round trip --")
    original = [make_bank(BANK_DOCK, {0: (True, 0xAA), 3: (False, 0x55)})]
    raw = write(original)
    check(len(raw) == 9 + CHUNK_SIZE * 2, "serialized size is header + 2 present chunks")
    reparsed = parse(raw)
    check(len(reparsed) == 1, "reparses to exactly one bank")
    check(reparsed[0].bank_id == BANK_DOCK, "bank id round-trips")
    check(reparsed[0].chunks[0].present and reparsed[0].chunks[0].writable,
          "chunk 0 round-trips as present+writable")
    check(reparsed[0].chunks[0].data == bytes([0xAA]) * CHUNK_SIZE,
          "chunk 0 data round-trips byte-for-byte")
    check(reparsed[0].chunks[3].present and not reparsed[0].chunks[3].writable,
          "chunk 3 round-trips as present+read-only")
    check(not reparsed[0].chunks[1].present, "chunk 1 (never set) round-trips as absent")

    # ---- Round trip: multiple banks (DOCK + EXROM + HOME) ----
    print("\n-- multi-bank round trip --")
    original = [
        make_bank(BANK_DOCK, {0: (True, 0x11)}),
        make_bank(BANK_EXROM, {0: (False, 0x22)}),
        make_bank(BANK_HOME, {0: (False, 0x33), 1: (True, 0x44)}),
    ]
    raw = write(original)
    reparsed = parse(raw)
    check(len(reparsed) == 3, "reparses to exactly three banks")
    check([b.bank_id for b in reparsed] == [BANK_DOCK, BANK_EXROM, BANK_HOME],
          "bank order and ids round-trip")
    check(reparsed[2].chunks[1].data == bytes([0x44]) * CHUNK_SIZE,
          "third bank's second chunk data round-trips")

    # ---- Error handling: truncated header ----
    print("\n-- error handling --")
    try:
        parse(bytes([0, 1, 2, 3]))  # only 4 bytes, header needs 9
        check(False, "truncated header raises ValueError")
    except ValueError:
        check(True, "truncated header raises ValueError")

    # ---- Error handling: truncated chunk data ----
    header = bytes([BANK_DOCK, 0x02, 0, 0, 0, 0, 0, 0, 0])  # chunk 0 = ROM+image
    try:
        parse(header + bytes(100))  # far short of a full 8192-byte chunk
        check(False, "truncated chunk data raises ValueError")
    except ValueError:
        check(True, "truncated chunk data raises ValueError")

    # ---- Empty file parses to zero banks (not an error) ----
    check(parse(b"") == [], "empty file parses to an empty bank list")

    print("\n=================================================")
    if failures == 0:
        print("ALL PASSED")
    else:
        print(f"{failures} CHECK(S) FAILED")

    return failures


if __name__ == "__main__":
    sys.exit(main())
