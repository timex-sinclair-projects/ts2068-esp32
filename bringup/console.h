#pragma once

#include <stdint.h>

// TS2068 colours (0-7, +8 for BRIGHT) as RGB332.
enum {
    C_BLACK, C_BLUE, C_RED, C_MAGENTA, C_GREEN, C_CYAN, C_YELLOW, C_WHITE,
    C_BRIGHT = 8,
};
uint8_t ts_colour(int c);

// A 40x30 text screen on the DVI frame buffer, 8x8 characters in the
// TS2068's font. Rows from console_scroll_top down scroll; rows above it are
// fixed (title, colour bars).
#define CON_COLS 40
#define CON_ROWS 30

void console_init(int scroll_top);
void console_clear(void);
void console_set_colour(int ink, int paper);
void console_putc(char c);
void console_puts(const char *s);

// Draws text at a fixed position without moving the cursor.
void console_at(int col, int row, int ink, int paper, const char *s);

// Prints to the screen and to stdio (UART0).
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// The same, in a colour, for PASS/FAIL lines.
void say_c(int ink, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
