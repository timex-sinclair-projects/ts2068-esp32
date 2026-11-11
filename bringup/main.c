// Phase 0 bring-up for the TS2068 on the Olimex RP2350pc (PLAN.md §3).
//
// Checks each part of the board the emulator needs, on its own, and reports
// on the HDMI screen and on UART0 (UEXT1 pins 3/4, 115200 8N1):
//   1. clocks: 252 MHz system, 126 MHz HSTX (an exact 25.2 MHz pixel clock)
//   2. video: 640x480 DVI from HSTX on core 1, with TS2068 colour bars
//   3. PSRAM: detect, 8 MB pattern test, bandwidth
//   4. audio: PWM tones through the jack and the amplifier
//   5. microSD: SPI init, CID/CSD, FAT root directory, read speed
//   6. USB: TinyUSB host through the hub; typed keys are echoed
//
// Keys: F1 switches 525/524-line video (60.00/60.11 Hz), F2 plays a tone,
// + and - change the amplifier volume, F3 reruns the SD test.

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "console.h"
#include "dvi.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "psram.h"
#include "sd.h"
#include "usbkbd.h"

#define SYS_KHZ 252000

// Flash clock divider. Runs from RAM: it changes how flash is read.
static void __no_inline_not_in_flash_func(set_flash_clkdiv)(uint div) {
    qmi_hw->m[0].timing = (qmi_hw->m[0].timing & ~QMI_M0_TIMING_CLKDIV_BITS) | (div << QMI_M0_TIMING_CLKDIV_LSB);
}

static void core1_main(void) {
    dvi_start();
    while (true) {
        __wfi();
    }
}

static void pass(const char *what) {
    say_c(C_GREEN | C_BRIGHT, "PASS ");
    say("%s\n", what);
}

static void fail(const char *what) {
    say_c(C_RED | C_BRIGHT, "FAIL ");
    say("%s\n", what);
}

// ----------------------------------------------------------------------------

static void draw_header(void) {
    memset(dvi_fb, ts_colour(C_BLACK), sizeof dvi_fb);
    char title[CON_COLS + 1];
    snprintf(title, sizeof title, "%-40.40s", " TS2068-2350 bring-up: Phase 0");
    console_at(0, 0, C_WHITE | C_BRIGHT, C_BLUE, title);

    // Rows 2-3: the 16 TS2068 colours, normal then BRIGHT, 20 pixels each.
    for (int c = 0; c < 16; c++) {
        uint8_t v = ts_colour((c & 7) | (c >= 8 ? C_BRIGHT : 0));
        for (int y = 16; y < 32; y++) {
            memset(&dvi_fb[y][c * 20], v, 20);
        }
    }
    console_at(0, 4, C_YELLOW, C_BLACK, "F1 524/525 lines  F2 tone  +/- volume");
    console_at(0, 5, C_YELLOW, C_BLACK, "F3 SD test again  type to echo");
}

// hz1000: the measured frame rate in mHz.
static void status_line(uint32_t hz1000, uint secs, int volume) {
    char s[64];
    snprintf(s, sizeof s, "%2lu.%03lu Hz %u lines  vol %3d%%  up %4us",
             (unsigned long)(hz1000 / 1000), (unsigned long)(hz1000 % 1000),
             dvi_total_lines(), volume, secs);
    char padded[CON_COLS + 1];
    snprintf(padded, sizeof padded, "%-40.40s", s);
    console_at(0, 1, C_CYAN | C_BRIGHT, C_BLACK, padded);
}

// ----------------------------------------------------------------------------

static void test_clocks(void) {
    uint32_t sys = clock_get_hz(clk_sys), hstx = clock_get_hz(clk_hstx), peri = clock_get_hz(clk_peri);
    say("clocks: sys %lu, hstx %lu, peri %lu MHz\n",
        (unsigned long)(sys / 1000000), (unsigned long)(hstx / 1000000), (unsigned long)(peri / 1000000));
    if (sys == SYS_KHZ * 1000 && hstx == sys / 2) {
        pass("clocks (pixel clock 25.2 MHz)");
    } else {
        fail("clocks");
    }
}

static uint32_t xorshift(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static void test_psram(void) {
    size_t size = psram_init(RP2350PC_PSRAM_CS_PIN);
    if (!size) {
        fail("PSRAM: no ID from CS1 (GPIO8)");
        return;
    }
    say("PSRAM: %u MB on CS1\n", (unsigned)(size >> 20));

    volatile uint32_t *p = PSRAM_UNCACHED;
    uint32_t words = size / 4;

    uint64_t t0 = time_us_64();
    uint32_t x = 0x2068;
    for (uint32_t i = 0; i < words; i++) {
        x = xorshift(x);
        p[i] = x;
    }
    uint64_t t1 = time_us_64();
    x = 0x2068;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < words; i++) {
        x = xorshift(x);
        if (p[i] != x) bad++;
    }
    uint64_t t2 = time_us_64();

    say("  write %lu KB/s, read %lu KB/s (uncached)\n",
        (unsigned long)((uint64_t)(size / 1024) * 1000000 / (t1 - t0)),
        (unsigned long)((uint64_t)(size / 1024) * 1000000 / (t2 - t1)));
    if (bad == 0) {
        pass("PSRAM 8 MB pattern");
    } else {
        say("  %lu of %lu words wrong\n", (unsigned long)bad, (unsigned long)words);
        fail("PSRAM pattern");
    }
}

static void test_sd(void) {
    say("SD: card detect reads %d\n", sd_detect_level());
    sd_info_t info;
    int r = sd_init(&info);
    if (r != 0) {
        say("  init failed at step %d\n", -r);
        fail("SD init (is a card in?)");
        return;
    }
    say("  %s, %lu MB, mfr %02x \"%s\", SPI %lu kHz\n", info.sdhc ? "SDHC/XC" : "SDSC",
        (unsigned long)(info.sectors / 2048), info.manufacturer, info.product,
        (unsigned long)(info.spi_hz / 1000));

    int n = sd_list_root(12);
    if (n < 0) {
        say("  FAT error %d\n", n);
        fail("SD FAT root");
        return;
    }

    static uint8_t buf[512];
    uint64_t t0 = time_us_64();
    int ok = 0;
    for (int i = 0; i < 256; i++) ok += sd_read((uint32_t)i, buf);
    uint64_t dt = time_us_64() - t0;
    say("  read %lu KB/s (single blocks)\n", (unsigned long)(128ull * 1000000 / dt));
    if (ok == 256) {
        pass("SD card");
    } else {
        fail("SD reads");
    }
}

// ----------------------------------------------------------------------------

int main(void) {
    // Flash to divider 3 first (84 MHz at 252), then voltage, then clock.
    set_flash_clkdiv(3);
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(2);
    set_sys_clock_khz(SYS_KHZ, true);
    dvi_clock_init();

    stdio_init_all();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    draw_header();
    console_init(7);
    multicore_launch_core1(core1_main);

    say("\nTS2068-2350 Phase 0 bring-up\n");
    test_clocks();

    sleep_ms(500);
    uint32_t f0 = dvi_frames;
    sleep_ms(1000);
    if (dvi_frames - f0 > 50) {
        pass("video (HSTX on core 1)");
    } else {
        fail("video: no frames counted");
    }

    test_psram();

    audio_init();
    int volume = 40;
    audio_tone(440, 150);
    sleep_ms(200);
    audio_tone(880, 150);
    say("audio: two tones on PWM 26/27\n");

    test_sd();

    usbkbd_init();
    say("USB: host started. Plug a keyboard into a\n");
    say("  USB-A port (not the USB-C PGM port).\n");
    say_c(C_WHITE | C_BRIGHT, "> ");

    // Frame rate from the interrupt's timestamps: frames between two frame
    // starts, over the time between them, measured every 5 s.
    uint32_t ref_frames = dvi_frames, ref_time = dvi_frame_time_us;
    uint32_t hz1000 = 0;
    uint secs = 0;
    uint64_t start = time_us_64();
    uint64_t last = start;

    while (true) {
        usbkbd_task();

        int c = usbkbd_getc();
        if (c == KEY_F1) {
            dvi_set_total_lines(dvi_total_lines() == 525 ? 524 : 525);
            say("\nvideo: %u lines\n> ", dvi_total_lines());
            hz1000 = 0;
            last = 0;  // measure again from the next pass
        } else if (c == KEY_F2) {
            audio_tone(440, 500);
        } else if (c == KEY_F3) {
            say("\n");
            test_sd();
            say("> ");
        } else if (c == '+' || c == '=') {
            volume = volume >= 100 ? 100 : volume + 10;
            audio_set_volume(volume);
            audio_tone(660, 80);
        } else if (c == '-') {
            volume = volume <= 0 ? 0 : volume - 10;
            audio_set_volume(volume);
            audio_tone(660, 80);
        } else if (c == '\r' || c == '\n') {
            say("\n> ");
        } else if (c == '\b') {
            console_putc('\b');
        } else if (c >= 32 && c < 127) {
            char s[2] = {(char)c, 0};
            say("%s", s);
            audio_tone(1200, 15);
        }

        uint64_t now = time_us_64();
        if (last == 0) {
            // Restart: take a reference frame.
            ref_frames = dvi_frames;
            ref_time = dvi_frame_time_us;
            last = now;
        } else if (now - last >= 5000000) {
            uint32_t f, t;
            do {  // a consistent pair, in case a frame starts in between
                f = dvi_frames;
                t = dvi_frame_time_us;
            } while (f != dvi_frames);
            if (t != ref_time) {
                hz1000 = (uint32_t)((uint64_t)(f - ref_frames) * 1000000000ull / (t - ref_time));
            }
            last = now;
        }
        uint s = (uint)((now - start) / 1000000);
        if (s != secs) {
            secs = s;
            gpio_put(PICO_DEFAULT_LED_PIN, s & 1);
            status_line(hz1000, secs, volume);
        }
    }
}
