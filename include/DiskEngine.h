/*

ESP2068 -- TS2068 port of ESPectrum

DiskEngine.h -- native disk for the TS2068: the engine behind the DOS
ROM's service calls. New for this port. See DISK_PROPOSAL.md in the
ts2068-2350 repository for the design, and dos/dos.asm for the ROM side.

The ROM side is the stock HOME ROM with a few patches (include/DosRom.h)
and a 16K DOS ROM the emulator overlays on Z80 0000h-3FFFh while the
module runs. Three ports, decoded on their low byte like every other 2068
port:

  OUT (D1h),A   a service call: A = the function, arguments in BC, DE,
                HL; the answer comes back in A. It runs synchronously,
                inside the OUT.
  OUT (D3h),A   the DOS ROM appears at 0000h-3FFFh (A is ignored)
  OUT (D5h),A   HOME is back

This class is the pure, host-testable core. It reaches the Z80 only
through DiskCpu, so test/host/diskengine_test.cpp drives it with a fake
CPU and a temporary folder; src/DosDisk.cpp is the firmware's glue.

Based on ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC
Copyright (c) 2023-2025 Víctor Iborra [Eremus] and 2023 David Crespo [dcrespo3d]
https://github.com/EremusOne/ESPectrum

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/

#ifndef DiskEngine_h
#define DiskEngine_h

#include <inttypes.h>
#include <string>
#include <vector>

// What a service call can see of the Z80: its registers at the OUT, and
// memory as currently mapped (the DOS ROM below 4000h, RAM above).
class DiskCpu {
public:
    virtual ~DiskCpu() {}
    virtual uint16_t bc() = 0;
    virtual uint16_t de() = 0;
    virtual uint16_t hl() = 0;
    virtual uint16_t ix() = 0;
    virtual uint16_t sp() = 0;
    virtual uint8_t peek(uint16_t addr) = 0;
    virtual void poke(uint16_t addr, uint8_t value) = 0;
};

class DiskEngine {
public:
    // Ports.
    static const uint8_t PORT_SVC = 0xD1;
    static const uint8_t PORT_DOS_ON = 0xD3;
    static const uint8_t PORT_DOS_OFF = 0xD5;

    // Service functions (A at OUT (PORT_SVC),A).
    // dos/dos.asm has the same list. The tape functions are called from the
    // EXROM's hooks (TAPE_NAME, R_TAPE, W_TAPE, TAPE_MOUNTED) and from the DOS
    // ROM (CLOSE_ALL, FORMAT).
    static const uint8_t SVC_CAT_OPEN = 0x01;   // HL = 0: the current folder; HL = 1: BC = length,
                                                //   DE = text: "" the mounted TAP, x.tap, a folder
    static const uint8_t SVC_CAT_CHAR = 0x02;   // A = next character of the listing, 0 at the end
    static const uint8_t SVC_TAPE_NAME = 0x03;  // SLVM's name, DE/BC; (T_ADDR) the command
    static const uint8_t SVC_R_TAPE = 0x04;     // (SP) = R_TAPE's AF: A the flag, carry LOAD;
                                                //   IX = where, DE = how many
    static const uint8_t SVC_W_TAPE = 0x05;     // (SP) = W_TAPE's AF: A the flag; IX, DE
    static const uint8_t SVC_TAPE_OURS = 0x06;  // 1 if SAVE goes to the engine (a TAP, a file)
    static const uint8_t SVC_CLOSE_ALL = 0x07;  // CLOSE #*: unmount the TAP
    static const uint8_t SVC_FORMAT = 0x08;     // DE/BC: a new, empty TAP, mounted
    static const uint8_t SVC_REPLACE = 0x09;    // L = 1: Y to "Replace?"; 0: N (answer D)
    // Phase 4's commands. Each may leave text for CAT_CHAR to hand out, as
    // CAT_OPEN does; a 1 in it (OUT_ASK) is a question: the ROM reads a key
    // and calls ANSWER. At the end (CAT_CHAR's 0) the ROM calls END for the
    // report, if any.
    static const uint8_t SVC_MOVE_TO = 0x0A;    // DE/BC: change folder ("-" the previous one)
    static const uint8_t SVC_MOVE = 0x0B;       // DE/BC TO IX/HL: rename, or into a folder
    static const uint8_t SVC_COPY = 0x0C;       // DE/BC TO IX/HL: copy (a pattern: into a folder)
    static const uint8_t SVC_ERASE = 0x0D;      // DE/BC: delete (a pattern: asks each)
    static const uint8_t SVC_LIST = 0x0E;       // DE/BC: show a file
    static const uint8_t SVC_ANSWER = 0x0F;     // L = 1 Y, 0 N (stop), 2 any other key
    static const uint8_t SVC_END = 0x10;        // the report the text ends with, or OK
    static const uint8_t OUT_ASK = 0x01;        // in the text: a question, read a key

    // The tape functions' answers, below every report code.
    static const uint8_t T_NOT_OURS = 0;        // nothing mounted: the stock tape routine runs
    static const uint8_t T_DONE = 1;            // R_TAPE/W_TAPE: done; TAPE_NAME: a TAP was
                                                //   mounted, load its first program
    static const uint8_t T_FAILED = 2;          // R_TAPE: a tape loading error
    static const uint8_t T_NATIVE = 2;          // TAPE_NAME: a native file; SLVM takes the
                                                //   name at NAME_BUF (NAME_LEN bytes)
    static const uint8_t T_ASK = 3;             // TAPE_NAME: the same, once the question
                                                //   at PROMPT_BUF is answered Y

    // Where TAPE_NAME leaves things for the EXROM hooks, in RAM the OS
    // leaves free (5CCCh-5EE9h).
    static const uint16_t NAME_LEN = 0x5E0F;
    static const uint16_t NAME_BUF = 0x5E10;    // 10 bytes
    static const uint16_t PROMPT_BUF = 0x5E20;  // up to 64 bytes, 0-ended

    // Answers: 0, or the RST 8 code of the report the ROM raises (the
    // report is the code + 1: 0Bh is C, 0Eh is F).
    static const uint8_t OK = 0x00;
    static const uint8_t R_NONSENSE = 0x0B;     // C Nonsense in BASIC
    static const uint8_t R_BREAK = 0x0C;        // D BREAK - CONT repeats: N to "Replace?"
    static const uint8_t R_NAME = 0x0E;         // F Invalid file name: not found, a bad name
    static const uint8_t R_PARAM = 0x19;        // Q Parameter error: the wrong type, a folder
    static const uint8_t R_IO = 0x12;           // J Invalid I/O device: no card, a read failed,
                                                //   CAT "" with nothing mounted
    static const uint8_t R_UNKNOWN_FN = 0x0B;   // a function this engine doesn't know

    // The most entries a CAT listing shows; the rest are counted ("and 12
    // more").
    static const int MAX_ENTRIES = 256;

    // `root` is the host folder the 2068 sees as "/" ("/sd/2068" on the
    // board). Nothing above it is reachable.
    explicit DiskEngine(const std::string& root = "");
    void setRoot(const std::string& root) { root_ = root; }

    // A service call: returns the new A.
    uint8_t service(uint8_t fn, DiskCpu& cpu);

    // The machine was reset.
    void reset();

    // The current folder, as the 2068 sees it ("/" or "/games/").
    const std::string& cwd() const { return cwd_; }

    // The mounted TAP, as the 2068 sees it ("/games/x.tap"), or "".
    const std::string& mounted() const { return tap_; }
    // The index of the block the next R_TAPE looks at first.
    size_t tapPointer() const { return tapPos_; }
    size_t tapBlocks() const { return blocks_.size(); }

    // Mount or unmount a TAP (a 2068 path); for the tests and the OSD.
    uint8_t mount(const std::string& path2068);
    void unmount();

    // The listing CAT prints for a folder, lines ending in CR (0Dh); for
    // the tests. Returns OK or a report code. With a pattern ("*.tap"),
    // only the names that match it; none is F.
    uint8_t buildListing(const std::string& folder, std::string& out, const std::string& pattern = "");

    // Wildcards: * any run of characters, ? any one; case ignored.
    static bool wildMatch(const char* pattern, const char* name);
    static bool hasWild(const std::string& s);

    // The commands, for the tests (service() calls them with the CPU's
    // strings). Text for the screen goes to `out`.
    uint8_t moveTo(const std::string& name);
    uint8_t move(const std::string& from, const std::string& to);
    uint8_t copy(const std::string& from, const std::string& to, std::string& out);
    uint8_t erase(const std::string& name, std::string& out);
    uint8_t answer(int key, std::string& out);   // 1 Y, 0 N, 2 another key
    uint8_t list(const std::string& name, std::string& out);
    uint8_t format(const std::string& name);
    // A native file's contents, from its first 128 bytes and its size:
    // false if it is neither a +3DOS file nor a raw 6912-byte screen.
    struct FileInfo {
        uint8_t type;      // 0 program, 1 number array, 2 character array, 3 bytes
        uint16_t length, param1, param2;
        uint32_t dataOffset;
    };
    static bool describe(const uint8_t* head, size_t headLen, long size, FileInfo& out);
    // The 128-byte +3DOS header for a data block (TS-Pico's native.py).
    static std::string plus3Header(uint8_t type, uint16_t length, uint16_t param1, uint16_t param2);
    // The tape name of a native file: its base name, the extension dropped,
    // cut to 10.
    static std::string tapeNameOf(const std::string& path);

    // The listing CAT "x.tap" and CAT "" print: the TAP's blocks.
    uint8_t buildTapListing(const std::string& path2068, std::string& out);

    // A name the 2068 gave, relative to the current folder, as a clean
    // absolute 2068 path ("/games/x.tap", a folder ending in '/'). False if
    // it's empty or climbs above "/".
    bool resolve(const std::string& name, std::string& path2068) const;

private:
    struct Block {
        uint32_t offset;   // in the file: the flag byte
        uint16_t len;      // flag + data + checksum
        uint8_t flag;
    };

    std::string root_;
    std::string cwd_ = "/";
    std::string listing_;
    size_t listingPos_ = 0;

    std::string prevCwd_;         // MOVE TO "-"
    std::vector<std::string> eraseQueue_;   // ERASE "pattern": the matches still to ask about
    size_t eraseNext_ = 0;
    uint8_t endReport_ = 0;       // what END answers

    std::string tap_;             // the mounted TAP, 2068 path; "" if none
    std::vector<Block> blocks_;
    size_t tapPos_ = 0;           // the next block R_TAPE looks at
    size_t examined_ = 0;         // blocks R_TAPE has passed over since the name

    // A native file, between TAPE_NAME and the blocks.
    enum Native { N_NONE, N_LOAD, N_ASK, N_SAVE };
    Native native_ = N_NONE;
    std::string nativePath_;               // 2068 path
    std::vector<std::string> nativeBlocks_; // LOAD: flag, data, checksum
    size_t nativePos_ = 0;
    std::string saveHeader_;               // SAVE: the 17-byte tape header

    std::string hostPath(const std::string& path2068) const;
    static const char* kindOf(const std::string& host, long size);
    static bool scanTap(const std::string& host, std::vector<Block>& out);
    static std::string text(DiskCpu& cpu);   // DE/BC
    uint8_t readTape(DiskCpu& cpu);
    uint8_t writeTape(DiskCpu& cpu);
    uint8_t tapeName(DiskCpu& cpu);
    uint8_t loadNative(DiskCpu& cpu, uint8_t command, const std::string& name);
    uint8_t saveNative(DiskCpu& cpu, const std::string& name);
    uint8_t replace(DiskCpu& cpu);
    void dropNative();
    static uint8_t serve(DiskCpu& cpu, const uint8_t* block, size_t len, bool load, uint16_t ix, uint16_t de);
    static std::string text2(DiskCpu& cpu);  // IX/HL
    void askNextErase(std::string& out);
    // Names in a folder (2068 path) that match a pattern, sorted; files only
    // if filesOnly.
    std::vector<std::string> matches(const std::string& folder, const std::string& pattern, bool filesOnly);
    // A 2068 path that is the current folder or holds it, or the mounted TAP.
    bool inUse(const std::string& path) const;
    // Splits "dir/pat" into the folder (2068 path, ending '/') and the last part.
    bool splitPattern(const std::string& name, std::string& folder, std::string& last) const;
    uint8_t catOpen(DiskCpu& cpu);
};

#endif
