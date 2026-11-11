#pragma once

#include <stdbool.h>
#include <stdint.h>

// microSD in SPI mode on SPI1 (CLK 10, MOSI 11, MISO 24, CS 9).
typedef struct {
    bool sdhc;            // block addressing
    uint32_t sectors;     // capacity in 512-byte sectors
    char product[6];      // from the CID
    uint8_t manufacturer;
    uint32_t spi_hz;
} sd_info_t;

// 0 on success, or a negative step number that failed.
int sd_init(sd_info_t *info);
bool sd_read(uint32_t lba, uint8_t *buf);

// Card-detect switch level (GPIO41, pulled up).
bool sd_detect_level(void);

// Lists the root directory of the first FAT partition through say().
// Returns the number of entries listed, or a negative error.
int sd_list_root(int max_entries);
