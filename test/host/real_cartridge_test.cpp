/*

ESP2068 — TS2068 port of ESPectrum

test/host/real_cartridge_test.cpp — the "real ROM/DOCK end-to-end test"
from PLAN.md's slice 3 writeup. Wraps two *genuine* TS2068 cartridge
dumps as in-memory .DCK buffers and runs them through the real
DockLoader, combined with the real HOME ROM via RomLoader. No test
framework, no ESP-IDF dependency.

This test is what caught a real bug: an earlier version of DockLoader's
autostart assumed an LROS cartridge could just be paged in and left to
execute from PC=0 (matching Z80::reset()'s natural state). Zebra OS-64's
real LROS header proved that wrong — see DockLoader.cpp's top comment
and PLAN.md for the full story. Keeping this test real-content, not
synthetic, is deliberate: test/dock/fixtures/synthetic.dck's header
bytes were never meant to resemble genuine cartridge metadata, so a
bug like this one couldn't have shown up there.

Everything here is read from local paths outside this repo and never
written anywhere — no cartridge/ROM byte is constructed into a file, and
the in-memory .DCK buffers this test builds are never persisted. See
test/dock/README.md's provenance policy. Skips (does not fail) any part
whose source file isn't present locally.

Build and run (from repo root, so default paths resolve):

    g++ -std=c++17 -Wall -I include -DESP2068_HOST_TEST \
        test/host/real_cartridge_test.cpp src/DockLoader.cpp \
        src/RomLoader.cpp src/SCLD.cpp \
        -o /tmp/real_cartridge_test \
    && /tmp/real_cartridge_test

*/

#include "DockLoader.h"
#include "RomLoader.h"
#include "SCLD.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;
static int skipped = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        printf("  PASS  %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

static void skip(const char* what) {
    printf("  SKIP  %s\n", what);
    skipped++;
}

static bool readWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size <= 0) { fclose(f); return false; }
    out.resize((size_t) size);
    size_t got = fread(out.data(), 1, (size_t) size, f);
    fclose(f);
    return got == (size_t) size;
}

// Wraps `rom` (must be a multiple of 8192 bytes) as a single-bank DOCK
// .DCK buffer, ROM-type (read-only), placing its chunks starting at
// `firstChunk`. Mirrors test/dock/dck_format.py's write() logic exactly
// -- see that module for the format itself.
static std::vector<uint8_t> wrapAsDock(const std::vector<uint8_t>& rom, int firstChunk) {
    int nChunks = (int)(rom.size() / 0x2000);
    std::vector<uint8_t> out;
    out.push_back(0); // bank id = DOCK
    for (int c = 0; c < 8; c++) {
        bool present = c >= firstChunk && c < firstChunk + nChunks;
        out.push_back(present ? 0x02 : 0x00); // 0x02 = ROM, image present
    }
    out.insert(out.end(), rom.begin(), rom.end());
    return out;
}

int main(int argc, char** argv) {

    std::string refLib = argc > 1
        ? std::string(argv[1])
        : "/Users/david/Documents/Projects/TS2068 Ref Library";

    std::string zebraPath = refLib + "/Zebra OS-64.BIN";
    std::string etoolkitPath = refLib + "/eToolkit.ROM";
    std::string homeRomPath = refLib + "/2068 ROMS/2068Home.BIN";

    printf("Real cartridge end-to-end test\n");
    printf("================================\n");
    printf("reference library: %s\n", refLib.c_str());

    SCLD::allocateMemory();
    SCLD::reset();

    // ---- Zebra OS-64: a real LROS cartridge ----
    printf("\n-- Zebra OS-64.BIN (real LROS cartridge) --\n");
    std::vector<uint8_t> zebra;
    if (!readWholeFile(zebraPath, zebra)) {
        skip("Zebra OS-64.BIN not found locally -- not a failure, just unavailable here");
    } else {
        check(zebra.size() == 16384, "Zebra OS-64.BIN is 16384 bytes (2 chunks)");

        // Sanity-check the raw header bytes against docs/zebra_os64_analysis.md
        // before trusting anything built from them.
        check(zebra[0] == 0x00 && zebra[1] == 0x01, "raw header: unused byte, then cartridge type = 1 (LROS)");
        check(zebra[4] == 0xFC, "raw header: chunk spec 0xFC (chunks 0-1 in use, low-active)");

        auto dck = wrapAsDock(zebra, 0);
        check(DockLoader::loadFromBuffer(dck.data(), dck.size()), "loads as a 2-chunk DOCK cartridge at chunks 0-1");
        check(DockLoader::hasAutostart(), "hasAutostart() is true (chunk 0 present)");
        check(DockLoader::autostartMmuSelect() == 0x03, "autostartMmuSelect() is 0x03 (chunks 0-1)");

        // The bug this test exists to catch: entry point must be the
        // header's real field (0x0005 here), not a hardcoded 0.
        check(DockLoader::autostartEntryPoint() == 0x0005,
              "autostartEntryPoint() is 0x0005 -- the cartridge's real header field, not PC=0");

        SCLD::OUT_F4(DockLoader::autostartMmuSelect());
        check(SCLD::memChunk[0][5] == 0xC3 && SCLD::memChunk[0][6] == 0x9E && SCLD::memChunk[0][7] == 0x0D,
              "paged-in chunk 0 at offset 5 is JP $0D9E (C3 9E 0D) -- the real entry point's actual instruction");
        check(SCLD::memChunkReadOnly[0] && SCLD::memChunkReadOnly[1], "both chunks are read-only (ROM cartridge)");

        DockLoader::unload();
        SCLD::OUT_F4(0x00);
    }

    // ---- eToolkit: a real AROS cartridge (should NOT autostart) ----
    printf("\n-- eToolkit.ROM (real AROS cartridge) --\n");
    std::vector<uint8_t> etoolkit;
    if (!readWholeFile(etoolkitPath, etoolkit)) {
        skip("eToolkit.ROM not found locally -- not a failure, just unavailable here");
    } else {
        check(etoolkit.size() == 24576, "eToolkit.ROM is 24576 bytes (3 chunks)");

        check(etoolkit[0] == 0x01 && etoolkit[1] == 0x02, "raw header: language=BASIC(1), cartridge type=AROS(2)");
        check(etoolkit[4] == 0x8F, "raw header: chunk spec 0x8F (chunks 4-6 in use, low-active)");

        auto dck = wrapAsDock(etoolkit, 4); // AROS header says chunks 4-6, not 0-2
        check(DockLoader::loadFromBuffer(dck.data(), dck.size()), "loads as a 3-chunk DOCK cartridge at chunks 4-6");
        check(!DockLoader::hasAutostart(),
              "hasAutostart() is false -- chunk 0 is NOT present, so LROS-style autostart correctly does not fire for this AROS cartridge");

        SCLD::OUT_F4(0b01110000); // chunks 4,5,6
        check(SCLD::memChunk[4][0] == 0x01 && SCLD::memChunk[4][1] == 0x02 && SCLD::memChunk[4][4] == 0x8F,
              "paged-in chunk 4 matches the real AROS header bytes verified earlier");

        DockLoader::unload();
        SCLD::OUT_F4(0x00);
    }

    // ---- Combined: real HOME ROM + real DOCK cartridge, consistent state ----
    printf("\n-- combined: real HOME ROM + real DOCK cartridge --\n");
    if (zebra.empty() || !RomLoader::loadHomeRomFromFile(homeRomPath)) {
        skip("Zebra and/or 2068Home.BIN not available locally -- skipping the combined check");
    } else {
        check(SCLD::memChunk[0][0] == 0xF3, "before any cartridge: chunk 0 shows the real HOME ROM (starts with DI/0xF3)");

        auto dck = wrapAsDock(zebra, 0);
        DockLoader::loadFromBuffer(dck.data(), dck.size());
        SCLD::OUT_F4(DockLoader::autostartMmuSelect());
        check(SCLD::memChunk[0][0] == 0x00 && SCLD::memChunk[0][1] == 0x01,
              "with the cartridge paged in: chunk 0 now shows Zebra's header, not HOME ROM");

        DockLoader::unload();
        SCLD::OUT_F4(0x00);
        check(SCLD::memChunk[0][0] == 0xF3,
              "after ejecting the cartridge: chunk 0 shows HOME ROM again, undamaged by the cartridge having been paged in");
    }

    printf("\n================================\n");
    if (failures == 0) {
        printf("ALL PASSED (%d skipped)\n", skipped);
    } else {
        printf("%d CHECK(S) FAILED (%d skipped)\n", failures, skipped);
    }

    return failures;
}
