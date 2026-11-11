/*

ESP2068 — TS2068 port of ESPectrum

DockLoader.cpp — see include/DockLoader.h.

Container format (bank ID + 8 chunk-type bytes per bank, 8192-byte images
for present chunks, repeat to EOF) matches test/dock/dck_format.py
exactly — that Python module was written first and cross-checked against
two independent copies of the source reference; this is a direct C++
port of the same logic, not a re-derivation.

Autostart design, and a real bug this caught:

TS2068-ESPECTRUM-PORT-PLAN.md's "Cartridge loading" section says LROS/
AROS autostart cartridges "page themselves in and vector execution on
reset" and points at check_trdos (Z80_JLS.cpp) as a working template —
that mechanism watches the Z80's PC on every jump/call/return, because
TR-DOS can be entered at any point during a running program, not just at
boot. This does NOT use that pattern: LROS cartridges are defined to
always autorun "after initialization is finished" (boot only, unlike
TR-DOS), so a one-shot apply at reset is enough — no continuous PC watch
needed.

An earlier version of this file also skipped parsing the cartridge's own
in-ROM header at all, reasoning that LROS "takes total control from
address 0" so the Z80 already lands in the right place after
Z80::reset(). **That reasoning was wrong, caught by testing against a
real cartridge** (Zebra OS-64.BIN, see PLAN.md's slice 3 "real ROM/DOCK
end-to-end test" writeup): a real LROS header occupies bytes 0-4 of
chunk 0 as pure metadata (unused byte, cartridge-type byte, a 2-byte
start-address field, a chunk-spec byte) — NOT executable code — and the
start-address field points at wherever the cartridge's actual entry
point is, which is often close to but not necessarily exactly address 0
(Zebra's is $0005: a JP instruction to $0D9E). Executing from PC=0
unconditionally, as the earlier version did, would run the header bytes
themselves as garbage instructions.

The fix: read the 2-byte start-address field (chunk 0, offset 2-3,
little-endian) when chunk 0 is present and expose it via
autostartEntryPoint(). CPU::reset()'s autostart hookup now calls
Z80::setRegPC() with it after paging the cartridge in, instead of
relying on Z80::reset()'s PC=0. The chunk-enable mask itself
(autostartMmuSelect()) is still derived from the .DCK container's own
chunk-presence bits rather than the header's chunk-spec byte — those two
should always agree for a correctly-packaged cartridge (verified true
for the real Zebra/eToolkit cartridges used in testing), and the
container's bits are what's actually fully spec'd and already parsed
here regardless.

This does not attempt AROS autostart (depends on a working System ROM,
which doesn't exist yet — see slice 1's still-open ROM-image TODO) or a
mid-session, check_trdos-style PC watch (nothing to watch for yet without
a real EXROM image with a "launch cartridge" entry point). Revisit both
once those exist.

Context for whenever AROS autostart IS attempted (as told directly by
the project owner, 2026-07-10 — not yet independently located in the
disassembly despite a search of the HOME ROM text for SYSCON/DOCK/
ARSFLG scan logic, so treat as reliable but not yet self-verified):
real HOME ROM init scans the DOCK connector and detects LROS vs AROS.
For AROS specifically, it jumps directly to the address the cartridge
specifies — but an AROS cartridge doesn't contain a real reset handler
at address 0 (no DI, etc.), so it can't safely take a RST 0. When RST 0
fires while the AROS cartridge is paged in, the hardware pages HOME ROM
back in to handle it properly, and the whole detect-and-jump sequence
runs again. This is what lets an AROS-style ROM replacement bootstrap
with minimal hardware engineering: it doesn't need its own complete RST
vector table, HOME ROM keeps catching RST 0 for it. If this port ever
emulates a real HOME-ROM-driven boot (rather than DockLoader computing
the jump itself, as it does now), this loop is what needs replicating —
and check_trdos-style PC watching (deliberately not built above,
because LROS doesn't need it) becomes directly relevant for it, since
RST 0 can fire at any point, not just at boot.

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

#include "DockLoader.h"
#include "SCLD.h"
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef ESP2068_HOST_TEST
#include <cstdlib>
static inline uint8_t* dockloader_alloc8k() { return (uint8_t*) calloc(0x2000, 1); }
static inline void dockloader_free(uint8_t* p) { free(p); }
#else
#include "esp_heap_caps.h"
// PSRAM first when there is any, as in SCLD.cpp: the 640-wide VGA mode
// needs the internal RAM.
static inline uint8_t* dockloader_alloc8k() {
    uint8_t* p = (uint8_t*) heap_caps_calloc(0x2000, 1, MALLOC_CAP_SPIRAM);
    return p ? p : (uint8_t*) heap_caps_calloc(0x2000, 1, MALLOC_CAP_8BIT);
}
static inline void dockloader_free(uint8_t* p) { heap_caps_free(p); }
#endif

namespace {

    const size_t HEADER_SIZE = 9;
    const size_t CHUNK_SIZE = 0x2000;
    const uint8_t BANK_DOCK = 0;
    const uint8_t BANK_EXROM = 254;
    const uint8_t BANK_HOME = 255;

    struct PendingChunk {
        uint8_t bankId;
        int index;       // 0..7
        bool writable;
        const uint8_t* src; // points into the caller's buffer, not owned
    };

    // Tracks every chunk buffer this loader has handed to SCLD, so
    // unload() can free them and eject the cartridge cleanly.
    std::vector<uint8_t*> allocatedChunks;

    // EXROM chunks the cartridge's own EXROM bank replaced, and what was
    // there before (normally the system EXROM from RomLoader), so unload()
    // puts the system EXROM back instead of leaving an empty socket.
    uint8_t exromReplacedMask = 0;
    uint8_t* exromReplaced[8] = { nullptr };

    uint8_t autostartMask = 0;
    bool autostartCandidate = false;
    uint16_t autostartEntry = 0;

}

void DockLoader::unload() {
    for (int c = 0; c < 8; c++) SCLD::unloadDockChunk(c);
    // Only the EXROM chunks this cartridge supplied: clearing all of them
    // also removed the system EXROM, so the HOME ROM's start-up copied an
    // empty function dispatcher to $6200 and the machine reset in a loop
    // (found on hardware with Zebra OS-64 in the dock).
    for (int c = 0; c < 8; c++) {
        if (exromReplacedMask & (1 << c)) {
            if (exromReplaced[c]) SCLD::loadExromChunk(c, exromReplaced[c]);
            else SCLD::unloadExromChunk(c);
        }
        exromReplaced[c] = nullptr;
    }
    exromReplacedMask = 0;
    for (uint8_t* p : allocatedChunks) dockloader_free(p);
    allocatedChunks.clear();
    autostartMask = 0;
    autostartCandidate = false;
    autostartEntry = 0;
    SCLD::resolveMemChunks();
}

bool DockLoader::loadFromBuffer(const uint8_t* data, size_t size) {

    // ---- Pass 1: validate structure and collect chunk locations. Does
    // not touch SCLD or allocate anything, so a bad file changes nothing. ----
    std::vector<PendingChunk> pending;
    size_t offset = 0;
    while (offset < size) {

        if (offset + HEADER_SIZE > size) return false; // truncated header

        uint8_t bankId = data[offset];
        const uint8_t* typeBytes = data + offset + 1;
        offset += HEADER_SIZE;

        for (int c = 0; c < 8; c++) {
            uint8_t t = typeBytes[c];
            if (!(t & 0x02)) continue; // no image for this chunk

            if (offset + CHUNK_SIZE > size) return false; // truncated chunk image

            PendingChunk pc;
            pc.bankId = bankId;
            pc.index = c;
            pc.writable = t & 0x01;
            pc.src = data + offset;
            pending.push_back(pc);

            offset += CHUNK_SIZE;
        }
    }

    // ---- Pass 2: the file is structurally valid. Apply it. ----
    unload(); // clear whatever cartridge (if any) was loaded before

    uint8_t dockMask = 0;
    for (const PendingChunk& pc : pending) {

        if (pc.bankId == BANK_HOME) {
            // Not this loader's job — see this file's top comment and
            // slice 1's still-open "real ROM image" TODO. Deliberately
            // not applied anywhere.
            continue;
        }
        if (pc.bankId != BANK_DOCK && pc.bankId != BANK_EXROM) {
            // Reserved bank IDs (1..253): nothing defined to do with them.
            continue;
        }

        uint8_t* buf = dockloader_alloc8k();
        if (!buf) { unload(); return false; } // out of memory: fail cleanly, not partially
        memcpy(buf, pc.src, CHUNK_SIZE);
        allocatedChunks.push_back(buf);

        if (pc.bankId == BANK_DOCK) {
            SCLD::loadDockChunk(pc.index, buf, pc.writable);
            dockMask |= (1 << pc.index);
            if (pc.index == 0) {
                // LROS header, chunk 0 offset 2-3: start address, LSB/MSB.
                // See this file's top comment -- reading this for real is
                // the fix for a bug real-cartridge testing caught.
                autostartEntry = buf[2] | (buf[3] << 8);
            }
        } else { // BANK_EXROM
            if (!(exromReplacedMask & (1 << pc.index))) {
                exromReplaced[pc.index] = SCLD::exromChunkData(pc.index);
                exromReplacedMask |= 1 << pc.index;
            }
            SCLD::loadExromChunk(pc.index, buf);
        }
    }

    autostartMask = dockMask;
    autostartCandidate = (dockMask & 0x01) != 0; // chunk 0 present: LROS convention
    SCLD::resolveMemChunks();

    return true;
}

bool DockLoader::loadFromFile(const std::string& fn) {

    FILE* file = fopen(fn.c_str(), "rb");
    if (file == NULL) {
        printf("DockLoader: error opening %s\n", fn.c_str());
        return false;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    if (size <= 0) {
        printf("DockLoader: %s is empty or unreadable\n", fn.c_str());
        fclose(file);
        return false;
    }

    uint8_t* buf = (uint8_t*) malloc((size_t) size);
    if (!buf) {
        printf("DockLoader: out of memory reading %s (%ld bytes)\n", fn.c_str(), size);
        fclose(file);
        return false;
    }

    size_t got = fread(buf, 1, (size_t) size, file);
    fclose(file);

    bool ok = (got == (size_t) size) && loadFromBuffer(buf, got);
    free(buf); // loadFromBuffer() copies whatever it keeps; this is scratch

    if (!ok) printf("DockLoader: %s failed to load (invalid .DCK)\n", fn.c_str());

    return ok;
}

bool DockLoader::hasAutostart() {
    return autostartCandidate;
}

uint8_t DockLoader::autostartMmuSelect() {
    return autostartMask;
}

uint16_t DockLoader::autostartEntryPoint() {
    return autostartEntry;
}
