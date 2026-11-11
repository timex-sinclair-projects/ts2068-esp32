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
    static const uint8_t SVC_TAPE_MOUNTED = 0x06; // 1 if a TAP is mounted
    static const uint8_t SVC_CLOSE_ALL = 0x07;  // CLOSE #*: unmount the TAP
    static const uint8_t SVC_FORMAT = 0x08;     // DE/BC: a new, empty TAP, mounted

    // The tape functions' answers, below every report code.
    static const uint8_t T_NOT_OURS = 0;        // nothing mounted: the stock tape routine runs
    static const uint8_t T_DONE = 1;            // R_TAPE/W_TAPE: done; TAPE_NAME: a TAP was
                                                //   mounted, load its first program
    static const uint8_t T_FAILED = 2;          // R_TAPE: a tape loading error

    // Answers: 0, or the RST 8 code of the report the ROM raises (the
    // report is the code + 1: 0Bh is C, 0Eh is F).
    static const uint8_t OK = 0x00;
    static const uint8_t R_NONSENSE = 0x0B;     // C Nonsense in BASIC
    static const uint8_t R_NAME = 0x0E;         // F Invalid file name
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
    // the tests. Returns OK or a report code.
    uint8_t buildListing(const std::string& folder, std::string& out);
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

    std::string tap_;             // the mounted TAP, 2068 path; "" if none
    std::vector<Block> blocks_;
    size_t tapPos_ = 0;           // the next block R_TAPE looks at
    size_t examined_ = 0;         // blocks R_TAPE has passed over since the name

    std::string hostPath(const std::string& path2068) const;
    static bool scanTap(const std::string& host, std::vector<Block>& out);
    static std::string text(DiskCpu& cpu);   // DE/BC
    uint8_t readTape(DiskCpu& cpu);
    uint8_t writeTape(DiskCpu& cpu);
    uint8_t tapeName(DiskCpu& cpu);
    uint8_t format(DiskCpu& cpu);
    uint8_t catOpen(DiskCpu& cpu);
};

#endif
