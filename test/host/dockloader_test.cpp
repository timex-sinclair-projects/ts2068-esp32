/*

ESP2068 — TS2068 port of ESPectrum

test/host/dockloader_test.cpp — standalone, host-compilable test for
DockLoader (include/DockLoader.h, src/DockLoader.cpp) against slice 4's
synthetic .DCK fixture. No test framework, no ESP-IDF dependency.

Build and run (from repo root, so the default fixture path resolves):

    g++ -std=c++17 -Wall -I include -DESP2068_HOST_TEST \
        test/host/dockloader_test.cpp src/DockLoader.cpp src/SCLD.cpp \
        -o /tmp/dockloader_test \
    && /tmp/dockloader_test

Optionally pass a different .dck path as argv[1].

*/

#include "DockLoader.h"
#include "SCLD.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

static int failures = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        printf("  PASS  %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

int main(int argc, char** argv) {

    std::string fixture = argc > 1 ? argv[1] : "test/dock/fixtures/synthetic.dck";

    printf("DockLoader host test\n");
    printf("=====================\n");
    printf("fixture: %s\n", fixture.c_str());

    SCLD::allocateMemory();
    SCLD::reset();

    // ---- Loading the synthetic fixture ----
    printf("\n-- load --\n");
    check(DockLoader::loadFromFile(fixture), "loadFromFile() succeeds on the synthetic fixture");
    check(DockLoader::hasAutostart(), "hasAutostart() is true (chunk 0 is present in the DOCK bank)");
    check(DockLoader::autostartMmuSelect() == 0x03,
          "autostartMmuSelect() is 0x03 (chunks 0 and 1, matching the fixture's two present chunks)");
    // Fixture's chunk 0 starts "ESP2068 ..." -> bytes 2-3 are 'P','2' (0x50,0x32);
    // little-endian entry point = 0x3250. Mechanical check that
    // autostartEntryPoint() reads the real header field, not a fixed 0 --
    // see DockLoader.cpp's top comment for why that distinction is the
    // whole point of this fix.
    check(DockLoader::autostartEntryPoint() == 0x3250,
          "autostartEntryPoint() reads chunk 0 offset 2-3 (0x3250 for this fixture), not a hardcoded 0");

    // ---- Applying the autostart mask makes the cartridge visible via memChunk[] ----
    printf("\n-- apply autostart mask --\n");
    SCLD::OUT_F4(DockLoader::autostartMmuSelect());
    check(SCLD::memChunk[0] != nullptr && SCLD::memChunk[1] != nullptr,
          "chunks 0 and 1 are non-null after paging in");
    check(memcmp(SCLD::memChunk[0], "ESP2068 SYNTHETIC TEST FIXTURE", 30) == 0,
          "chunk 0 content matches the synthetic RAM marker string");
    check(SCLD::memChunk[1][0] == 0 && SCLD::memChunk[1][5] == 5 && SCLD::memChunk[1][255] == 255,
          "chunk 1 content matches the synthetic ROM byte ramp");
    check(!SCLD::memChunkReadOnly[0], "chunk 0 (RAM, writable=true in the fixture) is not read-only");
    check(SCLD::memChunkReadOnly[1], "chunk 1 (ROM, writable=false in the fixture) is read-only");

    // ---- Chunks the cartridge didn't populate are untouched (still HOME) ----
    printf("\n-- unrelated chunks are untouched --\n");
    check(!SCLD::memChunkReadOnly[2], "chunk 2 (not in autostart mask) is still writable HOME RAM");

    // ---- Unload: cartridge chunks fall back to the empty socket ----
    printf("\n-- unload --\n");
    DockLoader::unload();
    SCLD::OUT_F4(0xFF); // force every chunk to the alternative bank; nothing is loaded now
    check(SCLD::memChunkReadOnly[0] && SCLD::memChunkReadOnly[1],
          "after unload(), chunks 0 and 1 fall back to the read-only empty socket");
    SCLD::OUT_F4(0x00);

    // ---- Error handling: bad path ----
    printf("\n-- error handling --\n");
    check(!DockLoader::loadFromFile("test/dock/fixtures/does_not_exist.dck"),
          "loadFromFile() on a missing path returns false");

    // ---- Error handling: a failed load doesn't corrupt a previously-good one ----
    check(DockLoader::loadFromFile(fixture), "reload the valid fixture as a known-good baseline");
    SCLD::OUT_F4(DockLoader::autostartMmuSelect()); // page the cartridge back in to actually read it below
    uint8_t before0 = SCLD::memChunk[0][0];
    uint8_t truncated[5] = { 0, 0x02, 0, 0, 0 }; // header claims a chunk 0 image but supplies no data
    check(!DockLoader::loadFromBuffer(truncated, sizeof(truncated)),
          "loadFromBuffer() on a truncated buffer returns false");
    check(SCLD::memChunk[0][0] == before0,
          "a failed load leaves the previously-loaded cartridge's data untouched");

    // Regression (found on hardware, 2026-11-11): loading a cartridge used
    // to clear every EXROM chunk, including the system EXROM RomLoader had
    // loaded, so the HOME ROM copied an empty function dispatcher and the
    // machine reset in a loop.
    printf("\n-- the system EXROM survives a cartridge --\n");
    DockLoader::unload();
    static uint8_t systemExrom[0x2000];
    memset(systemExrom, 0xE5, sizeof(systemExrom));
    SCLD::loadExromImage(systemExrom, 1);

    std::vector<uint8_t> dockOnly(9 + 0x2000, 0x11);
    dockOnly[0] = 0;                                 // DOCK bank
    for (int c = 1; c <= 8; c++) dockOnly[c] = c == 1 ? 0x02 : 0x00; // chunk 0 ROM + image
    check(DockLoader::loadFromBuffer(dockOnly.data(), dockOnly.size()), "a DOCK-only cartridge loads");
    check(SCLD::exromChunkData(0) == systemExrom, "loading it leaves the system EXROM in chunk 0");
    DockLoader::unload();
    check(SCLD::exromChunkData(0) == systemExrom, "unloading it leaves the system EXROM in chunk 0");

    std::vector<uint8_t> withExrom(9 + 0x2000, 0x22);
    withExrom[0] = 254;                              // EXROM bank
    for (int c = 1; c <= 8; c++) withExrom[c] = c == 1 ? 0x02 : 0x00;
    check(DockLoader::loadFromBuffer(withExrom.data(), withExrom.size()), "a cartridge with its own EXROM chunk 0 loads");
    check(SCLD::exromChunkData(0) != systemExrom && SCLD::exromChunkData(0)[0] == 0x22,
          "its EXROM chunk replaces the system one while loaded");
    DockLoader::unload();
    check(SCLD::exromChunkData(0) == systemExrom, "unloading it puts the system EXROM back");
    SCLD::unloadExromChunk(0);

    printf("\n=====================\n");
    if (failures == 0) {
        printf("ALL PASSED\n");
    } else {
        printf("%d CHECK(S) FAILED\n", failures);
    }

    return failures;
}
