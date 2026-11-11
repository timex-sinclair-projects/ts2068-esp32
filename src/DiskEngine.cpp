// DiskEngine: the native disk's service calls. See include/DiskEngine.h.

#include "DiskEngine.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// A listing keeps its names in one buffer, reserved above 16K so the ESP32
// puts it in PSRAM rather than the little internal RAM left; an entry is
// its offset there.
const size_t NAMES_RESERVE = 20000;
const size_t LISTING_RESERVE = 20000;

struct Entry {
    uint32_t name;      // offset in the names buffer, NUL-ended
    bool folder;
    long size;
};

bool lessNoCase(const char* a, const char* b) {
    for (;; a++, b++) {
        int ca = tolower((unsigned char) *a), cb = tolower((unsigned char) *b);
        if (ca != cb || !ca) return ca < cb;
    }
}

// A name as the 2068 can print it: 20h-7Eh, anything else as '?'.
std::string printable(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out += (c >= 0x20 && c < 0x7F) ? (char) c : '?';
    return out;
}

// At most `width` characters; a longer name ends in '>'.
std::string fit(const std::string& s, size_t width) {
    if (s.size() <= width) return s;
    return s.substr(0, width - 1) + ">";
}

// A size in 6 characters or fewer: bytes up to 99999, then K, then M.
std::string sizeText(long n) {
    char buf[16];
    if (n < 100000) snprintf(buf, sizeof buf, "%ld", n);
    else if (n < 100000L * 1024) snprintf(buf, sizeof buf, "%ldK", (n + 1023) / 1024);
    else snprintf(buf, sizeof buf, "%ldM", (n + 1024L * 1024 - 1) / (1024L * 1024));
    return buf;
}

bool endsWithNoCase(const std::string& s, const char* tail) {
    size_t n = strlen(tail);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char) s[s.size() - n + i]) != tolower((unsigned char) tail[i])) return false;
    return true;
}

bool equalNoCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() && endsWithNoCase(a, b.c_str());
}

// The 2068 system variables the tape functions read.
const uint16_t FLAGS = 0x5C3B;     // bit 7 set: run time
const uint16_t T_ADDR = 0x5C74;    // after SLVM: 0 SAVE, 1 LOAD, 2 VERIFY, 3 MERGE

const uint16_t CH_ADD = 0x5C5D;    // the next character of the BASIC line

const uint8_t TOK_SCREEN = 0xAA, TOK_CODE = 0xAF, TOK_DATA = 0xE4;

const char* const HEADER_TYPES[4] = {"Prog", "Num()", "Chr()", "Code"};

const size_t PLUS3_LEN = 128;
const char PLUS3_SIG[] = "PLUS3DOS\x1a";
const long SCREEN_LEN = 6912;
const uint16_t SCREEN_ADDR = 16384;

} // namespace

DiskEngine::DiskEngine(const std::string& root) : root_(root) {}

void DiskEngine::reset() {
    cwd_ = "/";
    listing_.clear();
    listingPos_ = 0;
    unmount();
    dropNative();
    prevCwd_.clear();
    eraseQueue_.clear();
    eraseNext_ = 0;
    endReport_ = OK;
    chCloseAll();
    pickState_ = PK_NONE;
    pickList_.clear();
    prFlush();
    prColumn_ = 0;
    prSuppress_ = true;
    prCtl_ = 0;
}

bool DiskEngine::resolve(const std::string& name, std::string& path2068) const {
    if (name.empty()) return false;
    std::vector<std::string> parts;
    std::string start = name[0] == '/' ? std::string("/") : cwd_;
    std::string all = start + "/" + name;
    std::string cur;
    for (size_t i = 0; i <= all.size(); i++) {
        if (i == all.size() || all[i] == '/') {
            if (cur == "..") {
                if (parts.empty()) return false;
                parts.pop_back();
            } else if (!cur.empty() && cur != ".")
                parts.push_back(cur);
            cur.clear();
        } else
            cur += all[i];
    }
    path2068 = "/";
    for (size_t i = 0; i < parts.size(); i++) path2068 += (i ? "/" : "") + parts[i];
    return true;
}

bool isTape(const std::string& name) {
    return endsWithNoCase(name, ".tap") || endsWithNoCase(name, ".tzx");
}

static uint32_t le(const uint8_t* p, int n) {
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

// A TZX: the ROM loads standard-speed data, so a block 10h is a block, and
// so is a block 11h whose timings are the ROM's (within a tenth). Anything
// else that carries data needs its own loader: skipped, and counted. The
// walk stops at a block it doesn't know or one cut short.
bool DiskEngine::scanTzx(FILE* f, long size, std::vector<Block>& out, unsigned& skipped) {
    uint8_t h[0x14];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, 10, f) != 10 || memcmp(h, "ZXTape!\x1A", 8) != 0) return false;
    long at = 10;
    auto near = [](uint32_t v, uint32_t rom) { return v * 10 >= rom * 9 && v * 10 <= rom * 11; };
    while (at < size) {
        if (fseek(f, at, SEEK_SET) != 0) break;
        int id = fgetc(f);
        if (id == EOF) break;
        long body = at + 1;
        // Each kind: its fixed fields, then where in them the length of
        // what follows is, its size, and what one unit of it is in bytes.
        static const struct { uint8_t id, fixed, lenAt, lenBytes, mul; } kinds[] = {
            {0x10, 4, 2, 2, 1},    {0x11, 0x12, 0x0F, 3, 1}, {0x12, 4, 0, 0, 0},    {0x13, 1, 0, 1, 2},
            {0x14, 0x0A, 7, 3, 1}, {0x15, 8, 5, 3, 1},       {0x18, 4, 0, 4, 1},    {0x19, 4, 0, 4, 1},
            {0x20, 2, 0, 0, 0},    {0x21, 1, 0, 1, 1},       {0x22, 0, 0, 0, 0},    {0x23, 2, 0, 0, 0},
            {0x24, 2, 0, 0, 0},    {0x25, 0, 0, 0, 0},       {0x26, 2, 0, 2, 2},    {0x27, 0, 0, 0, 0},
            {0x28, 2, 0, 2, 1},    {0x2A, 4, 0, 0, 0},       {0x2B, 5, 0, 0, 0},    {0x30, 1, 0, 1, 1},
            {0x31, 2, 1, 1, 1},    {0x32, 2, 0, 2, 1},       {0x33, 1, 0, 1, 3},    {0x35, 0x14, 0x10, 4, 1},
            {0x40, 4, 1, 3, 1},    {0x5A, 9, 0, 0, 0},
        };
        int k = -1;
        for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
            if (kinds[i].id == id) k = (int) i;
        if (k < 0) break;
        uint8_t fixed = kinds[k].fixed;
        if (body + fixed > size || (fixed && fread(h, 1, fixed, f) != fixed)) break;
        uint32_t extra = kinds[k].lenBytes ? le(h + kinds[k].lenAt, kinds[k].lenBytes) * kinds[k].mul : 0;
        long next = body + fixed + extra;
        if (next > size) break;
        if (id == 0x10 || id == 0x11) {
            uint32_t len = id == 0x10 ? le(h + 2, 2) : le(h + 0x0F, 3);
            bool rom = id == 0x10
                || (near(le(h + 6, 2), 855) && near(le(h + 8, 2), 1710) && h[0x0C] == 8);
            if (len >= 2 && len <= 0xFFFF && rom) {
                fseek(f, body + fixed, SEEK_SET);
                uint8_t flag = (uint8_t) fgetc(f);
                out.push_back({(uint32_t) (body + fixed), (uint16_t) len, flag});
            } else if (len) skipped++;
        } else if (id == 0x14 || id == 0x15 || id == 0x18 || id == 0x19)
            skipped++;
        at = next;
    }
    return true;
}

bool DiskEngine::scanTap(const std::string& host, std::vector<Block>& out, unsigned* skipped) {
    out.clear();
    if (skipped) *skipped = 0;
    FILE* f = fopen(host.c_str(), "rb");
    if (!f) return false;
    if (endsWithNoCase(host, ".tzx")) {
        unsigned n = 0;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        bool ok = scanTzx(f, size, out, n);
        fclose(f);
        if (skipped) *skipped = n;
        return ok;
    }
    uint32_t at = 0;
    uint8_t head[3];
    while (fread(head, 1, 3, f) == 3) {
        uint16_t len = head[0] | (head[1] << 8);
        if (len < 2) break;                         // not a block: the end, as far as we go
        if (fseek(f, at + 2 + len, SEEK_SET) != 0) break;
        // A block cut short at the end of the file isn't listed.
        long here = ftell(f);
        if (fseek(f, 0, SEEK_END) != 0) break;
        long size = ftell(f);
        if ((long) (at + 2 + len) > size) break;
        fseek(f, here, SEEK_SET);
        out.push_back({at + 2, len, head[2]});
        at += 2 + len;
    }
    fclose(f);
    return true;
}

uint8_t DiskEngine::mount(const std::string& path2068) {
    std::vector<Block> blocks;
    struct stat st;
    std::string host = hostPath(path2068);
    if (stat(host.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return R_NAME;
    if (!scanTap(host, blocks)) return R_IO;
    tap_ = path2068;
    blocks_.swap(blocks);
    tapPos_ = 0;
    examined_ = 0;
    return OK;
}

void DiskEngine::unmount() {
    tap_.clear();
    blocks_.clear();
    tapPos_ = 0;
    examined_ = 0;
}

std::string DiskEngine::text(DiskCpu& cpu) {
    std::string s;
    uint16_t de = cpu.de(), bc = cpu.bc();
    for (uint16_t i = 0; i < bc && i < 255; i++) s += (char) cpu.peek(de + i);
    return s;
}

std::string DiskEngine::hostPath(const std::string& path2068) const {
    // path2068 is absolute and already inside the root ("/" or "/x/").
    std::string p = root_ + path2068;
    if (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

// What CAT shows in a file's type column: the +3DOS header's type, Scr for
// a raw screen, TAP, or nothing.
const char* DiskEngine::kindOf(const std::string& host, long size) {
    if (endsWithNoCase(host, ".tap")) return "TAP";
    if (endsWithNoCase(host, ".tzx")) return "TZX";
    if (endsWithNoCase(host, ".dck")) return "DCK";
    if (endsWithNoCase(host, ".rom")) return "ROM";
    if (size < (long) PLUS3_LEN && size != SCREEN_LEN) return "";
    uint8_t head[PLUS3_LEN];
    FILE* f = fopen(host.c_str(), "rb");
    if (!f) return "";
    size_t got = fread(head, 1, sizeof head, f);
    fclose(f);
    FileInfo info;
    if (!describe(head, got, size, info)) return "";
    if (info.dataOffset == 0) return "Scr";
    return info.type < 4 ? HEADER_TYPES[info.type] : "?";
}

uint8_t DiskEngine::buildListing(const std::string& folder, std::string& out, const std::string& pattern) {
    out.clear();
    std::string host = hostPath(folder);
    DIR* d = opendir(host.c_str());
    if (!d) return R_IO;

    std::string names;
    names.reserve(NAMES_RESERVE);
    std::vector<Entry> entries;
    entries.reserve(MAX_ENTRIES);
    int more = 0;
    while (struct dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.empty() || name[0] == '.') continue;   // ., .., hidden files
        if (!pattern.empty() && !wildMatch(pattern.c_str(), name.c_str())) continue;
        if ((int) entries.size() >= MAX_ENTRIES || names.size() + name.size() + 1 > names.capacity()) {
            more++;
            continue;
        }
        struct stat st;
        if (stat((host + "/" + name).c_str(), &st) != 0) continue;
        entries.push_back({(uint32_t) names.size(), S_ISDIR(st.st_mode) != 0,
                           S_ISDIR(st.st_mode) ? 0 : (long) st.st_size});
        names += name;
        names += '\0';
    }
    closedir(d);

    const char* base = names.c_str();
    std::sort(entries.begin(), entries.end(), [base](const Entry& a, const Entry& b) {
        if (a.folder != b.folder) return a.folder;       // folders first
        return lessNoCase(base + a.name, base + b.name);
    });

    // Every line is 31 characters or fewer: at 32 the 2068 wraps to the
    // next line before the CR, leaving a blank line.
    if (!pattern.empty() && entries.empty() && !more) return R_NAME;
    out.reserve(LISTING_RESERVE);
    out += fit(printable(folder + pattern), 31) + "\r";
    int folders = 0, files = 0;
    char line[64];
    for (const Entry& e : entries) {
        std::string name = printable(base + e.name);
        if (e.folder) {
            out += "    <" + fit(name, 25) + ">\r";
            folders++;
        } else {
            files++;
            std::string path = folder + (folder.back() == '/' ? "" : "/") + (base + e.name);
            char mark = (!tap_.empty() && equalNoCase(path, tap_)) ? '>' : ' ';
            snprintf(line, sizeof line, "%3d%c%-14s %-5s %6s", files, mark, fit(name, 14).c_str(),
                     kindOf(host + "/" + (base + e.name), e.size), sizeText(e.size).c_str());
            out += line;
            out += "\r";
        }
    }
    if (more) {
        snprintf(line, sizeof line, "and %d more", more);
        out += line;
        out += "\r";
    }
    snprintf(line, sizeof line, "%d folder%s, %d file%s", folders, folders == 1 ? "" : "s",
             files, files == 1 ? "" : "s");
    out += line;
    out += "\r";
    return OK;
}

uint8_t DiskEngine::buildTapListing(const std::string& path2068, std::string& out) {
    out.clear();
    std::vector<Block> blocks;
    unsigned skipped = 0;
    std::string host = hostPath(path2068);
    struct stat st;
    if (stat(host.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return R_NAME;
    if (!scanTap(host, blocks, &skipped)) return R_IO;
    bool isMounted = !tap_.empty() && equalNoCase(path2068, tap_);
    FILE* f = fopen(host.c_str(), "rb");
    if (!f) return R_IO;

    out.reserve(LISTING_RESERVE);
    out += fit(printable(path2068), 31) + "\r";
    char line[64];
    for (size_t i = 0; i < blocks.size(); i++) {
        const Block& b = blocks[i];
        char mark = (isMounted && i == tapPos_) ? '>' : ' ';
        uint8_t h[18] = {};
        bool header = b.flag == 0x00 && b.len == 19 && fseek(f, b.offset + 1, SEEK_SET) == 0
                      && fread(h, 1, 17, f) == 17 && h[0] < 4;
        if (header) {
            std::string name;
            for (int k = 1; k <= 10; k++) name += (h[k] >= 0x20 && h[k] < 0x7F) ? (char) h[k] : '?';
            unsigned len = h[11] | (h[12] << 8);
            snprintf(line, sizeof line, "%c%3u %-5s %-10s %6u", mark, (unsigned) i + 1, HEADER_TYPES[h[0]],
                     name.c_str(), len);
        } else {
            char type[8];
            if (b.flag == 0xFF) snprintf(type, sizeof type, "Data");
            else snprintf(type, sizeof type, "Fl%02X", b.flag);
            snprintf(line, sizeof line, "%c%3u %-5s %-10s %6u", mark, (unsigned) i + 1, type, "",
                     (unsigned) b.len - 2);
        }
        out += line;
        out += "\r";
    }
    fclose(f);
    if (isMounted && tapPos_ >= blocks.size() && !blocks.empty()) out += ">   (the end)\r";
    if (skipped) {
        snprintf(line, sizeof line, "%u not for LOAD (turbo)", skipped);
        out += line;
        out += "\r";
    }
    snprintf(line, sizeof line, "%u block%s%s", (unsigned) blocks.size(), blocks.size() == 1 ? "" : "s",
             isMounted ? ", mounted" : "");
    out += line;
    out += "\r";
    return OK;
}

uint8_t DiskEngine::catOpen(DiskCpu& cpu) {
    return catInto(cpu.hl() != 0, cpu.hl() ? text(cpu) : std::string(), listing_);
}

uint8_t DiskEngine::catInto(bool arg, const std::string& name, std::string& out) {
    if (!arg) return buildListing(cwd_, out);
    std::string path;
    if (name.empty()) {
        if (tap_.empty()) return R_IO;
        return buildTapListing(tap_, out);
    }
    if (hasWild(name)) {
        std::string folder, last;
        if (!splitPattern(name, folder, last)) return R_NAME;
        return buildListing(folder, out, last);
    }
    if (!resolve(name, path)) return R_NAME;
    struct stat st;
    if (stat(hostPath(path).c_str(), &st) != 0) return R_NAME;
    if (S_ISDIR(st.st_mode)) return buildListing(path == "/" ? path : path + "/", out);
    if (isTape(path)) return buildTapListing(path, out);
    return R_NAME;
}

void DiskEngine::dropNative() {
    native_ = N_NONE;
    nativePath_.clear();
    nativeBlocks_.clear();
    nativePos_ = 0;
    saveHeader_.clear();
}

bool DiskEngine::describe(const uint8_t* head, size_t headLen, long size, FileInfo& out) {
    if (headLen >= PLUS3_LEN && memcmp(head, PLUS3_SIG, 9) == 0) {
        uint8_t sum = 0;
        for (int i = 0; i < 127; i++) sum += head[i];
        if (sum == head[127]) {
            out.type = head[15];
            out.length = head[16] | (head[17] << 8);
            out.param1 = head[18] | (head[19] << 8);
            out.param2 = head[20] | (head[21] << 8);
            out.dataOffset = PLUS3_LEN;
            return (long) PLUS3_LEN + out.length <= size;   // not cut short
        }
    }
    if (size == SCREEN_LEN) {
        out = {3, (uint16_t) SCREEN_LEN, SCREEN_ADDR, 32768, 0};
        return true;
    }
    return false;
}

std::string DiskEngine::plus3Header(uint8_t type, uint16_t length, uint16_t param1, uint16_t param2) {
    std::string h(PLUS3_LEN, '\0');
    memcpy(&h[0], PLUS3_SIG, 9);
    h[9] = 1;                                           // issue 1, version 0
    uint32_t total = PLUS3_LEN + length;
    for (int i = 0; i < 4; i++) h[11 + i] = (char) ((total >> (8 * i)) & 0xFF);
    h[15] = (char) type;
    uint16_t f[3] = {length, param1, param2};
    for (int i = 0; i < 3; i++) {
        h[16 + 2 * i] = (char) (f[i] & 0xFF);
        h[17 + 2 * i] = (char) (f[i] >> 8);
    }
    uint8_t sum = 0;
    for (int i = 0; i < 127; i++) sum += (uint8_t) h[i];
    h[127] = (char) sum;
    return h;
}

std::string DiskEngine::tapeNameOf(const std::string& path) {
    std::string base = path.substr(path.rfind('/') + 1);
    size_t dot = base.rfind('.');
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
    return base.substr(0, 10);
}

uint8_t DiskEngine::tapeName(DiskCpu& cpu) {
    examined_ = 0;
    dropNative();
    if (!(cpu.peek(FLAGS) & 0x80)) return T_NOT_OURS;   // the syntax pass
    return tapeNamed(cpu, cpu.peek(T_ADDR), text(cpu));
}

uint8_t DiskEngine::tapeNamed(DiskCpu& cpu, uint8_t command, const std::string& name) {
    std::string path;
    // LOAD/VERIFY/MERGE "x.tap": mount it (it replaces any TAP mounted) and
    // take its first program. SAVE "x.tap" is a block name like any other.
    if (command >= 1 && command <= 3 && isTape(name) && resolve(name, path)) {
        struct stat st;
        if (stat(hostPath(path).c_str(), &st) == 0 && !S_ISDIR(st.st_mode)) {
            uint8_t r = mount(path);
            return r == OK ? T_DONE : r;
        }
    }
    // LOAD "x.dck" / "x.rom": a cartridge, docked with a reset. Not for
    // VERIFY or MERGE.
    if (command >= 1 && command <= 3 && (endsWithNoCase(name, ".dck") || endsWithNoCase(name, ".rom"))) {
        if (command != 1) return R_PARAM;
        return dock(name);
    }
    // With a TAP mounted, everything goes to it; without one, to a native
    // file. A TZX is read-only: SAVE is J before anything is written.
    if (!tap_.empty()) return (command == 0 && endsWithNoCase(tap_, ".tzx")) ? R_IO : T_NOT_OURS;
    if (name.empty() && command >= 1 && command <= 3) return pickStart(command);   // LOAD "": pick
    if (command == 0) return saveNative(cpu, name);
    if (command <= 3) return loadNative(cpu, command, name);
    return T_NOT_OURS;
}

bool DiskEngine::dckValid(const uint8_t* data, size_t size) {
    size_t at = 0;
    if (size == 0) return false;
    while (at < size) {
        if (at + 9 > size) return false;
        const uint8_t* types = data + at + 1;
        at += 9;
        for (int c = 0; c < 8; c++)
            if (types[c] & 0x02) {
                if (at + 0x2000 > size) return false;
                at += 0x2000;
            }
    }
    return true;
}

uint8_t DiskEngine::dock(const std::string& name) {
    std::string path;
    if (!resolve(name, path)) return R_NAME;
    std::string host = hostPath(path);
    struct stat st;
    if (stat(host.c_str(), &st) != 0) return R_NAME;
    if (S_ISDIR(st.st_mode)) return R_PARAM;
    bool rom = endsWithNoCase(path, ".rom");
    // A .DCK has a 9-byte header per bank and up to 16 banks' chunks; a
    // cartridge ROM fills at most the DOCK's 64K.
    long most = rom ? 0x10000 : 9 * 256 + 0x20000;
    if (st.st_size <= 0 || st.st_size > most) return R_PARAM;
    std::vector<uint8_t> data((size_t) st.st_size);
    FILE* f = fopen(host.c_str(), "rb");
    if (!f) return R_IO;
    bool ok = fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok) return R_IO;
    if (rom) {
        size_t chunks = (data.size() + 0x1FFF) / 0x2000;
        std::vector<uint8_t> dck(9, 0);
        for (size_t c = 0; c < chunks; c++) dck[1 + c] = 0x02;    // ROM, with its image
        data.resize(chunks * 0x2000, 0xFF);                       // a short last chunk: empty ROM
        dck.insert(dck.end(), data.begin(), data.end());
        data.swap(dck);
    } else if (!dckValid(data.data(), data.size()))
        return R_PARAM;
    dock_.swap(data);
    return R_END;
}

bool DiskEngine::takeDock(std::vector<uint8_t>& out) {
    if (dock_.empty()) return false;
    out.swap(dock_);
    dock_.clear();
    return true;
}

uint8_t DiskEngine::loadNative(DiskCpu& cpu, uint8_t command, const std::string& name) {
    if (name.empty()) return R_IO;
    std::string path;
    if (!resolve(name, path)) return R_NAME;
    std::string host = hostPath(path);
    struct stat st;
    if (stat(host.c_str(), &st) != 0) return R_NAME;
    if (S_ISDIR(st.st_mode)) return R_PARAM;

    // What the statement wants: the token after the name (SLVM hasn't
    // looked at it yet).
    uint16_t at = cpu.peek(CH_ADD) | (cpu.peek(CH_ADD + 1) << 8);
    uint8_t token = cpu.peek(at);
    for (int i = 0; i < 64 && token == ' '; i++) token = cpu.peek(++at);
    enum { PROGRAM, BYTES, ARRAY } want = PROGRAM;
    if (token == TOK_CODE || token == TOK_SCREEN) want = BYTES;
    else if (token == TOK_DATA) want = ARRAY;

    FILE* f = fopen(host.c_str(), "rb");
    if (!f) return R_IO;
    uint8_t head[PLUS3_LEN];
    size_t got = fread(head, 1, sizeof head, f);
    FileInfo info;
    if (!describe(head, got, (long) st.st_size, info)) {
        // A file made elsewhere: as CODE it loads whole, at the statement's
        // address (32768 if it gives none).
        if (want != BYTES || st.st_size <= 0 || st.st_size > 0xFFFF) { fclose(f); return R_NAME; }
        info = {3, (uint16_t) st.st_size, 32768, 32768, 0};
    }
    if (command == 3) want = PROGRAM;                   // MERGE: a program, always
    bool fits = (want == PROGRAM && info.type == 0) || (want == BYTES && info.type == 3)
                || (want == ARRAY && (info.type == 1 || info.type == 2));
    if (!fits) { fclose(f); return R_PARAM; }

    std::string data(info.length + 2, '\0');
    data[0] = (char) 0xFF;
    bool ok = fseek(f, info.dataOffset, SEEK_SET) == 0
              && fread(&data[1], 1, info.length, f) == info.length;
    fclose(f);
    if (!ok) return R_IO;
    uint8_t x = 0xFF;
    for (size_t i = 1; i <= info.length; i++) x ^= (uint8_t) data[i];
    data[info.length + 1] = (char) x;

    std::string h(19, '\0');
    h[1] = (char) info.type;
    std::string tn = tapeNameOf(path);
    tn.resize(10, ' ');
    memcpy(&h[2], tn.data(), 10);
    uint16_t fld[3] = {info.length, info.param1, info.param2};
    for (int i = 0; i < 3; i++) {
        h[12 + 2 * i] = (char) (fld[i] & 0xFF);
        h[13 + 2 * i] = (char) (fld[i] >> 8);
    }
    x = 0;
    for (int i = 0; i < 18; i++) x ^= (uint8_t) h[i];
    h[18] = (char) x;

    nativeBlocks_ = {h, data};
    nativePos_ = 0;
    nativePath_ = path;
    native_ = N_LOAD;
    cpu.poke(NAME_LEN, 0);                              // any name: the one block there is
    return T_NATIVE;
}

uint8_t DiskEngine::saveNative(DiskCpu& cpu, const std::string& name) {
    std::string path;
    if (name.empty() || name.back() == '/' || isTape(name)) return R_NAME;
    if (!resolve(name, path) || path == "/") return R_NAME;
    struct stat st;
    std::string folder = path.substr(0, path.rfind('/'));
    if (!folder.empty() && (stat(hostPath(folder).c_str(), &st) != 0 || !S_ISDIR(st.st_mode))) return R_NAME;
    bool exists = stat(hostPath(path).c_str(), &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) return R_PARAM;

    std::string base = path.substr(path.rfind('/') + 1);
    std::string tn = tapeNameOf(path);
    if (tn.empty()) tn = base.substr(0, 10);
    for (size_t i = 0; i < tn.size(); i++) cpu.poke(NAME_BUF + i, (uint8_t) tn[i]);
    cpu.poke(NAME_LEN, (uint8_t) tn.size());
    nativePath_ = path;
    if (!exists) {
        native_ = N_SAVE;
        return T_NATIVE;
    }
    // "Replace name? (Y/N)" on one line of the lower screen.
    std::string q = "Replace " + fit(printable(base), 16) + "? (Y/N)";
    for (size_t i = 0; i < q.size(); i++) cpu.poke(PROMPT_BUF + i, (uint8_t) q[i]);
    cpu.poke(PROMPT_BUF + q.size(), 0);
    native_ = N_ASK;
    return T_ASK;
}

uint8_t DiskEngine::replace(DiskCpu& cpu) {
    if (native_ != N_ASK) return R_NONSENSE;
    if ((cpu.hl() & 0xFF) == 1) {
        native_ = N_SAVE;
        return OK;
    }
    dropNative();
    return R_BREAK;
}

uint8_t DiskEngine::serve(DiskCpu& cpu, const uint8_t* block, size_t len, bool load, uint16_t ix, uint16_t de) {
    // block: flag, data, checksum.
    if (len != (size_t) de + 2) return T_FAILED;
    uint8_t x = 0;
    for (size_t i = 0; i < len; i++) x ^= block[i];
    if (x != 0) return T_FAILED;
    for (uint16_t i = 0; i < de; i++) {
        uint8_t c = block[1 + i];
        if (load) cpu.poke(ix + i, c);
        else if (cpu.peek(ix + i) != c) return T_FAILED;
    }
    return T_DONE;
}

uint8_t DiskEngine::readTape(DiskCpu& cpu) {
    uint16_t sp = cpu.sp();
    bool load = cpu.peek(sp) & 0x01;                    // the pushed F: carry
    uint8_t flag = cpu.peek(sp + 1);                    // the pushed A
    uint16_t ix = cpu.ix(), de = cpu.de();

    if (native_ == N_LOAD) {
        // The file's two blocks, once.
        while (nativePos_ < nativeBlocks_.size() && (uint8_t) nativeBlocks_[nativePos_][0] != flag) nativePos_++;
        if (nativePos_ >= nativeBlocks_.size()) {
            dropNative();
            return R_NAME;
        }
        const std::string& b = nativeBlocks_[nativePos_++];
        uint8_t r = serve(cpu, (const uint8_t*) b.data(), b.size(), load, ix, de);
        if (flag != 0x00) dropNative();
        return r;
    }
    if (tap_.empty()) return T_NOT_OURS;

    // Forward from the pointer, round the end, until a block with this
    // flag; once every block has been passed over since the name (LOAD
    // looking for a name the TAP hasn't got), F.
    const Block* found = nullptr;
    while (examined_ < blocks_.size()) {
        const Block& b = blocks_[tapPos_];
        tapPos_ = (tapPos_ + 1) % blocks_.size();
        examined_++;
        if (b.flag == flag) { found = &b; break; }
    }
    if (!found) {
        examined_ = 0;
        return R_NAME;
    }
    if (flag != 0x00) examined_ = 0;                    // data: the next LOAD starts afresh
    if (found->len != (uint32_t) de + 2) return T_FAILED;

    FILE* f = fopen(hostPath(tap_).c_str(), "rb");
    if (!f) return R_IO;
    std::vector<uint8_t> data(found->len);
    bool ok = fseek(f, found->offset, SEEK_SET) == 0 && fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok) return R_IO;
    return serve(cpu, data.data(), data.size(), load, ix, de);
}

uint8_t DiskEngine::writeTape(DiskCpu& cpu) {
    uint16_t sp = cpu.sp();
    uint8_t flag = cpu.peek(sp + 1);
    uint16_t ix = cpu.ix(), de = cpu.de();

    if (native_ == N_SAVE) {
        if (flag == 0x00 && de == 17) {
            saveHeader_.clear();
            for (int i = 0; i < 17; i++) saveHeader_ += (char) cpu.peek(ix + i);
            return T_DONE;
        }
        if (saveHeader_.size() != 17) { dropNative(); return R_IO; }
        const uint8_t* h = (const uint8_t*) saveHeader_.data();
        uint8_t type = h[0];
        uint16_t p1 = h[13] | (h[14] << 8), p2 = h[15] | (h[16] << 8);
        std::string out;
        // A screen is the raw 6912 bytes every tool reads; anything else
        // has the +3DOS header.
        if (!(type == 3 && de == SCREEN_LEN && p1 == SCREEN_ADDR)) out = plus3Header(type, de, p1, p2);
        out.reserve(out.size() + de);
        for (uint32_t i = 0; i < de; i++) out += (char) cpu.peek(ix + i);
        std::string host = hostPath(nativePath_);
        dropNative();
        FILE* f = fopen(host.c_str(), "wb");
        if (!f) return R_IO;
        bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
        ok = (fclose(f) == 0) && ok;
        return ok ? T_DONE : R_IO;
    }
    if (tap_.empty()) return T_NOT_OURS;
    if ((uint32_t) de + 2 > 0xFFFF || endsWithNoCase(tap_, ".tzx")) return R_IO;
    std::vector<uint8_t> block(de + 4);
    uint16_t len = de + 2;
    block[0] = len & 0xFF;
    block[1] = len >> 8;
    block[2] = flag;
    uint8_t x = flag;
    for (uint16_t i = 0; i < de; i++) {
        block[3 + i] = cpu.peek(ix + i);
        x ^= block[3 + i];
    }
    block[3 + de] = x;

    std::string host = hostPath(tap_);
    FILE* f = fopen(host.c_str(), "ab");
    if (!f) return R_IO;
    fseek(f, 0, SEEK_END);
    long at = ftell(f);
    bool ok = at >= 0 && fwrite(block.data(), 1, block.size(), f) == block.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) return R_IO;
    blocks_.push_back({(uint32_t) at + 2, len, flag});
    return T_DONE;
}

bool DiskEngine::wildMatch(const char* p, const char* n) {
    // Iterative, with one backtrack point for the last '*'.
    const char *star = nullptr, *mark = nullptr;
    while (*n) {
        if (*p == '*') { star = p++; mark = n; continue; }
        if (*p && (*p == '?' || tolower((unsigned char) *p) == tolower((unsigned char) *n))) { p++; n++; continue; }
        if (star) { p = star + 1; n = ++mark; continue; }
        return false;
    }
    while (*p == '*') p++;
    return !*p;
}

bool DiskEngine::hasWild(const std::string& s) {
    return s.find_first_of("*?") != std::string::npos;
}

bool DiskEngine::splitPattern(const std::string& name, std::string& folder, std::string& last) const {
    size_t slash = name.rfind('/');
    last = slash == std::string::npos ? name : name.substr(slash + 1);
    std::string dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : name.substr(0, slash));
    if (hasWild(dir) || last.empty()) return false;     // wildcards in the last part only
    std::string path;
    if (!resolve(dir, path)) return false;
    struct stat st;
    if (stat(hostPath(path).c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    folder = path == "/" ? path : path + "/";
    return true;
}

std::vector<std::string> DiskEngine::matches(const std::string& folder, const std::string& pattern, bool filesOnly) {
    std::vector<std::string> out;
    std::string host = hostPath(folder);
    DIR* d = opendir(host.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.empty() || name[0] == '.' || !wildMatch(pattern.c_str(), name.c_str())) continue;
        struct stat st;
        if (stat((host + "/" + name).c_str(), &st) != 0) continue;
        if (filesOnly && S_ISDIR(st.st_mode)) continue;
        out.push_back(name);
        if (out.size() >= (size_t) MAX_ENTRIES) break;
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return lessNoCase(a.c_str(), b.c_str());
    });
    return out;
}

bool DiskEngine::inUse(const std::string& path) const {
    if (!tap_.empty() && equalNoCase(path, tap_)) return true;
    for (const Channel& c : chans_)
        if (c.f && equalNoCase(path, c.path)) return true;
    std::string p = path == "/" ? path : path + "/";
    return cwd_.size() >= p.size() && equalNoCase(cwd_.substr(0, p.size()), p);
}

std::string DiskEngine::text2(DiskCpu& cpu) {
    std::string s;
    uint16_t at = cpu.ix(), len = cpu.hl();
    for (uint16_t i = 0; i < len && i < 255; i++) s += (char) cpu.peek(at + i);
    return s;
}

uint8_t DiskEngine::moveTo(const std::string& name) {
    std::string target;
    if (name == "-") {
        if (prevCwd_.empty()) return OK;                // nowhere to go back to
        target = prevCwd_;
    } else {
        std::string path;
        if (name.empty()) return pickStart(PICK_FOLDER);
        if (hasWild(name) || !resolve(name, path)) return R_NAME;
        struct stat st;
        if (stat(hostPath(path).c_str(), &st) != 0) return R_NAME;
        if (!S_ISDIR(st.st_mode)) return R_PARAM;
        target = path == "/" ? path : path + "/";
    }
    if (target != cwd_) {
        prevCwd_ = cwd_;
        cwd_ = target;
        unmount();                                      // changing folder unmounts the TAP
    }
    return OK;
}

uint8_t DiskEngine::move(const std::string& from, const std::string& to) {
    if (hasWild(from) || hasWild(to)) return R_PARAM;
    std::string src, dst;
    if (from.empty() || to.empty() || !resolve(from, src) || !resolve(to, dst) || src == "/") return R_NAME;
    struct stat st;
    if (stat(hostPath(src).c_str(), &st) != 0) return R_NAME;
    bool srcDir = S_ISDIR(st.st_mode);
    if (inUse(src)) return R_PARAM;
    if (stat(hostPath(dst).c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode)) return R_NAME;        // never over a file
        dst = (dst == "/" ? "" : dst) + "/" + src.substr(src.rfind('/') + 1);
        if (stat(hostPath(dst).c_str(), &st) == 0) return R_NAME;
    } else {
        std::string folder = dst.substr(0, dst.rfind('/'));
        if (!folder.empty() && (stat(hostPath(folder).c_str(), &st) != 0 || !S_ISDIR(st.st_mode))) return R_NAME;
    }
    if (srcDir && dst.size() > src.size() && equalNoCase(dst.substr(0, src.size() + 1), src + "/")) return R_PARAM;
    return rename(hostPath(src).c_str(), hostPath(dst).c_str()) == 0 ? OK : R_IO;
}

namespace {
bool copyFile(const std::string& from, const std::string& to) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    std::vector<char> buf(4096);
    bool ok = true;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), in)) > 0)
        if (fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) remove(to.c_str());
    return ok;
}
} // namespace

uint8_t DiskEngine::copy(const std::string& from, const std::string& to, std::string& out) {
    out.clear();
    std::string dst;
    if (from.empty() || to.empty() || hasWild(to) || !resolve(to, dst)) return R_NAME;
    struct stat st;
    bool dstDir = stat(hostPath(dst).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    bool dstFile = !dstDir && stat(hostPath(dst).c_str(), &st) == 0;
    std::string dstFolder = dst == "/" ? "/" : dst + "/";

    if (hasWild(from)) {
        std::string folder, pattern;
        if (!splitPattern(from, folder, pattern)) return R_NAME;
        if (!dstDir) return R_PARAM;                    // a pattern copies into a folder
        std::vector<std::string> names = matches(folder, pattern, true);
        if (names.empty()) return R_NAME;
        for (const std::string& n : names) {
            std::string target = dstFolder + n;
            const char* what;
            if (stat(hostPath(target).c_str(), &st) == 0) what = "exists";
            else if (equalNoCase(folder + n, target)) what = "exists";
            else what = copyFile(hostPath(folder + n), hostPath(target)) ? "copied" : "failed";
            out += fit(printable(n), 24) + " " + what + "\r";
        }
        return OK;
    }
    std::string src;
    if (!resolve(from, src)) return R_NAME;
    if (stat(hostPath(src).c_str(), &st) != 0) return R_NAME;
    if (S_ISDIR(st.st_mode)) return R_PARAM;
    if (dstDir) dst = dstFolder + src.substr(src.rfind('/') + 1);
    else if (dstFile) return R_NAME;
    else {
        std::string folder = dst.substr(0, dst.rfind('/'));
        if (!folder.empty() && (stat(hostPath(folder).c_str(), &st) != 0 || !S_ISDIR(st.st_mode))) return R_NAME;
    }
    if (stat(hostPath(dst).c_str(), &st) == 0) return R_NAME;   // never over a file
    return copyFile(hostPath(src), hostPath(dst)) ? OK : R_IO;
}

void DiskEngine::askNextErase(std::string& out) {
    if (eraseNext_ >= eraseQueue_.size()) {
        eraseQueue_.clear();
        return;
    }
    std::string path = eraseQueue_[eraseNext_];
    out += "Erase " + fit(printable(path.substr(path.rfind('/') + 1)), 16) + " (Y/N)? ";
    out += (char) OUT_ASK;
}

uint8_t DiskEngine::erase(const std::string& name, std::string& out) {
    out.clear();
    eraseQueue_.clear();
    eraseNext_ = 0;
    if (name.empty()) return R_NAME;
    if (hasWild(name)) {
        std::string folder, pattern;
        if (!splitPattern(name, folder, pattern)) return R_NAME;
        for (const std::string& n : matches(folder, pattern, true)) eraseQueue_.push_back(folder + n);
        if (eraseQueue_.empty()) return R_NAME;
        askNextErase(out);
        return OK;
    }
    std::string path;
    bool folderWanted = name.back() == '/';
    if (!resolve(name, path) || path == "/") return R_NAME;
    struct stat st;
    if (stat(hostPath(path).c_str(), &st) != 0) return R_NAME;
    if (inUse(path)) return R_PARAM;
    if (S_ISDIR(st.st_mode)) {
        if (!folderWanted) return R_PARAM;              // a folder is ERASE "dir/"
        if (!matches(path + "/", "*", false).empty()) return R_PARAM;   // not empty
        return rmdir(hostPath(path).c_str()) == 0 ? OK : R_PARAM;
    }
    if (folderWanted) return R_NAME;
    return remove(hostPath(path).c_str()) == 0 ? OK : R_IO;
}

uint8_t DiskEngine::answer(int key, std::string& out) {
    out.clear();
    if (eraseNext_ >= eraseQueue_.size()) return OK;
    std::string path = eraseQueue_[eraseNext_++];
    if (key == 0) {                                     // N: stop
        out += "N\r";
        eraseQueue_.clear();
        return OK;
    }
    if (key == 1) {
        if (inUse(path)) out += "Y - in use\r";
        else if (remove(hostPath(path).c_str()) == 0) out += "Y\r";
        else out += "Y - failed\r";
    } else
        out += "-\r";                                   // any other key: skipped
    askNextErase(out);
    return OK;
}

uint8_t DiskEngine::list(const std::string& name, std::string& out) {
    out.clear();
    std::string path;
    if (name.empty() || !resolve(name, path)) return R_NAME;
    if (hasWild(name)) return R_PARAM;
    std::string host = hostPath(path);
    struct stat st;
    if (stat(host.c_str(), &st) != 0) return R_NAME;
    if (S_ISDIR(st.st_mode)) return R_PARAM;
    FILE* f = fopen(host.c_str(), "rb");
    if (!f) return R_IO;
    const size_t TEXT_MAX = 8192, DUMP_MAX = 2048;
    std::vector<uint8_t> data(TEXT_MAX);
    size_t n = fread(data.data(), 1, data.size(), f);
    fclose(f);
    data.resize(n);
    bool text = true;
    for (uint8_t c : data)
        if (!(c == '\r' || c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F))) { text = false; break; }
    char line[64];
    if (text) {
        for (size_t i = 0; i < data.size(); i++) {
            uint8_t c = data[i];
            if (c == '\r') { out += '\r'; if (i + 1 < data.size() && data[i + 1] == '\n') i++; }
            else if (c == '\n') out += '\r';
            else if (c == '\t') out += ' ';
            else out += (char) c;
        }
        if (!out.empty() && out.back() != '\r') out += '\r';
        if ((long) data.size() < st.st_size) {
            snprintf(line, sizeof line, "and %ld more bytes\r", (long) st.st_size - (long) data.size());
            out += line;
        }
        return OK;
    }
    size_t shown = std::min(data.size(), DUMP_MAX);
    for (size_t at = 0; at < shown; at += 8) {
        snprintf(line, sizeof line, "%04X ", (unsigned) at);
        out += line;
        std::string ascii;
        for (size_t k = 0; k < 8; k++) {
            if (at + k < shown) {
                snprintf(line, sizeof line, "%02X", data[at + k]);
                out += line;
                uint8_t c = data[at + k];
                ascii += (c >= 0x20 && c < 0x7F) ? (char) c : '.';
            } else
                out += "  ";
        }
        out += " " + ascii + "\r";
    }
    if ((long) shown < st.st_size) {
        snprintf(line, sizeof line, "and %ld more bytes\r", (long) st.st_size - (long) shown);
        out += line;
    }
    return OK;
}

uint8_t DiskEngine::format(const std::string& nameIn) {
    std::string name = nameIn, path;
    if (name.empty()) return R_NAME;
    struct stat st;
    if (name.back() == '/') {                           // a folder
        name.pop_back();
        if (name.empty() || hasWild(name) || !resolve(name, path) || path == "/") return R_NAME;
        if (stat(hostPath(path).c_str(), &st) == 0) return R_NAME;
        std::string folder = path.substr(0, path.rfind('/'));
        if (!folder.empty() && (stat(hostPath(folder).c_str(), &st) != 0 || !S_ISDIR(st.st_mode))) return R_NAME;
        return mkdir(hostPath(path).c_str(), 0755) == 0 ? OK : R_IO;
    }
    if (hasWild(name)) return R_NAME;
    std::string base = name.substr(name.rfind('/') + 1);
    if (!endsWithNoCase(base, ".tap")) {
        if (base.find('.') != std::string::npos) return R_PARAM;   // "notes.txt", "v1.2"
        name += ".tap";
    } else if (base.size() == 4) return R_NAME;         // only ".tap"
    if (!resolve(name, path)) return R_NAME;
    std::string host = hostPath(path);
    if (stat(host.c_str(), &st) == 0) return R_NAME;    // never over an existing file
    FILE* f = fopen(host.c_str(), "wb");
    if (!f) return R_IO;
    if (fclose(f) != 0) return R_IO;
    return mount(path);
}

uint8_t DiskEngine::service(uint8_t fn, DiskCpu& cpu) {
    switch (fn) {
        case SVC_CAT_OPEN: {
            listing_.clear();
            listingPos_ = 0;
            uint8_t r = catOpen(cpu);
            if (r != OK) listing_.clear();
            return r;
        }
        case SVC_CAT_CHAR:
            if (listingPos_ >= listing_.size()) return 0;
            return (uint8_t) listing_[listingPos_++];
        case SVC_TAPE_NAME:
            return tapeName(cpu);
        case SVC_R_TAPE:
            return readTape(cpu);
        case SVC_W_TAPE:
            return writeTape(cpu);
        case SVC_TAPE_OURS:
            return (!tap_.empty() || native_ == N_SAVE) ? 1 : 0;
        case SVC_REPLACE:
            return replace(cpu);
        case SVC_CLOSE_ALL:
            unmount();
            chCloseAll();
            return OK;
        case SVC_CH_OPEN: {
            int stream = cpu.peek(STREAM_N);
            chSweep(cpu, stream);
            int reclen = cpu.peek(CH_ARG) | (cpu.peek(CH_ARG + 1) << 8);
            return chOpen(stream, text(cpu), text2(cpu), reclen);
        }
        case SVC_CH_OUT: {
            uint16_t rec = cpu.peek(CURCHL) | (cpu.peek(CURCHL + 1) << 8);
            if (cpu.peek(rec + 4) == 'P') return prOut(cpu.de() & 0xFF);
            int stream = chStreamOf(cpu);
            if (stream < 0) return R_STREAM;
            return chOut(stream, cpu.de() & 0xFF);
        }
        case SVC_CH_IN: {
            int stream = chStreamOf(cpu);
            if (stream < 0) return R_STREAM;
            uint8_t c = 0;
            uint8_t r = chIn(stream, c);
            if (r == OK) cpu.poke(CH_BYTE, c);
            return r;
        }
        case SVC_CH_CLOSE:
            return chClose(cpu.hl() & 0xFF);
        case SVC_CH_RESTORE:
            return chRestore(cpu.hl() & 0xFF);
        case SVC_PICK_CHAR:
            if (pickOutPos_ >= pickOut_.size()) return PICK_DONE;
            return (uint8_t) pickOut_[pickOutPos_++];
        case SVC_PICK_KEY:
            return pickKey(cpu.de() & 0xFF, cpu.de() >> 8, cpu.hl() & 0xFF, cpu.hl() >> 8);
        case SVC_PICK_END:
            return pickEnd(cpu);
        case SVC_PR_COPY:
            prFlush();
            return prCopy(cpu);
        case SVC_CH_CAT:
            return chCat(cpu.ix() & 0xFF, cpu.hl() != 0, cpu.hl() ? text(cpu) : std::string());
        case SVC_FORMAT:
            return format(text(cpu));
        case SVC_MOVE_TO:
            return moveTo(text(cpu));
        case SVC_MOVE:
            return move(text(cpu), text2(cpu));
        case SVC_COPY:
        case SVC_ERASE:
        case SVC_LIST:
        case SVC_ANSWER: {
            std::string out;
            uint8_t r;
            if (fn == SVC_COPY) r = copy(text(cpu), text2(cpu), out);
            else if (fn == SVC_ERASE) r = erase(text(cpu), out);
            else if (fn == SVC_LIST) r = list(text(cpu), out);
            else r = answer(cpu.hl() & 0xFF, out);
            if (fn != SVC_ANSWER) {
                listing_.clear();
                listingPos_ = 0;
                endReport_ = OK;
            }
            if (r == OK) listing_ += out;
            return r;
        }
        case SVC_END: {
            uint8_t r = endReport_;
            endReport_ = OK;
            return r;
        }
        default:
            return R_UNKNOWN_FN;
    }
}

//------------------------------------------------------------------------------
// The file picker. Line 0 says what it is for and where; line 1 the keys;
// lines 2-20 the entries, the one picked in inverse. LOAD's list has the
// folders (to go into) and what LOAD takes (a tape, a cartridge, a +3DOS
// file, a screen); MOVE TO's has "." (this one) first, then the folders.
//------------------------------------------------------------------------------

namespace {

const char* const PICK_KEYS = "6/7 5/8 move  ENTER  SPACE quit";

} // namespace

uint8_t DiskEngine::pickStart(uint8_t command) {
    pickState_ = PK_OPEN;
    pickCommand_ = command;
    pickDir_ = cwd_;
    pickChosen_.clear();
    pickRead();
    pickPage();
    return T_PICK;
}

void DiskEngine::pickRead() {
    pickList_.clear();
    pickAt_ = pickTop_ = 0;
    bool folders = pickCommand_ == PICK_FOLDER;
    if (folders) pickList_.push_back({".", true, ""});
    if (pickDir_ != "/") pickList_.push_back({"..", true, ""});
    std::vector<PickEntry> dirs, files;
    std::string host = hostPath(pickDir_);
    if (DIR* d = opendir(host.c_str())) {
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.empty() || n[0] == '.') continue;
            struct stat st;
            if (stat((host + "/" + n).c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) dirs.push_back({n, true, ""});
            else if (!folders) {
                const char* kind = kindOf(host + "/" + n, (long) st.st_size);
                bool cart = !strcmp(kind, "DCK") || !strcmp(kind, "ROM");
                if (*kind && (!cart || pickCommand_ == 1)) files.push_back({n, false, kind});
            }
            if (dirs.size() + files.size() >= (size_t) MAX_ENTRIES) break;
        }
        closedir(d);
    }
    auto byName = [](const PickEntry& a, const PickEntry& b) { return lessNoCase(a.name.c_str(), b.name.c_str()); };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    pickList_.insert(pickList_.end(), dirs.begin(), dirs.end());
    pickList_.insert(pickList_.end(), files.begin(), files.end());
}

// An entry's line (row 0 the first under the keys), at its place.
void DiskEngine::pickLine(int row) {
    pickOut_ += (char) 0x16;
    pickOut_ += (char) (2 + row);
    pickOut_ += (char) 0;
    int i = pickTop_ + row;
    std::string line;
    if (i < (int) pickList_.size()) {
        const PickEntry& e = pickList_[i];
        if (e.folder && e.name == ".") line = " . (this folder)";
        else if (e.folder) line = " <" + fit(printable(e.name), 24) + ">";
        else {
            line = " " + fit(printable(e.name), 25);
            line.resize(27, ' ');
            line += e.kind;
        }
    }
    line.resize(32, ' ');
    bool on = i == pickAt_ && i < (int) pickList_.size();
    if (on) pickOut_ += "\x14\x01";
    pickOut_ += line;
    if (on) pickOut_ += std::string("\x14\x00", 2);
}

void DiskEngine::pickPage() {
    pickOut_.clear();
    pickOutPos_ = 0;
    std::string title = pickState_ != PK_OPEN ? ""
                      : (pickCommand_ == PICK_FOLDER ? "MOVE TO " : pickCommand_ == 2 ? "VERIFY "
                         : pickCommand_ == 3 ? "MERGE " : "LOAD ") + printable(pickDir_);
    std::string keys = pickState_ != PK_OPEN ? "" : PICK_KEYS;
    for (int y = 0; y < 2; y++) {
        std::string line = fit(y ? keys : title, 32);
        line.resize(32, ' ');
        pickOut_ += (char) 0x16;
        pickOut_ += (char) y;
        pickOut_ += (char) 0;
        pickOut_ += line;
    }
    if (pickState_ != PK_OPEN) pickList_.clear();       // done: a clear screen
    for (int r = 0; r < PICK_ROWS; r++) pickLine(r);
    if (pickState_ != PK_OPEN) pickOut_ += std::string("\x16\x00\x00", 3);   // what follows: at the top
}

uint8_t DiskEngine::pickKey(uint8_t rowEF, uint8_t rowF7, uint8_t rowBF, uint8_t row7F) {
    if (pickState_ != PK_OPEN) return 0;
    pickOut_.clear();
    pickOutPos_ = 0;
    int n = (int) pickList_.size(), was = pickAt_;
    if (!(row7F & 0x01)) {                              // SPACE (BREAK too)
        pickState_ = PK_QUIT;
        pickPage();
        return 1;
    }
    if (!(rowBF & 0x01)) {                              // ENTER
        if (pickAt_ >= n) return 0;
        const PickEntry& e = pickList_[pickAt_];
        if (e.name == ".") {
            pickChosen_ = pickDir_;
        } else if (e.name == "..") {
            std::string d = pickDir_.substr(0, pickDir_.size() - 1);
            d = d.substr(0, d.rfind('/') + 1);
            if (pickCommand_ == PICK_FOLDER) {
                pickChosen_ = d;                        // MOVE TO: a folder is the pick
                pickState_ = PK_PICKED;
                pickPage();
                return 1;
            }
            pickDir_ = d;
            pickRead();
            pickPage();
            return 1;
        } else if (e.folder && pickCommand_ != PICK_FOLDER) {
            pickDir_ += e.name + "/";
            pickRead();
            pickPage();
            return 1;
        } else
            pickChosen_ = pickDir_ + e.name;
        pickState_ = PK_PICKED;
        pickPage();
        return 1;
    }
    if (!(rowEF & 0x08)) pickAt_--;                     // 7: up
    else if (!(rowEF & 0x10)) pickAt_++;                // 6: down
    else if (!(rowF7 & 0x10)) pickAt_ -= PICK_ROWS;     // 5: a page back
    else if (!(rowEF & 0x04)) pickAt_ += PICK_ROWS;     // 8: a page on
    else return 0;
    if (pickAt_ >= n) pickAt_ = n - 1;
    if (pickAt_ < 0) pickAt_ = 0;
    if (pickAt_ == was) return 0;
    int top = pickTop_;
    if (pickAt_ < pickTop_) pickTop_ = pickAt_ / PICK_ROWS * PICK_ROWS;
    if (pickAt_ >= pickTop_ + PICK_ROWS) pickTop_ = pickAt_ / PICK_ROWS * PICK_ROWS;
    if (pickTop_ != top) {
        pickPage();
        return 1;
    }
    pickLine(was - pickTop_);                           // just the two lines
    pickLine(pickAt_ - pickTop_);
    return 1;
}

uint8_t DiskEngine::pickEnd(DiskCpu& cpu) {
    int state = pickState_;
    if (state == PK_OPEN) return T_PICK;
    pickState_ = PK_NONE;
    pickList_.clear();
    if (state == PK_QUIT) return R_BREAK;
    if (state != PK_PICKED) return R_IO;
    if (pickCommand_ == PICK_FOLDER) return moveTo(pickChosen_);
    return tapeNamed(cpu, pickCommand_, pickChosen_);
}

std::string DiskEngine::pickText() {
    std::string s = pickOut_.substr(pickOutPos_);
    pickOutPos_ = pickOut_.size();
    return s;
}
