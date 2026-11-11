// DiskEngine: the native disk's service calls. See include/DiskEngine.h.

#include "DiskEngine.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

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

const char* const HEADER_TYPES[4] = {"Prog", "Num()", "Chr()", "Code"};

} // namespace

DiskEngine::DiskEngine(const std::string& root) : root_(root) {}

void DiskEngine::reset() {
    cwd_ = "/";
    listing_.clear();
    listingPos_ = 0;
    unmount();
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

bool DiskEngine::scanTap(const std::string& host, std::vector<Block>& out) {
    out.clear();
    FILE* f = fopen(host.c_str(), "rb");
    if (!f) return false;
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

uint8_t DiskEngine::buildListing(const std::string& folder, std::string& out) {
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
    out.reserve(LISTING_RESERVE);
    out += fit(printable(folder), 31) + "\r";
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
            snprintf(line, sizeof line, "%3d%c%-20s %6s", files, mark, fit(name, 20).c_str(), sizeText(e.size).c_str());
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
    std::string host = hostPath(path2068);
    struct stat st;
    if (stat(host.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return R_NAME;
    if (!scanTap(host, blocks)) return R_IO;
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
    snprintf(line, sizeof line, "%u block%s%s", (unsigned) blocks.size(), blocks.size() == 1 ? "" : "s",
             isMounted ? ", mounted" : "");
    out += line;
    out += "\r";
    return OK;
}

uint8_t DiskEngine::catOpen(DiskCpu& cpu) {
    if (cpu.hl() == 0) return buildListing(cwd_, listing_);
    std::string name = text(cpu), path;
    if (name.empty()) {
        if (tap_.empty()) return R_IO;
        return buildTapListing(tap_, listing_);
    }
    if (!resolve(name, path)) return R_NAME;
    struct stat st;
    if (stat(hostPath(path).c_str(), &st) != 0) return R_NAME;
    if (S_ISDIR(st.st_mode)) return buildListing(path == "/" ? path : path + "/", listing_);
    if (endsWithNoCase(path, ".tap")) return buildTapListing(path, listing_);
    return R_NAME;
}

uint8_t DiskEngine::tapeName(DiskCpu& cpu) {
    examined_ = 0;
    if (!(cpu.peek(FLAGS) & 0x80)) return T_NOT_OURS;   // the syntax pass
    uint8_t command = cpu.peek(T_ADDR);
    std::string name = text(cpu), path;
    // LOAD/VERIFY/MERGE "x.tap": mount it (it replaces any TAP mounted) and
    // take its first program. SAVE "x.tap" is a block name like any other.
    if (command >= 1 && command <= 3 && endsWithNoCase(name, ".tap") && resolve(name, path)) {
        struct stat st;
        if (stat(hostPath(path).c_str(), &st) == 0 && !S_ISDIR(st.st_mode)) {
            uint8_t r = mount(path);
            return r == OK ? T_DONE : r;
        }
    }
    // With a TAP mounted, everything goes to it; without one, the stock
    // tape routines run (the emulator's tape player).
    return T_NOT_OURS;
}

uint8_t DiskEngine::readTape(DiskCpu& cpu) {
    if (tap_.empty()) return T_NOT_OURS;
    uint16_t sp = cpu.sp();
    bool load = cpu.peek(sp) & 0x01;                    // the pushed F: carry
    uint8_t flag = cpu.peek(sp + 1);                    // the pushed A
    uint16_t ix = cpu.ix(), de = cpu.de();

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
    uint8_t x = 0;
    for (uint8_t c : data) x ^= c;
    if (x != 0) return T_FAILED;
    for (uint16_t i = 0; i < de; i++) {
        uint8_t c = data[1 + i];
        if (load) cpu.poke(ix + i, c);
        else if (cpu.peek(ix + i) != c) return T_FAILED;
    }
    return T_DONE;
}

uint8_t DiskEngine::writeTape(DiskCpu& cpu) {
    if (tap_.empty()) return T_NOT_OURS;
    uint16_t sp = cpu.sp();
    uint8_t flag = cpu.peek(sp + 1);
    uint16_t ix = cpu.ix(), de = cpu.de();
    if ((uint32_t) de + 2 > 0xFFFF) return R_IO;
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

uint8_t DiskEngine::format(DiskCpu& cpu) {
    std::string name = text(cpu), path;
    if (name.empty() || name.back() == '/') return R_NAME;
    if (!endsWithNoCase(name, ".tap")) name += ".tap";
    if (!resolve(name, path)) return R_NAME;
    std::string host = hostPath(path);
    struct stat st;
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
        case SVC_TAPE_MOUNTED:
            return tap_.empty() ? 0 : 1;
        case SVC_CLOSE_ALL:
            unmount();
            return OK;
        case SVC_FORMAT:
            return format(cpu);
        default:
            return R_UNKNOWN_FN;
    }
}
