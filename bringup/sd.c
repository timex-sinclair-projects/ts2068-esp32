// microSD over SPI1, with just enough FAT to list the root directory.
//
// The RP2350pc wires the socket for SPI only (DAT1/DAT2 have pull-ups and
// nothing else), which is what the TS-Pico firmware's own TS/sdcard.py
// expects. Rev A1 netlist: CLK GPIO10, CMD/MOSI 11, DAT0/MISO 24,
// DAT3/CS 9, card detect 41.

#include "sd.h"

#include <stdio.h>
#include <string.h>

#include "console.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"

#define SD_SPI spi1

static bool block_addressing;

static inline void cs(bool on) {
    gpio_put(RP2350PC_SD_CS_PIN, !on);
}

static uint8_t xfer(uint8_t b) {
    uint8_t r;
    spi_write_read_blocking(SD_SPI, &b, &r, 1);
    return r;
}

static bool wait_ready(uint32_t ms) {
    absolute_time_t end = make_timeout_time_ms(ms);
    while (xfer(0xff) != 0xff) {
        if (time_reached(end)) return false;
    }
    return true;
}

static void deselect(void) {
    cs(false);
    xfer(0xff);  // releases MISO
}

// Sends a command with CS held low; returns R1. The caller deselects.
static uint8_t command(uint8_t cmd, uint32_t arg) {
    cs(true);
    if (cmd != 0 && !wait_ready(500)) return 0xff;
    uint8_t frame[6] = {
        (uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg,
        cmd == 0 ? 0x95 : cmd == 8 ? 0x87 : 0x01,
    };
    spi_write_blocking(SD_SPI, frame, sizeof frame);
    uint8_t r = 0xff;
    for (int i = 0; i < 10; i++) {
        r = xfer(0xff);
        if (!(r & 0x80)) break;
    }
    return r;
}

static uint8_t acmd(uint8_t cmd, uint32_t arg) {
    command(55, 0);
    deselect();
    return command(cmd, arg);
}

static bool read_data(uint8_t *buf, uint len) {
    absolute_time_t end = make_timeout_time_ms(200);
    uint8_t t;
    while ((t = xfer(0xff)) == 0xff) {
        if (time_reached(end)) return false;
    }
    if (t != 0xfe) return false;
    spi_read_blocking(SD_SPI, 0xff, buf, len);
    xfer(0xff);  // CRC
    xfer(0xff);
    return true;
}

bool sd_detect_level(void) {
    return gpio_get(RP2350PC_SD_DETECT_PIN);
}

int sd_init(sd_info_t *info) {
    memset(info, 0, sizeof *info);
    gpio_init(RP2350PC_SD_DETECT_PIN);
    gpio_pull_up(RP2350PC_SD_DETECT_PIN);

    spi_init(SD_SPI, 400 * 1000);
    gpio_set_function(RP2350PC_SD_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(RP2350PC_SD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(RP2350PC_SD_MISO_PIN, GPIO_FUNC_SPI);
    gpio_pull_up(RP2350PC_SD_MISO_PIN);
    gpio_init(RP2350PC_SD_CS_PIN);
    gpio_set_dir(RP2350PC_SD_CS_PIN, GPIO_OUT);
    cs(false);

    // 80+ clocks with CS high.
    for (int i = 0; i < 10; i++) xfer(0xff);

    uint8_t r = 0xff;
    for (int i = 0; i < 10 && r != 0x01; i++) {
        r = command(0, 0);
        deselect();
    }
    if (r != 0x01) return -1;

    bool v2 = false;
    r = command(8, 0x1aa);
    if (r == 0x01) {
        uint8_t b[4];
        spi_read_blocking(SD_SPI, 0xff, b, 4);
        if ((b[2] & 0x0f) != 0x01 || b[3] != 0xaa) {
            deselect();
            return -2;
        }
        v2 = true;
    }
    deselect();

    absolute_time_t end = make_timeout_time_ms(1500);
    do {
        r = acmd(41, v2 ? 0x40000000 : 0);
        deselect();
        if (time_reached(end)) return -3;
    } while (r != 0);

    block_addressing = false;
    if (v2) {
        r = command(58, 0);
        uint8_t ocr[4];
        spi_read_blocking(SD_SPI, 0xff, ocr, 4);
        deselect();
        if (r != 0) return -4;
        block_addressing = (ocr[0] & 0x40) != 0;
    }
    if (!block_addressing) {
        command(16, 512);
        deselect();
    }
    info->sdhc = block_addressing;

    info->spi_hz = spi_set_baudrate(SD_SPI, 25 * 1000 * 1000);

    uint8_t csd[16];
    if (command(9, 0) != 0 || !read_data(csd, 16)) {
        deselect();
        return -5;
    }
    deselect();
    if ((csd[0] >> 6) == 1) {
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3f) << 16) | (csd[8] << 8) | csd[9];
        info->sectors = (c_size + 1) * 1024;
    } else {
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03) << 10) | (csd[7] << 2) | (csd[8] >> 6);
        uint32_t mult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
        uint32_t read_bl_len = csd[5] & 0x0f;
        info->sectors = ((c_size + 1) << (mult + 2)) << read_bl_len >> 9;
    }

    uint8_t cid[16];
    if (command(10, 0) == 0 && read_data(cid, 16)) {
        info->manufacturer = cid[0];
        memcpy(info->product, &cid[3], 5);
        info->product[5] = 0;
    }
    deselect();
    return 0;
}

bool sd_read(uint32_t lba, uint8_t *buf) {
    bool ok = command(17, block_addressing ? lba : lba * 512) == 0 && read_data(buf, 512);
    deselect();
    return ok;
}

// ----------------------------------------------------------------------------
// FAT, read-only, root directory only.

static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int sd_list_root(int max_entries) {
    static uint8_t buf[512];
    if (!sd_read(0, buf)) return -1;
    if (buf[510] != 0x55 || buf[511] != 0xaa) return -2;

    uint32_t part = 0;
    uint8_t type = buf[0x1c2];
    // An MBR rather than a boot sector: no jump instruction at the start.
    if (buf[0] != 0xeb && buf[0] != 0xe9) {
        part = le32(&buf[0x1c6]);
        if (!sd_read(part, buf)) return -3;
    } else {
        type = 0;
    }

    uint16_t bps = le16(&buf[0x0b]);
    uint8_t spc = buf[0x0d];
    uint16_t reserved = le16(&buf[0x0e]);
    uint8_t nfats = buf[0x10];
    uint16_t root_entries = le16(&buf[0x11]);
    uint32_t fat_size = le16(&buf[0x16]);
    bool fat32 = fat_size == 0;
    if (fat32) fat_size = le32(&buf[0x24]);
    uint32_t root_cluster = fat32 ? le32(&buf[0x2c]) : 0;
    if (bps != 512 || spc == 0 || nfats == 0) return -4;

    char label[12] = {0};
    memcpy(label, &buf[fat32 ? 0x47 : 0x2b], 11);
    say("  partition at %lu, type %02x, %s, %u sectors/cluster\n",
        (unsigned long)part, type, fat32 ? "FAT32" : "FAT16", spc);

    uint32_t fat_start = part + reserved;
    uint32_t root_start = fat_start + nfats * fat_size;
    uint32_t data_start = root_start + (fat32 ? 0 : (root_entries * 32 + 511) / 512);

    int listed = 0;
    uint32_t cluster = root_cluster;
    uint32_t fixed_sectors = fat32 ? 0 : (root_entries * 32 + 511) / 512;
    for (uint32_t n = 0;; n++) {
        uint32_t lba;
        if (fat32) {
            if (cluster < 2 || cluster >= 0x0ffffff8) break;
            uint32_t in_cluster = n % spc;
            lba = data_start + (cluster - 2) * spc + in_cluster;
        } else {
            if (n >= fixed_sectors) break;
            lba = root_start + n;
        }
        if (!sd_read(lba, buf)) return -5;
        for (int e = 0; e < 16; e++) {
            const uint8_t *d = &buf[e * 32];
            if (d[0] == 0) return listed;
            if (d[0] == 0xe5 || d[11] == 0x0f) continue;
            char name[13];
            int k = 0;
            for (int i = 0; i < 8 && d[i] != ' '; i++) name[k++] = (char)d[i];
            if (d[8] != ' ') {
                name[k++] = '.';
                for (int i = 8; i < 11 && d[i] != ' '; i++) name[k++] = (char)d[i];
            }
            name[k] = 0;
            if (d[11] & 0x08) {
                say("  [volume %s]\n", name);
            } else if (d[11] & 0x10) {
                say("  %-12s  <DIR>\n", name);
            } else {
                say("  %-12s %8lu\n", name, (unsigned long)le32(&d[28]));
            }
            if (++listed >= max_entries) return listed;
        }
        if (fat32 && (n % spc) == (uint32_t)spc - 1) {
            // Next cluster from the FAT.
            if (!sd_read(fat_start + cluster / 128, buf)) return -6;
            cluster = le32(&buf[(cluster % 128) * 4]) & 0x0fffffff;
        }
    }
    return listed;
}
