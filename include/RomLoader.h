/*

ESP2068 — TS2068 port of ESPectrum

RomLoader.h — loads the TS2068 HOME ROM (16K) and EXROM (8K, or any
larger whole multiple of 8K up to 64K — see SCLD.cpp's loadExromChunk()
comment for why EXROM isn't fixed at 8K) from raw binary files. New for
this port; there is no upstream ESPectrum
equivalent. Deliberately does *not* follow ESPectrum's existing "custom
ROM" mechanism for the Spectrum machines (OSDMain.cpp's updateROM(),
Config.cpp) — that mechanism self-reflashes the running OTA partition
with a patched firmware image located by scanning flash for a magic
byte sequence baked into a compiled-in placeholder array. It's a clever
trick but deeply specific to the Spectrum machines' multi-ROM-variant
setup; a TS2068 loader is much better served by the plain "read a file
into a buffer, hand it to SCLD" shape this file has instead.

No ROM image is embedded in this repo or read from anywhere but a
caller-supplied path/buffer — see test/dock/README.md's provenance
policy, which applies here too. This file's own test
(test/host/romloader_test.cpp) validates against real ROM content by
pointing at a local reference-library path outside this repo, and skips
that part gracefully if the path isn't present.

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

#ifndef RomLoader_h
#define RomLoader_h

#include <inttypes.h>
#include <cstddef>
#include <string>

class RomLoader {
public:

    // Conventional SD-card paths this port looks for a HOME ROM/EXROM at
    // boot, pending real OSD file-picker wiring (Phase 3, PLAN.md). Not
    // configurable yet — a fixed bootstrapping convention, not a final
    // interface: drop your own ROM dumps here rather than a menu pick.
    static const char* DEFAULT_HOME_ROM_PATH;
    static const char* DEFAULT_EXROM_PATH;

    // Reads a 16K raw HOME ROM binary from fn and loads it via
    // SCLD::loadHomeRom(). Returns false (SCLD's HOME ROM content
    // untouched) if the file can't be opened/read or isn't exactly 16K.
    static bool loadHomeRomFromFile(const std::string& fn);

    // Reads a raw EXROM binary from fn (any whole multiple of 8K, 8K to
    // 64K) and loads it via SCLD::loadExromImage(). Same failure
    // semantics as loadHomeRomFromFile(), plus: fails if the file size
    // isn't a whole 8K multiple in range. Allocates and owns the buffer
    // SCLD ends up pointing at (matching loadExromImage()'s existing
    // ownership-transfer contract, same as DockLoader's usage).
    static bool loadExromFromFile(const std::string& fn);

    // Buffer-based versions (host-testable, no file I/O). loadHomeRom
    // copies into storage SCLD already owns, so it needs no allocation of
    // its own; loadExrom allocates the buffer SCLD ends up owning, so
    // RomLoader.cpp still carries the same ESP2068_HOST_TEST
    // allocator-shim convention as SCLD.cpp/DockLoader.cpp for that one.
    static bool loadHomeRomFromBuffer(const uint8_t* data, size_t size);
    static bool loadExromFromBuffer(const uint8_t* data, size_t size);

};

#endif // RomLoader_h
