# TS2068 + TS-Pico on the Olimex ESP32-SBC-FabGL: Plan

The goal is a standalone Timex/Sinclair 2068 with TS-Pico functionality on the **Olimex ESP32-SBC-FabGL** (Rev B). The board has a classic ESP32 with VGA, PS/2 keyboard and mouse, microSD and mono audio.

A classic ESP32 has no room to run the real TS-Pico firmware beside the emulator. The plan instead **puts the real firmware on a Raspberry Pi Pico connected by a UART**, and runs the emulator on the ESP32. ts2068-p4 named this as its Gate 1 fallback ("put the firmware on a separate RP2040/RP2350 attached by UART or SPI").

---

## 1. The board (Rev B KiCad netlist, BOM and user manual rev 3.0)

| Item | Detail |
|---|---|
| Module | ESP32-WROVER-E: dual Xtensa LX6 at 240 MHz, about 320 KB of usable internal SRAM. Olimex says 4 MB flash and 8 MB PSRAM, but the classic ESP32 maps only 4 MB of PSRAM directly |
| Video | VGA through a 2-bit-per-channel resistor DAC (64 colours). R = GPIO22/21, G = 19/18, B = 5/4, HSYNC 23, VSYNC 15. This is the TTGO VGA32 / FabGL layout |
| Keyboard | Two PS/2 sockets: keyboard on CLK 33 / DAT 32, mouse on 26/27. **No USB host** |
| Audio | Mono, GPIO25 into the jack through an RC filter. No amplifier; a buzzer sounds when the jack is empty |
| microSD | SPI on HSPI: CLK 14, MOSI 12, MISO 35, CS 13. The card-detect switch isn't wired to a GPIO |
| CH32V003 | A co-processor on the same SPI bus, selected when GPIO13 is high, with its IRQ on GPI36. It drives the **UEXT connector** by bit-banging, so UEXT is a slow path |
| **AccessBus** (8-pin header) | +5V, +3.3V, GND, ESP_EN, **GPIO2 = U1TXD, GPI34 = U1RXD, GPIO0 = U1RTS, GPI39 = U1CTS**. This is the ESP32's second hardware UART, with flow control |
| USB | USB-C to a CH340X: programming with auto-reset, and the serial console |
| Free GPIOs | **None.** Everything is assigned. AccessBus is the only fast way to attach another chip. GPIO26/27 come free if the mouse port isn't used |

**What runs on it today:** ESPectrum (EremusOne, GPL-3) runs on this board out of the box and detects it automatically. It runs the Z80 on **core 0**, with video (I2S DMA, bitluni's VGA6Bit) and audio (LEDC PWM on GPIO25) on **core 1**. FabGL (GPL-3) supplies its PS/2 driver.

## 2. The emulator

The emulator is a TS2068 port of ESPectrum for this class of board (TTGO VGA32 / SBC-FabGL). ESPectrum supplies the chassis: the Z80 core, the VGA driver, the AY, the PS/2 keyboard, SD and the OSD. The port adds:
- The SCLD, with ports F4/FF and the 8-chunk memory model, with EXROM chunk-indexed.
- HOME, DOCK and EXROM, `.DCK` loading and LROS autostart.
- All eight SCLD screen modes, including 64-column hi-res.
- Keyboard on FE, the AY on F5/F6, the printer on FB, border and beeper.
- ROM loading from SD.
- Native disk commands on the SD card (DISK_PROPOSAL.md).
- Host tests in `test/host/`.

It still renders whole frames rather than following the beam, ignores contention, and has no Timex joysticks.

## 3. Where the TS-Pico firmware can run

| Option | Verdict |
|---|---|
| **A. MicroPython on the same ESP32** | **Ruled out.** ESPectrum already uses both cores: Z80 on core 0, video and audio on core 1. MicroPython's task wants core 1 too. The firmware needs about 190 KB of heap and 122 KB of bytecode, against about 320 KB of internal SRAM that the emulator also needs. Flash writes (LittleFS) stall PSRAM and cache on this chip as well. |
| **B. The real firmware on a Pico, over AccessBus UART1** | **Proposed.** The firmware runs natively in MicroPython's rp2 port, as on a real TS-Pico, unchanged except for a new board layer. It needs one Pico (RP2040 or RP2350) and a microSD breakout. |
| C. Reimplement the TS-Pico in C on the ESP32 | Ruled out, as in ts2068-p4 (D1): about 12.5K lines of behaviour, and two firmwares that drift apart. The emulator's old `D:` virtual disk was a separate protocol of this kind. |
| D. Bridge to `pico_host` on a PC over Wi-Fi | A development aid only, as in ts2068-p4. |

### 3.1 How option B works

```
┌──────────── ESP32-SBC-FabGL ────────────┐        ┌────────── Pico (RP2040/RP2350) ──────────┐
│ core 0: Z80 + TS2068 machine            │        │ MicroPython rp2 + TS-Pico firmware        │
│   port 0Eh/0Fh hook ─► bus mirror (C)   │ UART1  │  board_link.py: MQ fed from the UART      │
│     · local TX FIFO, status byte        │◄──────►│  link.c (C module): framing, TX streaming │
│     · OUT → frame out, status = BUSY    │ GPIO2→ │  SD card (SPI) with /TAP, /help, slots    │
│     · IN 0Eh from FIFO; waits if empty  │ ←GPI34 │                                           │
│ core 1: VGA, audio (ESPectrum)          │ RTS/CTS│  powered from AccessBus +5V               │
└─────────────────────────────────────────┘        └───────────────────────────────────────────┘
```

**Not a round trip per port access.** As on the v3 card and in ts2068-p4's `p4bridge` (D4), the ESP32 keeps a **local mirror** of the bus model. That means a TX FIFO that the Pico fills ahead of demand, and the status byte, which the Pico pushes whenever it changes.
- `OUT` sets the status to BUSY first and then sends the word.
- `IN 0Fh` reads the mirrored status.
- `IN 0Eh` pops from the local FIFO.
- Only an `IN 0Eh` that finds the FIFO empty stalls the Z80, with the same timeout and `00h` result as on real hardware.

**Link budget:**
- At 3 Mbaud a byte takes about 3.3 µs. The TS2068 ROM reads one LOAD byte every 44–50 µs, so a streamed LOAD keeps up with a large margin.
- Hardware RTS/CTS on GPIO0/GPI39 keeps the FIFOs from overrunning.

**Strapping safety:** the Pico drives only GPI34 and GPI39. Both are input-only pins and neither is a strapping pin. It never drives GPIO0 or GPIO2, so it can't push the ESP32 into download mode at reset.

**Memory hooks:**
- At boot, the firmware's `boot_rom`, `load_dock` and `map_slots` (`tpi:boot`, `tpi:dock`, `tpi:zx48`) send ROM and DOCK images down the link into the emulator's memory map.
- So the Pico's SD card can be **the only card**: the emulator would load the TS-Pico ROM from the Pico rather than from its own SD (§5, Q2).

**Upstream:** a `board_link` board layer in tspico-firmware-build, the same shape as the P4's `board_p4` proposal (issue #354). Until it lands, it can be a small set of stand-ins.

**Wire protocol:** keep **bridge v1** frames (`EMULATOR_BRIDGE.md`), plus a few link-only messages: status-changed, a TX block, ROM/DOCK image chunks and reset. Document it as bridge v1.1 so fuse-tspico and zesarux-tspico keep working.

**A second use of the same link:** the same Pico firmware build lets **desktop emulators use real hardware**. A Pico plugged into a PC over USB-serial could stand in for `pico_host`.

---

## 4. Phases and gates

### Phase 0: Bring up the emulator on the board
- Build the `nopsram` environment with PlatformIO and flash it over the USB-C port (CH340X). Put the stock TS2068 ROMs on the SD at its fixed paths.
- Check what works: boot to the TS2068 copyright screen, the PS/2 keyboard, BASIC, the beeper and AY, the VGA mode and hi-res, and DOCK cartridges.
- Record the results in PLAN.md, and fix what fails there.
- **Gate 0:** stock TS2068 BASIC runs at full speed with keyboard and sound.

**Status (2026-11-11): boots to a clean copyright screen; keyboard, sound and speed not yet tested (no PS/2 keyboard yet).**
- The board is an ESP32-D0WD-V3 (rev 3), 4 MB flash, 4 MB of usable PSRAM. The `psram` build is the right base, not `nopsram`.
- New environment `fabgl2068` (`pio run -e fabgl2068`): the `psram` build plus `-D TS2068_FORCE_2068`, since nothing else selects the 2068. It needs its own `sdkconfig.fabgl2068` (a copy of `sdkconfig.psram`): PlatformIO reads `sdkconfig.<env>`, and without it PSRAM was off.
- ROMs: stock `TS2068_U16.BIN` / `TS2068_U20.BIN` from the reference library, at `/2068/home.rom` and `/2068/exrom.rom` on the card.
- Five bugs fixed, none visible to the host tests:
  1. ROMs were read before the SD card was mounted (`ESPectrum.cpp`).
  2. VGA line buffers for the 512-wide mode (about 120 KB of internal DMA RAM) didn't fit; the SCLD's 64 KB now goes in PSRAM (`SCLD.cpp`). About 37 KB of internal RAM is left after setup.
  3. `Z80::exec_nocheck()` fetches opcodes from `MemESP::ramCurrent`, bypassing the SCLD; the 2068 now runs through `Z80::execute()` (`CPU.cpp`). This is slower per instruction: check full speed.
  4. `RenderFrame2068` wrote pixels in plain byte order; the I2S DMA wants `x ^ 2` (`Video.cpp`).
  5. The 512-active VGA mode filled the monitor's width: no side borders, 1.25x too wide. Now standard 640x480 timing (the TK 320x240 mode doubled) with the 320x240 2068 frame at 2x and 64-pixel side borders; MemESP's Spectrum pages go to PSRAM in 2068 mode to make room. Confirmed right on the monitor.
- Serial keyboard (`-D TS2068_SERIAL_KEYS`, `tools/sendkeys.py`): UART0 bytes become injected key presses. Typing `PRINT "HELLO"` works. Opening the port resets the board (macOS pulses RTS/DTR), so the script waits for boot first. The firmware prints frame timing every 300 frames.
- **Speed: full speed.** The checked `execute()` loop took about 17,200 us of the 16,639 us frame (3% slow). `Z80::exec_nocheck_2068()` fetches from `SCLD::memChunk[]` directly and adds the 4 T-states inline: about 12,550 us at the copyright screen and 13,320 us running `10 GO TO 10`, so 20% headroom with SCLD memory still in PSRAM. Frames are paced at 60.2 Hz measured (300 frames in about 4.98 s).
- **Beeper works:** `BEEP 1,0: BEEP 1,7: BEEP 1,12` typed over serial plays three rising notes from the audio jack. (BEEP is extended mode then SYMBOL SHIFT + Z, per the ROM's `SEKEYS` table.)
- **AY works:** `10 SOUND 7,62;8,15;0,200;1,0` / `20 PAUSE 300` / `RUN` plays a 5-second tone. A direct `SOUND` command only chirps, and that is correct ROM behaviour: the report routine (`0EC8`) writes R7=255, silencing the AY, and while waiting for input the ROM writes R7=64/R14=0/R7=255 every frame. Test AY sound from a running program.
- Typing over serial: the ROM ignores keys while it stores a line or runs a command, so `sendkeys.py` pauses 0.6 s after each ENTER.
- **Hi-res works:** `OUT 255,6` shows 64 columns, DFILE1 in the even ones. The odd ones (DFILE2, `$6000`) show the top third filled: that is the function dispatcher the HOME ROM copies from EXROM `$1000` to `$6200`–`$682F` at startup (`LD DE,$6200 / LD BC,$0630 / LDIR`), checked byte for byte on the board. A real 2068 matches: `PRINT PEEK 25088` gives 221 (`$DD`) on the user's machine. Programs that use hi-res move it out first (EXROM `CHNG_VID`).
- **DOCK cartridges work:** Zebra OS-64 (LROS, from the reference library, wrapped as a `.DCK`) at `/2068/cart.dck` boots to its 64-column screen. Three fixes:
  1. Nothing on the board loaded a cartridge: the boot now loads `/sd/2068/cart.dck` if present (no OSD picker yet).
  2. `CPU::reset()` jumped straight to an LROS cartridge's entry point, skipping the ROM's start-up. Removed: as on a real 2068, the HOME ROM starts up, then the EXROM's `EXTINIT` (`BLDSCT` reads the dock header, `NORMSVAR`) enters the cartridge through `GOTO_BANK`.
  3. `DockLoader::unload()`, run before every load, cleared all EXROM chunks, including the system EXROM: the dispatcher copy to `$6200` copied zeros and the machine reset in a loop. It now clears only the chunks the cartridge supplied and restores what a cartridge EXROM chunk replaced. Regression checks in `test/host/dockloader_test.cpp`.
- Still to check: a real PS/2 keyboard. Phase 0 is otherwise done.

### Phase 1: Emulator side of the TS-Pico hook
- Add ports 0Eh/0Fh to `Ports.cpp`'s `is2068` switch, behind a `TSPicoLink` interface.
- Write two back ends:
  - **Loopback replay:** P4's `test/traces` frames, checked offline in a host test.
  - **UART1:** the bus mirror described in §3.1, on GPIO2/34/0/39.
- Load the 32K `TSPICO-23.ROM`: 16K HOME plus 16K EXROM. The emulator already accepts an EXROM of 8K to 64K.
- **Gate 1:** the TS-Pico ROM boots, and with no Pico attached it reads "no device" (`IN 0Fh` = FFh, `IN 0Eh` = 00h).

**Phase 1 status (2026-11-11): Gate 1 passed on the board.** The work was done on branch `tspico-link`, since merged into `main`.

**Code:**
- `TSPicoBus` is the bus mirror, written as pure, host-tested C++.
- `TSPicoUart.cpp` is the UART1 back end.
- The build flag `TS2068_TSPICO` is on in `fabgl2068`. It gives ports 0Eh/0Fh to the TS-Pico instead of the emulator's own VirtualDisk, since the two can't share them.

**Protocol:** [`TSPICO-LINK.md`](TSPICO-LINK.md) defines bridge v1.1, including what the Pico side must do in Phase 2. In outline:
- **Emulator to Pico:** bridge-v1 frames for the Z80's writes, plus op 1 acking each byte taken and op 5 for an underrun.
- **Pico to emulator:**
  - `80 y s` status, where `s` is a write count that rejects stale status.
  - `81 n …` data, with 4096 bytes of credit.
  - `82 'T' 'P' 11h` HELLO.
- **No Pico:** a HELLO probe once a second.
- **Flow control:** RTS/CTS only while connected, because GPI34/39 float.

**Host test:** `test/host/tspico_test.cpp` has 36 unit checks. It also replays both reference traces (fuse-smoke, 2,485 frames; zesarux-smoke, 2,191) against a fake Pico that answers late. Every reply matches and there are no underruns.

**On the board:**
- `/sd/2068/tspico.rom` (`TSPICO-23.ROM`) loads as HOME + 16K EXROM.
- `PRINT IN 15; IN 14` shows `255 0`.
- `CAT` ends in an error report rather than hanging.

**Test tool:** `tools/sendkeys.py` gained `--boot-wait` and `--key-time`. The TS-Pico ROM takes longer to reach the copyright screen, so use `--boot-wait 5 --key-time 0.25`.

**Card:** Zebra's `cart.dck` is parked as `cart.dck.off`.

**Not yet tested:** the UART at 3 Mbaud with a real Pico. That comes with Phase 2.

### Phase 2: Pico side (in tspico-firmware-build)
- Write the `board_link` board layer and a `link` C module. The module owns the UART at 3 Mbaud with RTS/CTS, stays ahead of TX demand, and pushes status changes, borrowing `tsbus.c`'s MQ and `put_block` structure.
- Check it from a PC first: a USB-serial adapter, with fuse-tspico's bridge pointed at the Pico instead of `pico_host`.
- **Gate 2:** the Phase 0 traces from ts2068-p4 replay against a real Pico with matching replies.

### Phase 3: Together
- Wire the Pico to AccessBus and power it from the header's +5V.
- Get `LOAD ""`, SAVE, `tpi:dir`, the file picker, the printer capture, `tpi:boot`/`tpi:dock`/`tpi:zx48` through the image messages, and firmware updates working.
- Measure how long the Z80 stalls per LOAD, against P4's 250 ms.
- **Gate 3:** the fuse-smoke and zesarux-smoke sessions match ts2068-p4's frame for frame, apart from timing.

### Phase 4: Finish
- Add OSD entries for the TS-Pico (status, reset the Pico, choose a ROM).
- Write a wiring guide and a small adapter: a Pico, a microSD socket and an AccessBus plug.
- Do releases (ESP32 image, Pico UF2, SD zip).
- Send `board_link` upstream.

---

### Emulator work outside the phases

**Phase 2 is paused (2026-11-11)** while a Pico board is designed for the AccessBus.

**Video modes (2026-11-11, branch `video-modes`, since merged into `main`): done and checked on the board.**
- **All eight SCLD screen modes are drawn:**
  - standard
  - second screen
  - extended colour (8×1 attributes), and its second-screen variant
  - the four hi-res variants
- **Rules:** which bytes each mode fetches comes from FUSE's `display_render.c`, in `SCLDVideo::cellBytes()`.
- **Hi-res border:** the border takes the hi-res paper colour, as in FUSE.
- **Screen memory:** the screen now always comes from HOME RAM, so a DOCK or EXROM page mapped at 4000h–7FFFh no longer replaces it.

**How it was checked:**
- **Serial screen dump:** in the SerialKeys build, Ctrl-V (`{SHOT}` in `tools/sendkeys.py`) sends the screen memory and the VGA frame.
- **Independent redraw:** `tools/check_screen.py` redraws the frame from the memory by FUSE's rules and compares pixel for pixel.
- **Results on the board:** all nine shots match on all 153,600 pixels. They covered modes 0–7 (`OUT 255,n`) and mode 6 with hi-res colour 3.

**Open:** FUSE draws hi-res ink and paper BRIGHT; the emulator draws them normal. Real hardware decides.

**Native disk (DISK_PROPOSAL.md): Phases 1-7 done 2026-11-11, gates passed on the board.**
- Bare `CAT` lists `/2068/` on the card; ordinary BASIC, and a program using `CAT`, run as before.
- The DOS module is a 16K ROM the emulator overlays on 0000h-3FFFh while it runs; the stock HOME ROM is patched at boot (DISK_PROPOSAL.md §2).
- The card's ROMs and cartridge moved to `/2068/rom/` (`tspico.rom` renamed `tspico.rom.off` so the native disk boots). The firmware still finds them in `/2068/`.
- Phase 2: TAP mount. `LOAD "x.tap"` mounts a TAP and loads from it; SAVE appends to it; `FORMAT`, `CAT ""`, `CAT "x.tap"` and `CLOSE #*` work. A multi-part loader runs from a TAP on the board. The genuine EXROM is now patched at boot too.
- Phase 3: native files. With nothing mounted, LOAD/SAVE/VERIFY/MERGE use files on the card with TS-Pico's +3DOS header (a screen raw), with `Replace?` and Q on a type mismatch. CAT shows each file's type.
- Phase 4: file management. MOVE TO (with `".."`, `"/"`, `"-"`), MOVE (rename, into a folder), COPY (a pattern into a folder), ERASE (silent; a pattern asks Y/N each), FORMAT "dir/", CAT "*.x", LIST "file" (text, or a hex dump). Changing folder unmounts the TAP.
- Phase 5: channels. `OPEN #n,"f:file"[,"r|w|a|u"[b][,reclen]]` with PRINT #, INPUT #, LIST #, INKEY$ #, `TAB` records, `RESTORE #n`, `CAT #s` and CLOSE #, as on the TS-Pico. The CHANS record is 6 bytes; each character is one service call from a HOME stub.
- Phase 6: the `D:` virtual disk is removed (VirtualDisk, its ports, the old ROM patches). The `nopsram` and `psram` builds work again.
- Phase 7: extras. `LOAD "x.dck"`/`"x.rom"` docks a cartridge and resets; a TZX mounts read-only like a TAP (standard-speed blocks); LPRINT/LLIST go to `/2068/print/printer.txt` and COPY to `copyNNN.bmp`; `LOAD ""` with nothing mounted and `MOVE TO ""` open a file picker on the 2068's screen. TZX is tested on the host only.
- The DISK_PROPOSAL phases are all done.
- **Open:** a TZX on the board. Phase 7's TZX support passed the host tests only: the card has no TZX, and nothing on the 2068 can write one. Put a TZX with a standard-speed BASIC loader on the card, then check `CAT "x.tzx"`, `LOAD "x.tzx"` (it loads and runs), `CAT ""` (the turbo count, if any), and that SAVE gives J.

## 5. Questions to settle together

1. **Where should the code live?** Settled: the emulator is in this repository. The Pico side belongs in tspico-firmware-build.
2. **One SD card or two?**
   - The Pico's card holds the TAPs, as on a real TS-Pico. The ESP32 can fetch the ROM from the Pico, so it needs no card.
   - Or the ESP32 keeps its own card for ROMs and snapshots.
   - I lean to one card, on the Pico.
3. **Which Pico?** RP2040 (the v2 firmware's home) or RP2350 (the v3 firmware's home, with more RAM)? Do you have the parts, a Pico and a microSD breakout? Your `Pico-2-Universal-Board` folder suggests you might.
4. **Which emulator base?** Settled: ESPectrum's, with its Z80_JLS core. The **Fuse-derived machine from ts2068-p4** (Fuse core, GPL-2+, beam-following) can be swapped in later if accuracy needs it.
5. **What about the mouse port?** Leave it for the mouse, or free GPIO26/27 (for example for a TS2068 joystick or a second UART)?

## 6. Risks

| Risk | Mitigation |
|---|---|
| Link stalls make LOAD slow or break timing | Local bus mirror, TX streaming ahead of demand, and RTS/CTS. Measure in Phase 3 |
| Power and current from the AccessBus 5V for the Pico and its SD | Measure. The board's MT3608 boost should manage a Pico (about 25 mA) plus an SD card (up to about 100 mA) |
| Upstream doesn't take `board_link` | Pin the firmware and keep stand-ins, as ts2068-p4 does |
| Classic ESP32 internal RAM | About 37 KB is left after setup (Phase 0). Only the mirror's FIFOs (a few KB) are added |
