// DiskEngine: the native disk's service calls. See include/DiskEngine.h.

#include "DiskEngine.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
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

} // namespace

DiskEngine::DiskEngine(const std::string& root) : root_(root) {}

void DiskEngine::reset() {
    cwd_ = "/";
    listing_.clear();
    listingPos_ = 0;
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
            snprintf(line, sizeof line, "%3d %-20s %6s", files, fit(name, 20).c_str(), sizeText(e.size).c_str());
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

uint8_t DiskEngine::service(uint8_t fn, DiskCpu& cpu) {
    switch (fn) {
        case SVC_CAT_OPEN: {
            listing_.clear();
            listingPos_ = 0;
            if (cpu.bc() != 0) return R_NAME;   // CAT "x": a later phase
            return buildListing(cwd_, listing_);
        }
        case SVC_CAT_CHAR:
            if (listingPos_ >= listing_.size()) return 0;
            return (uint8_t) listing_[listingPos_++];
        default:
            return R_UNKNOWN_FN;
    }
}
