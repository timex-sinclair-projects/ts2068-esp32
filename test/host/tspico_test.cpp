/*

ESP2068 — TS2068 port of ESPectrum

test/host/tspico_test.cpp — host test for TSPicoBus (include/TSPicoBus.h),
the local mirror of the TS-Pico's ports 0Eh/0Fh.

Two parts:

  1. Unit tests against a scripted link: no Pico, probing, connecting
     through noise, auto-busy and stale status, data and acks, underruns,
     resets, a Pico that reboots, a link that dies.
  2. Replay of the reference traces in test/traces (recorded from the real
     TS-Pico firmware under pico_host, with Fuse and ZEsarUX; see
     esp32-2068's test/traces/README.md). A fake Pico plays the
     firmware's side of the trace over the v1.1 link, delivering a few
     bytes at a time, and the test plays the Z80's side through the bus
     mirror and checks every reply.

Build and run (from repo root, so the trace paths resolve):

    g++ -std=c++17 -Wall -I include -DESP2068_HOST_TEST \
        test/host/tspico_test.cpp src/TSPicoBus.cpp -o /tmp/tspico_test \
    && /tmp/tspico_test

*/

#include "TSPicoBus.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

// ---- A scripted link -------------------------------------------------------

struct ScriptLink : TSPicoLink {
    std::deque<uint8_t> toBus;          // what the "Pico" sends
    std::vector<uint8_t> fromBus;       // what the bus sent
    int64_t t = 0;
    int64_t tick = 10;                  // the clock moves on every look at it
    int chunk = 3;                      // bytes per receive()
    int64_t holdUntil = 0;              // nothing arrives before this time
    bool dead = false;
    int connectedCalls = 0;
    bool lastConnected = false;

    bool send(const uint8_t* p, int n) override {
        if (dead) return false;
        fromBus.insert(fromBus.end(), p, p + n);
        return true;
    }
    int receive(uint8_t* p, int max) override {
        int n = 0;
        if (t < holdUntil) return 0;
        while (n < max && n < chunk && !toBus.empty()) {
            p[n++] = toBus.front();
            toBus.pop_front();
        }
        return n;
    }
    int64_t nowUs() override { return t += tick; }
    void connectedChanged(bool c) override { connectedCalls++; lastConnected = c; }

    void push(std::initializer_list<int> bytes) { for (int b : bytes) toBus.push_back((uint8_t) b); }
    void hello(int version = TSPicoBus::LINK_VERSION) { push({0x82, 'T', 'P', version}); }
    void status(int y, int seq) { push({0x80, y, seq}); }
    int count(uint8_t op) const {
        int n = 0;
        for (size_t i = 0; i + 1 < fromBus.size(); i += 2) if (fromBus[i] == op) n++;
        return n;
    }
};

static void drain(TSPicoBus& bus, int n = 50) { for (int i = 0; i < n; i++) bus.poll(); }

static void connect(TSPicoBus& bus, ScriptLink& link, int y = 0xFF) {
    bus.poll();         // first probe
    link.hello();
    link.status(y, 0);
    drain(bus);
}

static void unitTests() {
    printf("unit tests\n");

    {
        TSPicoBus bus;  // no link at all
        check(bus.in(0x0F) == 0xFF && bus.in(0x0E) == 0x00, "no link: IN 0Fh = FFh, IN 0Eh = 00h");
        bus.out(0x0E, 0x12);
        check(bus.in(0x0F) == 0xFF, "no link: OUT changes nothing");
    }

    {
        ScriptLink link;
        TSPicoBus bus(&link);
        int64_t t0 = link.t;
        check(bus.in(0x0F) == 0xFF, "no Pico: IN 0Fh = FFh");
        check(bus.in(0x0E) == 0x00, "no Pico: IN 0Eh = 00h");
        check(link.t - t0 < 1000, "no Pico: IN 0Eh doesn't wait");
        bus.out(0x0E, 0x12);
        bus.out(0x0F, 0x03);
        check(bus.in(0x0F) == 0xFF, "no Pico: OUT doesn't go busy");
        check(link.count(TSPicoBus::OP_OUT_DATA) == 0 && link.count(TSPicoBus::OP_OUT_STATUS) == 0,
              "no Pico: writes aren't sent");
        check(link.count(TSPicoBus::OP_HELLO) == 1, "no Pico: one HELLO probe at first");
        drain(bus, 100);
        check(link.count(TSPicoBus::OP_HELLO) == 1, "no Pico: no second probe within a second");
        link.t += TSPicoBus::PROBE_US;
        bus.poll();
        check(link.count(TSPicoBus::OP_HELLO) == 2, "no Pico: probes again after a second");
        check(link.fromBus[1] == TSPicoBus::LINK_VERSION, "HELLO carries the link version");
    }

    {
        ScriptLink link;
        TSPicoBus bus(&link);
        bus.poll();
        link.push({0x80, 0x00, 0x81, 0x05, 0x82, 'T', 0x13, 0xFF, 0x80});  // noise, false starts
        drain(bus);
        check(!bus.connected(), "noise doesn't connect");
        link.hello();
        link.status(0xFF, 0);
        drain(bus);
        check(bus.connected(), "a HELLO in noise connects");
        check(link.connectedCalls == 1 && link.lastConnected, "link told: connected");
        check(bus.in(0x0F) == 0xFF, "status from the Pico");
    }

    {
        ScriptLink link;
        TSPicoBus bus(&link);
        bus.poll();
        link.hello(0x21);
        drain(bus);
        check(!bus.connected(), "a HELLO of another major version doesn't connect");
    }

    {
        ScriptLink link;
        TSPicoBus bus(&link);
        connect(bus, link);
        link.fromBus.clear();
        bus.out(0x0F, 0x03);
        check(bus.in(0x0F) == 0x00, "OUT 0Fh: busy at once");
        check(link.fromBus.size() == 2 && link.fromBus[0] == TSPicoBus::OP_OUT_STATUS && link.fromBus[1] == 0x03,
              "OUT 0Fh sent as op 3");
        bus.out(0x0E, 0x55);
        check(link.fromBus[2] == TSPicoBus::OP_OUT_DATA && link.fromBus[3] == 0x55, "OUT 0Eh sent as op 0");
        link.status(0xFF, 1);       // the answer to the first write only
        drain(bus);
        check(bus.in(0x0F) == 0x00 && bus.stats().staleStatus == 1, "a status from before our last write is dropped");
        link.status(0xF7, 2);
        drain(bus);
        check(bus.in(0x0F) == 0xF7, "a status after our last write is taken");
    }

    {
        ScriptLink link;
        TSPicoBus bus(&link);
        connect(bus, link);
        link.fromBus.clear();
        link.push({0x81, 3, 'a', 'b', 'c', 0x81, 0, 0x81, 1, 'd'});
        uint8_t a = bus.in(0x0E), b = bus.in(0x0E), c = bus.in(0x0E), d = bus.in(0x0E);
        check(a == 'a' && b == 'b' && c == 'c' && d == 'd', "data blocks read in order (and an empty block)");
        bool acks = link.fromBus.size() == 8;
        for (int i = 0; acks && i < 4; i++)
            acks = link.fromBus[2 * i] == TSPicoBus::OP_IN_DATA && link.fromBus[2 * i + 1] == "abcd"[i];
        check(acks, "each byte taken is acked with op 1 and the byte");

        link.fromBus.clear();
        int64_t t0 = link.t;
        uint8_t u = bus.in(0x0E);
        check(u == 0x00 && link.t - t0 >= TSPicoBus::TX_WAIT_US, "empty FIFO: waits 50 ms, reads 00h");
        check(bus.stats().underruns == 1 && link.count(TSPicoBus::OP_IN_UNDERRUN) == 1, "underrun counted and sent as op 5");

        link.holdUntil = link.t + 20000;   // the byte arrives 20 ms into the wait
        link.push({0x81, 1, 0x42});
        t0 = link.t;
        check(bus.in(0x0E) == 0x42 && bus.stats().stalls == 2 && link.t - t0 < TSPicoBus::TX_WAIT_US,
              "IN 0Eh waits for a byte in flight");
    }

    {
        ScriptLink link;
        TSPicoBus bus(&link);
        connect(bus, link);
        std::vector<uint8_t> big(TSPicoBus::FIFO_SIZE + 10, 0x77);
        for (size_t i = 0; i < big.size(); i += 255) {
            int n = (int) std::min<size_t>(255, big.size() - i);
            link.push({0x81, n});
            for (int j = 0; j < n; j++) link.toBus.push_back(big[i + j]);
        }
        drain(bus, 5000);
        check(bus.fifoCount() == TSPicoBus::FIFO_SIZE && bus.stats().fifoOverruns == 10, "an overfilled FIFO drops and counts");
    }

    {
        ScriptLink link;
        TSPicoBus bus(&link);
        connect(bus, link);
        link.push({0x81, 2, 1, 2});
        drain(bus);
        link.fromBus.clear();
        bus.machineReset();
        check(bus.state() == TSPicoBus::SYNCING && bus.in(0x0F) == 0x00 && bus.fifoCount() == 0,
              "reset: busy, FIFO empty, waiting for the Pico");
        check(link.fromBus.size() == 2 && link.fromBus[0] == TSPicoBus::OP_HELLO, "reset sends HELLO");
        bus.out(0x0F, 0x03);                      // the ROM writes before the answer
        link.push({0x81, 1, 9});                  // stale: sent before our HELLO
        link.status(0xFF, 0);                     // stale
        drain(bus);
        check(bus.fifoCount() == 0 && bus.in(0x0F) == 0x00, "reset: messages from before the HELLO are dropped");
        link.hello();
        link.status(0xFF, 1);                     // after the write made while syncing
        drain(bus);
        check(bus.connected() && bus.in(0x0F) == 0xFF, "reset: the answer reconnects; writes since count");

        bus.machineReset();
        link.t += TSPicoBus::SYNC_US + 1;
        bus.poll();
        check(!bus.connected() && bus.in(0x0F) == 0xFF && bus.in(0x0E) == 0x00, "reset with no answer: no Pico");
        check(!link.lastConnected, "link told: disconnected");
    }

    {
        ScriptLink link;
        TSPicoBus bus(&link);
        connect(bus, link);
        link.push({0x81, 2, 1, 2});
        bus.out(0x0E, 1);
        drain(bus);
        link.hello();               // the Pico rebooted
        link.status(0xFB, 0);
        drain(bus);
        check(bus.connected() && bus.fifoCount() == 0 && bus.in(0x0F) == 0xFB, "a rebooted Pico: FIFO cleared, its status taken");
    }

    {
        ScriptLink link;
        TSPicoBus bus(&link);
        connect(bus, link);
        link.dead = true;
        bus.out(0x0E, 1);
        check(!bus.connected() && bus.in(0x0F) == 0xFF, "a send that fails: no Pico");
    }

    {
        ScriptLink link;
        TSPicoBus bus(&link);
        check(bus.in(0x10) == 0xFF, "other ports ignored");
    }
}

// ---- Trace replay ------------------------------------------------------------

struct Line { std::string op; uint8_t value, reply; };

static bool readTrace(const char* path, std::vector<Line>& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::string op, arrow;
    unsigned v, r;
    while (f >> op >> std::hex >> v >> arrow >> r) out.push_back({op, (uint8_t) v, (uint8_t) r});
    return !out.empty();
}

// The firmware's side of a trace. After the Z80's k-th write, it sends what
// the trace shows the Z80 read before its next write: status changes and
// data. Like the firmware, it changes the status only once the Z80 has taken
// the data before it.
struct TracePico : TSPicoLink {
    struct Ev { bool isStatus; uint8_t v; };
    std::vector<std::vector<Ev>> segments;  // segments[k]: after write k
    std::vector<uint8_t> writes;            // the trace's writes, in order, and their ops
    std::vector<uint8_t> writeOps;

    // Bytes for the bus, each with the time it arrives: the Pico answers
    // LATENCY_US after the Z80's access, never at once.
    static const int64_t LATENCY_US = 200;
    std::deque<std::pair<int64_t, uint8_t>> toBus;
    std::vector<Ev> pending;
    size_t next = 0;
    int nWrites = 0;
    uint32_t emitted = 0, acked = 0;
    uint8_t y = 0xFF;
    int64_t t = 0;
    int chunk = 5;
    void put(uint8_t b) { toBus.push_back({t + LATENCY_US, b}); }
    int badWrites = 0, badAcks = 0, underruns = 0;
    std::vector<uint8_t> sentData;          // every data byte, to check acks

    explicit TracePico(const std::vector<Line>& lines) {
        segments.emplace_back();
        for (const Line& l : lines) {
            if (l.op == "OUT0E" || l.op == "OUT0F") {
                writeOps.push_back(l.op == "OUT0E" ? TSPicoBus::OP_OUT_DATA : TSPicoBus::OP_OUT_STATUS);
                writes.push_back(l.value);
                segments.emplace_back();
            } else if (l.op == "IN0F") {
                segments.back().push_back({true, l.reply});
            } else if (l.op == "IN0E") {
                segments.back().push_back({false, l.reply});
            }
        }
    }

    void startSegment(int k) {
        pending = k < (int) segments.size() ? segments[k] : std::vector<Ev>();
        next = 0;
        pump();
    }

    void pump() {
        std::vector<uint8_t> block;
        auto flush = [&]() {
            if (block.empty()) return;
            put(TSPicoBus::MSG_DATA);
            put((uint8_t) block.size());
            for (uint8_t b : block) put(b);
            block.clear();
        };
        while (next < pending.size()) {
            const Ev& e = pending[next];
            if (e.isStatus) {
                if (emitted != acked) break;   // wait for the Z80 to take the data (block bytes count as emitted)
                if (e.v != y) {
                    y = e.v;
                    put(TSPicoBus::MSG_STATUS);
                    put(y);
                    put((uint8_t) nWrites);
                }
            } else {
                if (emitted - acked >= (uint32_t) TSPicoBus::FIFO_SIZE) break;   // the bus's FIFO is full
                block.push_back(e.v);
                sentData.push_back(e.v);
                emitted++;
                if (block.size() == 255) flush();
            }
            next++;
        }
        flush();
    }

    bool send(const uint8_t* p, int n) override {
        for (int i = 0; i + 1 < n; i += 2) {
            uint8_t op = p[i], v = p[i + 1];
            switch (op) {
                case TSPicoBus::OP_HELLO:
                    toBus.clear();
                    nWrites = 0;
                    emitted = acked = 0;
                    sentData.clear();
                    for (int b : {0x82, (int) 'T', (int) 'P', (int) TSPicoBus::LINK_VERSION}) put((uint8_t) b);
                    put(TSPicoBus::MSG_STATUS);
                    put(y);
                    put(0);
                    startSegment(0);
                    break;
                case TSPicoBus::OP_OUT_DATA:
                case TSPicoBus::OP_OUT_STATUS:
                    if (nWrites >= (int) writes.size() || writeOps[nWrites] != op || writes[nWrites] != v) badWrites++;
                    nWrites++;
                    y = 0x00;   // auto-busy
                    startSegment(nWrites);
                    break;
                case TSPicoBus::OP_IN_DATA:
                    if (acked >= sentData.size() || sentData[acked] != v) badAcks++;
                    acked++;
                    pump();
                    break;
                case TSPicoBus::OP_IN_UNDERRUN:
                    underruns++;
                    break;
            }
        }
        return true;
    }
    int receive(uint8_t* p, int max) override {
        t += 3;     // the Z80 side spends time looking, too
        int n = 0;
        while (n < max && n < chunk && !toBus.empty() && toBus.front().first <= t) {
            p[n++] = toBus.front().second;
            toBus.pop_front();
        }
        return n;
    }
    int64_t nowUs() override { return t += 3; }
};

static void replay(const char* path) {
    printf("replay %s\n", path);
    std::vector<Line> lines;
    if (!readTrace(path, lines)) {
        printf("  FAIL can't read %s\n", path);
        failures++;
        return;
    }
    TracePico pico(lines);
    TSPicoBus bus(&pico);
    for (int i = 0; i < 100 && !bus.connected(); i++) bus.poll();
    check(bus.connected(), "connects");

    int frames = 0, inData = 0, statusWaits = 0, bad = 0;
    for (size_t i = 0; i < lines.size() && bad < 5; i++) {
        const Line& l = lines[i];
        if (l.op == "HELLO") continue;
        frames++;
        if (l.op == "OUT0E") bus.out(0x0E, l.value);
        else if (l.op == "OUT0F") bus.out(0x0F, l.value);
        else if (l.op == "IN0F") {
            int polls = 0;
            while (bus.in(0x0F) != l.reply && polls < 100000) polls++;
            if (polls == 100000) {
                printf("  line %zu: IN0F never read %02X (reads %02X)\n", i + 1, l.reply, bus.status());
                bad++;
            }
            statusWaits++;
        } else if (l.op == "IN0E") {
            uint8_t got = bus.in(0x0E);
            if (got != l.reply) {
                printf("  line %zu: IN0E read %02X, trace has %02X\n", i + 1, got, l.reply);
                bad++;
            }
            inData++;
        }
    }
    printf("  %d frames: %d data reads, %d status waits; %u stalls, %u stale status\n",
           frames, inData, statusWaits, bus.stats().stalls, bus.stats().staleStatus);
    check(bad == 0, "every reply matches the trace");
    check(pico.badWrites == 0, "every write reaches the Pico in order");
    check(pico.badAcks == 0, "every byte taken is acked in order");
    check(bus.stats().underruns == 0 && pico.underruns == 0, "no underruns");
    check(bus.connected(), "still connected at the end");
}

int main() {
    unitTests();
    replay("test/traces/fuse-smoke.frames");
    replay("test/traces/zesarux-smoke.frames");
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
