#include "console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dvi.h"
#include "font8x8.h"

static int cur_col, cur_row, top_row;
static int ink = C_WHITE, paper = C_BLACK;

uint8_t ts_colour(int c) {
    // TS2068 levels: normal about 0xD7, BRIGHT 0xFF.
    uint8_t v = (c & C_BRIGHT) ? 0xff : 0xd7;
    uint8_t r = (c & 2) ? v : 0, g = (c & 4) ? v : 0, b = (c & 1) ? v : 0;
    return rgb332(r, g, b);
}

static void draw_char(int col, int row, char ch, int fg, int bg) {
    if (col < 0 || col >= CON_COLS || row < 0 || row >= CON_ROWS) return;
    if (ch < 32 || ch > 127) ch = '?';
    const uint8_t *glyph = font8x8[ch - 32];
    uint8_t f = ts_colour(fg), b = ts_colour(bg);
    for (int y = 0; y < 8; y++) {
        uint8_t *p = &dvi_fb[row * 8 + y][col * 8];
        uint8_t bits = glyph[y];
        for (int x = 0; x < 8; x++) {
            p[x] = (bits & (0x80 >> x)) ? f : b;
        }
    }
}

void console_init(int scroll_top) {
    top_row = scroll_top;
    cur_col = 0;
    cur_row = scroll_top;
}

void console_clear(void) {
    memset(&dvi_fb[top_row * 8][0], ts_colour(paper), (CON_ROWS - top_row) * 8 * DVI_FB_W);
    cur_col = 0;
    cur_row = top_row;
}

void console_set_colour(int i, int p) {
    ink = i;
    paper = p;
}

static void scroll(void) {
    memmove(&dvi_fb[top_row * 8][0], &dvi_fb[(top_row + 1) * 8][0],
            (CON_ROWS - top_row - 1) * 8 * DVI_FB_W);
    memset(&dvi_fb[(CON_ROWS - 1) * 8][0], ts_colour(paper), 8 * DVI_FB_W);
}

void console_putc(char c) {
    if (c == '\r') {
        cur_col = 0;
        return;
    }
    if (c == '\n' || cur_col >= CON_COLS) {
        cur_col = 0;
        if (++cur_row >= CON_ROWS) {
            scroll();
            cur_row = CON_ROWS - 1;
        }
        if (c == '\n') return;
    }
    if (c == '\b') {
        if (cur_col > 0) draw_char(--cur_col, cur_row, ' ', ink, paper);
        return;
    }
    draw_char(cur_col++, cur_row, c, ink, paper);
}

void console_puts(const char *s) {
    while (*s) console_putc(*s++);
}

void console_at(int col, int row, int fg, int bg, const char *s) {
    while (*s) draw_char(col++, row, *s++, fg, bg);
}

static void vsay(const char *fmt, va_list ap) {
    char buf[160];
    vsnprintf(buf, sizeof buf, fmt, ap);
    console_puts(buf);
    fputs(buf, stdout);
}

void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsay(fmt, ap);
    va_end(ap);
}

void say_c(int c, const char *fmt, ...) {
    int old = ink;
    ink = c;
    va_list ap;
    va_start(ap, fmt);
    vsay(fmt, ap);
    va_end(ap);
    ink = old;
}
