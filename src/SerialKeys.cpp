// SerialKeys: a TS2068 keyboard over UART0. See SerialKeys.h.

#ifdef ESP2068_SERIAL_KEYS

#include "SerialKeys.h"

#include <cstdio>

#include "driver/uart.h"
#include "fabgl.h"

namespace {

enum : uint8_t { MOD_CS = 1, MOD_SS = 2 };

struct Chord {
    uint8_t mods;
    fabgl::VirtualKey vk; // VK_NONE: modifiers only
};

// Frames a key is held down, then frames all keys are up before the next.
const int HOLD_FRAMES = 3;
const int GAP_FRAMES = 3;

const int QUEUE_SIZE = 512;
Chord queue[QUEUE_SIZE];
int qHead = 0, qTail = 0;

bool installed = false;
uint8_t pendingMods = 0; // from Ctrl-A / Ctrl-B, for the next key
bool lastWasCR = false;

enum { IDLE, HOLDING, GAP } state = IDLE;
int countdown = 0;
Chord current;

int statFrames = 0;
int64_t statSum = 0, statMax = 0, cpuSum = 0, cpuMax = 0;

void push(uint8_t mods, fabgl::VirtualKey vk) {
    int next = (qTail + 1) % QUEUE_SIZE;
    if (next == qHead) return; // full: drop
    queue[qTail] = { (uint8_t)(mods | pendingMods), vk };
    qTail = next;
    pendingMods = 0;
}

fabgl::VirtualKey digitKey(char c) { return (fabgl::VirtualKey)(fabgl::VK_0 + (c - '0')); }
fabgl::VirtualKey letterKey(char c) { return (fabgl::VirtualKey)(fabgl::VK_a + (c - 'a')); }

// SYMBOL SHIFT symbols and the key each one is on.
const char* const SYMBOLS = "!@#$%&'()_<>;\"^-+=:?/*,.";
const char* const SYMBOL_KEYS = "123456789" "0rtoph" "jklzcvbnm";

void decode(uint8_t c) {
    bool wasCR = lastWasCR;
    lastWasCR = (c == '\r');

    if (c >= 'a' && c <= 'z') { push(0, letterKey(c)); return; }
    if (c >= 'A' && c <= 'Z') { push(MOD_CS, letterKey(c - 'A' + 'a')); return; }
    if (c >= '0' && c <= '9') { push(0, digitKey(c)); return; }

    switch (c) {
        case ' ': push(0, fabgl::VK_SPACE); return;
        case '\r': push(0, fabgl::VK_RETURN); return;
        case '\n': if (!wasCR) push(0, fabgl::VK_RETURN); return;
        case 0x08:
        case 0x7f: push(MOD_CS, fabgl::VK_0); return;
        case 0x01: pendingMods |= MOD_SS; return;
        case 0x02: pendingMods |= MOD_CS; return;
        case 0x03: push(MOD_CS, fabgl::VK_SPACE); return;
        case 0x05: push(MOD_CS | MOD_SS, fabgl::VK_NONE); return;
    }

    for (int i = 0; SYMBOLS[i]; i++) {
        if (SYMBOLS[i] == c) {
            char k = SYMBOL_KEYS[i];
            push(MOD_SS, k >= '0' && k <= '9' ? digitKey(k) : letterKey(k));
            return;
        }
    }
    // Anything else is ignored.
}

void inject(fabgl::Keyboard* kbd, const Chord& ch, bool down) {
    // Modifiers go down first and come up last.
    if (down) {
        if (ch.mods & MOD_CS) kbd->injectVirtualKey(fabgl::VK_LSHIFT, true, false);
        if (ch.mods & MOD_SS) kbd->injectVirtualKey(fabgl::VK_LCTRL, true, false);
        if (ch.vk != fabgl::VK_NONE) kbd->injectVirtualKey(ch.vk, true, false);
    } else {
        if (ch.vk != fabgl::VK_NONE) kbd->injectVirtualKey(ch.vk, false, false);
        if (ch.mods & MOD_SS) kbd->injectVirtualKey(fabgl::VK_LCTRL, false, false);
        if (ch.mods & MOD_CS) kbd->injectVirtualKey(fabgl::VK_LSHIFT, false, false);
    }
}

} // namespace

int64_t SerialKeys::cpuUs = 0;

void SerialKeys::init() {
    // RX ring buffer only; printf keeps writing to the FIFO directly.
    installed = uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0) == ESP_OK;
    printf("SerialKeys: %s\n", installed ? "ready on UART0" : "UART0 driver install failed");
}

void SerialKeys::frame(fabgl::Keyboard* kbd, int64_t elapsedUs) {

    if (elapsedUs > 0) {
        statSum += elapsedUs;
        if (elapsedUs > statMax) statMax = elapsedUs;
        cpuSum += cpuUs;
        if (cpuUs > cpuMax) cpuMax = cpuUs;
        if (++statFrames == 300) {
            printf("speed: frame %d us avg, %d max; Z80 %d us avg, %d max (2068 frame 16639 us)\n",
                   (int)(statSum / statFrames), (int)statMax,
                   (int)(cpuSum / statFrames), (int)cpuMax);
            statFrames = 0;
            statSum = statMax = cpuSum = cpuMax = 0;
        }
    }

    if (!installed || !kbd) return;

    uint8_t buf[64];
    int n;
    while ((n = uart_read_bytes(UART_NUM_0, buf, sizeof buf, 0)) > 0) {
        for (int i = 0; i < n; i++) decode(buf[i]);
    }

    switch (state) {
        case IDLE:
            if (qHead != qTail) {
                current = queue[qHead];
                qHead = (qHead + 1) % QUEUE_SIZE;
                inject(kbd, current, true);
                state = HOLDING;
                countdown = HOLD_FRAMES;
            }
            break;
        case HOLDING:
            if (--countdown <= 0) {
                inject(kbd, current, false);
                state = GAP;
                countdown = GAP_FRAMES;
            }
            break;
        case GAP:
            if (--countdown <= 0) state = IDLE;
            break;
    }
}

#endif
