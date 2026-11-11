/*

ts2068-esp32 — TS2068 port of ESPectrum

SCLDVideo.cpp — see include/SCLDVideo.h.

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

#include "SCLDVideo.h"

int SCLDVideo::standardBitmapOffset(int row, int col) {
    return ((row & 0xC0) << 5) | ((row & 0x07) << 8) | ((row & 0x38) << 2) | col;
}

int SCLDVideo::standardAttributeOffset(int row, int col) {
    return 0x1800 + (row >> 3) * 32 + col;
}

void SCLDVideo::expandStandardByte(uint8_t bmp, uint8_t att, uint8_t out[16]) {

    uint8_t ink = att & 0x07;
    uint8_t paper = (att >> 3) & 0x07;

    for (int i = 0; i < 8; i++) {
        uint8_t pixel = (bmp & (0x80 >> i)) ? ink : paper; // bit 7 = leftmost
        out[2*i] = pixel;
        out[2*i + 1] = pixel; // physical pixel-doubling, see SCLDVideo.h
    }
}

void SCLDVideo::expandHiresBytePair(uint8_t dfile1Byte, uint8_t dfile2Byte, uint8_t out[16]) {

    for (int i = 0; i < 8; i++) {
        out[i]     = (dfile1Byte & (0x80 >> i)) ? 1 : 0; // bit 7 = leftmost; block, not interleaved
        out[8 + i] = (dfile2Byte & (0x80 >> i)) ? 1 : 0;
    }
}

void SCLDVideo::hiresInkPaperColors(uint8_t hiresInkPaper, uint8_t& inkOut, uint8_t& paperOut) {
    // TS2068-ESPECTRUM-PORT-PLAN.md's port 0xFF table (000 black/white,
    // 001 blue/yellow, ... 111 white/black) reduces to ink==the 3-bit
    // field itself and paper==its bitwise complement, verified against
    // all 8 entries individually before relying on the shortcut here.
    inkOut = hiresInkPaper & 0x07;
    paperOut = inkOut ^ 0x07;
}

void SCLDVideo::cellBytes(uint8_t mode, const uint8_t* dfile1, const uint8_t* dfile2,
                          int row, int col, uint8_t& data, uint8_t& second) {

    mode &= 0x07;
    int bmp = standardBitmapOffset(row, col);
    int att = standardAttributeOffset(row, col);
    const uint8_t* base = (mode & 0x01) ? dfile2 : dfile1;   // FUSE's display_get_addr()

    data = base[bmp];

    switch (mode) {
        case 0: case 1: second = base[att]; break;          // 8x8 attributes from the same file
        case 2: case 3: second = dfile2[bmp]; break;        // 8x1 attributes, DFILE2's bitmap area
        case 4: case 5: second = base[att]; break;          // hi-res: attribute bytes as pixels
        case 6:         second = dfile2[bmp]; break;        // true hi-res
        default:        second = data; break;               // 7: doubled columns
    }
}
