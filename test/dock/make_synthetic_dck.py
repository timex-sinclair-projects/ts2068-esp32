#!/usr/bin/env python3
"""
ESP2068 -- TS2068 port of ESPectrum

test/dock/make_synthetic_dck.py -- generates a small, purely synthetic
.DCK fixture for testing the container-format parser (slice 3, not yet
written). This is NOT a real cartridge dump and does not contain, derive
from, or attempt to resemble any real TS2068 software or ROM -- every
byte is a generated, clearly-synthetic pattern. See test/dock/README.md
for why real cartridge/ROM dumps are deliberately not checked into this
repo, and why this fixture only proves the container format parses
correctly, not that anything in it would run.

Produces test/dock/fixtures/synthetic.dck: one DOCK bank with
  chunk 0: RAM, initial content = a repeating marker string
  chunk 1: ROM, initial content = an incrementing byte ramp
  chunks 2-7: absent
"""

import os

from dck_format import Chunk, Bank, write, CHUNK_SIZE, BANK_DOCK


def marker_chunk():
    text = b"ESP2068 SYNTHETIC TEST FIXTURE - NOT REAL SOFTWARE - "
    data = (text * (CHUNK_SIZE // len(text) + 1))[:CHUNK_SIZE]
    return data


def ramp_chunk():
    return bytes(i & 0xFF for i in range(CHUNK_SIZE))


def main():
    chunks = []
    chunks.append(Chunk(0, 0x03, marker_chunk()))   # RAM, image present
    chunks.append(Chunk(1, 0x02, ramp_chunk()))      # ROM, image present
    for i in range(2, 8):
        chunks.append(Chunk(i, 0x00, None))          # absent

    bank = Bank(BANK_DOCK, chunks)
    raw = write([bank])

    out_dir = os.path.join(os.path.dirname(__file__), "fixtures")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "synthetic.dck")
    with open(out_path, "wb") as f:
        f.write(raw)

    print(f"wrote {out_path} ({len(raw)} bytes)")


if __name__ == "__main__":
    main()
