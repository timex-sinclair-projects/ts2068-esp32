/*

ESP2068 — TS2068 port of ESPectrum

SCLD.cpp — see include/SCLD.h for the interface contract this implements.

Backing store: HOME ROM (16K, chunks 0-1) + HOME RAM (48K, chunks 2-7) are
real allocated memory, always present. DOCK (up to 8x8K, one per chunk) and
EXROM (up to 8x8K, chunk-indexed exactly like DOCK — see loadExromImage())
start unpopulated and resolve to a shared, zeroed, read-only "empty socket"
page so memChunk[] is never null.

Compiles two ways:
  - As part of the ESP-IDF/PlatformIO firmware (src/CMakeLists.txt globs
    everything under src/, so this needs no extra build-file wiring): allocates
    from internal SRAM via heap_caps_calloc/MALLOC_CAP_8BIT, matching
    MemESP.cpp's existing allocation pattern. Nothing here needs PSRAM,
    per TS2068-ESPECTRUM-PORT-PLAN.md's memory budget.
  - Host-side, when ESP2068_HOST_TEST is defined (see test/host/), using
    plain calloc() instead — same resolve logic, no ESP-IDF dependency,
    so the unit test in test/host/scld_test.cpp exercises the real code,
    not a reimplementation of it.

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

#include "SCLD.h"
#include <cstddef>
#include <cstring>

#ifdef ESP2068_HOST_TEST
#include <cstdlib>
static inline uint8_t* scld_alloc8k() { return (uint8_t*) calloc(0x2000, 1); }
#else
#include "esp_heap_caps.h"
// PSRAM first when there is any: the TS2068's 512-wide VGA mode needs most
// of the internal DMA-capable RAM for its line buffers.
static inline uint8_t* scld_alloc8k() {
    uint8_t* p = (uint8_t*) heap_caps_calloc(0x2000, 1, MALLOC_CAP_SPIRAM);
    return p ? p : (uint8_t*) heap_caps_calloc(0x2000, 1, MALLOC_CAP_8BIT);
}
#endif

uint8_t SCLD::mmuSelect = 0x00;
uint8_t SCLD::displayControl = 0x00;
uint8_t SCLD::videoMode = 0;
uint8_t SCLD::hiresInkPaper = 0;
bool    SCLD::intInhibit = false;
bool    SCLD::exromSelect = false;

uint8_t* SCLD::memChunk[8] = { nullptr };
bool     SCLD::memChunkReadOnly[8] = { false };

namespace {

    // chunks 0-1 : HOME ROM (0x0000-0x3fff). chunks 2-7 : HOME RAM (0x4000-0xffff).
    uint8_t* homeChunk[8] = { nullptr };

    // One pointer per possible chunk position; only the ones a loaded .DCK
    // actually populates are non-null. Real DOCK carts top out near 24K (3
    // chunks) but the register only supports up to 8, so the model does too.
    uint8_t* dockChunk[8] = { nullptr };
    bool     dockChunkWritable[8] = { false };

    // One pointer per possible chunk position, exactly like dockChunk[]
    // above — corrected 2026-07-11 from an earlier "single 8K image,
    // chip-select-mirrored across every chunk" assumption that real
    // evidence has since contradicted: the project owner's own TS-Pico
    // EXROM replacement (gus-exrom.rom, in the local reference library)
    // is genuinely 16384 bytes, exactly double stock's 8192, and its
    // boot-time patch specifically activates EXROM in chunks 0 *and* 1
    // together to reach "the EXROM's extended code area" (gus-rom-
    // analysis.md) — which would be pointless if both chunks showed an
    // identical mirror. Real EXROM chip-select is per-chunk, same as
    // DOCK's already-correct model, not a single mirrored image.
    uint8_t* exromChunk[8] = { nullptr };

    // Shared fallback for any DOCK/EXROM chunk nothing has loaded yet.
    // Zeroed, read-only: an empty cartridge socket doesn't crash the CPU
    // core, it just reads back a fixed, harmless value.
    uint8_t emptySocket[0x2000] = { 0 };

}

void SCLD::allocateMemory() {
    // c 0-1: HOME ROM (0x0000-0x3fff). c 2-7: HOME RAM (0x4000-0xffff).
    for (int c = 0; c < 8; c++) homeChunk[c] = scld_alloc8k();
}

bool SCLD::loadHomeRom(const uint8_t* data, size_t size) {
    if (size != 0x4000) return false;
    if (!homeChunk[0] || !homeChunk[1]) return false; // allocateMemory() not called yet
    memcpy(homeChunk[0], data, 0x2000);
    memcpy(homeChunk[1], data + 0x2000, 0x2000);
    return true;
}

uint8_t* SCLD::homePage(int chunk) {
    return homeChunk[chunk];
}

uint8_t* SCLD::dockPage(int chunk) {
    return dockChunk[chunk] ? dockChunk[chunk] : emptySocket;
}

uint8_t* SCLD::exromPage(int chunk) {
    return exromChunk[chunk] ? exromChunk[chunk] : emptySocket;
}

void SCLD::loadDockChunk(int chunk, uint8_t* data, bool writable) {
    dockChunk[chunk] = data;
    dockChunkWritable[chunk] = writable;
}

void SCLD::unloadDockChunk(int chunk) {
    dockChunk[chunk] = nullptr;
    dockChunkWritable[chunk] = false;
}

void SCLD::loadExromChunk(int chunk, uint8_t* data) {
    if (chunk >= 0 && chunk < 8) exromChunk[chunk] = data;
}

void SCLD::unloadExromChunk(int chunk) {
    if (chunk >= 0 && chunk < 8) exromChunk[chunk] = nullptr;
}

void SCLD::loadExromImage(uint8_t* data, int numChunks) {
    for (int c = 0; c < 8; c++) {
        loadExromChunk(c, (c < numChunks) ? (data + c * 0x2000) : nullptr);
    }
}

void SCLD::resolveMemChunks() {
    for (int c = 0; c < 8; c++) {
        if (mmuSelect & (1 << c)) {
            if (exromSelect) {
                memChunk[c] = exromPage(c);
                memChunkReadOnly[c] = true; // EXROM is always ROM
            } else {
                memChunk[c] = dockPage(c);
                memChunkReadOnly[c] = dockChunk[c] ? !dockChunkWritable[c] : true;
            }
        } else {
            memChunk[c] = homePage(c);
            memChunkReadOnly[c] = (c < 2); // HOME ROM below 0x4000, RAM above
        }
    }
}

void SCLD::OUT_F4(uint8_t value) {
    mmuSelect = value;
    resolveMemChunks();
}

uint8_t SCLD::IN_F4() {
    return mmuSelect;
}

void SCLD::OUT_FF(uint8_t value) {
    displayControl = value;
    videoMode      = value & 0x07;
    hiresInkPaper  = (value >> 3) & 0x07;
    intInhibit     = value & 0x40;
    exromSelect    = value & 0x80;
    resolveMemChunks();
}

uint8_t SCLD::IN_FF() {
    return displayControl;
}

void SCLD::reset() {
    mmuSelect = 0x00;
    OUT_FF(0x00); // also resolves memChunk[]
}
