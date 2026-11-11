# ts2068-esp32

A standalone Timex/Sinclair 2068 with TS-Pico functionality on the **Olimex ESP32-SBC-FabGL** (Rev B): VGA, PS/2 keyboard, microSD and audio on a single board.

This repository holds the plan and design documents for the project, plus bring-up code and tools. The emulator itself is [ESP2068](https://github.com/factus10/ESP2068), a TS2068 port of ESPectrum. Work for this project happens on its branches.

## How it fits together

- **The ESP32 runs the emulator.** ESP2068 runs the Z80 on core 0, with VGA video and audio on core 1.
- **A Raspberry Pi Pico runs the real TS-Pico firmware.** The firmware runs unchanged, apart from a new board layer. It connects to the ESP32's second hardware UART on the **AccessBus** header (3 Mbaud, RTS/CTS).
- **The ESP32 keeps a local mirror of the TS-Pico's bus state**, so a port access doesn't wait for a round trip over the link. The wire protocol is bridge v1 from the TS-Pico emulator bridge, extended as v1.1, so fuse-tspico and zesarux-tspico keep working.

[PLAN.md](PLAN.md) has the board details, the options that were considered and ruled out, the link budget and the risks.

## Status (2026-11-11)

| Phase | State |
|---|---|
| 0. ESP2068 on the board | Boots to the copyright screen at full speed. Keyboard and sound are not yet tested. |
| 1. Emulator side of the TS-Pico link | Gate 1 passed on the board (ESP2068 branch `tspico-link`). The link has not been tested with a real Pico yet. |
| 2. Pico side (in tspico-firmware-build) | Paused while a Pico board for the AccessBus is designed. |
| 3. Together | Not started. |
| 4. Finish (OSD, wiring guide, adapter, releases) | Not started. |

Done outside the phases:
- **All eight SCLD screen modes are drawn.** On the board they match an independent redraw pixel for pixel (ESP2068 branch `video-modes`).
- **Native disk commands:** the 2068's own BASIC keywords (`CAT`, `LOAD`, `SAVE`, `OPEN #` and others) work on the SD card. Phases 1–7 of [DISK_PROPOSAL.md](DISK_PROPOSAL.md) pass their gates on the board.

## Contents

| Path | What it is |
|---|---|
| [PLAN.md](PLAN.md) | The project plan: board, architecture, phases and gates, open questions, risks |
| [DISK_PROPOSAL.md](DISK_PROPOSAL.md) | Native disk commands for ESP2068: design, behaviour and phases |
| [bringup/](bringup/) | A pico-sdk bring-up program for the **Olimex RP2350pc**, the target in draft 1 of the plan. It checks HDMI, PSRAM, SD, USB keyboard and audio. See its [README](bringup/README.md). |
| [tools/make_font.py](tools/make_font.py) | Makes `bringup/font8x8.h` from the character set in a TS2068 ROM |

## Related projects

- [ESP2068](https://github.com/factus10/ESP2068): the emulator
- [tspico-firmware-build](https://github.com/timex-sinclair-projects/tspico-firmware-build): the TS-Pico firmware
- [ts2068-p4](https://github.com/timex-sinclair-projects/ts2068-p4): the same goal on an ESP32-P4 board
- [fuse-tspico](https://github.com/timex-sinclair-projects/fuse-tspico) and [zesarux-tspico](https://github.com/timex-sinclair-projects/zesarux-tspico): desktop emulators bridged to the TS-Pico firmware

## License

MIT; see [LICENSE](LICENSE). Some files carry their own licenses:
- `bringup/pico_sdk_import.cmake` and `bringup/boards/olimex_rp2350pc.h` are BSD-3-Clause (Raspberry Pi), and `bringup/dvi.c` is based on BSD-3-Clause pico-examples code. Their notices are kept in the files.
- `bringup/font8x8.h` is generated from the TS-Pico ROM, which is MIT.

ESP2068 and the emulators it builds on (ESPectrum, FabGL) are GPL-3. This MIT license doesn't cover them.
