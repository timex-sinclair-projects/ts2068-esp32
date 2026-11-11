/*

ESP2068 — TS2068 port of ESPectrum

SCLDVideo.h — pixel-expansion core for the TS2068's SCLD display modes.
New for this port; there is no upstream ESPectrum equivalent.

This is deliberately narrow: it answers "given these source bytes/rows/
columns, which pixels are ink vs paper, and what memory offset holds
them" and nothing else — not color/RGB conversion, not framebuffer
writes, not VGA/DMA timing. That keeps it pure and host-testable (see
test/host/scldvideo_test.cpp), which matters because this *spatial*
math (bit order, interleave order, row addressing) is exactly the kind
of off-by-one that's easy to get subtly wrong and hard to notice by eye
on real hardware — a wrong ink/paper swap still "looks like a picture,"
just a mirrored or miscolored one. It also turned out to matter in
practice: the attribute-row formula that used to live inline in
Video.cpp (not here, not tested) had exactly this kind of bug — see
below.

Corrected 2026-07-10 against the FUSE emulator's source (a mature,
real, TS2068-emulating GPL codebase — /Users/david/Downloads/fuse-1.9.0
locally; peripherals/scld.c, display.c, display.h, ui/fb/fbdisplay.c),
after an earlier version of this file got two things wrong that a TS2068
reference library's summarized docs hadn't caught:

  - Standard mode's attribute-row addressing used to live inline in
    Video.cpp as `0x1800 + row * 32` (row = pixel row, 0..191) — wrong,
    since attributes are per 8x8 character cell and only change every 8
    pixel rows. FUSE's display_attr_start[y] = 6144 + 32*(y/8) is the
    correct shape; standardAttributeOffset() below matches it exactly.
    This was live in the shipped renderer with no test ever exercising
    it — the bitmap-row formula (standardBitmapOffset(), below) *was*
    hand-verified against known Spectrum addressing facts before
    shipping and turned out to be correct (independently re-confirmed
    against FUSE's display_line_start[] init loop, exact algebraic
    match) — the asymmetry is the point: hand-verification without an
    automated test is exactly how the attribute-row bug got through.

  - Hi-res mode's dual-file interleave used to be bit-by-bit alternating
    (DFILE1 bit i at output 2i, DFILE2 bit i at output 2i+1) — wrong.
    FUSE's uidisplay_plot16 (ui/fb/fbdisplay.c) shows a 16-bit value
    `(dfile1_byte << 8) | dfile2_byte` expanded MSB-first straight
    across 16 pixels — i.e. a BLOCK arrangement (all 8 bits of the
    DFILE1 byte, then all 8 bits of the DFILE2 byte), not an interleave.
    expandHiresBytePair() below matches FUSE's real behavior now.

  - What FUSE calls "hires" is a four-way family (DECR bits 2..0 = 4/5/
    6/7: HIRESATTR/HIRESATTRALTD/HIRES/HIRESDOUBLECOL — see
    peripherals/scld.h), not the single mode this port implements. Only
    HIRES (0x06 exactly — data from DFILE1's bitmap + DFILE2's bitmap,
    both via the same interleaved-thirds addressing) is implemented
    here; expandHiresBytePair() is only correct for that one. The other
    three source their second byte from an attribute area instead of a
    bitmap area (HIRESATTR: DFILE1's own attribute byte; HIRESATTRALTD:
    DFILE2's attribute byte; HIRESDOUBLECOL: the same DFILE1 byte
    twice) — see display.c lines ~391-412 for the exact per-mode byte
    sourcing if implementing them later. Video.cpp's caller is
    responsible for only invoking hi-res rendering when
    SCLD::videoMode == 0x06 exactly, not just "bit 2 is set".
    (Superseded 2026-11-11: cellBytes() below now sources the bytes for
    all eight DECR modes, matching FUSE's display_render.c; the block
    expansion above is right for every hi-res mode once the right two
    bytes are passed in.)

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

#ifndef SCLDVideo_h
#define SCLDVideo_h

#include <inttypes.h>

class SCLDVideo {
public:

    // Byte offset (relative to a display file's own base, i.e. relative
    // to SCLD::DFILE1_BASE/DFILE2_BASE) of the bitmap byte at pixel row
    // `row` (0..191), byte column `col` (0..31) — the classic Spectrum/
    // TS2068 "interleaved thirds" layout. Confirmed against FUSE's
    // display_line_start[] init loop (display.c) — exact algebraic
    // match to this formula, not just a passing resemblance.
    static int standardBitmapOffset(int row, int col);

    // Byte offset (relative to a display file's own base) of the
    // attribute byte covering pixel row `row`, byte column `col`.
    // Attributes are per 8x8 character cell, so this only changes every
    // 8 rows — confirmed against FUSE's display_attr_start[] (display.c:
    // 6144 + 32*(y/8)). Getting the row>>3 right here is the fix for a
    // real bug — see this header's top comment.
    static int standardAttributeOffset(int row, int col);

    // Expands one standard-mode source byte (bitmap) + its attribute byte
    // into 16 output pixels (physical, pixel-doubled). out[i] is a color
    // index 0..7 (the standard Spectrum ink/paper palette — see
    // TS2068-ESPECTRUM-PORT-PLAN.md/the reference library's "Attribute
    // Byte Format": bits 2..0 = ink, 5..3 = paper). BRIGHT/FLASH are
    // attribute-level modifiers, not per-pixel, so this function leaves
    // them out of `out[]` — callers read att's bits 6/7 directly.
    static void expandStandardByte(uint8_t bmp, uint8_t att, uint8_t out[16]);

    // Expands one hi-res-mode (DECR == 0x06 exactly, "HIRES" in FUSE's
    // naming) source byte pair into 16 output pixels: out[0..7] are
    // dfile1Byte's bits 7..0 (MSB first), out[8..15] are dfile2Byte's
    // bits 7..0 — a BLOCK arrangement, not bit-by-bit interleave (see
    // this header's top comment for why that correction happened).
    // out[i] is 0 (paper) or 1 (ink) — hi-res mode has no per-pixel
    // color, just the two colors SCLD::hiresInkPaper selects globally.
    static void expandHiresBytePair(uint8_t dfile1Byte, uint8_t dfile2Byte, uint8_t out[16]);

    // Decodes SCLD::hiresInkPaper (0..7) into the two Spectrum-palette
    // color indices it selects, per TS2068-ESPECTRUM-PORT-PLAN.md's
    // port 0xFF table. inkOut/paperOut are 0..7, same palette as
    // expandStandardByte()'s out[] values.
    static void hiresInkPaperColors(uint8_t hiresInkPaper, uint8_t& inkOut, uint8_t& paperOut);

    // The two bytes the SCLD fetches for byte column `col` of pixel row
    // `row` in screen mode `mode` (DECR bits 2..0), from the two display
    // files (dfile1 = HOME RAM 0x4000, dfile2 = 0x6000). Matches FUSE's
    // display_write_if_dirty_timex() / display_get_attr_byte()
    // (display_render.c) and display_get_addr() (display.h): bit 0 picks
    // the file the bitmap byte comes from, bit 1 takes attributes 8x1
    // from DFILE2's bitmap area, bit 2 is hi-res.
    //
    //   mode  data (bitmap)        second
    //   0     DFILE1 bitmap        DFILE1 attribute (8x8)   standard
    //   1     DFILE2 bitmap        DFILE2 attribute (8x8)   second screen
    //   2     DFILE1 bitmap        DFILE2 bitmap, same row  extended colour (8x1)
    //   3     DFILE2 bitmap        DFILE2 bitmap, same row  (the same byte as data)
    //   4     DFILE1 bitmap        DFILE1 attribute area    hi-res, right half from attributes
    //   5     DFILE2 bitmap        DFILE2 attribute area    the same from the second screen
    //   6     DFILE1 bitmap        DFILE2 bitmap            true hi-res
    //   7     DFILE2 bitmap        the same byte again      hi-res, doubled columns
    //
    // In modes 0-3 `second` is an attribute byte (expandStandardByte());
    // in 4-7 it is the right-hand 8 pixels (expandHiresBytePair()).
    static void cellBytes(uint8_t mode, const uint8_t* dfile1, const uint8_t* dfile2,
                          int row, int col, uint8_t& data, uint8_t& second);

};

#endif // SCLDVideo_h
