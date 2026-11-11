/*
 * Olimex RP2350pc, Rev A/A1.
 *
 * Pin assignments are from the Rev A1 KiCad netlist (OLIMEX/RP2350pc,
 * HARDWARE/RP2350pc-Rev.A1), cross-checked against the BOM. See PLAN.md §1.1.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _BOARDS_OLIMEX_RP2350PC_H
#define _BOARDS_OLIMEX_RP2350PC_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define OLIMEX_RP2350PC

// RP2350B (QFN-80, 48 GPIO).
#define PICO_RP2350A 0

// --- UART: UEXT1 pins 3/4 (the console; native USB is the host port) ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

// --- I2C0: HDMI DDC (through BSS138 shifters) and the ES8311's control bus ---
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 32
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 33
#endif

// --- SPI1: the microSD socket (SPI mode only; DAT1/DAT2 are not wired) ---
#ifndef PICO_DEFAULT_SPI
#define PICO_DEFAULT_SPI 1
#endif
#ifndef PICO_DEFAULT_SPI_SCK_PIN
#define PICO_DEFAULT_SPI_SCK_PIN 10
#endif
#ifndef PICO_DEFAULT_SPI_TX_PIN
#define PICO_DEFAULT_SPI_TX_PIN 11
#endif
#ifndef PICO_DEFAULT_SPI_RX_PIN
#define PICO_DEFAULT_SPI_RX_PIN 24
#endif
#ifndef PICO_DEFAULT_SPI_CSN_PIN
#define PICO_DEFAULT_SPI_CSN_PIN 9
#endif

// --- Board-specific pins ---
#define RP2350PC_SD_SCK_PIN      10
#define RP2350PC_SD_MOSI_PIN     11
#define RP2350PC_SD_MISO_PIN     24
#define RP2350PC_SD_CS_PIN       9
#define RP2350PC_SD_DETECT_PIN   41   // also on the EXT1 header

// HSTX: D0 = 12/13, CK = 14/15, D2 = 16/17, D1 = 18/19 (Pico DVI Sock order)
#define RP2350PC_HDMI_CEC_PIN    20
#define RP2350PC_HDMI_HPD_PIN    21

#define RP2350PC_PSRAM_CS_PIN    8    // QMI CS1 (not 47, as on other RP2350B boards)
#define PICO_PSRAM_CS_PIN        RP2350PC_PSRAM_CS_PIN

#define RP2350PC_AUDIO_PWM_L_PIN 26   // PWM5_A, RC-filtered to the jack and amplifier
#define RP2350PC_AUDIO_PWM_R_PIN 27   // PWM5_B
#define RP2350PC_CODEC_PWR_DIS_N_PIN 22  // ES8311 analog LDO enable (low = off)
#define RP2350PC_CODEC_MCLK_PIN  23
#define RP2350PC_CODEC_DOUT_PIN  28   // ES8311 ASDOUT -> MCU
#define RP2350PC_CODEC_LRCK_PIN  29
#define RP2350PC_CODEC_SCLK_PIN  30
#define RP2350PC_CODEC_DIN_PIN   31   // MCU -> ES8311 DSDIN
#define RP2350PC_AMP_VOL_PIN     34   // PWM9_A; higher duty = higher VOL voltage
#define RP2350PC_AMP_SHDN_PIN    35   // high = amplifier off (inverted by T4)
#define RP2350PC_PWR_SENSE_PIN   46
#define RP2350PC_BAT_SENSE_PIN   47

// --- Flash: W25Q128JV, 16 MB ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

// Divider 3, as on the TS-Pico v3: the SDK re-enables XIP with this divider
// after every flash erase or program, and at 252 MHz divider 2 would run the
// flash at 126 MHz.
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 3
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
