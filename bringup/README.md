# Phase 0 bring-up

A plain pico-sdk program (no MicroPython) that checks each part of the RP2350pc the emulator needs, on its own (PLAN.md §3, Phase 0). It reports on the HDMI screen and on UART0.

## Building

The build needs pico-sdk **2.2.0** (1.5.x has no RP2350) and the Arm GNU toolchain. Both live in `~/tools`:

```bash
export PICO_SDK_PATH=~/tools/pico-sdk-2.2.0
export PATH=~/tools/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi/bin:$PATH
cmake -S bringup -B build/bringup -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build/bringup
```

The output is `build/bringup/bringup.uf2`.

## Flashing

The RP2350pc has two USB-C ports. **USB-PGM1** is for programming; plugging into it holds the USB hub in reset. **USB-PWR1** is power only. (User manual p.14.)

1. Move the cable to **USB-PGM1**.
2. Hold **BOOT**, switch the power on, then release BOOT. The board shows up as a drive (or `picotool info` finds it).
3. Run `picotool load -x build/bringup/bringup.uf2`, or copy the UF2 onto the drive.
4. **To test USB keyboards:** switch off, move the cable to **USB-PWR1**, and switch on again. While the PGM port is plugged in, the hub stays in reset.

## What it checks

| Test | Pass looks like |
|---|---|
| Clocks | sys 252 MHz, hstx 126 MHz (a 25.2 MHz pixel clock) |
| Video | 640×480 picture: a blue title bar, 16 colour bars (8 normal, then 8 BRIGHT, in TS2068 order: black, blue, red, magenta, green, cyan, yellow, white), and text in the 2068 font. The status line shows the frame rate measured from the video interrupt |
| PSRAM | 8 MB found on CS1 (GPIO8), the pattern test passes, plus write and read speeds |
| Audio | Two short tones (440 Hz, then 880 Hz) from the speakers or the headphone jack |
| microSD | Card size and CID, the root directory of a FAT card, read speed |
| USB | `USB: device N up` for the hub and for each device; keys typed on a keyboard echo with a click |

## Keys

| Key | Action |
|---|---|
| F1 | Switches the video between 525 lines (60.00 Hz) and 524 lines (60.11 Hz, the TS2068's rate). Note whether the monitor keeps the picture |
| F2 | Plays a 440 Hz tone |
| `+` / `-` | Amplifier volume (PWM on AMP_VOL, GPIO34) |
| F3 | Runs the SD test again |

## Serial

UART0 is on **UEXT1** pin 3 (TX, GPIO0) and pin 4 (RX, GPIO1), at 115200 8N1 and 3.3 V. It gets the same text as the screen.
