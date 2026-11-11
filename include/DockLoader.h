/*

ESP2068 — TS2068 port of ESPectrum

DockLoader.h — .DCK cartridge container loader (slice 3, PLAN.md Phase 2).
New for this port; there is no upstream ESPectrum equivalent. Parses the
.DCK format documented in test/dock/README.md and populates SCLD's
DOCK/EXROM backing store (include/SCLD.h) accordingly.

See src/DockLoader.cpp's top comment for the autostart design. An
earlier version of this file didn't parse the cartridge's own in-ROM
LROS header at all (the byte layout wasn't pinned down with enough
confidence from research alone) and just relied on the Z80 landing at
PC=0 after reset — verified wrong (2026-07-10) against a real LROS
cartridge (Zebra OS-64, see PLAN.md's slice 3 writeup): its header
occupies bytes 0-4 of chunk 0, and the actual entry point is a JP
instruction sitting at whatever address the header's offset 2-3 field
specifies (here, $0005, not $0000). Executing from PC=0 unconditionally
would have executed the header's own bytes as garbage instructions.
This version reads that field for real.

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

#ifndef DockLoader_h
#define DockLoader_h

#include <inttypes.h>
#include <cstddef>
#include <string>

class DockLoader {
public:

    // Parses .DCK bytes already in memory and, only if the whole file is
    // structurally valid, applies it to SCLD (all-or-nothing — a bad file
    // never leaves SCLD half-updated). `data`/`size` are the caller's;
    // DockLoader copies whatever it needs and does not hold onto the
    // pointer afterwards. Returns false and leaves SCLD's cartridge state
    // untouched on any structural error (truncated header, truncated
    // chunk data — mirrors test/dock/dck_format.py's parse() exactly).
    //
    // Host-testable: no ESP-IDF dependency, works identically under
    // ESP2068_HOST_TEST (see SCLD.cpp for the same convention).
    static bool loadFromBuffer(const uint8_t* data, size_t size);

    // Reads fn fully into memory (plain stdio — fopen/fread/fclose, no
    // ESP-IDF-specific headers, so this is host-testable too) and calls
    // loadFromBuffer(). Returns false if the file can't be opened/read or
    // fails loadFromBuffer()'s validation.
    static bool loadFromFile(const std::string& fn);

    // Frees whatever the last successful load allocated and clears SCLD's
    // DOCK/EXROM chunks back to unpopulated (cartridge eject). Safe to
    // call with nothing loaded.
    static void unload();

    // True if the last successful load's DOCK bank had chunk 0 present —
    // the LROS convention (see DockLoader.cpp) for "this cartridge takes
    // over at reset."
    static bool hasAutostart();

    // The port 0xF4 value to apply on reset for the autostart candidate:
    // one bit per DOCK chunk the cartridge actually populated. Only
    // meaningful when hasAutostart() is true.
    static uint8_t autostartMmuSelect();

    // The address to set the Z80's PC to on autostart: the LROS header's
    // own start-address field (chunk 0, offset 2-3, little-endian) --
    // NOT necessarily 0. Real cartridges put header metadata at 0-4 and
    // point this field at their actual entry point (which is often a
    // short JP a few bytes in, but is not guaranteed to be). Only
    // meaningful when hasAutostart() is true.
    static uint16_t autostartEntryPoint();

};

#endif // DockLoader_h
