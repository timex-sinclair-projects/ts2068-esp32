// 640x480 DVI through HSTX's command expander and TMDS encoder.
//
// Based on pico-examples hstx/dvi_out_hstx_encoder (BSD-3-Clause, Raspberry
// Pi Ltd). The RP2350pc's HDMI pins match the Pico DVI Sock that example
// targets: D0 = GPIO12/13, CK = 14/15, D2 = 16/17, D1 = 18/19.
//
// Copyright (c) 2024 Raspberry Pi Ltd.
// SPDX-License-Identifier: BSD-3-Clause
//
// Changes from the example:
// - clk_hstx is clk_sys / 2 = 126 MHz, so the pixel clock is exactly
//   25.2 MHz (the example leaves it at 125 MHz: 25.0 MHz, 59.5 Hz).
// - The picture is a 320x240 frame buffer, doubled both ways. Each source
//   row is expanded into one of two 640-byte line buffers a line ahead of
//   the DMA, so the interrupt never writes a line that is being sent.
// - The frame can be 524 lines instead of 525 (one line less of front
//   porch): 25.2 MHz / (800 x 524) = 60.114 Hz, the TS2068's 60.1145 Hz.

#include "dvi.h"

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/structs/timer.h"

uint8_t dvi_fb[DVI_FB_H][DVI_FB_W] __attribute__((aligned(4)));
volatile uint32_t dvi_frames;
volatile uint32_t dvi_frame_time_us;

// ----------------------------------------------------------------------------
// DVI constants

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define H_FRONT_PORCH   16
#define H_SYNC_WIDTH    96
#define H_BACK_PORCH    48
#define H_ACTIVE_PIXELS 640

#define V_SYNC_WIDTH    2
#define V_BACK_PORCH    33
#define V_ACTIVE_LINES  480

#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// Front porch lines: 10 gives 525 lines a frame, 9 gives 524.
static volatile uint v_front_porch = 10;

// ----------------------------------------------------------------------------
// HSTX command lists, padded with NOPs as in the example so the DMA doesn't
// ping-pong faster than the interrupt can follow.

static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (H_BACK_PORCH + H_ACTIVE_PIXELS),
    SYNC_V1_H1,
    HSTX_CMD_NOP
};

static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (H_BACK_PORCH + H_ACTIVE_PIXELS),
    SYNC_V0_H1,
    HSTX_CMD_NOP
};

static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS | H_ACTIVE_PIXELS
};

// Two expanded lines, used by source-row parity.
static uint32_t line_buf[2][H_ACTIVE_PIXELS / 4];

// ----------------------------------------------------------------------------
// Line expansion: 320 RGB332 pixels -> 640, each pixel twice.

static void __not_in_flash_func(expand_row)(uint32_t *dst, const uint32_t *src) {
    for (uint i = 0; i < DVI_FB_W / 4; i++) {
        uint32_t w = src[i];
        uint32_t a = w & 0xff, b = (w >> 8) & 0xff, c = (w >> 16) & 0xff, d = w >> 24;
        dst[0] = (a | b << 16) * 0x0101u;
        dst[1] = (c | d << 16) * 0x0101u;
        dst += 2;
    }
}

// ----------------------------------------------------------------------------
// DMA

#define DMACH_PING 0
#define DMACH_PONG 1

static bool dma_pong = false;
static uint v_scanline = 2;
static bool vactive_cmdlist_posted = false;

void __scratch_x("") dma_irq_handler(void) {
    uint ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong = !dma_pong;

    uint vfp = v_front_porch;
    uint v_active_start = vfp + V_SYNC_WIDTH + V_BACK_PORCH;
    uint v_total = v_active_start + V_ACTIVE_LINES;

    if (v_scanline >= vfp && v_scanline < vfp + V_SYNC_WIDTH) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
    } else if (v_scanline < v_active_start) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
        if (v_scanline == v_active_start - 1) {
            // Row 0 for the first active line.
            expand_row(line_buf[0], (const uint32_t *)dvi_fb[0]);
        }
    } else if (!vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;
    } else {
        uint y = v_scanline - v_active_start;
        uint row = y >> 1;
        ch->read_addr = (uintptr_t)line_buf[row & 1];
        ch->transfer_count = H_ACTIVE_PIXELS / sizeof(uint32_t);
        vactive_cmdlist_posted = false;
        // On a row's second line, its partner buffer was last sent two lines
        // ago: fill it with the next row.
        if ((y & 1) && row + 1 < DVI_FB_H) {
            expand_row(line_buf[(row + 1) & 1], (const uint32_t *)dvi_fb[row + 1]);
        }
    }

    if (!vactive_cmdlist_posted) {
        v_scanline++;
        if (v_scanline >= v_total) {
            v_scanline = 0;
            dvi_frame_time_us = timer0_hw->timerawl;
            dvi_frames++;
        }
    }
}

// ----------------------------------------------------------------------------

void dvi_clock_init(void) {
    uint32_t sys = clock_get_hz(clk_sys);
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys, sys / 2);
}

void dvi_set_total_lines(uint lines) {
    v_front_porch = lines == 524 ? 9 : 10;
}

uint dvi_total_lines(void) {
    return v_front_porch + V_SYNC_WIDTH + V_BACK_PORCH + V_ACTIVE_LINES;
}

void dvi_start(void) {
    // TMDS encoder for RGB332: R in bits 7:5 (lane 2), G in 4:2 (lane 1),
    // B in 1:0 (lane 0).
    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // Pixels come four to a word; control symbols are a whole word.
    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // A clock period of 5 HSTX cycles, 2 bits a cycle: 10 bits a pixel.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Clock pair on HSTX bits 2/3 (GPIO14/15).
    hstx_ctrl_hw->bit[2] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[3] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        static const int lane_to_output_bit[3] = {0, 6, 4};
        int bit = lane_to_output_bit[lane];
        uint32_t sel =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit    ] = sel;
        hstx_ctrl_hw->bit[bit + 1] = sel | HSTX_CTRL_BIT0_INV_BITS;
    }

    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, 0);  // HSTX
    }

    dma_channel_claim(DMACH_PING);
    dma_channel_claim(DMACH_PONG);

    dma_channel_config c;
    c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo,
                          vblank_line_vsync_off, count_of(vblank_line_vsync_off), false);
    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo,
                          vblank_line_vsync_off, count_of(vblank_line_vsync_off), false);

    dma_hw->ints0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    dma_hw->inte0 = (1u << DMACH_PING) | (1u << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, 0);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(DMACH_PING);
}
