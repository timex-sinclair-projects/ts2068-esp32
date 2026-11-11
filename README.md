# ts2068-esp32

A standalone Timex/Sinclair 2068 with TS-Pico functionality on the **Olimex ESP32-SBC-FabGL** (Rev B): VGA, PS/2 keyboard, microSD and audio on a single board.

The emulator in this repository is **ESP2068**, a TS2068 port of ESPectrum. It used to be a separate repository and was merged in here with its full history, including ESPectrum's.

## History

This project is built on **[ESPectrum](https://github.com/EremusOne/ESPectrum)** by Víctor Iborra ("EremusOne"), a ZX Spectrum emulator for ESP32 boards that already runs on the ESP32-SBC-FabGL. ESPectrum has its own lineage:
- It grew out of David Crespo's [ZX-ESPectrum-Wiimote](https://github.com/dcrespo3d/ZX-ESPectrum-Wiimote).
- That is a fork of [ZX-ESPectrum](https://github.com/rampa069/ZX-ESPectrum) by Ramón Martínez ("Rampa") and Jorge Fuertes ("Queru").
- ZX-ESPectrum was inspired by Pete Todd's [PaseVGA](https://github.com/retrogubbins/paseVGA).

ESPectrum also brings in:
- a Z80 core derived from José Luis Sánchez's [z80cpp](https://github.com/jsanchezv/z80cpp)
- the VGA driver from bitluni's [ESP32Lib](https://github.com/bitluni/ESP32Lib)
- AY emulation from Alexander Sashnov's libayemu
- the PS/2 driver from Fabrizio di Vittorio's [FabGL](https://github.com/fdivitto/FabGL)

**ESP2068** turns ESPectrum into a Timex/Sinclair 2068. It adds the SCLD and its screen modes, the HOME/DOCK/EXROM memory map, the AY on the 2068's ports, and `.DCK` cartridges. This project adds the TS-Pico link and native disk commands on top of that.

The plan started out on another board. Draft 1 targeted the Olimex RP2350pc, which would have run the TS-Pico firmware on the same chip as the emulator. Draft 2 moved to the ESP32-SBC-FabGL, the board actually on hand, and to ESPectrum. A classic ESP32 has no room for the firmware, so draft 2 moves it to a separate Pico.

## How it fits together

- **The ESP32 runs the emulator.** ESP2068 runs the Z80 on core 0, with VGA video and audio on core 1.
- **A Raspberry Pi Pico runs the real TS-Pico firmware.** The firmware runs unchanged, apart from a new board layer. It connects to the ESP32's second hardware UART on the **AccessBus** header (3 Mbaud, RTS/CTS).
- **The ESP32 keeps a local mirror of the TS-Pico's bus state**, so a port access doesn't wait for a round trip over the link. The wire protocol is bridge v1 from the TS-Pico emulator bridge, extended as v1.1, so fuse-tspico and zesarux-tspico keep working.

[BOARD_PLAN.md](BOARD_PLAN.md) has the board details, the options that were considered and ruled out, the link budget and the risks.

## Building

The build uses [PlatformIO](https://platformio.org). The `fabgl2068` environment is the TS2068 build for the ESP32-SBC-FabGL:

```bash
pio run -e fabgl2068 -t upload
```

The host tests are in `test/host/`. [CLAUDE.md](CLAUDE.md) and [PLAN.md](PLAN.md) have the details of the port, and BOARD_PLAN.md §4 has the board setup and SD card paths.

## Status (2026-11-11)

| Phase | State |
|---|---|
| 0. ESP2068 on the board | Boots to the copyright screen at full speed. Keyboard and sound are not yet tested. |
| 1. Emulator side of the TS-Pico link | Gate 1 passed on the board. The link has not been tested with a real Pico yet. |
| 2. Pico side (in tspico-firmware-build) | Paused while a Pico board for the AccessBus is designed. |
| 3. Together | Not started. |
| 4. Finish (OSD, wiring guide, adapter, releases) | Not started. |

Done outside the phases:
- **All eight SCLD screen modes are drawn.** On the board they match an independent redraw pixel for pixel.
- **Native disk commands:** the 2068's own BASIC keywords (`CAT`, `LOAD`, `SAVE`, `OPEN #` and others) work on the SD card. Phases 1–7 of [DISK_PROPOSAL.md](DISK_PROPOSAL.md) pass their gates on the board.

## Contents

| Path | What it is |
|---|---|
| [BOARD_PLAN.md](BOARD_PLAN.md) | The project plan for this board: architecture, phases and gates, open questions, risks |
| [PLAN.md](PLAN.md) | The phased plan for porting ESPectrum to the TS2068 |
| [TS2068-ESPECTRUM-PORT-PLAN.md](TS2068-ESPECTRUM-PORT-PLAN.md) | The port's technical design: SCLD registers, memory model, display, timing, cartridges |
| [TSPICO-LINK.md](TSPICO-LINK.md) | The TS-Pico link protocol (bridge v1.1) between the ESP32 and the Pico |
| [DISK_PROPOSAL.md](DISK_PROPOSAL.md) | Native disk commands: design, behaviour and phases |
| [CLAUDE.md](CLAUDE.md) | Working notes on the port's state and findings |
| [ESPECTRUM_README.md](ESPECTRUM_README.md) | ESPectrum's own README, with its full credits |
| `src/`, `include/`, `components/` | The emulator (ESPectrum plus the TS2068 work) |
| `dos/` | The native disk's DOS ROM (Z80 assembly) |
| `test/` | Host tests and the `.DCK` cartridge tools |
| `tools/` | Build and test tools: the DOS ROM build, serial keys, screen check |

## Related projects

- [tspico-firmware-build](https://github.com/timex-sinclair-projects/tspico-firmware-build): the TS-Pico firmware
- [ts2068-p4](https://github.com/timex-sinclair-projects/ts2068-p4): the same goal on an ESP32-P4 board
- [fuse-tspico](https://github.com/timex-sinclair-projects/fuse-tspico) and [zesarux-tspico](https://github.com/timex-sinclair-projects/zesarux-tspico): desktop emulators bridged to the TS-Pico firmware

## License

GNU General Public License v3.0; see [LICENSE](LICENSE). That's ESPectrum's license, and this project is based on it. Parts brought in by ESPectrum keep their own notices; see ESPECTRUM_README.md.
