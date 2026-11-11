# ROM patches for the virtual disk feature (Phase 4, Pieces B/C/D)

Z80 assembly source patching the real TS2068 HOME ROM and EXROM to add
the virtual disk feature — see [`TS2068-VIRTUAL-DISK-PLAN.md`](../TS2068-VIRTUAL-DISK-PLAN.md)
for the full design and [`PLAN.md`](../PLAN.md)'s "Phase 4" for
sequencing. This is the first place in this project that authors new
Z80 machine code rather than reading or hex-patching existing bytes —
see `PLAN.md`'s Phase 4 writeup for why that's flagged as a genuinely
different kind of work from everything before it.

## What's here

- **`base/2068Home.BIN` / `base/2068Exrom.BIN`** — the real, genuine
  stock Timex TS2068 ROMs, copied in from the local reference library.
  Committing these is deliberate, not an oversight: per the project's
  resolved provenance policy (see [`../test/dock/README.md`](../test/dock/README.md)),
  Timex's own stock TS2068 ROMs are public domain, confirmed by a
  direct, on-file statement from Timex Group USA's own Senior VP — this
  is the one case where committing real ROM bytes into this repo is
  fine. These files are the assembler's `INCBIN` source, not test
  fixtures — the patch `.asm` files splice new code around specific,
  hex-verified byte offsets in these exact files.
- **`base/TS2068_U16.BIN` / `base/TS2068_U20.BIN`** — the HOME and
  EXROM dumped from an unmodified TS2068 (the reference library's
  `TS2068_ROMS_NoMod`), the same ROMs as on the board's card. They differ
  from the pair above in a few hundred bytes, none at a patch site. The
  native disk (`tools/build_dos.py`, `dos/dos.asm`) checks its HOME and
  EXROM patches against both pairs and builds against this one; the `D:`
  patches here don't use it.
- **`2068_exrom_patch.asm`** — the three EXROM patch points (`W_TAPE`
  `$0068`, `R_TAPE` `$00FC`, the `PGPSTR`-call redirect `$0215`) plus
  `CAT`'s listing routine (called from the HOME ROM patch via
  `BANK_ENABLE`). All new code lives in `$1624`-`$1CFF`, confirmed
  dead/padding space in the real 8K EXROM (disassembly-labeled
  `BLK1`/`BLK2`/`UNUSED1` in the local reference library's
  `docs/Timex Sinclair 2068 EXROM.txt`) — no size extension, the
  output stays exactly 8192 bytes.
- **`2068_home_patch.asm`** — the two HOME ROM patch points (`CAT`
  `$25C8`, `CLOSE #4` `$139F`). New code lives in `$3CDC`-`$3CFF`, a
  second confirmed-dead 36-byte pocket immediately before `CHRSET`
  (independently corroborated by the reference library's
  `gus-rom-analysis.md` — TS-Pico's own real, shipped ROM uses this
  exact same pocket for its own trampoline).
- **`out/2068Home.BIN` / `out/2068Exrom.BIN`** — the assembled,
  patched derivative ROMs. Committed deliberately (regenerable, but a
  real, small, deterministic build artifact someone can put straight
  onto an SD card without needing `sjasmplus` installed — same
  precedent as `test/dock/fixtures/synthetic.dck`).
- **`verify_patches.py`** — hex-level, static verification. Confirms
  every patch point's redirect bytes, that everything outside the
  patch points and the two new-code regions is byte-identical to the
  real ROM (including `CHRSET`, the untouched `FORMAT`/`MOVE`/`ERASE`
  dead stubs, etc.), and spot-checks that the new code's opening
  instructions decode as intended. Confirms the bytes are right, not
  that a Z80 executing them behaves correctly — that's what
  `test_zesarux.py` is for. 24/24 checks passing.
- **`test_zesarux.py`** — Phase 4 Piece D: runs the assembled ROM
  inside a real, independent Z80 emulator ([ZEsarUX](https://github.com/chernandezba/zesarux),
  driven via its ZRCP scripting protocol) and confirms the patches
  actually *behave* correctly when executed — not just that the bytes
  are right. Confirms live: the patched ROM boots completely correctly
  through the real cold-start sequence to a genuine interactive BASIC
  ready prompt; ordinary BASIC still works (`PRINT 2+2` => `4`, a real
  regression check); `CLOSE4_intercept`'s logic (both the `STRMN==4`
  and `STRMN!=4` paths); the full `CAT_trampoline` → `BANK_ENABLE` →
  `CAT_listing` chain, including confirming `BANK_ENABLE` (a real,
  live ROM routine, not project code) genuinely pages the patched
  EXROM into chunk 0. Does *not* confirm the `D:`-prefixed command flow
  end-to-end — ZEsarUX has no knowledge of the `$0E`/`$0F` protocol, so
  there's no real `VirtualDisk` backend answering it here; see the
  script's own docstring and `PLAN.md`'s Phase 4 Piece D writeup for
  exactly what is and isn't covered. 17/17 checks passing. Requires
  ZEsarUX running with ZRCP enabled first — see the script's docstring
  for the exact launch command.

## Rebuilding

```
sjasmplus --raw=patches/out/2068Exrom.BIN --sym=patches/out/2068Exrom.sym patches/2068_exrom_patch.asm
sjasmplus --raw=patches/out/2068Home.BIN  --sym=patches/out/2068Home.sym  patches/2068_home_patch.asm
python3 patches/verify_patches.py

# Then, to also re-run the live Z80 check (test_zesarux.py):
cat patches/out/2068Home.BIN patches/out/2068Exrom.BIN > /tmp/ts2068_patched.rom
pkill -f zesarux; sleep 2
nohup /Applications/zesarux.app/Contents/MacOS/zesarux \
  --noconfigfile --machine TS2068 --romfile /tmp/ts2068_patched.rom \
  --enable-remoteprotocol --vo null --ao null \
  > /tmp/zesarux.log 2>&1 &
sleep 8
python3 patches/test_zesarux.py
```

Assemble the EXROM patch first — the HOME ROM patch's `CAT_trampoline`
calls into a fixed address in the assembled EXROM (`CAT_listing`,
currently `$1691`), confirmed via `out/2068Exrom.sym`, not guessed. If
you edit `2068_exrom_patch.asm` in a way that moves `CAT_listing`,
update the `CAT_listing EQU` line in `2068_home_patch.asm` to match
before reassembling the HOME ROM patch. If any address referenced by
`test_zesarux.py` moves (check `out/2068Exrom.sym`/`out/2068Home.sym`),
update the script's hardcoded addresses to match.

Requires [`sjasmplus`](https://github.com/z00m128/sjasmplus) — no
Homebrew formula exists; install from the project's own GitHub
releases. See `PLAN.md`'s Phase 4 writeup for why this tool was chosen
(verified, not assumed, against exactly this INCBIN-splice-plus-ORG
workflow). `test_zesarux.py` additionally requires
[ZEsarUX](https://github.com/chernandezba/zesarux) (no Python
dependencies beyond the standard library — it's plain `socket`/`time`).

## What's confirmed vs. what's a design choice, not yet run on real hardware

Every patch point's original bytes, the two free-space regions, `TADDR`
(`$5C74`, SAVE/LOAD/VERIFY/MERGE distinction), `STRMN` (`$5CCB`),
`BANK_ENABLE` (`$6499`) and its real `B`/`C` calling convention, `RST
$10`'s real target (`$11ED`), and `R_TAPE`'s real carry-flag
success/failure convention were all hex-verified against the real ROM
files and/or a labeled disassembly before being used here — see
`TS2068-VIRTUAL-DISK-PLAN.md`'s "Where the new code lives" section for
citations. A few things are reasoned design choices, not hex-verified
ROM facts:

- The `"D:"` prefix check is case-sensitive (`"D:"` only) — matches
  every example in the design doc's command-syntax section, but
  nothing forces this; easy to relax later.
- `D_MODE_FLAG`/`CAT_LINE_BUF` (`$5DDD`/`$5DDE`) are placed in the
  reference library's documented reserved region, just past TS-Pico's
  own claimed block — corroborated by two independent real-world
  sources but not verified against a boot-time sysvar-defaults trace.
- `D:` `VERIFY` currently behaves identically to `LOAD` (bytes get
  written to memory, not just compared) — a known simplification.
  `VirtualDisk`'s `READ_HEADER`/`READ_DATA` have no true
  non-destructive verify-only mode today.
- Non-`.tap` "direct load" and `.tzx` containers are out of scope —
  `VirtualDisk::mountRead()` already fails cleanly for both (Phase 4
  Piece A).

**Update: real Z80 execution is now confirmed** (`test_zesarux.py`,
Phase 4 Piece D) — the patched ROM boots, runs ordinary BASIC
correctly, and both `CLOSE4_intercept` and the `CAT_trampoline`/
`BANK_ENABLE`/`CAT_listing` chain behave exactly as designed when a
real, independent Z80 core executes them. That testing also found and
fixed a real bug: `CAT_listing` had no way to terminate without a real
`VirtualDisk` backend ever answering `STATUS_EOF`/`STATUS_ERROR` on
port `$0F` — it now has a defensive `DJNZ`-based iteration bound
(`CAT_MAX_LINES`, 200), confirmed live in both branches. What's still
not confirmed, and can't be by more emulator scripting: the actual
`D:`-prefixed command flow end-to-end, since that needs a real
`VirtualDisk` handler answering `$0E`/`$0F`, which only exists as
real ESP32 firmware today — ZEsarUX has no such backend. See `PLAN.md`'s
Phase 4 Piece D writeup for the full account.
