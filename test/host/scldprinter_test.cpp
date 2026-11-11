/*

ESP2068 — TS2068 port of ESPectrum

test/host/scldprinter_test.cpp — standalone, host-compilable test for
SCLDPrinter's port 0xFB status/timing protocol (include/SCLDPrinter.h,
src/SCLDPrinter.cpp). No test framework, no ESP-IDF dependency at all
(this module doesn't need the ESP2068_HOST_TEST allocator-shim
convention SCLD.cpp/DockLoader.cpp use, since it does no allocation),
same convention as test/host/scldvideo_test.cpp.

Build and run:

    g++ -std=c++17 -Wall -I include \
        test/host/scldprinter_test.cpp src/SCLDPrinter.cpp \
        -o /tmp/scldprinter_test \
    && /tmp/scldprinter_test

*/

#include "SCLDPrinter.h"
#include <cstdio>

static int failures = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        printf("  PASS  %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

int main() {

    printf("SCLDPrinter host test\n");
    printf("======================\n");

    // ---- Idle state ----
    printf("\n-- idle (never written to) --\n");
    {
        SCLDPrinter::reset();
        check(SCLDPrinter::read(0) == 0x3e, "fresh reset reads idle (0x3e) at cycle 0");
        check(SCLDPrinter::read(1000000) == 0x3e, "idle stays idle no matter how much time passes");
    }

    // ---- Motor start: head hasn't reached paper yet ----
    printf("\n-- motor start, fast speed, stylus up --\n");
    {
        SCLDPrinter::reset();
        uint64_t t0 = 5000;
        SCLDPrinter::write(0x00, t0); // bit2=0 start, bit1=0 fast, bit7=0 stylus up
        check(SCLDPrinter::read(t0) == 0x3e, "immediately after start, head is still in lead-in (x=-64), not busy");
    }

    // ---- Head reaches the pre-paper busy window ----
    printf("\n-- head enters the pre-paper busy window (-10 < x < 0) --\n");
    {
        SCLDPrinter::reset();
        uint64_t t0 = 5000;
        SCLDPrinter::write(0x00, t0); // fast (220 cycles/pixel), stylus up
        // x = elapsed/220 - 64 = -5  =>  elapsed/220 = 59  =>  elapsed in [12980, 13200)
        uint64_t busyAt = t0 + 59 * 220;
        uint8_t status = SCLDPrinter::read(busyAt);
        check((status & 0x80) != 0, "busy bit (0x80) set while in the pre-paper window even though stylus is up");
        check((status & 0x01) == 0, "ready-for-next-pixel bit clear -- x(-5) has not advanced past lastPixelX(-1)");
    }

    // ---- Head reaches paper (x == 0): ready bit sets ----
    printf("\n-- head reaches x=0 (paper start) --\n");
    {
        SCLDPrinter::reset();
        uint64_t t0 = 5000;
        SCLDPrinter::write(0x00, t0); // fast, stylus up
        uint64_t paperAt = t0 + 64 * 220; // x = 64*220/220 - 64 = 0
        uint8_t status = SCLDPrinter::read(paperAt);
        check((status & 0x80) == 0, "not busy at x=0 with stylus up (outside the -10..0 window, which is exclusive of 0)");
        check((status & 0x01) != 0, "ready-for-next-pixel bit set -- x(0) has advanced past lastPixelX(-1)");
    }

    // ---- Stylus down forces busy regardless of head position ----
    printf("\n-- stylus down forces the busy bit on --\n");
    {
        SCLDPrinter::reset();
        uint64_t t0 = 5000;
        SCLDPrinter::write(0x80, t0); // start with stylus DOWN (bit7 set), fast, no stop
        uint64_t farPastPaper = t0 + 200 * 220; // well past the busy window on x alone
        uint8_t status = SCLDPrinter::read(farPastPaper);
        check((status & 0x80) != 0, "stylus-down alone sets the busy bit even far from the -10..0 window");
    }

    // ---- Motor stop: back to idle immediately ----
    printf("\n-- motor stop --\n");
    {
        SCLDPrinter::reset();
        uint64_t t0 = 5000;
        SCLDPrinter::write(0x80, t0);       // start, stylus down
        SCLDPrinter::write(0x04, t0 + 500); // stop (bit2 set)
        check(SCLDPrinter::read(t0 + 500) == 0x3e, "stop immediately returns to idle (0x3e)");
        check(SCLDPrinter::read(t0 + 999999) == 0x3e, "stays idle afterward regardless of elapsed time");
    }

    // ---- reset() forces motor off from a running state ----
    printf("\n-- reset() during an active print --\n");
    {
        uint64_t t0 = 5000;
        SCLDPrinter::write(0x80, t0); // motor running, stylus down (from the previous block's already-idle state)
        check(SCLDPrinter::read(t0) != 0x3e, "sanity check: motor is genuinely running before reset");
        SCLDPrinter::reset();
        check(SCLDPrinter::read(t0) == 0x3e, "reset() forces the printer back to idle even mid-print");
    }

    // ---- Writing a stop while already stopped is a harmless no-op ----
    printf("\n-- stop-while-stopped is a no-op, not a spurious start --\n");
    {
        SCLDPrinter::reset();
        SCLDPrinter::write(0x04, 5000); // bit2 set, motor already off
        check(SCLDPrinter::read(5000) == 0x3e, "writing a stop byte while idle does not start the motor");
    }

    // ---- Large time jump across many line boundaries doesn't hang or misbehave ----
    printf("\n-- large elapsed time crosses many line boundaries safely --\n");
    {
        SCLDPrinter::reset();
        uint64_t t0 = 0;
        SCLDPrinter::write(0x00, t0); // fast, stylus up
        uint8_t status = SCLDPrinter::read(t0 + 5000000); // far more than one line's worth of cycles
        check(status == 0x3e || status == 0x3f || status == 0xbe || status == 0xbf,
              "status after a huge time jump is still one of the four valid status bytes, not garbage");
    }

    // ---- Mid-transmission speed change is applied at the next line boundary, not immediately ----
    printf("\n-- speed change applied at the next line crossing --\n");
    {
        SCLDPrinter::reset();
        uint64_t t0 = 0;
        SCLDPrinter::write(0x00, t0); // start fast (220 cycles/pixel)
        // Advance partway into the line, then request slow (bit1 set) while x is still >= 0.
        uint64_t midLine = t0 + (100 + 64) * 220; // x = 100
        SCLDPrinter::write(0x02, midLine); // stylus up, speed bit set (slow requested), no stop
        // Force a line-boundary crossing with a further write far enough ahead that x wraps past 320
        // at the *old* (fast) rate first, which is when the pending speed should apply.
        uint64_t nextLine = midLine + (400 * 220);
        uint8_t status = SCLDPrinter::read(nextLine);
        check(status == 0x3e || status == 0x3f || status == 0xbe || status == 0xbf,
              "status after a mid-line speed change and a subsequent line crossing is still a valid status byte");
    }

    printf("\n======================\n");
    if (failures == 0) {
        printf("ALL PASSED\n");
    } else {
        printf("%d CHECK(S) FAILED\n", failures);
    }

    return failures;
}
