#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pico/types.h"

// 640x480 DVI from HSTX, showing a 320x240 RGB332 frame buffer with every
// pixel doubled in both directions (the TS2068's screen plus border, doubled).
#define DVI_FB_W 320
#define DVI_FB_H 240

extern uint8_t dvi_fb[DVI_FB_H][DVI_FB_W];

// Frames sent since dvi_start(); updated in the DMA interrupt.
extern volatile uint32_t dvi_frames;
// time_us_32() when the last frame started, taken in the interrupt.
extern volatile uint32_t dvi_frame_time_us;

// Sets clk_hstx to clk_sys / 2. Call after the system clock is final.
void dvi_clock_init(void);

// Starts the output, with the DMA interrupt on the calling core.
void dvi_start(void);

// Total lines per frame: 525 (standard 480p, 60.00 Hz at 25.2 MHz) or 524
// (60.11 Hz, the TS2068's own rate). Takes effect at the next frame.
void dvi_set_total_lines(uint lines);
uint dvi_total_lines(void);

static inline uint8_t rgb332(uint8_t r, uint8_t g, uint8_t b) {
    return (r & 0xe0) | ((g & 0xe0) >> 3) | (b >> 6);
}
