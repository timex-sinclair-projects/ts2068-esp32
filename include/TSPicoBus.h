/*

ESP2068 — TS2068 port of ESPectrum

TSPicoBus.h — the TS-Pico's ports 0Eh/0Fh, mirrored locally, with the real
TS-Pico firmware on a separate Pico at the other end of a serial link.

A round trip to the Pico on every port access would be far too slow, so
this class keeps a local copy of the bus state the Z80 sees:

  - the status byte (IN 0Fh), which the Pico pushes whenever its firmware
    sets it;
  - a FIFO of bytes for IN 0Eh, which the Pico fills ahead of demand.

The Z80's side then works like the board's PIO (TS_IO_DUAL):

  OUT 0Eh / OUT 0Fh   status goes to 00h (BUSY) at once, and the byte goes
                      to the Pico
  IN 0Fh              the mirrored status byte
  IN 0Eh              the next byte from the FIFO. Only when the FIFO is
                      empty does it wait for the Pico, up to 50 ms, then
                      read 00h (as pico_host and the P4 bridge do).

With no Pico answering, the ports read as an empty socket: IN 0Fh = FFh,
IN 0Eh = 00h, and writes go nowhere. The ROM's commands then fail with a
report instead of hanging.

The link protocol is bridge v1.1; TSPICO-LINK.md describes it. In short,
the emulator sends bridge-v1 frames for the Z80's writes and for each byte
it takes from the FIFO, and the Pico sends status changes and data blocks.

This class is pure and host-testable: the transport is a TSPicoLink, which
is UART1 on the board (src/TSPicoUart.cpp) and a scripted fake Pico in
test/host/tspico_test.cpp.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifndef TSPicoBus_h
#define TSPicoBus_h

#include <cstdint>

// The byte transport to the Pico.
class TSPicoLink {
public:
    virtual ~TSPicoLink() {}
    // Sends every byte, or returns false if the link can't take them (the
    // Pico has stopped reading). Must not block for long.
    virtual bool send(const uint8_t* p, int n) = 0;
    // Whatever bytes have arrived, up to max. Never blocks.
    virtual int receive(uint8_t* p, int max) = 0;
    // A monotonic clock in microseconds.
    virtual int64_t nowUs() = 0;
    // Told when the Pico answers or goes away (the UART turns hardware
    // flow control on only while a Pico is there to drive CTS).
    virtual void connectedChanged(bool connected) { (void) connected; }
};

class TSPicoBus {
public:

    // Bridge op codes, emulator to Pico. 0, 1, 3 and 4 are bridge v1's.
    enum : uint8_t {
        OP_OUT_DATA = 0,     // OUT (0Eh),v
        OP_IN_DATA = 1,      // IN (0Eh) took byte v from the mirror's FIFO
        OP_OUT_STATUS = 3,   // OUT (0Fh),v
        OP_HELLO = 4,        // start over; v = link version
        OP_IN_UNDERRUN = 5,  // IN (0Eh) found the FIFO empty and read 00h
    };

    // Message tags, Pico to emulator.
    enum : uint8_t {
        MSG_STATUS = 0x80,   // 80 y s: status is y, after taking s (mod 256) writes
        MSG_DATA = 0x81,     // 81 n d1..dn: n bytes (1-255) for IN (0Eh)
        MSG_HELLO = 0x82,    // 82 'T' 'P' v: link version v; the Pico has started over
    };

    static const uint8_t LINK_VERSION = 0x11;   // bridge v1.1

    static const int FIFO_SIZE = 4096;          // the Pico keeps at most this many unread
    static const int64_t TX_WAIT_US = 50000;    // an empty FIFO waits this long, then reads 00h
    static const int64_t PROBE_US = 1000000;    // with no Pico, send HELLO this often
    static const int64_t SYNC_US = 500000;      // a HELLO with no answer in this time: no Pico

    enum State { DISCONNECTED, SYNCING, CONNECTED };

    struct Stats {
        uint32_t writes;        // OUT 0Eh/0Fh sent to the Pico
        uint32_t reads;         // IN 0Eh answered from the FIFO
        uint32_t underruns;     // IN 0Eh with nothing after TX_WAIT_US: read 00h
        uint32_t stalls;        // IN 0Eh that had to wait for the Pico
        uint32_t staleStatus;   // status messages dropped: sent before the Pico saw our last write
        uint32_t fifoOverruns;  // data bytes dropped: the Pico overfilled the FIFO
        uint32_t linkErrors;    // sends that failed, unknown tags, bad HELLOs
        uint32_t connects;      // times a Pico answered
    };

    explicit TSPicoBus(TSPicoLink* link = nullptr);
    void setLink(TSPicoLink* link);

    // The emulated machine was reset: start over with the Pico (or look for
    // one, if none has answered yet).
    void machineReset();

    // Reads the link and handles what came. Call once a frame; IN does it
    // too, so a polling loop sees status changes as they arrive.
    void poll();

    // A Z80 access to port 0Eh or 0Fh (low address byte; others ignored).
    void out(uint8_t port, uint8_t value);
    uint8_t in(uint8_t port);

    State state() const { return st; }
    bool connected() const { return st == CONNECTED; }
    int fifoCount() const { return count; }
    uint8_t status() const { return y; }
    const Stats& stats() const { return s; }

private:
    void sendFrame(uint8_t op, uint8_t value);
    void startSync();
    void lost();
    void setLinkUp(bool up);
    void handleByte(uint8_t b);
    void handleHello(uint8_t version);
    void fifoClear() { head = tail = count = 0; }

    TSPicoLink* link;
    State st = DISCONNECTED;
    int64_t sinceUs = 0;        // when the current HELLO went out (SYNCING) or the last probe (DISCONNECTED)
    bool probed = false;
    bool linkUp = false;        // last told to the link

    uint8_t y = 0xFF;           // the status byte the Z80 reads
    uint8_t writesSent = 0;     // Z80 writes since the last HELLO, mod 256

    uint8_t fifo[FIFO_SIZE];
    int head = 0, tail = 0, count = 0;

    // Incoming message parser.
    uint8_t tag = 0;            // 0: waiting for a tag
    uint8_t msg[4];
    int got = 0, need = 0;
    uint8_t window[4] = {0, 0, 0, 0};   // last 4 bytes, to spot a HELLO in noise

    Stats s = {};
};

#endif
