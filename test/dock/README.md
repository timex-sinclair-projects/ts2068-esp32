# DOCK image corpus + test harness (slice 4)

Tooling and test fixtures for the `.DCK` cartridge container format, per
[PLAN.md](../../PLAN.md)'s Phase 2 slice 4 and
[TS2068-ESPECTRUM-PORT-PLAN.md](../../TS2068-ESPECTRUM-PORT-PLAN.md)'s
"Cartridge loading" section. This is offline Python tooling (inspecting
and generating `.dck` files on a dev machine), not firmware code — the
real C++ `.DCK` parser that runs on-device is slice 3's job and hasn't
been written yet. This exists to unblock slice 3's testing once it starts,
and to nail down the container format ahead of time so slice 3 doesn't
have to re-derive it.

## The format

No file signature/magic number. A sequence of banks, each a 9-byte header
followed by whichever chunk images the header says are present:

```
byte 0:      bank ID       0=DOCK, 254=EXROM, 255=HOME, 1..253=reserved
bytes 1..8:  one type byte per 8K chunk (8 chunks = 64K, matching the
             SCLD's port 0xF4 chunk granularity):
               bit D0 (0x01): 0=read-only, 1=read/write
               bit D1 (0x02): 0=no image follows, 1=image follows (8192 bytes)
             so: 0x00=absent, 0x01=RAM/zero-filled, 0x02=ROM+image, 0x03=RAM+image
```

Chunk images (8192 bytes each, no padding) follow the header in chunk
order, only for chunks with D1 set. Another 9-byte header can follow the
last chunk image for a second bank, and so on to end of file.

**Source:** this is the format documented for the Warajevo emulator's
`.DCK` container and carried forward by later TS2068 emulators — see the
"File Formats" reference at
[hal.varese.it/filesmuseo/sinclair/spectrumfaq/fileform.html](https://www.hal.varese.it/filesmuseo/sinclair/spectrumfaq/fileform.html).
Corroborated against a second independent copy of the same reference
during research; not yet checked against a hex dump of a real, known-good
`.dck` file (nobody on this project has fed one through `dck_inspect.py`
yet) — do that the first time a real cartridge dump is available, and fix
`dck_format.py`/this doc together if anything doesn't match.

## Tools

- **`dck_format.py`** — parser/writer library (`parse(bytes) -> [Bank]`,
  `write([Bank]) -> bytes`). Raises `ValueError` on structurally invalid
  files (truncated header, truncated chunk data) rather than silently
  misreading them.
- **`dck_inspect.py <file.dck>`** — prints a file's bank/chunk structure
  and a short hash per present chunk, without interpreting chunk contents
  as anything in particular. Useful for sanity-checking *any* `.dck` —
  real or synthetic — someone drops in locally.
- **`make_synthetic_dck.py`** — generates `fixtures/synthetic.dck`: one
  DOCK bank, a RAM chunk and a ROM chunk with clearly-synthetic filler
  content (a marker string and a byte ramp — not real code, not derived
  from anything real), the rest of the chunks absent. Proves the
  *container format* parses correctly; proves nothing about running real
  software, since there isn't any in it.
- **`test_dck_format.py`** — round-trip and error-handling checks for
  `dck_format.py` itself. Run: `python3 test/dock/test_dck_format.py`.

Regenerate the fixture (deterministic, same bytes every time) with:

```
python3 test/dock/make_synthetic_dck.py
python3 test/dock/dck_inspect.py test/dock/fixtures/synthetic.dck
```

## Provenance policy — read before adding anything to `fixtures/`

This repo is **public**. Real `.DCK` cartridge dumps and third-party ROM
images are very likely still copyrighted by whoever actually authored
them. Nothing in this directory is a real dump, and nothing here was
fetched from the internet — `synthetic.dck` is generated from a
byte-pattern string, not extracted from any real cartridge or ROM.

**Exception: Timex's own stock TS2068 ROMs are public domain, confirmed
by direct citation, not just assumed.** Louis M. Galie (Senior Vice
President, Timex Group USA Inc. — the modern corporate successor holding
whatever rights descend from Timex Sinclair/Timex Computer Corporation)
stated, in writing, when asked by TS2068 archivist Bruno Florindo for
permission to scan and preserve Timex USA material:

> Any form of documentation associated with Timex Sinclair computers
> which is marked as "copyright Timex" or "copyright Timex Computer
> Company" can now be considered "public domain". Timex Sinclair
> documentation distributed by Timex Corporation or Timex Computer
> Company in any form which is not marked as copyrighted or protected is
> considered by Timex to also be "public domain". [...] In simple terms,
> if it came from Timex or Timex Computer company and it is not
> otherwise marked as belonging to someone else, you can disseminate it
> freely.

(Forwarded to the project owner 2023-11-06; on file locally, not
committed to this repo since the original correspondence includes
personal email addresses.) This covers the genuine stock `2068Home.BIN`
(16K HOME ROM) and `2068Exrom.BIN` (8K EXROM) — confirmed as real Timex
releases via the documented `$FF` version byte at HOME ROM offset
`$0013` — and this project's own patched derivatives of them (see
`PLAN.md`'s Phase 4, the virtual disk feature). Whatever portions of the
ROM trace back to Sinclair-licensed ZX Spectrum code are additionally
covered by Amstrad's long-standing public permission for emulator
authors to include their copyrighted ROM code, the same permission every
ZX-Spectrum-compatible emulator already relies on.

**This exception does NOT extend to anything Timex didn't itself author
or distribute** — Galie's own statement excludes "materials for which we
do not have ownership." Third-party content in the local reference
library (TS-Pico's `gus-home.rom`/`gus-exrom.rom`, by Gustavo Pane; real
cartridge dumps like `Zebra OS-64.BIN`; community tools like
`eToolkit.ROM`) stays under the original conservative policy below —
confirm redistribution rights before committing any of it here.

Before adding a real image to this repo (fixtures/ or anywhere else),
**for anything other than Timex's own stock TS2068 ROMs**:

- Confirm you actually hold the rights to redistribute it — dumps you
  made yourself from hardware you own are the clearest case; a file
  someone handed you on a forum is not.
- Prefer homebrew or public-domain test software for anything committed
  publicly. Real commercial cartridges are fine to use **privately** for
  bring-up/testing on your own machine — just don't commit them here.
- If in doubt, don't commit it — reference where to obtain it instead
  (a README pointer), the way "load your own ROM from SD card" already
  works for the Spectrum machines in this codebase.
- For local-only testing, drop real dumps under `test/dock/real/` —
  that path is gitignored so they can't be committed by accident.

This is the same policy called out in `PLAN.md`'s risk register; repeated
here because this is the directory where it actually matters.

## What's still open (not this increment)

- A real, known-good `.dck` to validate the format spec above against —
  needed before slice 3 can trust this parser.
- Per-mode display test cards (standard/extended-colour/hi-res) for
  slice 2 to render against — not started; these are screen-content
  fixtures, a different shape of test data than `.DCK` files.
- The actual C++ loader (slice 3) that will use this format inside the
  firmware — this directory only has offline Python tooling so far.
