/*

ESP2068 — TS2068 port of ESPectrum

test/host/diskengine_test.cpp — host test for the native disk, Phases 1
to 5: DiskEngine's CAT listings, TAP mounting and tape services, native
files, file management and file channels (with a fake CPU and a scratch
folder), DosDisk's HOME and EXROM patches against both stock ROM pairs,
and SCLD's DOS ROM overlay.

Build and run (from repo root, after tools/build_dos.py):

    g++ -std=c++17 -Wall -I include -DESP2068_HOST_TEST \
        test/host/diskengine_test.cpp src/DiskEngine.cpp src/DiskChannels.cpp src/DosDisk.cpp \
        src/SCLD.cpp \
        -o /tmp/diskengine_test \
    && /tmp/diskengine_test

*/

#include "DiskEngine.h"
#include "DosDisk.h"
#include "DosRom.h"
#include "SCLD.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

static int failures = 0;

static void check(bool cond, const char* what) {
    printf("  %s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

static std::vector<uint8_t> readFile(const char* path) {
    std::vector<uint8_t> v;
    FILE* f = fopen(path, "rb");
    if (!f) return v;
    int c;
    while ((c = fgetc(f)) != EOF) v.push_back((uint8_t) c);
    fclose(f);
    return v;
}

static void writeFile(const std::string& path, size_t size) {
    FILE* f = fopen(path.c_str(), "wb");
    for (size_t i = 0; i < size; i++) fputc(0, f);
    fclose(f);
}

// A CPU with registers to set and 64K of flat memory.
class FakeCpu : public DiskCpu {
public:
    uint16_t BC = 0, DE = 0, HL = 0, IX = 0, SP = 0xFF00;
    uint8_t mem[65536] = {};
    uint16_t bc() override { return BC; }
    uint16_t de() override { return DE; }
    uint16_t hl() override { return HL; }
    uint16_t ix() override { return IX; }
    uint16_t sp() override { return SP; }
    // A string at F000h for DE/BC.
    void str(const std::string& t) {
        DE = 0xF000;
        BC = (uint16_t) t.size();
        for (size_t i = 0; i < t.size(); i++) mem[0xF000 + i] = (uint8_t) t[i];
    }
    // R_TAPE's/W_TAPE's pushed AF.
    void pushedAF(uint8_t a, bool carry) { mem[SP] = carry ? 1 : 0; mem[SP + 1] = a; }
    uint8_t peek(uint16_t a) override { return mem[a]; }
    void poke(uint16_t a, uint8_t v) override { mem[a] = v; }
};

static std::string drain(DiskEngine& eng, DiskCpu& cpu) {
    std::string got;
    for (int i = 0; i < 100000; i++) {
        uint8_t c = eng.service(DiskEngine::SVC_CAT_CHAR, cpu);
        if (c == 0) break;
        got += (char) c;
    }
    return got;
}

// A TAP block: length, flag, data, checksum.
static std::string tapBlock(uint8_t flag, const std::string& data) {
    std::string b;
    uint16_t len = (uint16_t) data.size() + 2;
    b += (char) (len & 0xFF);
    b += (char) (len >> 8);
    b += (char) flag;
    uint8_t x = flag;
    for (char c : data) { b += c; x ^= (uint8_t) c; }
    b += (char) x;
    return b;
}

static std::string tapHeader(uint8_t type, const std::string& name, uint16_t len, uint16_t p1, uint16_t p2) {
    std::string h;
    h += (char) type;
    std::string n = name;
    n.resize(10, ' ');
    h += n;
    for (uint16_t v : {len, p1, p2}) { h += (char) (v & 0xFF); h += (char) (v >> 8); }
    return tapBlock(0x00, h);
}

static void writeBytes(const std::string& path, const std::string& bytes) {
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
}

// A file's row in a folder listing, after the index and mark.
static std::string row(const char* name, const char* type, const char* size) {
    char b[64];
    snprintf(b, sizeof b, "%-14s %-5s %6s", name, type, size);
    return b;
}

static std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\r') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur + "<no CR>");
    return out;
}

// Everything a channel takes: each byte through chOut.
static uint8_t chPrint(DiskEngine& eng, int s, const std::string& bytes) {
    for (unsigned char c : bytes) {
        uint8_t r = eng.chOut(s, c);
        if (r != DiskEngine::OK) return r;
    }
    return DiskEngine::OK;
}

// INPUT #s's view: bytes up to a CR (not kept), or the answer that stopped it.
static std::string chLine(DiskEngine& eng, int s, uint8_t* stop = nullptr) {
    std::string out;
    for (int i = 0; i < 100000; i++) {
        uint8_t c = 0, r = eng.chIn(s, c);
        if (r != DiskEngine::OK) { if (stop) *stop = r; return out; }
        if (c == 0x0D) { if (stop) *stop = DiskEngine::OK; return out; }
        out += (char) c;
    }
    return out;
}

static std::string fileText(const std::string& path) {
    std::vector<uint8_t> v = readFile(path.c_str());
    return std::string(v.begin(), v.end());
}

static std::string tab(uint16_t n) {
    std::string t = "\x17";
    t += (char) (n & 0xFF);
    t += (char) (n >> 8);
    return t;
}

int main() {
    printf("DiskEngine host test\n");
    printf("====================\n");

    // ---- A scratch folder for the 2068's "/" ----
    std::string root = "/tmp/diskengine_test_root";
    system(("rm -rf " + root).c_str());
    mkdir(root.c_str(), 0755);
    mkdir((root + "/games").c_str(), 0755);
    mkdir((root + "/Basic").c_str(), 0755);
    writeFile(root + "/zeta.tap", 1234);
    writeFile(root + "/alpha.bas", 99999);
    writeFile(root + "/Big.bin", 100000);
    writeFile(root + "/a-very-long-file-name-indeed.tap", 10);
    writeFile(root + "/.hidden", 5);

    printf("\nCAT listing\n");
    DiskEngine eng(root);
    std::string text;
    check(eng.buildListing("/", text) == DiskEngine::OK, "the root lists");
    std::vector<std::string> L = lines(text);
    for (auto& l : L) printf("        |%s|\n", l.c_str());
    check(L.size() == 8, "a header, 2 folders, 4 files and a summary");
    check(L.size() > 0 && L[0] == "/", "the header is the folder");
    check(L.size() > 2 && L[1] == "    <Basic>" && L[2] == "    <games>", "folders first, sorted ignoring case");
    check(L.size() > 3 && L[3].find("a-very-long-f> TAP") != std::string::npos, "a long name is cut with '>'");
    check(L.size() > 4 && L[4] == "  2 alpha.bas             99999", "a file line: index, name, size in bytes");
    check(L.size() > 5 && L[5] == "  3 Big.bin                 98K", "100000 bytes and up in K");
    check(L.size() > 6 && L[6].find("zeta.tap") != std::string::npos, "files sorted ignoring case");
    check(L.size() > 7 && L[7] == "2 folders, 4 files", "the summary");
    check(text.find(".hidden") == std::string::npos, "hidden files are left out");
    bool short_ = true;
    for (auto& l : L) if (l.size() > 31) short_ = false;
    check(short_, "every line fits in 31 columns (no wrap before the CR)");

    printf("\nService calls\n");
    FakeCpu cpu;
    cpu.BC = 0;
    cpu.HL = 0;
    check(eng.service(DiskEngine::SVC_CAT_OPEN, cpu) == DiskEngine::OK, "CAT_OPEN, the current folder: OK");
    std::string got;
    for (int i = 0; i < 10000; i++) {
        uint8_t c = eng.service(DiskEngine::SVC_CAT_CHAR, cpu);
        if (c == 0) break;
        got += (char) c;
    }
    check(got == text, "CAT_CHAR hands out the listing, then 0");
    check(eng.service(DiskEngine::SVC_CAT_CHAR, cpu) == 0, "and stays at 0");
    cpu.HL = 1;
    cpu.str("games");
    check(eng.service(DiskEngine::SVC_CAT_OPEN, cpu) == DiskEngine::OK, "CAT \"games\": OK");
    check(drain(eng, cpu).rfind("/games/\r", 0) == 0, "and lists /games/");
    cpu.str("nothere");
    check(eng.service(DiskEngine::SVC_CAT_OPEN, cpu) == DiskEngine::R_NAME, "CAT \"nothere\": F");
    cpu.str("..");
    check(eng.service(DiskEngine::SVC_CAT_OPEN, cpu) == DiskEngine::R_NAME, "CAT \"..\" from /: F (nothing above the root)");
    cpu.str("");
    check(eng.service(DiskEngine::SVC_CAT_OPEN, cpu) == DiskEngine::R_IO, "CAT \"\" with nothing mounted: J");
    cpu.HL = 0;
    check(eng.service(0x7F, cpu) == DiskEngine::R_UNKNOWN_FN, "an unknown function: a report");

    // A folder with more than MAX_ENTRIES files.
    mkdir((root + "/many").c_str(), 0755);
    for (int i = 0; i < DiskEngine::MAX_ENTRIES + 5; i++) writeFile(root + "/many/f" + std::to_string(i), 1);
    std::string many;
    check(eng.buildListing("/many/", many) == DiskEngine::OK, "a folder of 261 files lists");
    std::vector<std::string> M = lines(many);
    check(M.size() == (size_t) DiskEngine::MAX_ENTRIES + 3, "header, 256 files, 'and 5 more', summary");
    check(M.size() > 2 && M[M.size() - 2] == "and 5 more", "the rest are counted");

    DiskEngine nowhere("/tmp/diskengine_test_no_such_folder");
    cpu.BC = 0;
    check(nowhere.service(DiskEngine::SVC_CAT_OPEN, cpu) == DiskEngine::R_IO, "no card (no root): J");


    printf("\nTAP mounting\n");
    {
        // loader (Prog), then pic (Code, 4 bytes), then a headerless block.
        std::string prog("\x00\x0a\x02\x00\xf9\x0d", 6);   // 10 RANDOMIZE
        std::string tap = tapHeader(0, "loader", (uint16_t) prog.size(), 10, (uint16_t) prog.size())
                        + tapBlock(0xFF, prog)
                        + tapHeader(3, "pic", 4, 0x9000, 0x8000)
                        + tapBlock(0xFF, "ABCD")
                        + tapBlock(0xFF, "xyz");
        writeBytes(root + "/games/game.tap", tap);
        writeBytes(root + "/games/cut.tap", tap + std::string("\x09\x00\xff\x01", 4));   // last block cut short

        DiskEngine e(root);
        FakeCpu c;
        c.mem[0x5C3B] = 0x80;                 // FLAGS: run time
        c.mem[0x5C74] = 1;                    // T_ADDR: LOAD

        c.str("games/game.tap");
        c.mem[0x5C3B] = 0x00;
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::T_NOT_OURS && e.mounted().empty(),
              "TAPE_NAME on the syntax pass: nothing happens");
        c.mem[0x5C3B] = 0x80;
        c.str("missing.tap");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::R_NAME && e.mounted().empty(),
              "LOAD \"missing.tap\": not a file: F");
        c.pushedAF(0x00, true);
        check(e.service(DiskEngine::SVC_R_TAPE, c) == DiskEngine::T_NOT_OURS, "R_TAPE, nothing mounted: the stock routine");
        check(e.service(DiskEngine::SVC_W_TAPE, c) == DiskEngine::T_NOT_OURS, "W_TAPE, nothing mounted: the stock routine");
        check(e.service(DiskEngine::SVC_TAPE_OURS, c) == 0, "TAPE_OURS: 0");

        c.str("games/game.tap");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::T_DONE, "LOAD \"games/game.tap\": mounted, T_DONE");
        check(e.mounted() == "/games/game.tap" && e.tapBlocks() == 5 && e.tapPointer() == 0, "5 blocks, the pointer at the start");
        check(e.service(DiskEngine::SVC_TAPE_OURS, c) == 1, "TAPE_OURS: 1");

        // LOAD "": the header, then the program.
        c.IX = 0x6000; c.DE = 17; c.pushedAF(0x00, true);
        check(e.service(DiskEngine::SVC_R_TAPE, c) == DiskEngine::T_DONE, "R_TAPE a header: done");
        check(c.mem[0x6000] == 0 && memcmp(c.mem + 0x6001, "loader    ", 10) == 0 && c.mem[0x600D] == 10, "the header is loader's, LINE 10");
        c.IX = 0x7000; c.DE = (uint16_t) prog.size(); c.pushedAF(0xFF, true);
        check(e.service(DiskEngine::SVC_R_TAPE, c) == DiskEngine::T_DONE, "R_TAPE the data: done");
        check(memcmp(c.mem + 0x7000, prog.data(), prog.size()) == 0, "the program is in memory");
        check(e.tapPointer() == 2, "the pointer at block 3");

        // LOAD "" CODE: the next header, then its 4 bytes.
        c.str("");
        e.service(DiskEngine::SVC_TAPE_NAME, c);
        c.IX = 0x6000; c.DE = 17; c.pushedAF(0x00, true);
        check(e.service(DiskEngine::SVC_R_TAPE, c) == DiskEngine::T_DONE && c.mem[0x6000] == 3, "the next header: Code");
        c.IX = 0x9000; c.DE = 4; c.pushedAF(0xFF, true);
        check(e.service(DiskEngine::SVC_R_TAPE, c) == DiskEngine::T_DONE && memcmp(c.mem + 0x9000, "ABCD", 4) == 0, "CODE loaded");
        // VERIFY: the same bytes pass, different ones fail.
        e.mount("/games/game.tap");
        c.IX = 0x6000; c.DE = 17; c.pushedAF(0x00, false);
        check(e.service(DiskEngine::SVC_R_TAPE, c) == DiskEngine::T_FAILED, "VERIFY a header against other bytes: failed");
        c.IX = 0x7000; c.DE = (uint16_t) prog.size(); c.pushedAF(0xFF, false);
        check(e.service(DiskEngine::SVC_R_TAPE, c) == DiskEngine::T_DONE, "VERIFY the program: done");
        // A length that differs: a loading error.
        c.IX = 0x9000; c.DE = 5; c.pushedAF(0xFF, true);
        e.mount("/games/game.tap");
        c.pushedAF(0xFF, true);
        check(e.service(DiskEngine::SVC_R_TAPE, c) == DiskEngine::T_FAILED, "data of another length: failed");

        // A name the TAP hasn't got: headers examined once round, then F.
        e.mount("/games/game.tap");
        c.str("nope");
        e.service(DiskEngine::SVC_TAPE_NAME, c);
        int headers = 0;
        uint8_t r = 0;
        for (int i = 0; i < 20; i++) {
            c.IX = 0x6000; c.DE = 17; c.pushedAF(0x00, true);
            r = e.service(DiskEngine::SVC_R_TAPE, c);
            if (r != DiskEngine::T_DONE) break;
            headers++;
        }
        check(headers == 2 && r == DiskEngine::R_NAME, "both headers, then F");
        // Wrap: with the pointer at block 5, LOAD "" goes round the end to
        // block 1's header.
        e.mount("/games/game.tap");
        c.str("");
        e.service(DiskEngine::SVC_TAPE_NAME, c);
        c.IX = 0x6000; c.DE = 17; c.pushedAF(0x00, true);
        e.service(DiskEngine::SVC_R_TAPE, c);
        c.IX = 0x7000; c.DE = (uint16_t) prog.size(); c.pushedAF(0xFF, true);
        e.service(DiskEngine::SVC_R_TAPE, c);
        c.IX = 0x6000; c.DE = 17; c.pushedAF(0x00, true);
        e.service(DiskEngine::SVC_R_TAPE, c);
        c.IX = 0x9000; c.DE = 4; c.pushedAF(0xFF, true);
        e.service(DiskEngine::SVC_R_TAPE, c);
        check(e.tapPointer() == 4, "the pointer at block 5");
        e.service(DiskEngine::SVC_TAPE_NAME, c);
        c.IX = 0x6000; c.DE = 17; c.pushedAF(0x00, true);
        c.mem[0x6000] = 0xEE;
        check(e.service(DiskEngine::SVC_R_TAPE, c) == DiskEngine::T_DONE && c.mem[0x6000] == 0 && e.tapPointer() == 1,
              "round the end to block 1's header");

        // SAVE appends: a header and the data.
        std::string before = std::string((char*) readFile((root + "/games/game.tap").c_str()).data(), tap.size());
        memcpy(c.mem + 0x6100, "\x03savedpic  \x03\x00\x00\x90\x00\x80", 17);
        c.IX = 0x6100; c.DE = 17; c.pushedAF(0x00, false);
        check(e.service(DiskEngine::SVC_W_TAPE, c) == DiskEngine::T_DONE, "W_TAPE the header: done");
        memcpy(c.mem + 0x9100, "xyz", 3);
        c.IX = 0x9100; c.DE = 3; c.pushedAF(0xFF, false);
        check(e.service(DiskEngine::SVC_W_TAPE, c) == DiskEngine::T_DONE, "W_TAPE the data: done");
        std::vector<uint8_t> after = readFile((root + "/games/game.tap").c_str());
        std::string want = tap + tapBlock(0x00, std::string("\x03savedpic  \x03\x00\x00\x90\x00\x80", 17)) + tapBlock(0xFF, "xyz");
        check(after.size() == want.size() && memcmp(after.data(), want.data(), want.size()) == 0, "the TAP has both blocks appended");
        check(e.tapBlocks() == 7, "7 blocks now");
        DiskEngine fresh(root);
        check(fresh.mount("/games/game.tap") == DiskEngine::OK && fresh.tapBlocks() == 7, "and a fresh mount finds 7");

        // Listings.
        c.HL = 1; c.str("");
        e.mount("/games/game.tap");
        check(e.service(DiskEngine::SVC_CAT_OPEN, c) == DiskEngine::OK, "CAT \"\": the mounted TAP");
        std::vector<std::string> T = lines(drain(e, c));
        for (auto& l : T) printf("        |%s|\n", l.c_str());
        check(T.size() == 9 && T[0] == "/games/game.tap", "header, 7 blocks, summary");
        check(T.size() > 1 && T[1] == ">  1 Prog  loader          6", "block 1: the pointer, Prog, the name, the length");
        check(T.size() > 3 && T[3] == "   3 Code  pic             4", "block 3: Code");
        check(T.size() > 5 && T[5] == "   5 Data                  3", "a headerless block: Data");
        check(T.size() > 8 && T[8] == "7 blocks, mounted", "the summary");
        bool fit31 = true;
        for (auto& l : T) if (l.size() > 31) fit31 = false;
        check(fit31, "every line fits in 31 columns");
        c.str("games/cut.tap");
        check(e.service(DiskEngine::SVC_CAT_OPEN, c) == DiskEngine::OK, "CAT \"games/cut.tap\": OK, not mounted");
        std::string cut = drain(e, c);
        check(cut.find("5 blocks\r") != std::string::npos && cut.find('>') == std::string::npos, "the cut block is left out, no pointer");
        check(e.mounted() == "/games/game.tap", "and game.tap is still the one mounted");
        c.HL = 0;
        e.service(DiskEngine::SVC_CAT_OPEN, c);
        // Bare CAT in /games/ isn't possible yet (no CD); list /games/ by name.
        c.HL = 1; c.str("/games");
        e.service(DiskEngine::SVC_CAT_OPEN, c);
        std::string g = drain(e, c);
        check(g.find("  2>game.tap") != std::string::npos, "a folder listing marks the mounted TAP");

        // CLOSE #* unmounts; a reset too.
        check(e.service(DiskEngine::SVC_CLOSE_ALL, c) == DiskEngine::OK && e.mounted().empty(), "CLOSE_ALL unmounts");
        e.mount("/games/game.tap");
        e.reset();
        check(e.mounted().empty(), "a reset unmounts");

        // Mounting another TAP replaces the first.
        writeBytes(root + "/other.tap", tapBlock(0xFF, "q"));
        e.mount("/games/game.tap");
        c.str("other.tap");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::T_DONE && e.mounted() == "/other.tap", "LOAD \"other.tap\": it replaces game.tap");
        // SAVE "x.tap" is a block name: nothing mounts.
        c.mem[0x5C74] = 0;
        c.str("games/game.tap");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::T_NOT_OURS && e.mounted() == "/other.tap", "SAVE \"games/game.tap\": a block name");

        // FORMAT.
        c.str("new");
        check(e.service(DiskEngine::SVC_FORMAT, c) == DiskEngine::OK && e.mounted() == "/new.tap", "FORMAT \"new\": /new.tap, mounted");
        check(readFile((root + "/new.tap").c_str()).empty() && e.tapBlocks() == 0, "empty");
        c.str("new.tap");
        check(e.service(DiskEngine::SVC_FORMAT, c) == DiskEngine::R_NAME, "FORMAT over an existing file: F");
        c.str("");
        check(e.service(DiskEngine::SVC_FORMAT, c) == DiskEngine::R_NAME, "FORMAT \"\": F");
        c.str("nofolder/x.tap");
        check(e.service(DiskEngine::SVC_FORMAT, c) == DiskEngine::R_IO, "FORMAT in a folder that isn't there: J");
        c.IX = 0x6000; c.DE = 17; c.pushedAF(0x00, true);
        check(e.service(DiskEngine::SVC_R_TAPE, c) == DiskEngine::R_NAME, "R_TAPE on an empty TAP: F");
    }


    printf("\nNative files\n");
    {
        DiskEngine e(root);
        FakeCpu c;
        c.mem[0x5C3B] = 0x80;                     // FLAGS: run time
        // CH_ADD points at what follows the name: 6000h.
        auto after = [&c](uint8_t tok) { c.mem[0x5C5D] = 0x00; c.mem[0x5C5E] = 0x60; c.mem[0x6000] = ' '; c.mem[0x6001] = tok; };
        auto cmd = [&c](uint8_t t) { c.mem[0x5C74] = t; };
        auto blocks = [&e, &c](uint16_t hdrAt, uint16_t dataAt, bool load) -> int {
            // The ROM: a header, then the data with the header's length.
            c.IX = hdrAt; c.DE = 17; c.pushedAF(0x00, load);
            uint8_t r = e.service(DiskEngine::SVC_R_TAPE, c);
            if (r != DiskEngine::T_DONE) return r;
            c.IX = dataAt; c.DE = c.mem[hdrAt + 11] | (c.mem[hdrAt + 12] << 8); c.pushedAF(0xFF, load);
            return e.service(DiskEngine::SVC_R_TAPE, c);
        };
        auto save = [&e, &c](uint8_t type, const std::string& name, uint16_t len, uint16_t p1, uint16_t p2, uint16_t from) {
            uint16_t h = 0x6100;
            c.mem[h] = type;
            for (int i = 0; i < 10; i++) c.mem[h + 1 + i] = i < (int) name.size() ? name[i] : ' ';
            uint16_t f[3] = {len, p1, p2};
            for (int i = 0; i < 3; i++) { c.mem[h + 11 + 2 * i] = f[i] & 0xFF; c.mem[h + 12 + 2 * i] = f[i] >> 8; }
            c.IX = h; c.DE = 17; c.pushedAF(0x00, false);
            uint8_t r = e.service(DiskEngine::SVC_W_TAPE, c);
            if (r != DiskEngine::T_DONE) return (int) r;
            c.IX = from; c.DE = len; c.pushedAF(0xFF, false);
            return (int) e.service(DiskEngine::SVC_W_TAPE, c);
        };

        // SAVE "games/hello.bas" LINE 10: a program.
        cmd(0); after(0xCA);
        c.str("games/hello.bas");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::T_NATIVE, "SAVE \"games/hello.bas\": a native file");
        check(c.mem[DiskEngine::NAME_LEN] == 5 && memcmp(c.mem + DiskEngine::NAME_BUF, "hello", 5) == 0, "SLVM gets the name hello");
        check(e.service(DiskEngine::SVC_TAPE_OURS, c) == 1, "TAPE_OURS: SAVE skips the tape prompt");
        const char prog[] = "\x00\x0a\x05\x00\xf5\x22\x68\x69\x22\x0d";   // 10 PRINT "hi"
        memcpy(c.mem + 0x7000, prog, 10);
        check(save(0, "hello", 10, 10, 10, 0x7000) == DiskEngine::T_DONE, "the header and the program go to the engine");
        std::vector<uint8_t> file = readFile((root + "/games/hello.bas").c_str());
        std::string want = DiskEngine::plus3Header(0, 10, 10, 10) + std::string(prog, 10);
        check(file.size() == 138 && memcmp(file.data(), want.data(), 138) == 0, "games/hello.bas: the +3DOS header and the program");
        check(memcmp(file.data(), "PLUS3DOS\x1a\x01\x00\x8a\x00\x00\x00\x00\x0a\x00\x0a\x00\x0a\x00", 22) == 0, "header fields as TS-Pico writes them");
        check(e.service(DiskEngine::SVC_TAPE_OURS, c) == 0, "done: TAPE_OURS 0");

        // LOAD "games/hello.bas": the header SLVM sees, then the program.
        cmd(1); after(0x0D);
        c.str("games/hello.bas");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::T_NATIVE && c.mem[DiskEngine::NAME_LEN] == 0, "LOAD: native, no name");
        memset(c.mem + 0x7000, 0, 16);
        check(blocks(0x6200, 0x7000, true) == DiskEngine::T_DONE, "the header, then the data");
        check(c.mem[0x6200] == 0 && memcmp(c.mem + 0x6201, "hello     ", 10) == 0 && c.mem[0x620D] == 10, "a Program header: hello, LINE 10");
        check(memcmp(c.mem + 0x7000, prog, 10) == 0, "the program is back");
        c.IX = 0x6200; c.DE = 17; c.pushedAF(0x00, true);
        check(e.service(DiskEngine::SVC_R_TAPE, c) == DiskEngine::T_NOT_OURS, "and then it's gone (nothing mounted: the stock routine)");
        // VERIFY, MERGE.
        cmd(2); c.str("games/hello.bas");
        e.service(DiskEngine::SVC_TAPE_NAME, c);
        check(blocks(0x6200, 0x7000, false) == DiskEngine::T_DONE, "VERIFY: the same bytes pass");
        c.mem[0x7003] ^= 1;
        c.str("games/hello.bas");
        e.service(DiskEngine::SVC_TAPE_NAME, c);
        check(blocks(0x6200, 0x7000, false) == DiskEngine::T_FAILED, "VERIFY: a changed byte fails");
        cmd(3); c.str("games/hello.bas");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::T_NATIVE, "MERGE a program: native");
        // The wrong type: Q.
        cmd(1); after(0xAF); c.str("games/hello.bas");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::R_PARAM, "LOAD a program as CODE: Q");
        after(0xE4);
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::R_PARAM, "as DATA: Q");

        // SAVE "pic" SCREEN$: raw 6912 bytes; LOAD it as SCREEN$ and CODE.
        cmd(0); after(0xAA); c.str("pic");
        for (int i = 0; i < 6912; i++) c.mem[0x4000 + i] = (uint8_t) (i * 7);
        e.service(DiskEngine::SVC_TAPE_NAME, c);
        check(save(3, "pic", 6912, 16384, 32768, 0x4000) == DiskEngine::T_DONE, "SAVE SCREEN$: done");
        file = readFile((root + "/pic").c_str());
        check(file.size() == 6912 && memcmp(file.data(), c.mem + 0x4000, 6912) == 0, "pic: the raw 6912 bytes");
        cmd(1); after(0xAA); c.str("pic");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::T_NATIVE, "LOAD \"pic\" SCREEN$: native");
        check(blocks(0x6200, 0xA000, true) == DiskEngine::T_DONE && memcmp(c.mem + 0xA000, c.mem + 0x4000, 6912) == 0
              && c.mem[0x620D] == 0x00 && c.mem[0x620E] == 0x40, "Code 16384,6912, the bytes");
        after(0x0D); c.str("pic");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::R_PARAM, "LOAD \"pic\" (a program): Q");

        // CODE and DATA keep their fields.
        cmd(0); after(0xAF); c.str("mc.bin");
        memcpy(c.mem + 0x8000, "\x01\xd2\x04\xc9", 4);
        e.service(DiskEngine::SVC_TAPE_NAME, c);
        check(save(3, "mc", 4, 32768, 32768, 0x8000) == DiskEngine::T_DONE, "SAVE \"mc.bin\" CODE 32768,4");
        cmd(0); after(0xE4); c.str("a.dat");
        memcpy(c.mem + 0x8100, "\x01\x02\x00\x00\x00\x00\x00", 7);
        e.service(DiskEngine::SVC_TAPE_NAME, c);
        check(save(1, "a", 7, 0xC100, 32768, 0x8100) == DiskEngine::T_DONE, "SAVE \"a.dat\" DATA a()");
        cmd(1); after(0xE4); c.str("a.dat");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::T_NATIVE && blocks(0x6200, 0x8200, true) == DiskEngine::T_DONE
              && c.mem[0x6200] == 1 && c.mem[0x620E] == 0xC1, "LOAD DATA: a number array, its name byte kept");

        // A file made elsewhere: as CODE it loads whole; otherwise F.
        writeBytes(root + "/raw.bin", "hello");
        cmd(1); after(0xAF); c.str("raw.bin");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::T_NATIVE && blocks(0x6200, 0x9000, true) == DiskEngine::T_DONE
              && memcmp(c.mem + 0x9000, "hello", 5) == 0 && c.mem[0x620D] == 0x00 && c.mem[0x620E] == 0x80, "a headerless file as CODE: whole, at 32768");
        after(0x0D);
        c.str("raw.bin");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::R_NAME, "as a program: F");

        // Missing names, folders, LOAD "".
        c.str("nope");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::R_NAME, "LOAD a file that isn't there: F");
        c.str("games");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::R_PARAM, "LOAD a folder: Q");
        c.str("");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::R_IO, "LOAD \"\" with nothing mounted: J");
        cmd(0);
        c.str("games");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::R_PARAM, "SAVE over a folder: Q");
        c.str("nofolder/x");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::R_NAME, "SAVE into a folder that isn't there: F");
        c.str("new.tap");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::R_NAME, "SAVE \"x.tap\" with nothing mounted: F (FORMAT makes TAPs)");
        c.str("a-much-longer-name.bas");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::T_NATIVE && c.mem[DiskEngine::NAME_LEN] == 10
              && memcmp(c.mem + DiskEngine::NAME_BUF, "a-much-lon", 10) == 0, "a long name: SLVM gets 10 characters");

        // Replace?
        c.str("games/hello.bas");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::T_ASK, "SAVE over a file: ask");
        check(std::string((char*) c.mem + DiskEngine::PROMPT_BUF) == "Replace hello.bas? (Y/N)", "Replace hello.bas? (Y/N)");
        check(e.service(DiskEngine::SVC_TAPE_OURS, c) == 0, "not ours until Y");
        c.HL = 0;
        check(e.service(DiskEngine::SVC_REPLACE, c) == DiskEngine::R_BREAK && e.service(DiskEngine::SVC_TAPE_OURS, c) == 0, "N: D, and nothing armed");
        e.service(DiskEngine::SVC_TAPE_NAME, c);
        c.HL = 1;
        check(e.service(DiskEngine::SVC_REPLACE, c) == DiskEngine::OK && e.service(DiskEngine::SVC_TAPE_OURS, c) == 1, "Y: the SAVE goes ahead");
        memcpy(c.mem + 0x7000, "\x00\x14\x02\x00\xfb\x0d", 6);
        check(save(0, "hello", 6, 0x8000, 6, 0x7000) == DiskEngine::T_DONE && readFile((root + "/games/hello.bas").c_str()).size() == 134, "replaced");

        // With a TAP mounted, nothing is native.
        writeBytes(root + "/t.tap", "");
        e.mount("/t.tap");
        cmd(0); c.str("x.bas");
        check(e.service(DiskEngine::SVC_TAPE_NAME, c) == DiskEngine::T_NOT_OURS, "a TAP mounted: SAVE goes to it");
        e.reset();

        // CAT shows the types.
        std::string L2;
        check(e.buildListing("/", L2) == DiskEngine::OK, "the root lists");
        for (auto& l : lines(L2)) printf("        |%s|\n", l.c_str());
        check(L2.find(row("a.dat", "Num()", "135")) != std::string::npos, "a.dat: Num()");
        check(L2.find(row("mc.bin", "Code", "132")) != std::string::npos, "mc.bin: Code");
        check(L2.find(row("pic", "Scr", "6912")) != std::string::npos, "pic: Scr");
        check(L2.find(row("raw.bin", "", "5")) != std::string::npos, "raw.bin: no type");
        e.buildListing("/games/", L2);
        check(L2.find(row("hello.bas", "Prog", "134")) != std::string::npos, "games/hello.bas: Prog");
    }


    printf("\nFile management\n");
    {
        std::string r2 = "/tmp/diskengine_test_fm";
        system(("rm -rf " + r2).c_str());
        mkdir(r2.c_str(), 0755);
        mkdir((r2 + "/games").c_str(), 0755);
        mkdir((r2 + "/backup").c_str(), 0755);
        writeBytes(r2 + "/a.tap", "AAAA");
        writeBytes(r2 + "/b.tap", "BB");
        writeBytes(r2 + "/notes.txt", "line one\r\nline two\nend");
        writeBytes(r2 + "/bin.dat", std::string("\x00\x01\x02hello\xff", 9));
        writeBytes(r2 + "/backup/b.tap", "old");
        DiskEngine e(r2);
        std::string out;

        check(DiskEngine::wildMatch("*.tap", "GAME.TAP") && DiskEngine::wildMatch("g?me*", "game1.bas")
              && !DiskEngine::wildMatch("*.tap", "a.tapx") && DiskEngine::wildMatch("*", "x") && !DiskEngine::wildMatch("a?", "a"),
              "wildcards: * and ?, case ignored");

        // CAT "*.tap"
        check(e.buildListing("/", out, "*.tap") == DiskEngine::OK, "CAT \"*.tap\": OK");
        std::vector<std::string> L = lines(out);
        check(L.size() == 4 && L[0] == "/*.tap" && L[3] == "0 folders, 2 files", "the matches only");
        check(e.buildListing("/", out, "*.zip") == DiskEngine::R_NAME, "no match: F");

        // MOVE TO
        check(e.moveTo("games") == DiskEngine::OK && e.cwd() == "/games/", "MOVE TO \"games\"");
        check(e.moveTo("..") == DiskEngine::OK && e.cwd() == "/", "MOVE TO \"..\"");
        check(e.moveTo("-") == DiskEngine::OK && e.cwd() == "/games/", "MOVE TO \"-\": back");
        check(e.moveTo("-") == DiskEngine::OK && e.cwd() == "/", "and back again");
        check(e.moveTo("/games") == DiskEngine::OK && e.moveTo("/") == DiskEngine::OK && e.cwd() == "/", "MOVE TO \"/\"");
        check(e.moveTo("nope") == DiskEngine::R_NAME && e.moveTo("a.tap") == DiskEngine::R_PARAM
              && e.moveTo("") == DiskEngine::R_NAME && e.moveTo("..") == DiskEngine::R_NAME, "missing F, a file Q, \"\" F, above the root F");
        e.mount("/a.tap");
        e.moveTo("games");
        check(e.mounted().empty(), "changing folder unmounts the TAP");
        e.moveTo("/");

        // MOVE a TO b
        check(e.move("b.tap", "c.tap") == DiskEngine::OK && readFile((r2 + "/c.tap").c_str()).size() == 2, "MOVE renames");
        check(e.move("c.tap", "games") == DiskEngine::OK && readFile((r2 + "/games/c.tap").c_str()).size() == 2, "MOVE into a folder keeps the name");
        check(e.move("a.tap", "notes.txt") == DiskEngine::R_NAME, "MOVE onto a file: F");
        check(e.move("nope", "x") == DiskEngine::R_NAME, "MOVE a missing file: F");
        check(e.move("games", "games/sub") == DiskEngine::R_PARAM, "MOVE a folder into itself: Q");
        e.mount("/a.tap");
        check(e.move("a.tap", "z.tap") == DiskEngine::R_PARAM, "MOVE the mounted TAP: Q");
        e.unmount();
        e.moveTo("games");
        check(e.move("/games", "/g2") == DiskEngine::R_PARAM, "MOVE the current folder: Q");
        e.moveTo("/");
        check(e.move("*.tap", "x") == DiskEngine::R_PARAM, "MOVE a pattern: Q");

        // COPY
        check(e.copy("a.tap", "a2.tap", out) == DiskEngine::OK && readFile((r2 + "/a2.tap").c_str()).size() == 4, "COPY copies");
        check(readFile((r2 + "/a.tap").c_str()).size() == 4, "the source is untouched");
        check(e.copy("a.tap", "a2.tap", out) == DiskEngine::R_NAME, "COPY onto a file: F");
        check(e.copy("a.tap", "games", out) == DiskEngine::OK && readFile((r2 + "/games/a.tap").c_str()).size() == 4, "COPY into a folder keeps the name");
        check(e.copy("games", "g", out) == DiskEngine::R_PARAM, "COPY a folder: Q");
        writeBytes(r2 + "/b.tap", "BB");
        check(e.copy("*.tap", "backup", out) == DiskEngine::OK, "COPY \"*.tap\" TO \"backup\"");
        L = lines(out);
        for (auto& l : L) printf("        |%s|\n", l.c_str());
        check(L.size() == 3 && L[0].find("a.tap") == 0 && L[0].find("copied") != std::string::npos
              && L[2].find("b.tap") == 0 && L[2].find("exists") != std::string::npos, "each copied or exists");
        check(readFile((r2 + "/backup/b.tap").c_str()).size() == 3, "an existing copy isn't overwritten");
        check(e.copy("*.tap", "a.tap", out) == DiskEngine::R_PARAM, "a pattern to a file: Q");
        check(e.copy("*.zip", "backup", out) == DiskEngine::R_NAME, "a pattern with no match: F");

        // FORMAT
        check(e.format("tools/") == DiskEngine::OK && e.moveTo("tools") == DiskEngine::OK, "FORMAT \"tools/\": a folder");
        e.moveTo("/");
        check(e.format("tools/") == DiskEngine::R_NAME, "FORMAT an existing folder: F");
        check(e.format("notes.txt") == DiskEngine::R_PARAM && e.format("v1.2") == DiskEngine::R_PARAM, "FORMAT \"notes.txt\" / \"v1.2\": Q");
        check(e.format(".tap") == DiskEngine::R_NAME, "FORMAT \".tap\": F");
        check(e.format("v1.2.tap") == DiskEngine::OK && e.mounted() == "/v1.2.tap", "FORMAT \"v1.2.tap\": a TAP, mounted");
        e.unmount();

        // ERASE
        check(e.erase("a2.tap", out) == DiskEngine::OK && out.empty() && readFile((r2 + "/a2.tap").c_str()).empty(), "ERASE: silently");
        check(e.erase("a2.tap", out) == DiskEngine::R_NAME, "ERASE a missing file: F");
        check(e.erase("tools", out) == DiskEngine::R_PARAM, "ERASE a folder without the /: Q");
        check(e.erase("games/", out) == DiskEngine::R_PARAM, "ERASE a folder that isn't empty: Q");
        check(e.erase("tools/", out) == DiskEngine::OK && e.moveTo("tools") == DiskEngine::R_NAME, "ERASE \"tools/\": gone");
        e.mount("/a.tap");
        check(e.erase("a.tap", out) == DiskEngine::R_PARAM, "ERASE the mounted TAP: Q");
        e.unmount();
        writeBytes(r2 + "/x1.bak", "1");
        writeBytes(r2 + "/x2.bak", "2");
        writeBytes(r2 + "/x3.bak", "3");
        writeBytes(r2 + "/x4.bak", "4");
        check(e.erase("*.bak", out) == DiskEngine::OK && out == std::string("Erase x1.bak (Y/N)? \x01"), "ERASE \"*.bak\": the first question");
        check(e.answer(1, out) == DiskEngine::OK && out == std::string("Y\rErase x2.bak (Y/N)? \x01") && readFile((r2 + "/x1.bak").c_str()).empty(), "Y erases, the next question");
        check(e.answer(2, out) == DiskEngine::OK && out == std::string("-\rErase x3.bak (Y/N)? \x01") && readFile((r2 + "/x2.bak").c_str()).size() == 1, "another key skips");
        check(e.answer(0, out) == DiskEngine::OK && out == "N\r", "N stops");
        check(e.answer(1, out) == DiskEngine::OK && out.empty() && readFile((r2 + "/x4.bak").c_str()).size() == 1, "and nothing more is asked");

        // The service protocol for ERASE: text, the question, ANSWER, END.
        FakeCpu c;
        c.str("*.bak");
        check(e.service(DiskEngine::SVC_ERASE, c) == DiskEngine::OK, "SVC_ERASE: OK");
        std::string got;
        uint8_t ch;
        while ((ch = e.service(DiskEngine::SVC_CAT_CHAR, c)) > DiskEngine::OUT_ASK) got += (char) ch;
        check(ch == DiskEngine::OUT_ASK && got == "Erase x2.bak (Y/N)? ", "CAT_CHAR: the question, then OUT_ASK");
        c.HL = 1;
        e.service(DiskEngine::SVC_ANSWER, c);
        got.clear();
        while ((ch = e.service(DiskEngine::SVC_CAT_CHAR, c)) > DiskEngine::OUT_ASK) got += (char) ch;
        check(ch == DiskEngine::OUT_ASK && got == "Y\rErase x3.bak (Y/N)? ", "after ANSWER, the next");
        c.HL = 0;
        e.service(DiskEngine::SVC_ANSWER, c);
        got.clear();
        while ((ch = e.service(DiskEngine::SVC_CAT_CHAR, c)) > DiskEngine::OUT_ASK) got += (char) ch;
        check(ch == 0 && got == "N\r" && e.service(DiskEngine::SVC_END, c) == DiskEngine::OK, "N, the end, END OK");

        // COPY and MOVE through the services: IX/HL is the second name.
        c.str("a.tap");
        const char* second = "a3.tap";
        memcpy(c.mem + 0xE000, second, 6);
        c.IX = 0xE000; c.HL = 6;
        check(e.service(DiskEngine::SVC_COPY, c) == DiskEngine::OK && readFile((r2 + "/a3.tap").c_str()).size() == 4, "SVC_COPY");
        c.str("a3.tap");
        memcpy(c.mem + 0xE000, "a4.tap", 6);
        check(e.service(DiskEngine::SVC_MOVE, c) == DiskEngine::OK && readFile((r2 + "/a4.tap").c_str()).size() == 4, "SVC_MOVE");
        c.str("games");
        check(e.service(DiskEngine::SVC_MOVE_TO, c) == DiskEngine::OK && e.cwd() == "/games/", "SVC_MOVE_TO");
        e.reset();
        check(e.cwd() == "/", "a reset: back to /");

        // LIST
        check(e.list("notes.txt", out) == DiskEngine::OK && out == "line one\rline two\rend\r", "LIST a text file: CR, LF and CRLF all end lines");
        check(e.list("bin.dat", out) == DiskEngine::OK && out == "0000 00010268656C6C6F ...hello\r0008 FF               .\r", "LIST a binary file: a hex dump");
        check(e.list("games", out) == DiskEngine::R_PARAM && e.list("*.tap", out) == DiskEngine::R_PARAM, "LIST a folder or a pattern: Q");
        check(e.list("nope", out) == DiskEngine::R_NAME, "LIST a missing file: F");
    }

    printf("\nFile channels\n");
    {
        std::string croot = root + "/ch";
        mkdir(croot.c_str(), 0755);
        DiskEngine ce(croot);
        typedef DiskEngine E;
        check(ce.chOpen(4, "f:none.txt", "", 0) == E::R_NAME, "OPEN #4,\"f:none.txt\" (read, missing): F");
        check(ce.chOpen(4, "q:x", "", 0) == E::R_IO, "an unknown device: J");
        check(ce.chOpen(4, "f:", "w", 0) == E::R_NAME, "no name: F");
        check(ce.chOpen(4, "f:a.txt", "x", 0) == E::R_PARAM, "a bad mode: Q");
        check(ce.chOpen(4, "f:a.txt", "wbb", 0) == E::R_PARAM, "a mode too long: Q");
        check(ce.chOpen(4, "f:a.txt", "a", 10) == E::R_PARAM, "append with a record length: Q");
        check(ce.chOpen(4, "f:a.txt", "w", 255) == E::R_PARAM, "a record length over 254: Q");

        // Text: CR -> LF, keywords expanded, controls dropped, the comma to column 16.
        check(ce.chOpen(4, "F:notes.txt", "W", 0) == E::OK, "OPEN #4,\"F:notes.txt\",\"W\": OK (case ignored)");
        check(ce.chIsOpen(4), "stream 4 is open");
        check(ce.chOpen(5, "f:notes.txt", "r", 0) == E::R_NAME, "the same file on another stream: F");
        check(chPrint(ce, 4, "Hello\r") == E::OK, "PRINT #4;\"Hello\"");
        check(chPrint(ce, 4, std::string("\x10\x02\x11\x05" "ab\x16\x01\x02" "c\x06" "d\r")) == E::OK,
              "INK, PAPER, AT dropped; the comma");
        check(chPrint(ce, 4, std::string("  10 \xf5\"x\"\xcb\xec 20\r")) == E::OK, "a LIST # line");
        check(chPrint(ce, 4, std::string("\xa6\xc5\xa5\xb1 x\r")) == E::OK, "functions and operators");
        check(chPrint(ce, 4, std::string("\x7b\x0c\x7e\r")) == E::OK, "the 2068's own keywords");
        check(ce.chIn(4, *(new uint8_t)) == E::R_PARAM, "INPUT # on a \"w\" channel: Q");
        check(ce.chClose(4) == E::OK && !ce.chIsOpen(4), "CLOSE #4");
        std::string t = fileText(croot + "/notes.txt");
        printf("        |%s|\n", t.c_str());
        check(t.substr(0, 6) == "Hello\n", "CR is written as LF");
        check(t.find("abc             d\n") != std::string::npos, "controls dropped, the comma to column 16");
        check(t.find("  10 PRINT \"x\" THEN GO TO  20\n") != std::string::npos, "keywords expanded with spaces");
        check(t.find("INKEY$ OR RNDLEN  x\n") != std::string::npos, "functions without a space before");
        check(t.find("ON ERR DELETE FREE \n") != std::string::npos, "ON ERR, DELETE, FREE");
        check(ce.chClose(4) == E::OK, "CLOSE # of a closed stream: nothing");

        // Reading text: LF, CRLF and CR line ends; a last line with none; Report 8.
        writeBytes(croot + "/in.txt", "one\ntwo\r\nthree\rfour");
        check(ce.chOpen(5, "f:in.txt", "", 0) == E::OK, "OPEN #5,\"f:in.txt\": read by default");
        uint8_t stop = 99;
        check(chLine(ce, 5) == "one" && chLine(ce, 5) == "two" && chLine(ce, 5) == "three", "LF, CRLF, CR");
        check(chLine(ce, 5, &stop) == "four" && stop == E::OK, "a last line without a line end still ends");
        check(chLine(ce, 5, &stop) == "" && stop == E::CH_EOF, "then the end of the file (Report 8)");
        check(chPrint(ce, 5, "dropped\r") == E::OK, "INPUT #'s prompt on a read channel is dropped");
        check(ce.chRestore(5) == E::OK && chLine(ce, 5) == "one", "RESTORE #5: back to the start");
        check(chPrint(ce, 5, tab(5)) == E::OK && chLine(ce, 5) == "two", "TAB 5 in a stream: byte 5");
        check(chPrint(ce, 5, tab(0)) == E::OK && chLine(ce, 5) == "19", "TAB 0: the size in bytes");
        ce.chClose(5);
        check(fileText(croot + "/in.txt") == "one\ntwo\r\nthree\rfour", "reading leaves the file alone");

        // Binary: bytes as they are, but 23 is TAB.
        check(ce.chOpen(6, "f:data.bin", "wb", 0) == E::OK, "OPEN #6,\"f:data.bin\",\"wb\"");
        check(chPrint(ce, 6, std::string("\x10\x02\xf5\r\x00\xff", 6)) == E::OK, "control codes, a keyword, CR, 0");
        ce.chClose(6);
        check(fileText(croot + "/data.bin") == std::string("\x10\x02\xf5\r\x00\xff", 6), "written unchanged");
        check(ce.chOpen(6, "f:data.bin", "rb", 0) == E::OK, "read back, binary");
        uint8_t b = 0;
        std::string back;
        while (ce.chIn(6, b) == E::OK) back += (char) b;
        check(back == std::string("\x10\x02\xf5\r\x00\xff", 6), "every byte, then the end");
        ce.chClose(6);

        // Append.
        check(ce.chOpen(4, "f:notes.txt", "a", 0) == E::OK, "OPEN #4,...,\"a\"");
        chPrint(ce, 4, "more\r");
        check(chPrint(ce, 4, tab(1)) == E::R_PARAM, "TAB on an append channel: Q");
        ce.chClose(4);
        t = fileText(croot + "/notes.txt");
        check(t.size() > 5 && t.substr(t.size() - 5) == "more\n", "appended at the end");

        // Records.
        check(ce.chOpen(7, "f:people.dat", "u", 10) == E::OK, "OPEN #7,\"f:people.dat\",\"u\",10 (made)");
        check(chPrint(ce, 7, tab(3) + "carol\r") == E::OK, "PRINT #7;TAB 3;\"carol\"");
        check(chPrint(ce, 7, tab(1) + "al" + "ice\r") == E::OK, "TAB 1, two items, one record");
        check(chPrint(ce, 7, "bob\r") == E::OK, "no TAB: the next record (2)");
        check(chPrint(ce, 7, tab(2) + "this is too long\r") == E::R_PARAM, "longer than the record: Q");
        check(chPrint(ce, 7, tab(4) + "dave") == E::OK && chPrint(ce, 7, "x\r") == E::OK,
              "PRINT ending in ; keeps the record open");
        check(chPrint(ce, 7, tab(0) + "x") == E::R_PARAM, "PRINT #7;TAB 0: Q");
        check(chPrint(ce, 7, tab(0)) == E::OK && chLine(ce, 7) == "4", "INPUT #7;TAB 0;n: 4 records");
        check(chPrint(ce, 7, tab(3)) == E::OK && chLine(ce, 7) == "carol     ", "INPUT #7;TAB 3;a$: the whole record");
        check(chLine(ce, 7) == "davex     ", "no TAB: the next one");
        check(chLine(ce, 7, &stop) == "" && stop == E::CH_EOF, "past the last record: Report 8");
        check(ce.chRestore(7) == E::OK && chLine(ce, 7) == "alice     ", "RESTORE #7: record 1");
        check(chPrint(ce, 7, tab(6) + "\r") == E::OK, "PRINT #7;TAB 6: an empty record, past the end");
        ce.chClose(7);
        check(fileText(croot + "/people.dat") == "alice     bob       carol     davex     " + std::string(20, ' '),
              "the file: records 1-6, the skipped one padded, no line ends");

        // "u" on a stream: a PRINT after an INPUT writes just after that line.
        writeBytes(croot + "/u.txt", "aa\nbb\n");
        check(ce.chOpen(8, "f:u.txt", "u", 0) == E::OK, "OPEN #8,\"f:u.txt\",\"u\"");
        check(chLine(ce, 8) == "aa", "INPUT #8 reads a line");
        chPrint(ce, 8, "XY\r");
        ce.chClose(8);
        check(fileText(croot + "/u.txt") == "aa\nXY\n", "PRINT #8 overwrote the next line");

        // CAT #s into a file.
        writeFile(croot + "/zz.tap", 0);
        check(ce.chCat(9, false, "") == E::R_STREAM, "CAT #9, not open: O");
        check(ce.chOpen(9, "f:cat.txt", "w", 0) == E::OK, "OPEN #9,\"f:cat.txt\",\"w\"");
        check(ce.chCat(9, true, "*.tap") == E::OK, "CAT #9;\"*.tap\"");
        ce.chClose(9);
        t = fileText(croot + "/cat.txt");
        printf("        |%s|\n", t.c_str());
        check(t.find("zz.tap") != std::string::npos && t.find("\n") != std::string::npos
              && t.find('\r') == std::string::npos, "the listing as text, LF line ends");
        check(ce.chOpen(9, "f:cat.txt", "r", 0) == E::OK && ce.chCat(9, false, "") == E::R_PARAM,
              "CAT # onto a read channel: Q");
        ce.chClose(9);

        // Open files can't be erased or moved; a reset closes them.
        ce.chOpen(10, "f:in.txt", "r", 0);
        std::string none;
        check(ce.erase("in.txt", none) == E::R_PARAM, "ERASE an open file: Q");
        ce.reset();
        check(!ce.chIsOpen(10), "a reset closes every channel");

        // Through the service calls: the stream comes from CURCHL's record.
        FakeCpu cc;
        cc.mem[E::STREAM_N] = 4;
        cc.mem[E::CH_ARG] = 0;
        cc.mem[E::CH_ARG + 1] = 0;
        cc.str("f:svc.txt");
        for (int i = 0; i < 2; i++) cc.mem[0xF100 + i] = "w"[i];
        cc.IX = 0xF100;
        cc.HL = 1;
        check(ce.service(E::SVC_CH_OPEN, cc) == E::OK, "CH_OPEN: stream (STREAM_N), spec DE/BC, mode IX/HL");
        // CHANS at 5CB6h: K S R P, then the F record; stream 4's entry points at it.
        uint16_t chans = 0x5CB6, rec = chans + 20;
        cc.mem[E::CHANS] = chans & 0xFF;
        cc.mem[E::CHANS + 1] = chans >> 8;
        cc.mem[rec + 4] = 'F';
        cc.mem[rec + 5] = 4;
        cc.mem[E::STRMS + 6 + 8] = 21;
        cc.mem[E::CURCHL] = rec & 0xFF;
        cc.mem[E::CURCHL + 1] = rec >> 8;
        cc.DE = 'A';
        check(ce.service(E::SVC_CH_OUT, cc) == E::OK, "CH_OUT: E through CURCHL's record");
        cc.DE = 0x0D;
        ce.service(E::SVC_CH_OUT, cc);
        cc.HL = 4;
        check(ce.service(E::SVC_CH_CLOSE, cc) == E::OK && fileText(croot + "/svc.txt") == "A\n", "CH_CLOSE: L = the stream");
        cc.HL = 1;
        cc.mem[0xF100] = 'r';
        cc.HL = 1;
        cc.str("f:svc.txt");
        check(ce.service(E::SVC_CH_OPEN, cc) == E::OK, "opened again to read");
        check(ce.service(E::SVC_CH_IN, cc) == E::OK && cc.mem[E::CH_BYTE] == 'A', "CH_IN: the byte at CH_BYTE");
        check(ce.service(E::SVC_CH_IN, cc) == E::OK && cc.mem[E::CH_BYTE] == 0x0D, "the line end as CR");
        check(ce.service(E::SVC_CH_IN, cc) == E::CH_EOF, "then CH_EOF");
        // NEW wipes CHANS: the next OPEN # finds stream 4 orphaned and closes it.
        cc.mem[E::STRMS + 6 + 8] = 0;
        cc.mem[E::STREAM_N] = 5;
        check(ce.service(E::SVC_CH_OPEN, cc) == E::OK && !ce.chIsOpen(4) && ce.chIsOpen(5),
              "after NEW the same file opens on another stream; the orphan is closed");
        cc.mem[rec + 4] = 'S';
        check(ce.service(E::SVC_CH_IN, cc) == E::R_STREAM, "CURCHL not an F record: O");
        ce.reset();
    }

    printf("\nROM patches\n");
    std::vector<uint8_t> stock = readFile("patches/base/TS2068_U16.BIN");
    std::vector<uint8_t> stockX = readFile("patches/base/TS2068_U20.BIN");
    std::vector<uint8_t> want = readFile("dos/out/home-dos.rom");
    std::vector<uint8_t> wantX = readFile("dos/out/exrom-dos.rom");
    check(stock.size() == 16384 && want.size() == 16384 && stockX.size() == 8192 && wantX.size() == 8192,
          "the genuine ROMs and the built ones are 16K and 8K");
    if (stock.size() == 16384 && want.size() == 16384 && stockX.size() == 8192 && wantX.size() == 8192) {
        std::vector<uint8_t> home = stock;
        check(DosDisk::patchHome(home.data()), "the genuine HOME ROM takes the patches");
        check(home == want, "the result is build_dos.py's home-dos.rom");
        std::vector<uint8_t> again = home;
        check(!DosDisk::patchHome(again.data()) && again == home, "a patched ROM is refused, untouched");
        std::vector<uint8_t> other = stock;
        other[0x25D7] = 0x00;
        std::vector<uint8_t> otherCopy = other;
        check(!DosDisk::patchHome(other.data()) && other == otherCopy, "a ROM that differs at a site is refused, untouched");

        std::vector<uint8_t> exrom = stockX;
        check(DosDisk::patchExrom(exrom.data()), "the genuine EXROM takes the patches");
        check(exrom == wantX, "the result is build_dos.py's exrom-dos.rom");
        check(exrom[0x0068] == 0xC3 && exrom[0x00FC] == 0xC3 && exrom[0x0215] == 0xCD && exrom[0x0851] == 0xC3,
              "W_TAPE, R_TAPE, SLVM's PGPSTR call and SAVE go to the hooks");
        check((exrom[0x0069] | exrom[0x006A] << 8) >= 0x1800 && (exrom[0x006A] << 8) < 0x1C00, "in BLK2");

        std::vector<uint8_t> home2 = readFile("patches/base/2068Home.BIN");
        std::vector<uint8_t> exrom2 = readFile("patches/base/2068Exrom.BIN");
        check(home2.size() == 16384 && DosDisk::patchHome(home2.data()), "the other stock HOME ROM takes them too");
        check(exrom2.size() == 8192 && DosDisk::patchExrom(exrom2.data()), "and the other stock EXROM");

        // The switch: HOME's OUT (D3h),A is followed, at the next address,
        // by the DOS ROM's code; the DOS ROM's OUT (D5h),A by HOME's.
        const uint8_t* dos = DosRom::image;
        check(home[0x25D6] == 0xC3 && home[0x25D7] == 0xDC && home[0x25D8] == 0x3C, "the keywords' stub jumps to 3CDCh");
        check(home[0x25D9] == 0x06 && home[0x25DA] == 0xD4 && home[0x25DB] == 0x18 && home[0x25DC] == 0xF9,
              "25D9h: LD B,D4h / JR 25D6h");
        check(home[0x25DD] == 0x06 && home[0x25DE] == 0xFF && home[0x25DF] == 0x18 && home[0x25E0] == 0xF5,
              "25DDh: LD B,FFh / JR 25D6h (COPY)");
        check(home[0x19D7] == 0x05 && home[0x19D8] == 0xDD && home[0x19D9] == 0x25, "COPY: class 05h, 25DDh");
        check(home[0x19AF] == 0x05 && home[0x19B0] == 0xD6 && home[0x19B1] == 0x25, "LIST: class 05h, 25D6h");
        check(home[0x1A03] == 0x05 && home[0x1A04] == 0xD9 && home[0x1A05] == 0x25, "CLOSE #: class 05h, 25D9h");
        check(home[0x1946] == 0xD2 && home[0x1947] == 0xC2, "CAT and FORMAT's syntax entries skip to class 05h");
        check(home[0x3CDC] == 0xF3 && home[0x3CDD] == 0xD3 && home[0x3CDE] == DiskEngine::PORT_DOS_ON, "HOME 3CDCh: DI / OUT (D3h),A");
        check(dos[0x3CDF] == 0xCD, "DOS 3CDFh: CALL DOS_MAIN");
        check(dos[0x3CE2] == 0xD3 && dos[0x3CE3] == DiskEngine::PORT_DOS_OFF, "DOS 3CE2h: OUT (D5h),A");
        check(home[0x3CE4] == 0xFB && home[0x3CE5] == 0xDA, "HOME 3CE4h: EI / JP C");
        check(dos[0x3CF2] == 0xD3 && dos[0x3CF3] == DiskEngine::PORT_DOS_OFF, "DOS 3CF2h: HOMECALL's OUT (D5h),A");
        check(home[0x3CF4] == 0xFB && home[0x3CF9] == 0xD3 && home[0x3CFA] == DiskEngine::PORT_DOS_ON, "HOME 3CF4h: EI ... OUT (D3h),A");
        check(dos[0x3CFB] == 0xC9 && home[0x3CFC] == 0xE9, "DOS 3CFBh: RET; HOME 3CFCh: JP (HL)");
        check(memcmp(home.data() + 0x3D00, stock.data() + 0x3D00, 0x300) == 0, "CHRSET is untouched");
    }

    printf("\nSCLD overlay\n");
    SCLD::allocateMemory();
    if (stock.size() == 16384) SCLD::loadHomeRom(stock.data(), stock.size());
    std::vector<uint8_t> exromImage = readFile("patches/base/TS2068_U20.BIN");
    if (exromImage.size() == 8192) SCLD::loadExromImage(exromImage.data(), 1);
    SCLD::reset();
    uint8_t* home0 = SCLD::memChunk[0];
    SCLD::setOverlay(true);
    check(SCLD::memChunk[0] == home0 && !SCLD::overlayOn(), "without an image, the overlay stays off");
    check(DosDisk::install(), "install() takes the stock ROM in SCLD");
    check(DosDisk::active(), "the native disk is on");
    check(DosDisk::tapeHooks() && exromImage[0x00FC] == 0xC3, "with the tape hooks in SCLD's EXROM");
    check(SCLD::homePage(1)[0x05D6] == 0xC3, "SCLD's HOME ROM is patched (25D6h)");
    SCLD::OUT_F4(0x00);
    SCLD::setOverlay(true);
    check(SCLD::memChunk[0] == DosDisk::romImage() && SCLD::memChunk[1] == DosDisk::romImage() + 0x2000, "overlay on: chunks 0-1 are the DOS ROM");
    check(SCLD::memChunkReadOnly[0] && SCLD::memChunkReadOnly[1], "read-only");
    check(SCLD::IN_F4() == 0x00, "the bank registers are untouched");
    SCLD::OUT_F4(0x01);
    check(SCLD::memChunk[0] == DosDisk::romImage(), "a write to F4h doesn't lift the overlay");
    SCLD::OUT_F4(0x00);
    SCLD::setOverlay(false);
    check(SCLD::memChunk[0] == SCLD::homePage(0) && SCLD::memChunk[1] == SCLD::homePage(1), "overlay off: HOME is back");
    SCLD::setOverlay(true);
    SCLD::reset();
    check(!SCLD::overlayOn() && SCLD::memChunk[0] == SCLD::homePage(0), "a reset turns the overlay off");
    check(!DosDisk::install(), "install() again refuses the already-patched ROM");

    printf("\n%s: %d failure%s\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
