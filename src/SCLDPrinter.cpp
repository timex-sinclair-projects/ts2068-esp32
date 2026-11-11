/*

ESP2068 — TS2068 port of ESPectrum

SCLDPrinter.cpp — see include/SCLDPrinter.h.

The stylus/motor timing model below is an original reimplementation of
the *behavior* documented in the TS2068 Technical Manual (Sec.
2.1.13.3/4.1.3) and independently confirmed against FUSE's
printer_zxp_read()/printer_zxp_write() (peripherals/printer.c) --
same observable status/timing semantics, different code and different
license (FUSE is GPL-2.0-or-later; this file, like the rest of this
port, is GPL-3.0-or-later), and with FUSE's pixel-buffer/file-output
half deliberately left out (see the header comment).

The four magic constants below all come from the real device's
physical geometry, cross-checked against FUSE's own (identically
named in spirit, differently named in code) constants:

  - CYCLES_PER_PIXEL_FAST/_SLOW (220/440): T-states the print head
    takes to advance one pixel at each of the two motor speeds
    (Technical Manual write-table bit 1: 0 = fast, 1 = slow). FUSE
    derives these as 440/zxpspeed with zxpspeed 2 (fast) or 1 (slow);
    inverted here into the two products directly since a fixed
    "cycles per pixel" is simpler to reason about than an indirect
    speed-then-divide.
  - LINE_PITCH (384) / LINE_ADVANCE_THRESHOLD (320): the print head's
    full physical travel per line (384 pixel-equivalents), of which
    only the first 256 are the printable area and 320 is where a
    write/read starts treating the head as having reached the next
    line -- the 64-pixel gap in each case is lead-in/lead-out margin.
  - HEAD_LEAD_IN (64): the head doesn't start over the paper the
    instant the motor starts; there's a fixed run-up before pixel 0.

Based on ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC
Copyright (c) 2023-2025 Víctor Iborra [Eremus] and 2023 David Crespo [dcrespo3d]
https://github.com/EremusOne/ESPectrum

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/

#include "SCLDPrinter.h"

namespace {
    constexpr uint16_t CYCLES_PER_PIXEL_FAST = 220;
    constexpr uint16_t CYCLES_PER_PIXEL_SLOW = 440;
    constexpr int32_t  LINE_PITCH = 384;
    constexpr int32_t  LINE_ADVANCE_THRESHOLD = 320;
    constexpr int32_t  HEAD_LEAD_IN = 64;
    constexpr int32_t  BUSY_WINDOW_START = -10; // x in (BUSY_WINDOW_START, 0) => head is in the pre-paper "about to print" window

    // Real device write cadence tops out around a few hundred T-states
    // per port write; a session where nobody has touched the port in
    // 400 TS2068 frames' worth of cycles (~6.6s at 60Hz) is idle, not
    // mid-print. Matches FUSE's own "limit height of blank paper"
    // clamp (printer_zxp_read/write's `if(frame>400) frame=400;`),
    // expressed directly in cycles since this class tracks one
    // monotonic counter rather than FUSE's separate (frame, cycle)
    // pair. 58688 = TS2068's TSTATES_PER_FRAME_2068 (cpuESP.h),
    // duplicated here rather than included, matching SCLDVideo.h's
    // convention of staying dependency-free.
    constexpr uint64_t MAX_ELAPSED_CYCLES = 400ull * 58688ull;

    uint64_t clampedElapsed(uint64_t cycles, uint64_t originCycles) {
        uint64_t elapsed = cycles - originCycles;
        return elapsed > MAX_ELAPSED_CYCLES ? MAX_ELAPSED_CYCLES : elapsed;
    }
}

bool SCLDPrinter::motorOn = false;
bool SCLDPrinter::stylusDown = false;
uint16_t SCLDPrinter::cyclesPerPixel = CYCLES_PER_PIXEL_FAST;
uint16_t SCLDPrinter::pendingCyclesPerPixel = 0;
uint64_t SCLDPrinter::originCycles = 0;
int32_t SCLDPrinter::lastPixelX = -1;

void SCLDPrinter::reset() {
    motorOn = false;
    stylusDown = false;
    cyclesPerPixel = CYCLES_PER_PIXEL_FAST;
    pendingCyclesPerPixel = 0;
    originCycles = 0;
    lastPixelX = -1;
}

uint8_t SCLDPrinter::read(uint64_t cycles) {

    if (!motorOn) return 0x3e; // idle: not busy, no pixel pending

    uint64_t elapsed = clampedElapsed(cycles, originCycles);
    int32_t cpp = cyclesPerPixel;
    int32_t pendingCpp = pendingCyclesPerPixel;
    int32_t x = (int32_t)(elapsed / (uint64_t)cpp) - HEAD_LEAD_IN;
    int32_t pix = lastPixelX;

    // Head may have crossed one or more line boundaries since the last
    // write; walk it back into this line's coordinate space without
    // mutating any persisted state (read() is a pure query).
    while (x > LINE_ADVANCE_THRESHOLD) {
        pix = -1;
        x -= LINE_PITCH;
        if (pendingCpp) {
            x = (x + HEAD_LEAD_IN) * cpp;
            cpp = pendingCpp;
            x = x / cpp - HEAD_LEAD_IN;
            pendingCpp = 0;
        }
    }

    uint8_t ans = ((x > BUSY_WINDOW_START && x < 0) || stylusDown) ? 0xbe : 0x3e;
    if (x > pix) ans |= 0x01; // "ready for next pixel"
    return ans;
}

void SCLDPrinter::write(uint8_t value, uint64_t cycles) {

    bool motorStop = value & 0x04;

    if (!motorOn) {
        if (motorStop) return; // already stopped; stop-while-stopped is a no-op
        motorOn = true;
        cyclesPerPixel = (value & 0x02) ? CYCLES_PER_PIXEL_SLOW : CYCLES_PER_PIXEL_FAST;
        pendingCyclesPerPixel = 0;
        originCycles = cycles;
        stylusDown = value & 0x80;
        lastPixelX = -1;
        return;
    }

    uint64_t elapsed = clampedElapsed(cycles, originCycles);
    int32_t cpp = cyclesPerPixel;
    int32_t x = (int32_t)(elapsed / (uint64_t)cpp) - HEAD_LEAD_IN;

    while (x >= LINE_ADVANCE_THRESHOLD) {
        originCycles += (uint64_t)cpp * LINE_PITCH;
        x -= LINE_PITCH;
        if (pendingCyclesPerPixel) {
            cyclesPerPixel = pendingCyclesPerPixel;
            pendingCyclesPerPixel = 0;
            cpp = cyclesPerPixel;
            x = (x + HEAD_LEAD_IN) * cyclesPerPixel / cpp - HEAD_LEAD_IN;
        }
    }
    if (x < 0) x = -1;

    if (motorStop) {
        motorOn = false;
        stylusDown = false;
        return;
    }

    lastPixelX = x;
    stylusDown = value & 0x80;
    uint16_t requestedCpp = (value & 0x02) ? CYCLES_PER_PIXEL_SLOW : CYCLES_PER_PIXEL_FAST;
    if (x < 0) {
        // Still in the lead-in margin for this line: apply the new
        // speed immediately, nothing pending to reconcile at a line
        // boundary yet.
        cyclesPerPixel = requestedCpp;
        pendingCyclesPerPixel = 0;
    } else {
        pendingCyclesPerPixel = (requestedCpp == cyclesPerPixel) ? 0 : requestedCpp;
    }
}
