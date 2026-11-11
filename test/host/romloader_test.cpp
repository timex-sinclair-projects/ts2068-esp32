/*

ESP2068 — TS2068 port of ESPectrum

test/host/romloader_test.cpp — standalone, host-compilable test for
RomLoader (include/RomLoader.h, src/RomLoader.cpp). No test framework,
no ESP-IDF dependency.

Two parts:
  1. Synthetic-buffer checks (always run, no external dependency): size
     validation, content round-trip via SCLD::memChunk[], including a
     multi-chunk (16K) EXROM confirming chunk-indexed (not mirrored)
     addressing -- see SCLD.cpp's loadExromChunk() comment.
  2. An optional real-ROM check against the actual TS2068 HOME ROM/EXROM
     dumps in a local reference library
     (/Users/david/Documents/Projects/TS2068 Ref Library/2068 ROMS/) --
     SKIPS (does not fail) if that path isn't present, since it's outside
     this repo and machine-specific (see RomLoader.h's top comment and
     test/dock/README.md's provenance policy: those ROM files are never
     copied into this repo). When it does run, it's a genuinely
     meaningful check: the ROM version byte at offset 0x13 is documented
     elsewhere (independent of this project) as always 0xFF on real
     TS2068 HOME ROMs -- this confirms RomLoader placed real content at
     the right address, not just that it copied *something*. A third,
     separately-optional check loads the project owner's own real
     TS-Pico EXROM (gus-exrom.rom, genuinely 16384 bytes, also outside
     this repo, same skip-not-fail convention) and confirms its two 8K
     chunks resolve to different content -- the actual real-hardware
     evidence that motivated the chunk-indexed EXROM fix in the first
     place, not just a synthetic stand-in for it.

Build and run (from repo root, so the default reference-library path
resolves):

    g++ -std=c++17 -Wall -I include -DESP2068_HOST_TEST \
        test/host/romloader_test.cpp src/RomLoader.cpp src/SCLD.cpp \
        -o /tmp/romloader_test \
    && /tmp/romloader_test

*/

#include "RomLoader.h"
#include "SCLD.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
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

static bool fileExists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

int main(int argc, char** argv) {

    printf("RomLoader host test\n");
    printf("=====================\n");

    SCLD::allocateMemory();
    SCLD::reset();

    // ---- Synthetic buffers: size validation ----
    printf("\n-- size validation --\n");
    {
        uint8_t tooSmall[100] = { 0 };
        check(!RomLoader::loadHomeRomFromBuffer(tooSmall, sizeof(tooSmall)),
              "loadHomeRomFromBuffer() rejects a buffer that isn't exactly 16384 bytes");
        check(!RomLoader::loadExromFromBuffer(tooSmall, sizeof(tooSmall)),
              "loadExromFromBuffer() rejects a buffer that isn't a whole multiple of 8192 bytes");

        uint8_t notAMultiple[3 * 0x2000 + 1] = { 0 };
        check(!RomLoader::loadExromFromBuffer(notAMultiple, sizeof(notAMultiple)),
              "loadExromFromBuffer() rejects a size one byte past a whole 8K multiple");

        uint8_t tooBig[9 * 0x2000] = { 0 };
        check(!RomLoader::loadExromFromBuffer(tooBig, sizeof(tooBig)),
              "loadExromFromBuffer() rejects more than 8 chunks (64K)");
    }

    // ---- Synthetic buffers: content round-trip ----
    printf("\n-- content round trip --\n");
    {
        uint8_t homeRom[0x4000];
        for (size_t i = 0; i < sizeof(homeRom); i++) homeRom[i] = (uint8_t)(i & 0xFF);
        check(RomLoader::loadHomeRomFromBuffer(homeRom, sizeof(homeRom)),
              "loadHomeRomFromBuffer() accepts a correctly-sized buffer");

        SCLD::OUT_F4(0x00); // HOME selected (reset default, explicit for clarity)
        check(SCLD::memChunk[0][0] == 0x00 && SCLD::memChunk[0][0x1FFF] == 0xFF,
              "chunk 0 (first half of the ROM) has the expected byte pattern at both ends");
        check(SCLD::memChunk[1][0] == 0x00 && SCLD::memChunk[1][0x1FFF] == 0xFF,
              "chunk 1 (second half of the ROM) has the expected byte pattern at both ends");
        check(SCLD::memChunkReadOnly[0] && SCLD::memChunkReadOnly[1],
              "both HOME ROM chunks are still read-only after loading real content");

        uint8_t exrom[0x2000];
        for (size_t i = 0; i < sizeof(exrom); i++) exrom[i] = (uint8_t)((i * 3) & 0xFF);
        check(RomLoader::loadExromFromBuffer(exrom, sizeof(exrom)),
              "loadExromFromBuffer() accepts a correctly-sized 8K buffer");

        SCLD::OUT_FF(0x80);   // exromSelect = true
        SCLD::OUT_F4(0x01);   // chunk 0's F4 bit set -> now shows EXROM
        check(SCLD::memChunk[0][1] == 3 && SCLD::memChunk[0][0x1FFF] == exrom[0x1FFF],
              "the paged-in EXROM chunk has the expected byte pattern");
        SCLD::OUT_FF(0x00);
        SCLD::OUT_F4(0x00);

        // A larger, 2-chunk (16K) EXROM -- the real-world case this fix is
        // for. Each chunk must resolve to ITS OWN half of the image, not a
        // mirror of chunk 0 -- see SCLD.cpp's loadExromChunk() comment.
        uint8_t exrom16k[2 * 0x2000];
        for (size_t i = 0; i < 0x2000; i++) exrom16k[i] = (uint8_t)(i & 0xFF);           // chunk 0: ramp
        for (size_t i = 0; i < 0x2000; i++) exrom16k[0x2000 + i] = (uint8_t)(0xFF - (i & 0xFF)); // chunk 1: inverse ramp
        check(RomLoader::loadExromFromBuffer(exrom16k, sizeof(exrom16k)),
              "loadExromFromBuffer() accepts a correctly-sized 16K (2-chunk) buffer");

        SCLD::OUT_FF(0x80);
        SCLD::OUT_F4(0x03);   // chunks 0 and 1 both select EXROM
        check(SCLD::memChunk[0][0] == 0x00 && SCLD::memChunk[0][0x1FFF] == 0xFF,
              "chunk 0 shows the first half of the 16K EXROM image");
        check(SCLD::memChunk[1][0] == 0xFF && SCLD::memChunk[1][0x1FFF] == 0x00,
              "chunk 1 shows the SECOND half of the 16K EXROM image -- genuinely different content, not a mirror of chunk 0");
        SCLD::OUT_FF(0x00);
        SCLD::OUT_F4(0x00);
    }

    // ---- File-not-found handling ----
    printf("\n-- error handling --\n");
    check(!RomLoader::loadHomeRomFromFile("test/dock/fixtures/does_not_exist.rom"),
          "loadHomeRomFromFile() on a missing path returns false");

    // ---- Real ROM content, if the local reference library is present ----
    printf("\n-- real TS2068 ROM content (optional) --\n");
    {
        std::string homePath = argc > 1
            ? std::string(argv[1])
            : "/Users/david/Documents/Projects/TS2068 Ref Library/2068 ROMS/2068Home.BIN";
        std::string exromPath = argc > 2
            ? std::string(argv[2])
            : "/Users/david/Documents/Projects/TS2068 Ref Library/2068 ROMS/2068Exrom.BIN";

        if (!fileExists(homePath) || !fileExists(exromPath)) {
            printf("  SKIP  reference library not found at %s -- not a failure, just unavailable here\n", homePath.c_str());
        } else {
            check(RomLoader::loadHomeRomFromFile(homePath), "loadHomeRomFromFile() loads the real HOME ROM");
            SCLD::OUT_F4(0x00);
            check(SCLD::memChunk[0][0x13] == 0xFF,
                  "real HOME ROM's version byte at $0013 is $FF (documented fact, confirmed against real content)");
            check(SCLD::memChunk[0][0] == 0xF3,
                  "real HOME ROM starts with DI ($F3), a sane reset entry point");

            check(RomLoader::loadExromFromFile(exromPath), "loadExromFromFile() loads the real EXROM");
            SCLD::OUT_FF(0x80);
            SCLD::OUT_F4(0x01);
            check(SCLD::memChunk[0][0] == 0xF3,
                  "real EXROM also starts with DI ($F3) -- both ROMs begin with a sane instruction");
            SCLD::OUT_FF(0x00);
            SCLD::OUT_F4(0x00);
        }
    }

    // ---- Real 16K TS-Pico EXROM, if present (separately optional) ----
    // The actual real-hardware evidence that motivated the chunk-indexed
    // EXROM fix: gus-exrom.rom is the project owner's own TS-Pico ROM,
    // genuinely 16384 bytes (double stock's 8192) -- confirms chunks 0
    // and 1 resolve to real, different content, not just the synthetic
    // ramp/inverse-ramp buffer above.
    printf("\n-- real 16K TS-Pico EXROM content (optional) --\n");
    {
        std::string picoExromPath = argc > 3
            ? std::string(argv[3])
            : "/Users/david/Documents/Projects/TS2068 Ref Library/gus-exrom.rom";

        if (!fileExists(picoExromPath)) {
            printf("  SKIP  gus-exrom.rom not found at %s -- not a failure, just unavailable here\n", picoExromPath.c_str());
        } else {
            check(RomLoader::loadExromFromFile(picoExromPath), "loadExromFromFile() loads the real 16K TS-Pico EXROM");
            SCLD::OUT_FF(0x80);
            SCLD::OUT_F4(0x03); // chunks 0 and 1 both select EXROM
            bool anyByteDiffers = false;
            for (int i = 0; i < 0x2000; i++) {
                if (SCLD::memChunk[0][i] != SCLD::memChunk[1][i]) { anyByteDiffers = true; break; }
            }
            check(anyByteDiffers,
                  "real TS-Pico EXROM: chunk 0 and chunk 1 are genuinely different content, confirming per-chunk (not mirrored) addressing on real hardware");
            SCLD::OUT_FF(0x00);
            SCLD::OUT_F4(0x00);
        }
    }

    printf("\n=====================\n");
    if (failures == 0) {
        printf("ALL PASSED\n");
    } else {
        printf("%d CHECK(S) FAILED\n", failures);
    }

    return failures;
}
