/*

ESP2068 -- TS2068 port of ESPectrum

DiskEngine.h -- native disk for the TS2068: the engine behind the DOS
ROM's service calls. New for this port. See DISK_PROPOSAL.md in the
ts2068-esp32 repository for the design, and dos/dos.asm for the ROM side.

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
    // The SCLD's video mode (port FFh, bits 2-0), for COPY.
    virtual uint8_t video() { return 0; }
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
    // Phase 5's file channels. OPEN #n,"f:name" puts a 6-byte record in
    // CHANS (output and input: HOME's stubs at CH_OUT_STUB/CH_IN_STUB;
    // 'F'; the stream), and the engine keeps the file. The stubs call
    // CH_OUT and CH_IN, which find the stream through CURCHL.
    static const uint8_t SVC_CH_OPEN = 0x11;    // DE/BC the spec ("f:name"), IX/HL the mode (HL 0:
                                                //   none); (STREAM_N) the stream, (CH_ARG) the
                                                //   record length (0: none)
    static const uint8_t SVC_CH_OUT = 0x12;     // E = a byte for CURCHL's channel
    static const uint8_t SVC_CH_IN = 0x13;      // CURCHL's next byte: OK with it at CH_BYTE, CH_EOF
    static const uint8_t SVC_CH_CLOSE = 0x14;   // L = the stream
    static const uint8_t SVC_CH_RESTORE = 0x15; // L = the stream: back to its start
    static const uint8_t SVC_CH_CAT = 0x16;     // CAT #s: HL, DE/BC as CAT_OPEN; IX = the stream
    static const uint8_t CH_EOF = 1;            // CH_IN: the end of the file (Report 8)
    // The printer. HOME's P record has CH_OUT as its output routine, so
    // LPRINT and LLIST come through SVC_CH_OUT as text for PRINT_TEXT; a
    // bare COPY is SVC_PR_COPY, the screen as the printer would print it
    // (the top 22 lines, one bit a dot) to copyNNN.bmp.
    static const uint8_t SVC_PR_COPY = 0x17;
    // The file picker: LOAD/VERIFY/MERGE "" with nothing mounted, and
    // MOVE TO "". TAPE_NAME or MOVE_TO answers T_PICK; then the ROM prints
    // PICK_CHAR's bytes (RST 10h, the upper screen) up to FFh, asks
    // PICK_END (T_PICK: still picking; anything else is the command's
    // answer, as TAPE_NAME's or MOVE_TO's would have been), and while
    // picking reads a key: PICK_KEY with E/D/L/H the keyboard rows EFh, F7h,
    // BFh, 7Fh answers 0 (nothing to do: the next key) or 1 (print again).
    static const uint8_t SVC_PICK_CHAR = 0x18;
    static const uint8_t SVC_PICK_KEY = 0x19;
    static const uint8_t SVC_PICK_END = 0x1A;
    static const uint8_t T_PICK = 0xFE;
    static const uint8_t PICK_DONE = 0xFF;      // PICK_CHAR: the end of what to print

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
    static const uint16_t CH_ARG = 0x5E0C;      // OPEN #'s record length, 2 bytes
    static const uint16_t CH_BYTE = 0x5E0E;     // CH_IN's byte
    static const uint16_t STREAM_N = 0x5CCB;    // the stream OPEN #/CLOSE # work on (HOME 140Fh)
    static const uint16_t CURCHL = 0x5C51;
    static const uint16_t CHANS = 0x5C4F;
    static const uint16_t STRMS = 0x5C10;       // streams -3..15, 2 bytes each
    static const int STREAMS = 16;

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
    static const uint8_t R_MEMORY = 0x03;       // 4 Out of memory
    static const uint8_t R_EOF = 0x07;          // 8 End of file
    static const uint8_t R_STREAM = 0x17;       // O Invalid stream: not a file channel
    static const uint8_t R_END = 0xFF;           // "0 OK": the statement ends here (a dock)

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
    // What CAT [name] lists: the folder (no name), a mounted tape (""), a
    // tape's blocks, a folder or a pattern.
    uint8_t catInto(bool arg, const std::string& name, std::string& out);

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

    // File channels, by stream (0-15); for the tests (service() calls
    // them). chIn answers OK with the byte in `c`, CH_EOF, or a report.
    uint8_t chOpen(int stream, const std::string& spec, const std::string& mode, int reclen);
    uint8_t chOut(int stream, uint8_t c);
    uint8_t chIn(int stream, uint8_t& c);
    uint8_t chClose(int stream);
    void chCloseAll();
    uint8_t chRestore(int stream);
    uint8_t chCat(int stream, bool arg, const std::string& name);
    bool chIsOpen(int stream) const { return stream >= 0 && stream < STREAMS && chans_[stream].f; }

    // The printer, for the tests (service() calls them). prOut takes a
    // byte LPRINT or LLIST sent; prFlush writes what is waiting (a line not
    // yet ended) to the file, between frames; prCopy writes the screen.
    uint8_t prOut(uint8_t b);
    uint8_t prFlush();
    // Once a frame: the text waiting goes to the file after half a second
    // with nothing more for the printer.
    void prTick();
    uint8_t prCopy(DiskCpu& cpu, std::string* written = nullptr);

    // The picker, for the tests (service() calls them). pickStart answers
    // T_PICK; command is LOAD's (1-3) or PICK_FOLDER for MOVE TO.
    static const uint8_t PICK_FOLDER = 0xFF;
    uint8_t pickStart(uint8_t command);
    uint8_t pickKey(uint8_t rowEF, uint8_t rowF7, uint8_t rowBF, uint8_t row7F);
    uint8_t pickEnd(DiskCpu& cpu);
    std::string pickText();                     // what PICK_CHAR hands out, all of it
    bool picking() const { return pickState_ == PK_OPEN; }
    static const int PICK_ROWS = 19;            // lines 2-20
    static const int PR_COLUMNS = 32;

    // LOAD "x.dck" / LOAD "x.rom": the cartridge is checked now and docked
    // by the firmware between frames, with a reset (takeDock). A .rom is
    // a raw cartridge image: its 8K chunks go in the DOCK from chunk 0.
    // Answers R_END (the statement ends), or a report.
    uint8_t dock(const std::string& name);
    // The cartridge waiting to be docked, as .DCK bytes; false if none.
    bool takeDock(std::vector<uint8_t>& out);
    // Whether .DCK bytes are whole (every header, every chunk image).
    static bool dckValid(const uint8_t* data, size_t size);

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

    // An open file channel.
    struct Channel {
        FILE* f = nullptr;
        std::string path;          // 2068 path
        char mode = 0;             // r w a u
        bool binary = false;
        int reclen = 0;            // 0: a stream (TAB n is byte n)
        long pos = 0;              // a stream: where the next read or write goes
        long filePos = -1;         // where the FILE is (-1: unknown)
        long size = -1;            // the file's size (-1: not yet asked)
        bool lastWrite = false;
        int ctl = 0;               // parameter bytes still to come for ctlCode
        uint8_t ctlCode = 0, ctlLo = 0;
        int column = 0;            // text: for the comma
        bool suppress = true;      // text: no space before the next keyword
        long record = 1;           // a record file: the next record
        bool recOpen = false;      // being written
        std::string recBuf;        // what it has so far
        bool query = false;        // TAB 0: the next read is the count
        std::string pending;       // bytes CH_IN hands out first
        size_t pendingPos = 0;
        bool lastInCR = true;      // text: the last byte read ended a line
    };
    Channel chans_[STREAMS];
    // The printer: the text not yet in the file, its column on the paper,
    // and a control code's parameters still to come.
    struct PickEntry {
        std::string name;
        bool folder;
        const char* kind;
    };
    enum { PK_NONE, PK_OPEN, PK_PICKED, PK_QUIT };
    int pickState_ = PK_NONE;
    uint8_t pickCommand_ = 0;
    std::string pickDir_;          // the folder being shown ("/" or "/x/")
    std::vector<PickEntry> pickList_;
    int pickAt_ = 0, pickTop_ = 0;
    std::string pickOut_;
    size_t pickOutPos_ = 0;
    std::string pickChosen_;       // the 2068 path picked
    void pickRead();
    void pickLine(int row);
    void pickPage();
    std::string prText_;
    int prColumn_ = 0;
    int prQuiet_ = 0;              // frames since the last byte
    bool prSuppress_ = true;
    int prCtl_ = 0;
    uint8_t prCtlCode_ = 0, prCtlLo_ = 0;
    uint8_t prPut(const char* utf8);
    uint8_t prTo(int column);
    bool prFolder();
    std::vector<uint8_t> dock_;   // a cartridge for takeDock
    uint8_t chWrite(Channel& c, const char* bytes, size_t n);
    uint8_t chWriteAt(Channel& c, long at, const char* bytes, size_t n);
    uint8_t chPut(Channel& c, uint8_t b);
    uint8_t chSeek(Channel& c, long n);
    uint8_t chEndRecord(Channel& c);
    uint8_t chToken(Channel& c, uint8_t code);
    bool chAt(Channel& c, long p, bool write);
    long chSize(Channel& c);
    void chSweep(DiskCpu& cpu, int except);
    int chStreamOf(DiskCpu& cpu);

    std::string hostPath(const std::string& path2068) const;
    static const char* kindOf(const std::string& host, long size);
    // The blocks of a TAP or (by its name) a TZX; *skipped counts a TZX's
    // data blocks the ROM can't load (turbo, pure data, direct recording).
    static bool scanTap(const std::string& host, std::vector<Block>& out, unsigned* skipped = nullptr);
    static bool scanTzx(FILE* f, long size, std::vector<Block>& out, unsigned& skipped);
    static std::string text(DiskCpu& cpu);   // DE/BC
    uint8_t readTape(DiskCpu& cpu);
    uint8_t writeTape(DiskCpu& cpu);
    uint8_t tapeName(DiskCpu& cpu);
    uint8_t tapeNamed(DiskCpu& cpu, uint8_t command, const std::string& name);
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
