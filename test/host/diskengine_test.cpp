/*

ESP2068 — TS2068 port of ESPectrum

test/host/diskengine_test.cpp — host test for the native disk, Phases 1
and 2: DiskEngine's CAT listings, TAP mounting and tape services (with a
fake CPU and a scratch folder), DosDisk's HOME and EXROM patches against
both stock ROM pairs, and SCLD's DOS ROM overlay.

Build and run (from repo root, after tools/build_dos.py):

    g++ -std=c++17 -Wall -I include -DESP2068_HOST_TEST \
        test/host/diskengine_test.cpp src/DiskEngine.cpp src/DosDisk.cpp src/SCLD.cpp \
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
