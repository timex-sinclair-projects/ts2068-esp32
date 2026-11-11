# Native disk commands: proposal

Draft 7, 2026-11-11. Decisions from our discussion are in §5. Phases 1-4 are built and passed their gates on the board; §2 describes what was built.

**The goal:** the 2068's own BASIC keywords work on the SD card, with no prefix:
- `CAT`, `FORMAT`, `MOVE`, `ERASE`, `COPY`
- `OPEN #`, `CLOSE #`, `LIST`, `RESTORE #`
- `LOAD`, `SAVE`, `VERIFY` and `MERGE`

The one exception is a TAP file. `LOAD "name.tap"` mounts it, and later loads and saves happen inside it until it is unmounted.

**How it's built:** a 16K DOS ROM, overlaid by the emulator while it runs, carries the BASIC side, and the ESP32 does the file work natively. The command set follows the TS-Pico's current `docs/DISK_COMMANDS_SPEC.md` (tspico-firmware-build `main`, `3dfcd15`), minus the `tpi:` prefix. `OPEN #` keeps the TS-Pico's device prefixes (`f:`), leaving room for others such as a network `x:` over the ESP32's Wi-Fi.

---

## 1. What we can borrow

**From the TS-Pico.** Its ROM already solved the hard ROM problems, and its sources are MIT-licensed:

- **Bare keywords.** On a stock 2068, `CAT`, `FORMAT`, `MOVE` and `ERASE` only parse as `CAT "x",`, and then stop with Report J.
  - TS-Pico moves their parameter-table entries to class 05h (HOME 1946h–1949h).
  - It replaces the dead stub at 25D6h with a jump into the EXROM module, which parses each statement in both passes.
- **`COPY`, `LIST` and `RESTORE` join them** (TS-Pico #341):
  - `COPY` goes from class 00h to 05h through 25E0h. A bare `COPY` is handed back to the printer COPY.
  - `LIST` and `RESTORE` route to the same hook. Every stock form (`LIST`, `LIST n`, `LIST #n`, `RESTORE`, `RESTORE n`) is handed back to HOME.
- **`OPEN #` / `CLOSE #`.** The stock ROM knows only K, S and P, and crashes on an unknown letter in `CLOSE #`.
  - TS-Pico adds trampolines in a dead SYSCON remnant at 1488h–14C6h.
  - It adds two fixed channel stubs at 14A0h and 14A9h.
  - It parses `,"mode"[,reclen]`.
- **A 16K EXROM.** Chunks 0 and 1 are mapped together, reached through the HOME↔EXROM thunks. The interrupt and bank-stack gotchas are already found and fixed: `DI` around the bank switch, and the 14B2h trap for the leaked bank stack.
- **The command set and its rules** (§3):
  - Every argument is a string expression.
  - Paths use `/`, wildcards are `*` and `?`, and nothing is ever overwritten.
  - Errors map onto stock reports, so `ON ERR` works.
- **Formats.** TAP, and native files with a 128-byte +3DOS header. A card can then move between a real TS-Pico and the emulator.
- **Build tooling.** `build-rom.py` uses a declarative patch list. Each entry has its address and its before and after bytes, and the build fails if the before bytes don't match.

**From the emulator's own `D:` virtual disk** (to be retired, §5):
- **Hook the tape byte routines.** The idea worth keeping is hooking R_TAPE (EXROM 00FCh) and W_TAPE (0068h). Everything above them then works unchanged: SLVM's name matching, `LINE` autorun, `CODE`, `DATA`, VERIFY, MERGE, and machine code that calls the ROM loader directly.
- **The C++ TAP scanner** can be reused.

**What I'd leave behind:**
- The TS-Pico byte protocol: SYNC, pre-headers, blind reads, response functions 81h/86h/88h.
- Its `tpi:` words.

Both exist because a separate chip sits on the bus. Here the "device" is the emulator itself.

---

## 2. Architecture

```
 BASIC statement
     │  HOME hooks: the syntax table, the keywords' shared stub (25D6h)
     ▼
 HOME 3CDCh: DI / OUT (D3h),A ──► the emulator overlays the DOS ROM on 0000h-3FFFh
     ▼
 DOS module, in the 16K DOS ROM (runs under DI)
   · syntax pass: checks the arguments, HOME routines through HOMECALL
   · run pass: puts pointers and lengths in registers
     │  OUT (D1h),A        one service call: function code in A
     ▼
 C++ DOS engine in the emulator (runs inside the OUT; the Z80 is frozen)
   · reads and writes Z80 memory and registers directly, does the SD I/O
   · returns a status in A; 0 = OK, otherwise the report the ROM raises
     │
 DOS ROM: OUT (D5h),A ──► HOME is back; EI, then CHECK-END, or the report
```

**Why an overlay, not a 16K EXROM.** The 2068's own bank switch (TS-Pico's route) writes port FFh then F4h with interrupts on, and keeps a bank stack in RAM that a report leaks. TS-Pico had to work around both. Here the emulator does the switch:
- **Atomic.** One `OUT` swaps chunks 0-1 for the DOS ROM, at the very next instruction fetch. The DOS ROM's code continues at the address after the `OUT`, so each side of a switch sits at matching addresses in both ROMs (HOME's 3CDCh-3CFCh pocket).
- **Nothing to leak.** The 2068's registers (ports F4h, FFh) are never touched, so a cartridge or the EXROM is exactly as it was. The module never raises a report: it returns the code, and HOME raises it once the DOS ROM is gone.
- **HOME calls.** `HOMECALL` (HL = the routine) switches back, turns interrupts on in HOME (so `Scroll?` and BREAK work), calls, and switches again.
- **The syntax pass ends through HOME's CHECK-END (1B44h)**, not a plain RET. A RET there runs the statement loop's run-time code, which jumps to the line in NEWPPC and syntax-checks a program line in place, doubling its hidden numbers. Found on the board in Phase 1.

**Service calls, not a protocol.** One port, with the function in A and the arguments in HL, DE, BC and IX, handled synchronously the way the `D:` virtual disk already reads registers.
- **What it removes:** no FIFO, no polling, no timeouts.
- **Port number:** its own, so it doesn't collide with the TS-Pico's 0Eh/0Fh. Both are built into one firmware, and the ROM decides which one is used (§5).
- **Cost:** the Z80 stalls for the length of an SD operation. That's a few milliseconds; a long COPY stalls longer. Audio may click during big transfers.

**LOAD and SAVE go through the tape routines.** Four EXROM sites jump to hooks in its unused BLK2 (1800h): R_TAPE, W_TAPE, SLVM's call to PGPSTR (where the full name is) and SAVE (so there is no "Start tape" prompt or pause with a TAP mounted). The hooks make service calls with the EXROM paged in. The engine serves or takes whole blocks:
- **A mounted TAP** serves its blocks in order.
- **A native file** with a +3DOS header is served as a two-block virtual tape. The engine builds the 17-byte tape header from the +3DOS header, then sends the data. SAVE does the reverse. The format is TS-Pico's (`TS/native.py`): issue 1, version 0, and a screen stored as the raw 6912 bytes.
- **Long names.** The hook at PGPSTR sees the full name and path. For a native SAVE it hands SLVM a short name instead (the base name without its extension, cut to 10), which goes in the header; for a native LOAD, no name, so SLVM takes the one header there is.
- **The type** is checked at the name, from the token after it (`CODE`, `SCREEN$`, `DATA`, or none), before anything loads: a mismatch is Report Q.
- **`Replace?`** is asked by the EXROM hook on the lower screen: it prints the question the engine left in spare RAM (5E20h) through CALL BANK to HOME's CHAN-OPEN and RST 10h, then reads Y or N from the keyboard ports. N is Report D.

So the BASIC semantics stay in the real ROM, and the ROM side of LOAD and SAVE is small.

**File channels don't need TS-Pico's 512-byte buffer in BASIC RAM.**
- The engine buffers on the C++ side.
- The CHANS record is then only the 5-byte header plus a handle number.
- Each character goes through a service call. That's cheap, since the call is synchronous.
- The prefix names the device (`f:` a file; later perhaps `x:` a network connection), so a channel's routines can dispatch on it.

**Printing.** CAT listings, `LIST "file"` and messages come back as text in a buffer, and the ROM prints them with `RST 10h` and the `Scroll?` pager. The engine never draws on the screen. Y/N prompts read the keyboard on the ROM side.

**The ROM image.**
- **Base:** the stock ROMs the user already has on the card (`/2068/rom/home.rom`, `exrom.rom`). The firmware patches them at boot after checking every patch site, and anchors for the ROM routines the DOS ROM and the hooks call, against the stock bytes. Any other HOME ROM (the TS-Pico ROM) runs unchanged, with the native disk off; an EXROM that doesn't fit leaves the disk commands on but TAP mounting off.
- **Build:** `tools/build_dos.py` assembles `dos/dos.asm` with sjasmplus three times (the DOS ROM, HOME's side of the switch, the EXROM hooks), checks declarative patch lists in the `build-rom.py` style against both stock ROM pairs, and writes `include/DosRom.h`.
- **Where it lives:** in the firmware. Nothing extra goes on the card.

---

## 3. User-facing behaviour

### Files and the card

- **The 2068's root is `/2068/` on the card.**
  - `MOVE TO "/"`, `CAT "/"` and every path stay inside that folder.
  - BASIC can't reach ESPectrum's folders or the rest of the card.
- **Names** are saved exactly as typed, with a +3DOS header carrying the type (TS-Pico's native-file format). CAT shows the type from the header.
- **Paths** use `/`, and `..` goes up but never above the root. Matching is case-insensitive.
- **Wildcards** are `*` and `?`, allowed in the last path component only.
- **Arguments.** Every string argument is a string expression (`CAT a$`, `ERASE "old"+n$`), up to 64 characters.

### LOAD, SAVE, VERIFY, MERGE

| Typed | Nothing mounted | A TAP mounted |
|---|---|---|
| `LOAD "game.tap"` | mounts it, then loads its first program (SLVM can't end the statement cleanly) | unmounts the old one, mounts this one, loads its first program |
| `LOAD ""` | the file picker: a list on the screen, 6/7 to move, ENTER to load (Phase 7) | next program on the tape, tape order |
| `LOAD "name"` | loads native file `name` from the current folder | searches the TAP forward from the pointer and wraps once; not found is Report F |
| `LOAD "x" CODE / SCREEN$ / DATA a()` | native file, type checked (Q on a mismatch) | inside the TAP |
| `LOAD "x.tzx"` | as a TAP: mounted read-only (SAVE is J); standard-speed blocks only | as a TAP |
| `LOAD "x.dck"` / `LOAD "x.rom"` | docks the cartridge and resets (VERIFY/MERGE: Q) | the same |
| `SAVE "name" ...` | writes native file `name`; if it exists, `Replace?` (N is Report D) | **appends** a header and a data block to the TAP |
| `VERIFY` / `MERGE` | the same as LOAD | the same as LOAD |
| m/c calling the ROM loader | — | served from the TAP, so multi-part BASIC+CODE loaders work |

- **Turbo and custom loaders** read the EAR bit themselves, so they can't work from this. Tape-signal playback from TAP/TZX is a separate project.
- **Nothing leaves the TAP while it is mounted.** Every LOAD, SAVE, VERIFY and MERGE goes to the TAP, whatever the name looks like (block names such as `"game.bin"` are common). To reach a native file, unmount first.
- **Unmounting** happens on:
  - `CLOSE #*`, which also closes every open file;
  - mounting another TAP;
  - changing folder;
  - a reset.

### Disk keywords

| Command | Does |
|---|---|
| `CAT` | lists the current folder: folders first in `<>`, then files with index and size; marks the mounted TAP |
| `CAT "dir"` / `CAT "*.tap"` | another folder, or the matches |
| `CAT "x.tap"` | the blocks in a TAP, without mounting it |
| `CAT ""` | the mounted TAP's blocks, with `>` at the pointer |
| `CAT #s[;"x"]` | the same listing as text into the file open on stream s |
| `MOVE TO "dir"` | change folder (`".."`, `"/"`, paths) |
| `MOVE TO "-"` | back to the previous folder |
| `MOVE TO ""` | pick a folder from a list (ENTER on a folder moves there) |
| `MOVE "old" TO "new"` | **rename**. If `new` is a folder, the file moves into it |
| `COPY "a" TO "b"` | **copy**. If `b` is a folder, the copy keeps its name |
| `COPY "*.tap" TO "backup"` | copy every match into a folder, listing each as `copied` or `exists` |
| `COPY` | the screen to the printer, which is the card: `/2068/print/copyNNN.bmp` |
| `LPRINT`, `LLIST` | text appended to `/2068/print/printer.txt` |
| `ERASE "x"` | delete, silently |
| `ERASE "*.bak"` | `Erase <name> (Y/N)?` for each; Y erases, any other key skips |
| `ERASE "dir/"` | remove an empty folder |
| `FORMAT "x.tap"` / `FORMAT "x"` | new empty TAP, mounted, so SAVE adds to it |
| `FORMAT "dir/"` | new folder. **Never formats the card** |
| `LIST "notes.txt"` | show a file: text as text, anything else as a hex dump |
| `LIST`, `LIST n`, `LIST #n` | unchanged: the program |
| `OPEN #n,"f:file"[,"r\|w\|a\|u"[,reclen]]` | a file channel for `PRINT #`, `INPUT #`, `LIST #`, `INKEY$ #`, as on the TS-Pico. The prefix names the device; K, S and P stay the stock channels |
| `INPUT #n;TAB r;...` / `PRINT #n;TAB r;...` | random access by record |
| `RESTORE #n` | back to the start of the file on stream n |
| `RESTORE`, `RESTORE n` | unchanged: the DATA pointer |
| `CLOSE #n` | closes it; data is safe after the close |
| `CLOSE #*` | closes every file channel and unmounts the TAP |

Left out for now:
- `FORMAT ":label"`: card labels have no meaning here.
- `OPEN #n,"d:pattern"`: directory listings as a channel; `CAT #s` covers that. The prefix scheme leaves room to add it later.
- **Other `OPEN #` devices** fit the same scheme later, for example `x:host:port` for a network connection over Wi-Fi.

**Errors** use TS-Pico's mapping onto stock reports:

| Report | Meaning |
|---|---|
| F *Invalid file name* | a bad or missing name, or no match |
| Q *Parameter error* | a request that can't be carried out, or a wrong type |
| 8 *End of file* | end of file |
| D | BREAK, or N at `Replace?` |
| O | not a file stream |

**Nothing is overwritten except by `SAVE`, after its prompt.** An existing destination is Report F for `COPY`, `MOVE` and `FORMAT`.

**A few new reports, if wanted**, could take ROM space the way TS-Pico added its report T: *No SD card*, *Disk full*. Otherwise those map onto J and 4.

**Later, cheap once the engine exists:**
- `LOAD "x.dck"` docks a cartridge and resets, using the emulator's DockLoader.
- `LOAD "x.rom"`.
- LPRINT/LLIST to a text file and COPY to a BMP, like the TS-Pico's printer capture.
- TZX, read-only, standard-speed blocks.
- The on-screen file picker for a bare `LOAD ""` and `MOVE TO ""`.

---

## 4. Phases

Each phase is tested on the board. The serial keyboard types the commands, and the screen dump plus PNG lets me read the results without you.

1. **ROM build and service call. Done 2026-11-11; gate passed on the board.**
   - Build: `tools/build_dos.py`, the stock base, CAT's HOME hooks re-verified against stock. No EXROM boot patch: the DOS ROM is an overlay (§2).
   - Firmware: ports D1h/D3h/D5h alongside the TS-Pico link.
   - Commands: bare `CAT`.
   - Gate: `CAT` lists `/2068/`, and ordinary BASIC is unaffected. Checked with a program holding `CAT : PRINT 9` between a FOR loop and a PRINT: it lists, runs and ends `0 OK, 30:1`. `CAT "x"` is rejected at the syntax check with the marker in place.
2. **TAP mount. Done 2026-11-11; gate passed on the board.**
   - The R_TAPE/W_TAPE hooks, mount and unmount (`CLOSE #*`), `CAT ""`, `CAT "x.tap"`, `FORMAT "x.tap"`. Also `CAT "folder"`.
   - Gate: a multi-part tape (BASIC loader + `LOAD "" CODE` + `LINE` autorun) loads and runs, and SAVE appends.
   - Checked on the board: `FORMAT "gate1"`, then `SAVE "loader" LINE 10` (line 10 is `LOAD "" CODE : PRINT USR 32768`) and `SAVE "mc" CODE 32768,4` (`LD BC,1234 / RET`). `CAT ""` showed the 4 blocks. After a reset, `LOAD "gate1.tap"` loaded the loader, which ran, loaded the code and printed 1234, ending `0 OK, 10:2`. `CAT` marked the mounted TAP with `>`; after `CLOSE #*`, `CAT ""` gave J.
3. **Native files. Done 2026-11-11; gate passed on the board.**
   - The +3DOS header and long names, for every type, with `Replace?`. CAT shows each file's type (Prog, Num(), Chr(), Code, Scr, TAP).
   - Gate: VERIFY and MERGE work, and a type mismatch gives Q.
   - Checked on the board: `SAVE "hello.bas" LINE 10`; the same again asked `Replace hello.bas? (Y/N)`, N gave D and Y replaced it. After a reset, `LOAD "hello.bas"` loaded and ran it; `VERIFY "hello.bas"` gave 0 OK; `LOAD "hello.bas" CODE` gave Q; `MERGE "hello.bas"` then `LIST` showed the line; `LOAD ""` gave J. `SAVE "s.scr" SCREEN$` wrote 6912 raw bytes, and after `CLS`, `LOAD "s.scr" SCREEN$` brought the screen back.
   - With nothing mounted, the stock tape routines are no longer reachable from BASIC, so the emulator's tape player isn't either (§5, decision 1). Machine code calling the ROM loader still gets the stock routine.
4. **File management. Done 2026-11-11; checked on the board.** MOVE TO, MOVE, COPY, ERASE, FORMAT folders, wildcards, Y/N, `LIST "file"`.
   - HOME patches as TS-Pico's: COPY's parameter list (19D7h) to class 05h and a stub at 25DDh (`LD B,FFh / JR 25D6h`); LIST's (19AFh) straight to 25D6h, told apart by T_ADDR = 19B0h. A bare COPY, and LIST, LIST n and LIST #n, go back to the stock routines (0A02h, 1545h) with C and HL.
   - The engine leaves text for the ROM to print, as CAT does. A byte 01h in it is a question: the ROM reads a key from the keyboard ports and calls ANSWER. At the end, END gives the report, if any.
   - Checked on the board: `FORMAT "work/"`, `MOVE TO "work"`, two SAVEs, `COPY "p1.bas" TO "p3.bas"`, `MOVE "p2.bas" TO "q.bas"`, `CAT`. `ERASE "p*.bas"` asked for each match: Y erased p1.bas, another key skipped p3.bas. `LIST "q.bas"` showed a hex dump. After a reset: `CAT "*.bas"` in /work/, `MOVE TO "-"` back to /, the stock `LIST` and `LIST 20`, `ERASE "work/"` (not empty) Q, `MOVE TO "nope"` F.
   - Not checked on the board: a bare `COPY` (the printer). It goes back to the stock routine the same way `CLOSE #n` does.
   - `MOVE TO ""` (the folder picker) came with Phase 7.
5. **Channels. Done 2026-11-11; gate passed on the board.** OPEN #/CLOSE #, PRINT #/INPUT #/LIST #/INKEY$ #, `TAB` records, `RESTORE #`, `CAT #s`.
   - The CHANS record is 6 bytes: CH_OUT, CH_IN (HOME stubs at 149Ch/14A5h), `F`, the stream. It sits before CHANS' closing 80h, so 16 records keep every STRMS offset below 80h; no padding to 512 bytes is needed.
   - HOME's dead 1488h–14C6h holds the OPEN/CLOSE trampolines, the two stubs, OPEN #'s syntax check (`,"mode"[,length]` at 1438h) and a helper that lets the DOS ROM pass HL to a HOME routine. OPEN # (145Eh) and CLOSE # (13A5h) switch to the DOS ROM's D_HOOK; K, S and P go back to the stock 1465h/13BEh. RESTORE's parameter list (19D0h) goes to 25D6h like LIST's.
   - Each character is one service call from HOME, with no bank switch, so TS-Pico's interrupt-window and bank-stack problems don't arise. The engine finds the stream through CURCHL.
   - Rules as TS-Pico's §4: text mode (CR as LF; CR, LF and CRLF read back; keywords expanded, the 2068's own included; controls dropped; the comma to 16-column stops), `b` for binary (23 is still TAB), modes r/w/a/u. A record file buffers each record and writes it whole at its CR, so a record that is too long is Q and leaves the file alone. `TAB 0` with INPUT is the count; with PRINT it is Q. A file open on another stream is F. An open file can't be erased or moved (Q). `CLOSE #*` closes every channel. After `NEW`, the next `OPEN #` closes channels whose records are gone.
   - Checked on the board:
     - `OPEN #4,"f:g5.txt","w"`, `PRINT #4;"hello",1+1`, `LIST #4`, `CLOSE #4`, and `LIST "g5.txt"` showed both lines.
     - `INPUT #5;a$` and `INPUT #5;LINE b$` read them back; one more INPUT gave `8 End of file`; `RESTORE #5` then `PRINT INKEY$ #5` printed `h`.
     - `OPEN #6,"f:r.dat","u",8`, `TAB 3`/`TAB 1` writes, `INPUT #6;TAB 0;n` gave 3, and `TAB 3` read back `carol` padded to 8. The same `OPEN #` in a program line, run, read record 1.
     - `CAT #7` wrote the listing to a file.
     - `CLOSE #*` closed stream 8, so `PRINT #8` gave O.
     - `OPEN #2` to a file sent ordinary `PRINT` there.
     - `OPEN #4,"p": CLOSE #4` still ran the stock routines.
     - After `NEW`, the same file opened on another stream.
     - Extra arguments to `OPEN #` were refused at entry with the `?` marker.
6. **Retire `D:`. Done 2026-11-11; checked on the board.** Removed: VirtualDisk (and its host test), its port 0Eh/0Fh path in Ports.cpp, the `D:` ROM patches and their checks (`patches/*.asm`, `verify_patches.py`, `test_zesarux.py`, `patches/out/`), and `TS2068-VIRTUAL-DISK-PLAN.md`. `patches/base/` keeps the stock ROM pairs `build_dos.py` checks against.
   - Ports 0Eh/0Fh are now the TS-Pico link's alone. Without `TS2068_TSPICO`, they read FFh and ignore writes.
   - Found on the way: `DosDisk.h` was included only inside the TS-Pico `#ifdef`, so the `nopsram` and `psram` builds had failed since Phase 1. All three environments build again.
   - Every host suite passes. On the board: `CAT`, a channel read and `LOAD "hello.bas"` (it ran).
7. **Extras. Done 2026-11-11; checked on the board, TZX on the host only.** DCK/ROM, TZX, printer capture, the file picker.
   - **Cartridges.** `LOAD "x.dck"` checks the file (its 9-byte bank headers and 8K chunks) and keeps it; the statement ends with 0 OK (answer FFh, raised as a report). Between frames `DosDisk::frame()` hands it to `DockLoader::loadFromBuffer()` and the emulator resets, so the chunks never change under a running hook. `LOAD "x.rom"` (up to 64K) becomes a DCK with DOCK chunks from 0, the last padded with FFh. VERIFY or MERGE of either is Q; a cut-short DCK is Q. CAT shows DCK and ROM. The answer code was first named `R_OK`, which `<unistd.h>` defines as 4: renamed `R_END`.
   - **TZX.** A TZX mounts like a TAP and is served by the same reader: a block 10h is a TAP block at its offset, and so is a block 11h whose bit timings are the ROM's (within a tenth). Turbo, pure-data and direct-recording blocks are skipped and counted ("2 not for LOAD (turbo)" in `CAT`); informational blocks are stepped over; an unknown block or one cut short ends the list. It is read-only: SAVE with a TZX mounted is J before anything is written, and `SAVE "x.tzx"` is F.
   - **Printer.** HOME's channel data at 11B9h makes the P record's output CH_OUT (149Ch), so LPRINT and LLIST bytes reach the engine; it sees the `P` at CURCHL+4. The text is laid out as the 2040 does: 32 columns, the comma to 16, TAB and AT's column, keywords spelled out, £ and the block graphics in UTF-8, a UDG as `?`. It goes to `/2068/print/printer.txt` (appended) at 512 bytes, after half a second with no more, at COPY and at a reset. Bare `COPY` calls PR_COPY (17h): the top 22 lines (as the 2040 prints them) as a 1-bit BMP, `copy001.bmp` onwards; screen 1 from 6000h, hi-res 512 dots wide. The 2040 itself (port FBh) still answers machine code that drives it.
   - **The picker.** `LOAD ""`, `VERIFY ""` and `MERGE ""` with nothing mounted, and `MOVE TO ""`, answer T_PICK (FEh). The engine keeps the list and the cursor and writes what to print: line 0 the command and folder, line 1 the keys, lines 2-20 the entries with the current one in INVERSE; a move redraws just two lines. The ROM's loop (in the EXROM hooks for LOAD, in the DOS ROM for MOVE TO) prints PICK_CHAR's bytes, asks PICK_END, and sends the keyboard rows EFh/F7h/BFh/7Fh to PICK_KEY. 6/7 move, 5/8 page, ENTER loads or goes into a folder, SPACE is D. LOAD lists the folders, then what it takes (a tape, a cartridge for LOAD only, a +3DOS file, a screen); MOVE TO lists `.`, `..` and the folders, and ENTER on one moves there. At the end the screen is cleared, and the pick goes through TAPE_NAME or MOVE TO as if typed.
   - Checked on the board:
     - `COPY "rom/cart.dck.off" TO "cart.dck"`, then `LOAD "cart.dck"`: docked as LROS, reset, the cartridge's own screen. (`/2068/cart.dck` is also where the firmware looks for a boot cartridge, so the copy was erased afterwards.)
     - `10 REM hello there`, `LLIST`, `LPRINT "hello",1`, a `PRINT` and `COPY`: `printer.txt` held `  10 REM hello there` and `hello` with 1 at column 16 (39 bytes); `copy001.bmp` was 5694 bytes (62 + 176 x 32).
     - `MOVE TO ""`: the list, 6 twice, ENTER: in `/rom/`. `LOAD ""`: 6 four times to `hello.bas`, ENTER: it loaded and ran. ENTER on `<rom>` showed its files; SPACE gave D.
   - Not checked on the board: a TZX (there is none on the card, and nothing writes one).

---

## 5. Decisions (2026-11-11)

1. **`LOAD ""` with nothing mounted:** a report until Phase 7; now the on-screen file picker.
2. **Unmounting a TAP:** `CLOSE #*` (also closes every file), plus the implicit cases: mounting another TAP, changing folder, reset.
3. **COPY and MOVE:** as in TS-Pico's current spec. `COPY "a" TO "b"` copies, `MOVE "a" TO "b"` renames or moves into a folder, and bare `COPY` stays the printer COPY (from Phase 7 the printer is the card, decision 13).
4. **File names:** exactly as typed, with a +3DOS header for the type.
5. **The card root:** a folder, `/2068/`. The ROMs and `cart.dck` sit there today, which would put them in reach of `ERASE`. **Proposed:** move them to `/2068/rom/` (and the cartridge to `/2068/rom/cart.dck`) when Phase 1 lands.
6. **TS-Pico:** both built into one firmware, with the ROM deciding. `tspico.rom` boots the TS-Pico ROM and link; the disk ROM boots native disk.
7. **The `D:` virtual disk:** retired (Phase 6). Its TAP-scanning code moves into the new engine.
8. **A mounted TAP is the only target** for LOAD, SAVE, VERIFY and MERGE until it is unmounted, whatever the name.
9. **`OPEN #` keeps the `f:` prefix,** as on the TS-Pico. Prefixes name devices, leaving room for a network `x:` over Wi-Fi.
10. **Phase 7's order:** DCK/ROM, TZX, the printer, the picker.
11. **`LOAD "x.rom"`** is a cartridge ROM: docked from DOCK chunk 0, then a reset.
12. **The file picker** is on the 2068's screen: the ROM prints the engine's list with RST 10h and reads the keys.
13. **Printer output** goes to `/2068/print/`: LPRINT and LLIST appended to `printer.txt`, each COPY a new `copyNNN.bmp`.

---

## 6. Risks

| Risk | Note |
|---|---|
| HOME patch addresses differ between Gustavo's v1.7 base and the stock ROM | Every TS-Pico HOME patch is re-checked against stock bytes by the build. The dead-code pockets (25D6h stub, 1488h–14C6h, 3CDCh) must be confirmed dead in stock |
| SD stalls freeze the Z80 | Fine for BASIC. Audio and video during big copies may glitch; measure |
| SLVM's 10-character names vs long SD names | The full-name catch at PGPSTR plus a synthesized header; Phase 3 gate |
| 8K for the DOS module | TS-Pico's module is about 2K and includes protocol code we drop. Wildcards, listings and file logic go in C++, keeping the Z80 side small |
| Keeping in step with the TS-Pico spec | It is still moving (#341, #404). Pin a commit (`3dfcd15`) and re-check before each phase |
