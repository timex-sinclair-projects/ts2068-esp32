#pragma once

#include <stddef.h>

#include "pico/types.h"

// The APS6404L on QMI CS1, mapped at 0x11000000 (cached) and 0x15000000
// (uncached). Returns the detected size in bytes, or 0 if none answered.
size_t psram_init(uint cs_pin);

#define PSRAM_CACHED   ((volatile uint32_t *)0x11000000)
#define PSRAM_UNCACHED ((volatile uint32_t *)0x15000000)
