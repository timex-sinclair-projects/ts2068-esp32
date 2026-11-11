// DiskEngine's file channels: OPEN #n,"f:name"[,"mode"[,reclen]] and what
// goes through them (PRINT #, INPUT #, LIST #, INKEY$ #, CAT #, RESTORE #,
// CLOSE #). See include/DiskEngine.h, and DISK_PROPOSAL.md in the
// ts2068-2350 repository; the rules are TS-Pico's (its spec, section 4).
//
// A channel is a stream (TAB n is byte n) or, given a record length, a
// record file (TAB n is record n, 1-based). Text mode, the default, writes
// CR as LF, expands keywords and drops print control codes; "b" passes
// bytes through, except 23, which is always TAB.

#include "DiskEngine.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include <sys/stat.h>

namespace {

// The 2068's keywords, A5h-FFh (as the Spectrum's), then its own.
const char* const KEYWORDS[] = {
    "RND", "INKEY$", "PI", "FN", "POINT", "SCREEN$", "ATTR", "AT", "TAB", "VAL$", "CODE",
    "VAL", "LEN", "SIN", "COS", "TAN", "ASN", "ACS", "ATN", "LN", "EXP", "INT", "SQR", "SGN",
    "ABS", "PEEK", "IN", "USR", "STR$", "CHR$", "NOT", "BIN", "OR", "AND", "<=", ">=", "<>",
    "LINE", "THEN", "TO", "STEP", "DEF FN", "CAT", "FORMAT", "MOVE", "ERASE", "OPEN #",
    "CLOSE #", "MERGE", "VERIFY", "BEEP", "CIRCLE", "INK", "PAPER", "FLASH", "BRIGHT",
    "INVERSE", "OVER", "OUT", "LPRINT", "LLIST", "STOP", "READ", "DATA", "RESTORE", "NEW",
    "BORDER", "CONTINUE", "DIM", "REM", "FOR", "GO TO", "GO SUB", "INPUT", "LOAD", "LIST",
    "LET", "PAUSE", "NEXT", "POKE", "PRINT", "PLOT", "RUN", "SAVE", "RANDOMIZE", "IF", "CLS",
    "DRAW", "CLEAR", "RETURN", "COPY"};

// 0Ch and 7Bh-7Fh are keywords on the 2068.
const char* extraKeyword(uint8_t code, bool& function) {
    function = false;
    switch (code) {
        case 0x0C: return "DELETE";
        case 0x7B: return "ON ERR";
        case 0x7C: function = true; return "STICK";
        case 0x7D: return "SOUND";
        case 0x7E: function = true; return "FREE";
        case 0x7F: return "RESET";
        default: return nullptr;
    }
}

bool sameNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (tolower((unsigned char) a[i]) != tolower((unsigned char) b[i])) return false;
    return true;
}

// The block graphics, 80h-8Fh: bit 0 the top right, 1 top left, 2 bottom
// right, 3 bottom left.
const char* const BLOCKS[16] = {
    " ", "\u259D", "\u2598", "\u2580", "\u2597", "\u2590", "\u259A", "\u259C",
    "\u2596", "\u259E", "\u258C", "\u259B", "\u2584", "\u259F", "\u2599", "\u2588"};

const char* const PRINT_FOLDER = "/print";
const char* const PRINT_TEXT = "/print/printer.txt";

const size_t MAX_NAME = 64;
const int MAX_RECLEN = 254;    // a record and its CR fit in 255

} // namespace

// Put the FILE at p for a read or a write (stdio wants a seek between the two).
bool DiskEngine::chAt(Channel& c, long p, bool write) {
    if (c.filePos == p && c.lastWrite == write) return true;
    if (fseek(c.f, p, SEEK_SET) != 0) return false;
    c.filePos = p;
    c.lastWrite = write;
    return true;
}

long DiskEngine::chSize(Channel& c) {
    if (c.size >= 0) return c.size;
    if (fseek(c.f, 0, SEEK_END) != 0) return 0;
    long n = ftell(c.f);
    c.filePos = n;
    c.size = n < 0 ? 0 : n;
    return c.size;
}

// Bytes into the file at `at`; what lies between its end and `at` is padding.
uint8_t DiskEngine::chWriteAt(Channel& c, long at, const char* bytes, size_t n) {
    long size = chSize(c);
    if (at > size) {
        std::string pad(at - size, c.binary ? '\0' : ' ');
        if (!chAt(c, size, true) || fwrite(pad.data(), 1, pad.size(), c.f) != pad.size()) return R_IO;
        c.filePos = at;
        c.size = at;
    }
    if (!chAt(c, at, true) || fwrite(bytes, 1, n, c.f) != n) return R_IO;
    c.filePos += n;
    if (c.filePos > c.size) c.size = c.filePos;
    return OK;
}

// Bytes for the file: into the record being made (a record file), at pos
// (a stream), at the end ("a"). "r" drops them (INPUT #'s prompt).
uint8_t DiskEngine::chWrite(Channel& c, const char* bytes, size_t n) {
    if (c.mode == 'r') return OK;
    if (c.query) return R_PARAM;                        // PRINT #s;TAB 0
    if (c.mode == 'a') return fwrite(bytes, 1, n, c.f) == n ? OK : R_IO;   // "ab": at the end
    if (c.reclen) {
        if (c.recBuf.size() + n > (size_t) c.reclen) {  // too long: Q, and the record is dropped
            c.recBuf.clear();
            c.recOpen = false;
            return R_PARAM;
        }
        c.recBuf.append(bytes, n);
        c.recOpen = true;
        return OK;
    }
    uint8_t r = chWriteAt(c, c.pos, bytes, n);
    if (r == OK) c.pos += n;
    return r;
}

// The record being made goes in the file, padded to its length, and the
// next one is next.
uint8_t DiskEngine::chEndRecord(Channel& c) {
    if (!c.reclen || !c.recOpen) return OK;
    c.recOpen = false;
    std::string rec = c.recBuf;
    rec.resize(c.reclen, c.binary ? '\0' : ' ');
    c.recBuf.clear();
    uint8_t r = chWriteAt(c, (c.record - 1) * c.reclen, rec.data(), rec.size());
    c.record++;
    return r;
}

// TAB n: record n (byte n in a stream). TAB 0 asks for the count.
uint8_t DiskEngine::chSeek(Channel& c, long n) {
    if (c.mode == 'a') return R_PARAM;
    uint8_t r = chEndRecord(c);
    c.pending.clear();
    c.pendingPos = 0;
    c.lastInCR = true;
    c.query = n == 0;
    if (n) {
        if (c.reclen) c.record = n;
        else c.pos = n - 1;
    }
    return r;
}

// One byte, text mode's way: a keyword expanded, the column kept for the comma.
uint8_t DiskEngine::chPut(Channel& c, uint8_t b) {
    char ch = (char) b;
    uint8_t r = chWrite(c, &ch, 1);
    c.column++;
    c.suppress = b == ' ';
    return r;
}

uint8_t DiskEngine::chToken(Channel& c, uint8_t code) {
    bool function;
    const char* word = extraKeyword(code, function);
    if (!word) {
        word = KEYWORDS[code - 0xA5];
        function = code < 0xC5;                         // RND .. BIN: no space before
    }
    uint8_t r = OK;
    if (!function && !c.suppress) r = chPut(c, ' ');
    for (const char* p = word; *p && r == OK; p++) r = chPut(c, *p);
    char last = word[strlen(word) - 1];
    bool noTrail = code >= 0xA5 && code <= 0xA7;        // RND, INKEY$, PI
    if (r == OK && !noTrail && (isalpha((unsigned char) last) || last == '$')) r = chPut(c, ' ');
    return r;
}

uint8_t DiskEngine::chOpen(int stream, const std::string& spec, const std::string& modeIn, int reclen) {
    if (stream < 0 || stream >= STREAMS) return R_STREAM;
    chClose(stream);                                    // a stream left open by NEW
    if (spec.size() < 2 || spec[1] != ':') return R_IO;
    if (tolower((unsigned char) spec[0]) != 'f') return R_IO;   // other devices: later
    std::string name = spec.substr(2), path;
    if (name.empty() || name.size() > MAX_NAME || hasWild(name) || !resolve(name, path)) return R_NAME;

    char mode = 'r';
    bool binary = false, gotMode = false;
    if (modeIn.size() > 2) return R_PARAM;
    for (char m : modeIn) {
        m = (char) tolower((unsigned char) m);
        if (m == 'b' && !binary) binary = true;
        else if (strchr("rwau", m) && !gotMode) {
            mode = m;
            gotMode = true;
        } else
            return R_PARAM;
    }
    if (!modeIn.empty() && !gotMode) return R_PARAM;    // "b" alone
    if (reclen < 0 || reclen > MAX_RECLEN || (reclen && mode == 'a')) return R_PARAM;

    for (int s = 0; s < STREAMS; s++)
        if (chans_[s].f && sameNoCase(chans_[s].path, path)) return R_NAME;   // open on another stream
    std::string host = hostPath(path);
    struct stat st;
    bool exists = stat(host.c_str(), &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) return R_PARAM;
    if (!exists && mode == 'r') return R_NAME;
    const char* how = mode == 'r' ? "rb" : mode == 'w' ? "wb" : mode == 'a' ? "ab" : exists ? "r+b" : "w+b";
    FILE* f = fopen(host.c_str(), how);
    if (!f) return R_IO;

    Channel& c = chans_[stream];
    c = Channel();
    c.f = f;
    c.path = path;
    c.mode = mode;
    c.binary = binary;
    c.reclen = reclen;
    return OK;
}

uint8_t DiskEngine::chOut(int stream, uint8_t b) {
    if (!chIsOpen(stream)) return R_STREAM;
    Channel& c = chans_[stream];
    if (c.ctl) {                                        // a control code's parameters
        if (--c.ctl) {
            c.ctlLo = b;
            return OK;
        }
        if (c.ctlCode == 0x17) return chSeek(c, c.ctlLo | (b << 8));
        return OK;
    }
    if (b == 0x17) {                                    // TAB n: always a seek
        c.ctlCode = b;
        c.ctl = 2;
        return OK;
    }
    if (b == 0x0D) {
        if (c.query) return R_PARAM;
        c.column = 0;
        c.suppress = true;
        if (c.reclen) {                                 // CR ends the record, not stored
            if (c.mode != 'r') c.recOpen = true;        //   (an empty one is all padding)
            return chEndRecord(c);
        }
        if (c.binary) {
            char cr = 0x0D;
            return chWrite(c, &cr, 1);
        }
        return chWrite(c, "\n", 1);
    }
    if (c.binary) {
        char ch = (char) b;
        return chWrite(c, &ch, 1);
    }
    if (b == 0x06) {                                    // the comma: to the next 16-column stop
        uint8_t r = OK;
        do r = chPut(c, ' ');
        while (r == OK && c.column % 16);
        return r;
    }
    if (b >= 0x10 && b <= 0x16) {                       // INK .. OVER: one byte; AT: two
        c.ctlCode = b;
        c.ctl = b == 0x16 ? 2 : 1;
        return OK;
    }
    bool function;
    if (b >= 0xA5 || extraKeyword(b, function)) return chToken(c, b);
    if (b < 0x20) return OK;                            // other control codes
    return chPut(c, b);
}

uint8_t DiskEngine::chIn(int stream, uint8_t& out) {
    if (!chIsOpen(stream)) return R_STREAM;
    Channel& c = chans_[stream];
    if (c.mode == 'w' || c.mode == 'a') return R_PARAM;
    if (c.ctl) c.ctl = 0;
    uint8_t r = chEndRecord(c);                         // what was printed goes first
    if (r != OK) return r;
    if (c.query) {                                      // TAB 0: the count, as text
        c.query = false;
        long size = chSize(c);
        long count = c.reclen ? (size + c.reclen - 1) / c.reclen : size;
        c.pending = std::to_string(count) + "\r";
        c.pendingPos = 0;
    }
    if (c.pendingPos < c.pending.size()) {
        out = (uint8_t) c.pending[c.pendingPos++];
        if (c.pendingPos == c.pending.size()) {
            c.pending.clear();
            c.pendingPos = 0;
        }
        return OK;
    }
    if (c.reclen) {                                     // a record file: the record, then CR
        long start = (c.record - 1) * c.reclen, size = chSize(c);
        if (start >= size) return CH_EOF;
        std::string rec(c.reclen, c.binary ? '\0' : ' ');
        long n = size - start < c.reclen ? size - start : c.reclen;
        if (!chAt(c, start, false) || fread(&rec[0], 1, n, c.f) != (size_t) n) return R_IO;
        c.filePos += n;
        c.record++;
        c.pending = rec + "\r";
        c.pendingPos = 1;
        out = (uint8_t) c.pending[0];
        return OK;
    }
    if (!chAt(c, c.pos, false)) return R_IO;
    int b = fgetc(c.f);
    if (b == EOF) {
        c.filePos = -1;
        if (!c.binary && !c.lastInCR) {                 // a last line with no line end
            c.lastInCR = true;
            out = 0x0D;
            return OK;
        }
        return CH_EOF;
    }
    c.pos++;
    c.filePos++;
    if (c.binary) {
        out = (uint8_t) b;
        return OK;
    }
    if (b == '\r') {                                    // CR, LF and CRLF are all a line end
        int next = fgetc(c.f);
        if (next == '\n') {
            c.pos++;
            c.filePos++;
        } else
            c.filePos = -1;
        b = 0x0D;
    } else if (b == '\n')
        b = 0x0D;
    c.lastInCR = b == 0x0D;
    out = (uint8_t) b;
    return OK;
}

uint8_t DiskEngine::chClose(int stream) {
    if (!chIsOpen(stream)) return OK;
    Channel& c = chans_[stream];
    uint8_t r = chEndRecord(c);
    if (fclose(c.f) != 0 && r == OK) r = R_IO;
    c = Channel();
    return r;
}

void DiskEngine::chCloseAll() {
    for (int s = 0; s < STREAMS; s++) chClose(s);
}

uint8_t DiskEngine::chRestore(int stream) {
    if (!chIsOpen(stream)) return R_STREAM;
    Channel& c = chans_[stream];
    if (c.mode == 'a') return chEndRecord(c);
    uint8_t r = chSeek(c, 1);
    c.ctl = 0;
    return r;
}

uint8_t DiskEngine::chCat(int stream, bool arg, const std::string& name) {
    if (!chIsOpen(stream)) return R_STREAM;
    if (chans_[stream].mode == 'r') return R_PARAM;
    std::string text;
    uint8_t r = catInto(arg, name, text);
    for (size_t i = 0; i < text.size() && r == OK; i++) r = chOut(stream, (uint8_t) text[i]);
    return r;
}

// The stream of the channel CURCHL points at: its record's last byte.
int DiskEngine::chStreamOf(DiskCpu& cpu) {
    uint16_t rec = cpu.peek(CURCHL) | (cpu.peek(CURCHL + 1) << 8);
    if (cpu.peek(rec + 4) != 'F') return -1;
    int stream = cpu.peek(rec + 5);
    return chIsOpen(stream) ? stream : -1;
}

// NEW (and anything else that rebuilds CHANS) drops the records without a
// CLOSE #. A channel whose stream no longer points at its record is closed.
void DiskEngine::chSweep(DiskCpu& cpu, int except) {
    uint16_t chans = cpu.peek(CHANS) | (cpu.peek(CHANS + 1) << 8);
    for (int s = 0; s < STREAMS; s++) {
        if (s == except || !chans_[s].f) continue;
        uint16_t entry = STRMS + 6 + 2 * s;
        uint16_t off = cpu.peek(entry) | (cpu.peek(entry + 1) << 8);
        bool live = off && off < 0x8000;
        if (live) {
            uint16_t rec = chans + off - 1;
            live = cpu.peek(rec + 4) == 'F' && cpu.peek(rec + 5) == s;
        }
        if (!live) chClose(s);
    }
}

//------------------------------------------------------------------------------
// The printer: LPRINT and LLIST as text in /print/printer.txt (appended to),
// COPY as copyNNN.bmp. The text is laid out the way the 2040 lays it out:
// 32 columns, a line full at the 32nd, the comma at 0 or 16, TAB and AT's
// column across the paper.
//------------------------------------------------------------------------------

bool DiskEngine::prFolder() {
    std::string host = hostPath(PRINT_FOLDER);
    struct stat st;
    if (stat(host.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(host.c_str(), 0755) == 0;
}

uint8_t DiskEngine::prFlush() {
    if (prText_.empty()) return OK;
    std::string text;
    text.swap(prText_);
    if (!prFolder()) return R_IO;
    FILE* f = fopen(hostPath(PRINT_TEXT).c_str(), "ab");
    if (!f) return R_IO;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = (fclose(f) == 0) && ok;
    return ok ? OK : R_IO;
}

// One character on the paper (some take more than a byte in UTF-8): a full
// line feeds first.
uint8_t DiskEngine::prPut(const char* utf8) {
    if (prColumn_ >= PR_COLUMNS) {
        prText_ += '\n';
        prColumn_ = 0;
    }
    prText_ += utf8;
    prColumn_++;
    prSuppress_ = utf8[0] == ' ' && !utf8[1];
    return OK;
}

// To a column: spaces, through a new line if it is behind us.
uint8_t DiskEngine::prTo(int column) {
    column %= PR_COLUMNS;
    if (column < prColumn_ && prColumn_ < PR_COLUMNS) {
        prText_ += '\n';
        prColumn_ = 0;
    } else if (prColumn_ >= PR_COLUMNS && column > 0) {
        prText_ += '\n';
        prColumn_ = 0;
    }
    while (prColumn_ < column) prPut(" ");
    return OK;
}

void DiskEngine::prTick() {
    if (!prText_.empty() && ++prQuiet_ >= 30) prFlush();
}

uint8_t DiskEngine::prOut(uint8_t b) {
    prQuiet_ = 0;
    if (prCtl_) {                                       // a control code's parameters
        if (--prCtl_) {
            prCtlLo_ = b;
            return OK;
        }
        if (prCtlCode_ == 0x16) return prTo(b);         // AT line,column: the column
        if (prCtlCode_ == 0x17) return prTo(prCtlLo_ | (b << 8));
        return OK;
    }
    uint8_t r = OK;
    if (b == 0x0D) {
        prText_ += '\n';
        prColumn_ = 0;
        prSuppress_ = true;
        if (prText_.size() >= 512) r = prFlush();
        return r;
    }
    if (b == 0x06) return prTo(prColumn_ < 16 && prColumn_ < PR_COLUMNS ? 16 : 0);   // the comma
    if (b >= 0x10 && b <= 0x17) {                       // INK .. OVER: one byte; AT, TAB: two
        prCtlCode_ = b;
        prCtl_ = b >= 0x16 ? 2 : 1;
        return OK;
    }
    bool function;
    const char* word = extraKeyword(b, function);
    if (b >= 0xA5 || word) {                            // as chToken: the screen's spacing
        if (!word) {
            word = KEYWORDS[b - 0xA5];
            function = b < 0xC5;
        }
        if (!function && !prSuppress_) prPut(" ");
        char one[2] = {0, 0};
        for (const char* p = word; *p; p++) {
            one[0] = *p;
            prPut(one);
        }
        char last = word[strlen(word) - 1];
        bool noTrail = b >= 0xA5 && b <= 0xA7;
        if (!noTrail && (isalpha((unsigned char) last) || last == '$')) prPut(" ");
        return OK;
    }
    if (b < 0x20) return OK;                            // other control codes
    if (b == 0x60) return prPut("£");              // the pound sign
    if (b >= 0x80 && b <= 0x8F) return prPut(BLOCKS[b & 0x0F]);
    if (b >= 0x90) return prPut("?");                   // a UDG: no telling what it is
    char one[2] = {(char) b, 0};
    return prPut(one);
}

namespace {

void put16(std::string& s, uint16_t v) { s += (char) (v & 0xFF); s += (char) (v >> 8); }
void put32(std::string& s, uint32_t v) { put16(s, v & 0xFFFF); put16(s, v >> 16); }

} // namespace

// COPY: the top 22 lines of the screen being shown, as the printer prints
// them (a dot for each ink pixel), in a 1-bit BMP. Hi-res is 512 dots wide.
uint8_t DiskEngine::prCopy(DiskCpu& cpu, std::string* written) {
    uint8_t video = cpu.video() & 0x07;
    bool hires = video & 0x04;
    uint16_t base = (!hires && (video & 0x03) == 0x01) ? 0x6000 : 0x4000;
    const int rows = 176, bytes = hires ? 64 : 32, width = bytes * 8;
    std::string bmp;
    uint32_t offset = 14 + 40 + 8, size = offset + rows * bytes;
    bmp += "BM";
    put32(bmp, size);
    put32(bmp, 0);
    put32(bmp, offset);
    put32(bmp, 40);
    put32(bmp, width);
    put32(bmp, rows);
    put16(bmp, 1);
    put16(bmp, 1);                                      // a bit a pixel
    put32(bmp, 0);
    put32(bmp, rows * bytes);
    put32(bmp, 2835);                                   // 72 dpi
    put32(bmp, 2835);
    put32(bmp, 2);
    put32(bmp, 2);
    put32(bmp, 0x00FFFFFF);                             // 0: paper
    put32(bmp, 0x00000000);                             // 1: a dot
    for (int y = rows - 1; y >= 0; y--) {               // bottom up
        uint16_t line = (uint16_t) (((y & 0xC0) << 5) | ((y & 0x07) << 8) | ((y & 0x38) << 2));
        for (int x = 0; x < bytes; x++) {
            if (hires) bmp += (char) cpu.peek((x & 1 ? 0x6000 : 0x4000) + line + x / 2);
            else bmp += (char) cpu.peek(base + line + x);
        }
    }
    if (!prFolder()) return R_IO;
    char name[32];
    for (int n = 1; n <= 999; n++) {
        snprintf(name, sizeof name, "%s/copy%03d.bmp", PRINT_FOLDER, n);
        std::string host = hostPath(name);
        struct stat st;
        if (stat(host.c_str(), &st) == 0) continue;
        FILE* f = fopen(host.c_str(), "wb");
        if (!f) return R_IO;
        bool ok = fwrite(bmp.data(), 1, bmp.size(), f) == bmp.size();
        ok = (fclose(f) == 0) && ok;
        if (ok && written) *written = name;
        return ok ? OK : R_IO;
    }
    return R_IO;                                        // copy999.bmp is there: full
}
