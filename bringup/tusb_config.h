#pragma once

// TinyUSB host on the RP2350's native USB, which feeds the FE1.1s hub.
// Based on the configuration paulscottrobson/rp2350-modules uses on this
// board (keyboard, mouse and USB stick through the hub).

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_OS                 OPT_OS_PICO
#define CFG_TUSB_DEBUG              0
#define CFG_TUH_ENABLED             1
#define CFG_TUSB_RHPORT0_MODE       OPT_MODE_HOST
#define BOARD_TUH_RHPORT            0
#define CFG_TUH_MAX_SPEED           OPT_MODE_FULL_SPEED

#define CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_ALIGN           __attribute__((aligned(4)))

#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB                 1
#define CFG_TUH_DEVICE_MAX          (4 + 1)   // the hub's four ports, and the hub
#define CFG_TUH_HID                 (3 * CFG_TUH_DEVICE_MAX)
#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64
