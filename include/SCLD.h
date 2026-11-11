/*

ESP2068 — TS2068 port of ESPectrum

SCLD.h — memory-management-unit and display-control register contract
for the Timex Sinclair 2068's SCLD chip. New for this port; there is no
upstream ESPectrum equivalent. See TS2068-ESPECTRUM-PORT-PLAN.md for the
full SCLD register spec this header implements, and PLAN.md (Phase 1)
for why this file exists on its own, ahead of any real implementation.

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

#ifndef SCLD_h
#define SCLD_h

#include <inttypes.h>
#include <cstddef>

// ---------------------------------------------------------------------------
// This is the interface every port slice codes against (see
// TS2068-ESPECTRUM-PORT-PLAN.md, "Interface to agree before anyone writes
// code"). Its shape is frozen as of PLAN.md Phase 1 — changing it once
// slices 2/3 are underway needs sign-off from whoever owns slice 1 (MMU +
// memory core), since the 8-slot memory model is the one place a mismatch
// is subtle rather than a compile error.
//
// Slice 1 (PLAN.md Phase 2) fills in real bodies: HOME ROM/RAM backing
// store is allocated by allocateMemory() and always present; DOCK/EXROM
// chunks resolve to a shared, read-only, zeroed "empty socket" page until
// something calls loadDockChunk()/loadExromImage() (slice 3's job) — real
// hardware doesn't crash when you read an empty cartridge socket, and
// neither does memChunk[], since it is never null.
//
// SCLD.cpp is written to also compile host-side (outside the ESP-IDF/
// PlatformIO build) when ESP2068_HOST_TEST is defined, so the exact same
// resolve logic backs both the firmware and test/host/scld_test.cpp — see
// PLAN.md Phase 2, slice 1 exit criteria ("a simple host-side harness").
// ---------------------------------------------------------------------------

class SCLD {
public:

    // ---- Port 0xF4 : Horizontal MMU chunk select (reset value 0x00) ----
    // Bit c (0..7) set   -> Z80 chunk c (address range [c*0x2000, c*0x2000+0x1fff])
    //                       is mapped to the alternative bank (DOCK or EXROM,
    //                       per exromSelect below).
    // Bit c clear        -> chunk c falls back to HOME (ROM below 0x4000,
    //                       RAM above — the 2068's normal Spectrum-shape layout).
    static uint8_t mmuSelect;

    // ---- Port 0xFF : Display Enhancement Control (reset value 0x00) ----
    static uint8_t displayControl;

    // Decoded fields of displayControl, kept in sync by OUT_FF(). See the
    // plan doc's "Port 0xFF" table for the authoritative bit layout.
    //
    // videoMode's three bits are independent flags, not a mutually-exclusive
    // enum (corrected 2026-07-10 against a real TS2068 reference library —
    // see the plan doc): bit 0 = second display file, bit 1 = hi-color, bit
    // 2 = 64-column/hi-res. Hi-res is bit 2 ALONE (0x04) — earlier drafts of
    // the plan doc said "110"/0x06, which was wrong. Combinations with bit 2
    // set are undocumented; treat as unsupported.
    static uint8_t videoMode;      // bits 2..0 : independent flags, see above
    static uint8_t hiresInkPaper;  // bits 5..3 : hi-res mode's global ink/paper pair
    static bool    intInhibit;     // bit 6     : 1 = 1/60s timer interrupt disabled
    static bool    exromSelect;    // bit 7     : 0 = DOCK, 1 = EXROM

    // ---- The 8-slot memory model ----
    // One pointer per 8K Z80 chunk, indexed address >> 13. This replaces
    // MemESP::ramCurrent[4] for this machine. Slice 1 repoints two call
    // sites at this: CPU.cpp's six Z80Ops::*_std bodies (the Z80 core's hot
    // path), and MemESP.h's own readbyte/writebyte/readword/writeword
    // inlines (a second, independent access path used by Snapshot.cpp and
    // Tape.cpp for direct pokes — easy to miss since it doesn't go through
    // Z80Ops at all).
    static uint8_t* memChunk[8];

    // Per-chunk write protect: true = writes to this chunk are dropped.
    // ESPectrum has no equivalent of this today. Its ROM protection is two
    // different hardcoded checks: `page == 0` in CPU.cpp's poke8_std/
    // poke16_std, and `page == pagingmode2A3` in MemESP::writebyte/
    // writeword. Both get replaced by this flag; there is no existing
    // mechanism to "generalize" from, budget it as new code.
    static bool memChunkReadOnly[8];

    // Recompute memChunk[]/memChunkReadOnly[] from mmuSelect, exromSelect,
    // and whatever HOME/DOCK/EXROM images are currently loaded. Call after
    // any write to port 0xF4, after any write to port 0xFF (cheap — 8
    // iterations — so it's simplest to always re-resolve rather than track
    // whether bit 7 specifically changed), and after cartridge load/eject.
    static void resolveMemChunks();

    // Per-chunk page resolvers, named to match the pseudocode in the plan
    // doc's "The 8-slot memory model" section 1:1.
    //   homePage(c)  -> HOME ROM for c in {0,1} (below 0x4000), HOME RAM for
    //                   c in {2..7}. Always a real, allocated page.
    //   dockPage(c)  -> the loaded .DCK's 8K image for chunk c, or the
    //                   shared empty-socket page if nothing is docked there.
    //   exromPage(c) -> chunk c's own 8K slice of the loaded EXROM image
    //                   (real EXROM chip-select is per-chunk, exactly like
    //                   DOCK's — see loadExromImage()'s comment for the
    //                   real-hardware evidence this corrects), or the
    //                   empty-socket page if that slice isn't loaded.
    static uint8_t* homePage(int chunk);
    static uint8_t* dockPage(int chunk);
    static uint8_t* exromPage(int chunk);

    // ---- Backing-store setup and image loading ----
    // Allocates HOME ROM (16K) + HOME RAM (48K) and the shared empty-socket
    // page. Call once at startup, before the first resolveMemChunks(). HOME
    // ROM content is zeroed until loadHomeRom() (below) is called — no real
    // TS2068 ROM image is embedded in this repo (see PLAN.md's risk
    // register on ROM/cartridge provenance); RomLoader.h reads one from an
    // SD card file instead.
    static void allocateMemory();

    // Copies a 16K HOME ROM image into the chunks allocateMemory() already
    // reserved for it (chunks 0-1). `size` must be exactly 0x4000 — returns
    // false and leaves the existing content untouched otherwise, or if
    // allocateMemory() hasn't run yet. Ownership of `data` stays with the
    // caller (unlike loadDockChunk()/loadExromImage() — this one copies
    // into storage SCLD already owns, rather than adopting the pointer).
    static bool loadHomeRom(const uint8_t* data, size_t size);

    // Slice 3 (cartridge loader) calls these as it parses a .DCK: point
    // chunk `chunk` at an already-loaded 8K image, RAM (writable) or ROM
    // (read-only) per the .DCK's per-chunk type marker. Ownership of `data`
    // stays with the caller — SCLD only stores the pointer. Call
    // resolveMemChunks() afterwards to make the change visible in memChunk[].
    static void loadDockChunk(int chunk, uint8_t* data, bool writable);
    static void unloadDockChunk(int chunk);

    // EXROM's own per-chunk primitives, same shape as loadDockChunk()/
    // unloadDockChunk() minus the writable flag (EXROM is always ROM).
    // DockLoader.cpp calls these directly, one chunk at a time, matching
    // how a .DCK's EXROM bank is itself already chunk-structured.
    static void loadExromChunk(int chunk, uint8_t* data);
    static void unloadExromChunk(int chunk);
    // The chunk's current EXROM image, or nullptr if the socket is empty
    // (unlike exromPage(), which substitutes the empty-socket page).
    // DockLoader uses it to put back the system EXROM a cartridge's own
    // EXROM bank replaced.
    static uint8_t* exromChunkData(int chunk);

    // Convenience wrapper over loadExromChunk() for RomLoader's simpler
    // case: one whole-file buffer holding `numChunks` (1..8) contiguous
    // 8K slices. Adopts `data` (SCLD owns it from here on, same ownership
    // model as loadDockChunk() — not a copy, unlike loadHomeRom()).
    static void loadExromImage(uint8_t* data, int numChunks);

    // ---- Port handlers ----
    // Both ports are decoded on their low byte on the 2068 — a fuller
    // decode than the Spectrum ULA's single-bit check on 0xFE, but NOT a
    // full 16-bit match (Ports.cpp checks `address & 0xFF`, not
    // `address == 0x00F4`). Corrected 2026-07-10 after a real bug: an
    // earlier version required the full 16-bit address, which only
    // matched real `OUT (n),A`/`IN A,(n)` traffic when the accumulator
    // happened to be 0 — real Z80 hardware puts the accumulator's value
    // on the address bus's upper byte for those instructions, so
    // `LD A,val; OUT ($F4),A` sends `(val<<8)|0xF4`, not `0x00F4`, for
    // any val. Confirmed against FUSE's SCLD port table
    // (peripherals/scld.c, mask 0x00ff) before fixing.
    static void    OUT_F4(uint8_t value);
    static uint8_t IN_F4();   // returns last value written (mmuSelect)
    static void    OUT_FF(uint8_t value);
    static uint8_t IN_FF();   // returns last value written (displayControl)

    // ---- Display: DFILE base + screen-select ----
    // Mirrors the *shape* of VIDEO::grmem (Video.h/.cpp) — which today
    // picks one of two 16K RAM banks via MemESP::videoLatch for ESPectrum's
    // 128K shadow-screen feature — applied to the 2068's two fixed-offset
    // display files instead of bank-switched ones.
    //
    // DFILE1_BASE lands at the start of chunk 2 (0x4000 >> 13 == 2) and
    // DFILE2_BASE at the start of chunk 3 (0x6000 >> 13 == 3): the 2068's
    // "+0x2000" display file spacing already lines up with one 8K chunk
    // boundary, which is what makes this fit the memChunk[] model cleanly.
    //
    // Which DFILE(s) a given videoMode actually needs is NOT a single
    // toggle and is deliberately left to slice 2, not decided here: mode
    // 000 reads DFILE1 only, 001 reads DFILE2 only, 010 reads DFILE1 for
    // pixels *and* DFILE2 for attributes simultaneously, and 110 interleaves
    // both column-by-column. A single "use secondary?" bool can't represent
    // 010/110, so this header stops at exposing the two bases.
    static const uint16_t DFILE1_BASE = 0x4000; // primary display file
    static const uint16_t DFILE2_BASE = 0x6000; // secondary display file (+0x2000)

    // Power-on/reset state: mmuSelect=0, displayControl=0, all chunks HOME,
    // the overlay off.
    static void reset();

    // ---- The native disk's DOS ROM overlay (DiskEngine.h) ----
    // Not 2068 hardware. While on, chunks 0 and 1 show `overlayImage`
    // (16K, read-only) whatever mmuSelect and exromSelect say; the
    // registers themselves are untouched, so turning it off puts back
    // exactly what was there. Without an image, setOverlay() does nothing.
    static void setOverlayImage(const uint8_t* image16k);
    static void setOverlay(bool on);
    static bool overlayOn() { return overlayActive; }

private:
    static const uint8_t* overlayImage;
    static bool overlayActive;
public:

};

#endif // SCLD_h
