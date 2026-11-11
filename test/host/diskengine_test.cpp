/*

ESP2068 — TS2068 port of ESPectrum

test/host/diskengine_test.cpp — host test for the native disk, Phase 1:
DiskEngine's CAT listing and service calls (with a fake CPU and a scratch
folder), DosDisk's HOME patches against the stock ROM, and SCLD's DOS ROM
overlay.

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
    uint16_t BC = 0, DE = 0, HL = 0;
    uint8_t mem[65536] = {};
    uint16_t bc() override { return BC; }
    uint16_t de() override { return DE; }
    uint16_t hl() override { return HL; }
    uint8_t peek(uint16_t a) override { return mem[a]; }
    void poke(uint16_t a, uint8_t v) override { mem[a] = v; }
};

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
    check(L.size() > 3 && L[3].find("a-very-long-file-na>") != std::string::npos, "a long name is cut with '>'");
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
    check(eng.service(DiskEngine::SVC_CAT_OPEN, cpu) == DiskEngine::OK, "CAT_OPEN, the current folder: OK");
    std::string got;
    for (int i = 0; i < 10000; i++) {
        uint8_t c = eng.service(DiskEngine::SVC_CAT_CHAR, cpu);
        if (c == 0) break;
        got += (char) c;
    }
    check(got == text, "CAT_CHAR hands out the listing, then 0");
    check(eng.service(DiskEngine::SVC_CAT_CHAR, cpu) == 0, "and stays at 0");
    cpu.BC = 3;
    check(eng.service(DiskEngine::SVC_CAT_OPEN, cpu) == DiskEngine::R_NAME, "CAT_OPEN with a folder: F for now");
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

    printf("\nHOME patches\n");
    std::vector<uint8_t> stock = readFile("patches/base/2068Home.BIN");
    std::vector<uint8_t> want = readFile("dos/out/home-dos.rom");
    check(stock.size() == 16384 && want.size() == 16384, "the stock HOME ROM and the built one are 16K");
    if (stock.size() == 16384 && want.size() == 16384) {
        std::vector<uint8_t> home = stock;
        check(DosDisk::patchHome(home.data()), "the stock HOME ROM takes the patches");
        check(home == want, "the result is build_dos.py's home-dos.rom");
        std::vector<uint8_t> again = home;
        check(!DosDisk::patchHome(again.data()) && again == home, "a patched ROM is refused, untouched");
        std::vector<uint8_t> other = stock;
        other[0x25C9] = 0x00;
        std::vector<uint8_t> otherCopy = other;
        check(!DosDisk::patchHome(other.data()) && other == otherCopy, "a ROM that differs at a site is refused, untouched");

        // The switch: HOME's OUT (D3h),A is followed, at the next address,
        // by the DOS ROM's CALL; the DOS ROM's OUT (D5h),A by HOME's code.
        const uint8_t* dos = DosRom::image;
        check(home[0x25C8] == 0xC3 && home[0x25C9] == 0xDC && home[0x25CA] == 0x3C, "CAT's stub jumps to 3CDCh");
        check(home[0x3CDF] == 0xD3 && home[0x3CE0] == DiskEngine::PORT_DOS_ON, "HOME 3CDFh: OUT (D3h),A");
        check(dos[0x3CE1] == 0xCD, "DOS 3CE1h: CALL DOS_MAIN");
        check(dos[0x3CE4] == 0xD3 && dos[0x3CE5] == DiskEngine::PORT_DOS_OFF, "DOS 3CE4h: OUT (D5h),A");
        check(home[0x3CE6] == 0xFB && home[0x3CE7] == 0x20 && home[0x3CE9] == 0xCD && home[0x3CEA] == 0x44 && home[0x3CEB] == 0x1B, "HOME 3CE6h: EI / JR NZ / CALL CHECK-END");
        check(dos[0x3CF1] == 0xD3 && dos[0x3CF2] == DiskEngine::PORT_DOS_OFF, "DOS 3CF1h: HOMECALL's OUT (D5h),A");
        check(home[0x3CF3] == 0xFB && home[0x3CF8] == 0xD3 && home[0x3CF9] == DiskEngine::PORT_DOS_ON, "HOME 3CF3h: EI ... OUT (D3h),A");
        check(dos[0x3CFA] == 0xC9, "DOS 3CFAh: RET");
        check(memcmp(home.data() + 0x3D00, stock.data() + 0x3D00, 0x300) == 0, "CHRSET is untouched");
    }

    printf("\nSCLD overlay\n");
    SCLD::allocateMemory();
    if (stock.size() == 16384) SCLD::loadHomeRom(stock.data(), stock.size());
    SCLD::reset();
    uint8_t* home0 = SCLD::memChunk[0];
    SCLD::setOverlay(true);
    check(SCLD::memChunk[0] == home0 && !SCLD::overlayOn(), "without an image, the overlay stays off");
    check(DosDisk::install(), "install() takes the stock ROM in SCLD");
    check(DosDisk::active(), "the native disk is on");
    check(SCLD::homePage(1)[0x05C8] == 0xC3, "SCLD's HOME ROM is patched (25C8h)");
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
