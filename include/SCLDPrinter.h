/*

ESP2068 — TS2068 port of ESPectrum

SCLDPrinter.h — port 0xFB (the TS 2040/ZX-Printer-compatible stylus
printer) status/timing protocol. New for this port; there is no
upstream ESPectrum equivalent.

The real device is not a byte-in/byte-out port at all: per the TS2068
Technical Manual (Sec. 2.1.13.3/4.1.3) and confirmed against FUSE
(peripherals/printer.c's printer_zxp_read/printer_zxp_write — a
mature, real, GPL, TS2068-emulating codebase, /Users/david/Downloads/
fuse-1.9.0 locally), it is a synchronous, software-timed stylus/motor
interface: writes latch a stylus up/down bit and a paper-feed speed,
and the "current pixel position" is derived purely from elapsed CPU
cycles since the motor started, not from any byte payload. Reads
return a status byte (busy/ready) computed the same way. Getting the
*timing* arithmetic right is what makes real ROM printer routines
(HOME ROM's PRSCAN/K_DUMP, Technical Manual Sec. 4.1.3) poll correctly
instead of hanging or mis-pacing themselves — that's this class's whole
job.

Deliberately out of scope: this class tracks stylus/motor state and
answers "is the head busy, is it ready for the next pixel" correctly,
but it does not accumulate pixel rows into any image or write anything
to SD. FUSE's own printer_zxp_write() does both (see its zxpline[]
buffer and printer_zxp_output_line()); this port only replicates the
status/timing half, which is the half real software actually depends
on for correctness (a print routine that never got a "ready" bit back
would hang; one that gets correct pacing but produces no visible
output just... produces no visible output, a real but separate gap,
tracked in PLAN.md). Persisting printed pages to SD is future work.

Also deliberately out of scope: this always behaves as if a working
printer is attached and responding (idle at boot, i.e. motor off,
exactly like real hardware powers up) — never the "no printer
attached" `0xff` sentinel FUSE can return when its own optional
capture setting is off. ESP2068 has no config surface for "user chose
not to attach a printer," and the task is to make the port work, not
to model its absence.

Kept fully framework-agnostic (no ESP-IDF, no CPU.h/Video.h
dependency) so it can be host-tested (test/host/scldprinter_test.cpp)
exactly like SCLDVideo.h. Callers pass in an absolute, monotonically
increasing cycle count — Ports.cpp supplies `CPU::global_tstates +
CPU::tstates`, the same "whole-session absolute T-state" idiom
Tape.cpp already uses for its own timing (see e.g. Tape.cpp's
`tapeStart`/`tapeCurrent` arithmetic) — rather than this class reaching
into CPU/Video globals itself.

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

#ifndef SCLDPrinter_h
#define SCLDPrinter_h

#include <inttypes.h>

class SCLDPrinter {
public:

    // Power-on/reset state: motor off, stylus up. Matches FUSE's
    // printer_zxp_reset(), which forces the same end state by replaying
    // a synthetic "motor stop" write rather than resetting fields
    // directly -- this does it directly, since there's no `zxpline[]`
    // buffer here that a synthetic write would otherwise need to flush.
    static void reset();

    // Port 0xFB read: the status byte real ROM printer routines poll.
    // `cycles` is the caller's absolute, monotonically increasing
    // T-state count (see this header's top comment). Pure/idempotent --
    // does not mutate state, matching FUSE's printer_zxp_read() (all
    // its position math is on local copies of the persisted fields).
    static uint8_t read(uint64_t cycles);

    // Port 0xFB write: `value`'s bits 7/1/2 are stylus/speed/motor-stop
    // (Technical Manual Sec. 2.1.13.3's write table) -- see SCLDPrinter.cpp
    // for the exact bit meanings. `cycles` is the same absolute T-state
    // count as read().
    static void write(uint8_t value, uint64_t cycles);

private:

    static bool motorOn;
    static bool stylusDown;
    static uint16_t cyclesPerPixel;         // CYCLES_PER_PIXEL_FAST or _SLOW
    static uint16_t pendingCyclesPerPixel;  // 0 = no speed change pending
    static uint64_t originCycles;           // absolute cycle count when the current print line began
    static int32_t lastPixelX;              // last x position written this line, -1 = none yet

};

#endif // SCLDPrinter_h
