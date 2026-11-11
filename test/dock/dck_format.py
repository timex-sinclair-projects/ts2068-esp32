#!/usr/bin/env python3
"""
ESP2068 -- TS2068 port of ESPectrum

test/dock/dck_format.py -- shared .DCK (TS2068 Command Cartridge) parser/
writer used by dck_inspect.py and make_synthetic_dck.py. Not part of the
firmware build (Python, lives under test/) -- slice 3 (cartridge loader,
see PLAN.md Phase 2) writes the real C++ parser in src/; this is offline
tooling to inspect and generate test fixtures for that loader.

Format, as documented for the Warajevo emulator's .DCK container and
carried forward by later TS2068 emulators (see test/dock/README.md for
sourcing -- this is a well-established, cross-referenced format, not
something invented for this project):

  No file signature/magic number. The file is a sequence of banks, each:

    byte 0:     bank ID
                  0        = DOCK bank
                  1..253   = reserved (expansions with >3 banks)
                  254      = EXROM bank
                  255      = HOME bank
    bytes 1..8: one chunk-type byte per 8K chunk (8 chunks = 64K bank,
                matching the Z80's full address space divided into the
                same 8K granularity the SCLD's port 0xF4 uses):
                  bit D0 (0x01): 0 = read-only chunk,  1 = read/write chunk
                  bit D1 (0x02): 0 = no image in file, 1 = image follows
                so: 0x00 = chunk not present, 0x01 = RAM/zero-filled,
                    0x02 = ROM with image, 0x03 = RAM with initial image.

  Immediately after each 9-byte header, the image data for every chunk
  with D1 set (0x02 or 0x03) follows, in chunk order, 8192 bytes each,
  with no padding between chunks or banks.

  Multiple banks: another 9-byte header follows the last chunk image,
  repeating until end of file.
"""

import struct

CHUNK_SIZE = 8192
HEADER_SIZE = 9

BANK_DOCK = 0
BANK_EXROM = 254
BANK_HOME = 255


def bank_name(bank_id):
    if bank_id == BANK_DOCK:
        return "DOCK"
    if bank_id == BANK_EXROM:
        return "EXROM"
    if bank_id == BANK_HOME:
        return "HOME"
    return f"reserved({bank_id})"


def chunk_type_name(t):
    present = bool(t & 0x02)
    writable = bool(t & 0x01)
    if t == 0x00:
        return "absent"
    return ("RAM" if writable else "ROM") + (" +image" if present else " zero-filled")


class Chunk:
    def __init__(self, index, type_byte, data):
        self.index = index          # 0..7, matches the SCLD's 8K chunk index
        self.type_byte = type_byte
        self.present = bool(type_byte & 0x02)
        self.writable = bool(type_byte & 0x01)
        self.data = data            # bytes, len==CHUNK_SIZE, or None if not present


class Bank:
    def __init__(self, bank_id, chunks):
        self.bank_id = bank_id
        self.chunks = chunks        # list of 8 Chunk


def parse(raw: bytes):
    """Parse a .DCK file's bytes into a list of Bank. Raises ValueError on
    a structurally invalid file (truncated header, truncated chunk data,
    or trailing bytes that don't form a full header)."""
    banks = []
    offset = 0
    n = len(raw)
    while offset < n:
        if offset + HEADER_SIZE > n:
            raise ValueError(
                f"truncated bank header at offset {offset}: "
                f"{n - offset} byte(s) left, need {HEADER_SIZE}"
            )
        bank_id = raw[offset]
        type_bytes = raw[offset + 1: offset + HEADER_SIZE]
        offset += HEADER_SIZE

        chunks = []
        for i, t in enumerate(type_bytes):
            data = None
            if t & 0x02:
                if offset + CHUNK_SIZE > n:
                    raise ValueError(
                        f"truncated chunk {i} image in bank id={bank_id} "
                        f"at offset {offset}: {n - offset} byte(s) left, "
                        f"need {CHUNK_SIZE}"
                    )
                data = raw[offset: offset + CHUNK_SIZE]
                offset += CHUNK_SIZE
            chunks.append(Chunk(i, t, data))

        banks.append(Bank(bank_id, chunks))

    return banks


def write(banks) -> bytes:
    """Serialize a list of Bank back into .DCK bytes. Inverse of parse()."""
    out = bytearray()
    for bank in banks:
        out += struct.pack("B", bank.bank_id)
        out += bytes(c.type_byte for c in bank.chunks)
        for c in bank.chunks:
            if c.present:
                if c.data is None or len(c.data) != CHUNK_SIZE:
                    raise ValueError(
                        f"chunk {c.index} marked present but data is not "
                        f"exactly {CHUNK_SIZE} bytes"
                    )
                out += c.data
    return bytes(out)
