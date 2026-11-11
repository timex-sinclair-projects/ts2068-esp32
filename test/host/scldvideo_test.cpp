/*

ESP2068 — TS2068 port of ESPectrum

test/host/scldvideo_test.cpp — standalone, host-compilable test for
SCLDVideo's byte-to-pixel expansion (include/SCLDVideo.h,
src/SCLDVideo.cpp). No test framework, no ESP-IDF dependency at all (this
module doesn't need the ESP2068_HOST_TEST allocator-shim convention
SCLD.cpp/DockLoader.cpp use, since it does no allocation).

Build and run:

    g++ -std=c++17 -Wall -I include \
        test/host/scldvideo_test.cpp src/SCLDVideo.cpp \
        -o /tmp/scldvideo_test \
    && /tmp/scldvideo_test

*/

#include "SCLDVideo.h"
#include <cstdio>
#include <cstring>

static int failures = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        printf("  PASS  %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

int main() {

    printf("SCLDVideo host test\n");
    printf("====================\n");

    // ---- Standard mode: bit order and pixel-doubling ----
    printf("\n-- expandStandardByte --\n");
    {
        // bmp = 0b10110000: ink shows at source pixels 0,2,3 (bit7,bit5,bit4)
        // att = ink 2 (red), paper 5 (cyan)
        uint8_t out[16];
        SCLDVideo::expandStandardByte(0b10110000, (5 << 3) | 2, out);
        uint8_t expected[16] = { 2,2, 5,5, 2,2, 2,2, 5,5, 5,5, 5,5, 5,5 };
        check(memcmp(out, expected, 16) == 0, "0b10110000/ink2paper5 expands to the expected 16-pixel doubled line");

        // All-paper byte: every output pixel should be the paper color
        SCLDVideo::expandStandardByte(0x00, (7 << 3) | 0, out);
        bool allPaper = true;
        for (int i = 0; i < 16; i++) if (out[i] != 7) allPaper = false;
        check(allPaper, "an all-zero bitmap byte expands to 16 paper-color pixels");

        // All-ink byte
        SCLDVideo::expandStandardByte(0xFF, (7 << 3) | 3, out);
        bool allInk = true;
        for (int i = 0; i < 16; i++) if (out[i] != 3) allInk = false;
        check(allInk, "an all-one bitmap byte expands to 16 ink-color pixels");

        // Bit 7 is confirmed leftmost (per the reference library's pixel
        // address formula comment) -- so a single high bit at bit 7 must
        // land at output pixels 0 and 1, not 14 and 15.
        SCLDVideo::expandStandardByte(0b10000000, (0 << 3) | 1, out);
        check(out[0] == 1 && out[1] == 1 && out[14] == 0 && out[15] == 0,
              "bit 7 (leftmost source pixel) maps to output pixels 0-1, not 14-15");
    }

    // ---- Hi-res mode: BLOCK arrangement (not interleaved), per FUSE's
    // uidisplay_plot16 -- corrected 2026-07-10, see SCLDVideo.h's top
    // comment. out[0..7] = all of dfile1Byte's bits MSB-first, out[8..15]
    // = all of dfile2Byte's bits MSB-first. ----
    printf("\n-- expandHiresBytePair (block arrangement, FUSE-confirmed) --\n");
    {
        uint8_t out[16];

        // DFILE1 all-ink, DFILE2 all-paper: first half 1, second half 0
        SCLDVideo::expandHiresBytePair(0xFF, 0x00, out);
        bool blockPattern = true;
        for (int i = 0; i < 8; i++) if (out[i] != 1) blockPattern = false;
        for (int i = 8; i < 16; i++) if (out[i] != 0) blockPattern = false;
        check(blockPattern, "DFILE1=0xFF/DFILE2=0x00 gives 1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0 (block, not alternating)");

        // Swap the source bytes: the block order should flip
        SCLDVideo::expandHiresBytePair(0x00, 0xFF, out);
        bool swappedBlock = true;
        for (int i = 0; i < 8; i++) if (out[i] != 0) swappedBlock = false;
        for (int i = 8; i < 16; i++) if (out[i] != 1) swappedBlock = false;
        check(swappedBlock, "DFILE1=0x00/DFILE2=0xFF gives the mirrored block, 0x8 then 1x8");

        // Bit 7 (leftmost) of each byte maps to output column 0 of its own
        // 8-wide block (position 0 for dfile1, position 8 for dfile2) --
        // NOT output columns 0/1 as the old interleaved version had it.
        SCLDVideo::expandHiresBytePair(0b10000000, 0b10000000, out);
        check(out[0] == 1 && out[1] == 0, "bit 7 of dfile1Byte maps to output column 0, not shared with dfile2 at column 1");
        check(out[8] == 1 && out[9] == 0, "bit 7 of dfile2Byte maps to output column 8, starting the second block");
    }

    // ---- standardBitmapOffset: interleaved-thirds addressing, checked
    // against known ZX Spectrum facts (independently confirmed against
    // FUSE's display_line_start[] init loop -- exact algebraic match) ----
    printf("\n-- standardBitmapOffset --\n");
    {
        check(SCLDVideo::standardBitmapOffset(0, 0) == 0x0000, "row 0, col 0 is offset 0");
        check(SCLDVideo::standardBitmapOffset(1, 0) == 0x0100, "row 1 (second pixel row of the top character row) is +0x100, not +32 -- the classic Spectrum non-linear row spacing");
        check(SCLDVideo::standardBitmapOffset(8, 0) == 0x0020, "row 8 (second character row, first pixel row) is +0x20");
        check(SCLDVideo::standardBitmapOffset(64, 0) == 0x0800, "row 64 (start of the second 'third') is +0x800");
        check(SCLDVideo::standardBitmapOffset(191, 31) == 0x17FF, "row 191, col 31 (last pixel, last column) lands exactly on the display file's 6144-byte end (0x1800-1)");
    }

    // ---- standardAttributeOffset: the fix for a real bug -- attributes
    // are per 8x8 character cell, so this must NOT change with every
    // pixel row, only every 8. An earlier version of this formula lived
    // inline in Video.cpp, untested, as `0x1800 + row*32` -- wrong. ----
    printf("\n-- standardAttributeOffset --\n");
    {
        check(SCLDVideo::standardAttributeOffset(0, 0) == 0x1800, "row 0, col 0 is the attribute area's first byte");
        check(SCLDVideo::standardAttributeOffset(0, 0) == SCLDVideo::standardAttributeOffset(7, 0),
              "rows 0 and 7 (same character row) share the same attribute offset -- this is exactly what the old row*32 formula got wrong");
        check(SCLDVideo::standardAttributeOffset(7, 0) != SCLDVideo::standardAttributeOffset(8, 0),
              "rows 7 and 8 (different character rows) do NOT share an attribute offset");
        check(SCLDVideo::standardAttributeOffset(8, 0) == 0x1820, "row 8 (second character row) is +32 bytes from row 0's attribute offset");
        check(SCLDVideo::standardAttributeOffset(191, 31) == 0x1AFF, "row 191, col 31 (last attribute) lands exactly on the attribute area's 768-byte end (0x1800+768-1)");
    }

    // ---- hiresInkPaperColors: all 8 entries of the plan doc's table ----
    printf("\n-- hiresInkPaperColors (all 8 pairs) --\n");
    {
        // {field value, expected ink, expected paper} per
        // TS2068-ESPECTRUM-PORT-PLAN.md's port 0xFF table.
        struct { uint8_t field, ink, paper; const char* name; } table[8] = {
            {0b000, 0, 7, "black/white"},
            {0b001, 1, 6, "blue/yellow"},
            {0b010, 2, 5, "red/cyan"},
            {0b011, 3, 4, "magenta/green"},
            {0b100, 4, 3, "green/magenta"},
            {0b101, 5, 2, "cyan/red"},
            {0b110, 6, 1, "yellow/blue"},
            {0b111, 7, 0, "white/black"},
        };
        for (auto& row : table) {
            uint8_t ink, paper;
            SCLDVideo::hiresInkPaperColors(row.field, ink, paper);
            char label[64];
            snprintf(label, sizeof(label), "0b%d%d%d (%s) -> ink=%d paper=%d",
                     (row.field >> 2) & 1, (row.field >> 1) & 1, row.field & 1,
                     row.name, row.ink, row.paper);
            check(ink == row.ink && paper == row.paper, label);
        }
    }

    // ---- cellBytes: which bytes each of the eight DECR modes fetches ----
    // Offsets worked out by hand from the Spectrum layout, not from
    // standardBitmapOffset(), and the mode table from FUSE's
    // display_render.c (display_write_if_dirty_timex) and display.h
    // (display_get_addr: bit 0 adds ALTDFILE_OFFSET).
    printf("\n-- cellBytes (all eight modes) --\n");
    {
        static uint8_t d1[8192], d2[8192];
        for (int i = 0; i < 8192; i++) {
            d1[i] = (uint8_t) (i * 7 + 1);
            d2[i] = (uint8_t) (i * 13 + 5);
        }
        struct Cell { int row, col, bmp, att; };
        const Cell cells[] = {
            {   0,  0, 0x0000, 0x1800 },
            {   9,  5, 0x0125, 0x1825 },   // char row 1, pixel line 1
            { 100, 31, 0x0C9F, 0x199F },   // middle third, char row 12, line 4
            { 191, 17, 0x17F1, 0x1AF1 },   // last line
        };
        bool ok[8] = { true, true, true, true, true, true, true, true };
        for (const Cell& c : cells) {
            uint8_t want[8][2] = {
                { d1[c.bmp], d1[c.att] },   // 0 standard
                { d2[c.bmp], d2[c.att] },   // 1 second screen
                { d1[c.bmp], d2[c.bmp] },   // 2 extended colour: 8x1 attributes from DFILE2's bitmap
                { d2[c.bmp], d2[c.bmp] },   // 3 extended colour, second screen
                { d1[c.bmp], d1[c.att] },   // 4 hi-res, right half from DFILE1 attributes
                { d2[c.bmp], d2[c.att] },   // 5 the same, second screen
                { d1[c.bmp], d2[c.bmp] },   // 6 true hi-res
                { d2[c.bmp], d2[c.bmp] },   // 7 doubled columns from DFILE2
            };
            for (int m = 0; m < 8; m++) {
                uint8_t data, second;
                // Upper DECR bits (hi-res colour) must not change the mode.
                SCLDVideo::cellBytes((uint8_t) (m | 0x38), d1, d2, c.row, c.col, data, second);
                if (data != want[m][0] || second != want[m][1]) ok[m] = false;
            }
        }
        const char* names[8] = {
            "mode 0 (standard): DFILE1 bitmap + DFILE1 attributes",
            "mode 1 (second screen): DFILE2 bitmap + DFILE2 attributes",
            "mode 2 (extended colour): DFILE1 bitmap + 8x1 attributes from DFILE2",
            "mode 3: DFILE2 bitmap + the same DFILE2 byte as attribute",
            "mode 4 (hi-res): DFILE1 bitmap + DFILE1 attribute bytes",
            "mode 5 (hi-res): DFILE2 bitmap + DFILE2 attribute bytes",
            "mode 6 (true hi-res): DFILE1 bitmap + DFILE2 bitmap",
            "mode 7 (hi-res doubled): DFILE2 bitmap twice",
        };
        for (int m = 0; m < 8; m++) check(ok[m], names[m]);
    }

    printf("\n====================\n");
    if (failures == 0) {
        printf("ALL PASSED\n");
    } else {
        printf("%d CHECK(S) FAILED\n", failures);
    }

    return failures;
}
