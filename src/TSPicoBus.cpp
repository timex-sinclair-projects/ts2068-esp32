// TSPicoBus: the TS-Pico's ports, mirrored locally. See TSPicoBus.h and
// TSPICO-LINK.md.

#include "TSPicoBus.h"

TSPicoBus::TSPicoBus(TSPicoLink* l) : link(l) {}

void TSPicoBus::setLink(TSPicoLink* l) {
    link = l;
    st = DISCONNECTED;
    probed = false;
    y = 0xFF;
    fifoClear();
}

void TSPicoBus::sendFrame(uint8_t op, uint8_t value) {
    if (!link) return;
    uint8_t f[2] = { op, value };
    if (!link->send(f, 2)) {
        s.linkErrors++;
        if (st != DISCONNECTED) lost();
    }
}

// The Pico stopped answering: the socket is empty again.
void TSPicoBus::lost() {
    st = DISCONNECTED;
    y = 0xFF;
    writesSent = 0;
    fifoClear();
    tag = 0;
    // Look for a Pico again after PROBE_US, not at once.
    probed = true;
    if (link) sinceUs = link->nowUs();
    setLinkUp(false);
}

void TSPicoBus::setLinkUp(bool up) {
    if (up == linkUp) return;
    linkUp = up;
    if (link) link->connectedChanged(up);
}

// Start over with a Pico that is there: everything in flight is stale until
// it answers the HELLO.
void TSPicoBus::startSync() {
    st = SYNCING;
    y = 0x00;
    writesSent = 0;
    fifoClear();
    tag = 0;
    sinceUs = link ? link->nowUs() : 0;
    sendFrame(OP_HELLO, LINK_VERSION);
}

void TSPicoBus::machineReset() {
    if (!link) return;
    if (st == DISCONNECTED) {
        probed = false;     // probe on the next poll
        return;
    }
    startSync();
}

void TSPicoBus::handleHello(uint8_t version) {
    if ((version >> 4) != (LINK_VERSION >> 4)) {
        s.linkErrors++;
        return;
    }
    // The Pico has started over: an answer to our HELLO, or a Pico that
    // just booted. Its status follows at once. Z80 writes made since our
    // HELLO went out came after it, so the Pico counts them: keep them.
    if (st != SYNCING) writesSent = 0;
    st = CONNECTED;
    y = 0x00;
    fifoClear();
    tag = 0;
    s.connects++;
    setLinkUp(true);
}

void TSPicoBus::handleByte(uint8_t b) {
    window[0] = window[1];
    window[1] = window[2];
    window[2] = window[3];
    window[3] = b;

    if (st == DISCONNECTED) {
        // Nothing to trust yet (the RX pin may be floating): only a whole
        // HELLO counts.
        if (window[0] == MSG_HELLO && window[1] == 'T' && window[2] == 'P')
            handleHello(window[3]);
        return;
    }

    if (tag == 0) {
        switch (b) {
            case MSG_STATUS: tag = b; got = 0; need = 2; return;
            case MSG_DATA:   tag = b; got = 0; need = 1; return;
            case MSG_HELLO:  tag = b; got = 0; need = 3; return;
            default: s.linkErrors++; return;
        }
    }

    if (tag == MSG_DATA && got >= 1) {
        // Data bytes go straight into the FIFO.
        if (st == CONNECTED) {
            if (count == FIFO_SIZE) {
                s.fifoOverruns++;
            } else {
                fifo[tail] = b;
                tail = (tail + 1) % FIFO_SIZE;
                count++;
            }
        }
        if (--need == 0) tag = 0;
        return;
    }

    msg[got++] = b;
    if (got < need) return;

    uint8_t t = tag;
    tag = 0;
    switch (t) {
        case MSG_STATUS:
            if (st != CONNECTED) break;     // SYNCING: from before our HELLO
            if (msg[1] == writesSent) y = msg[0];
            else s.staleStatus++;           // the Pico hasn't seen our last write yet
            break;
        case MSG_DATA:
            // The count byte.
            if (msg[0]) { tag = MSG_DATA; need = msg[0]; }
            break;
        case MSG_HELLO:
            if (msg[0] == 'T' && msg[1] == 'P') handleHello(msg[2]);
            else s.linkErrors++;
            break;
    }
}

void TSPicoBus::poll() {
    if (!link) return;

    uint8_t buf[256];
    for (int rounds = 0; rounds < 16; rounds++) {
        int n = link->receive(buf, sizeof buf);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) handleByte(buf[i]);
    }

    if (st == CONNECTED) return;
    int64_t now = link->nowUs();
    if (st == SYNCING) {
        if (now - sinceUs > SYNC_US) lost();
    } else if (!probed || now - sinceUs >= PROBE_US) {
        // Look for a Pico. The Z80 still sees an empty socket meanwhile.
        probed = true;
        sinceUs = now;
        uint8_t f[2] = { OP_HELLO, LINK_VERSION };
        link->send(f, 2);   // no Pico, nothing listening: a failure means nothing
    }
}

void TSPicoBus::out(uint8_t port, uint8_t value) {
    if (port != 0x0E && port != 0x0F) return;
    if (st == DISCONNECTED) return;
    // Auto-busy, as the board's PIO does on every write.
    y = 0x00;
    writesSent++;
    s.writes++;
    sendFrame(port == 0x0E ? OP_OUT_DATA : OP_OUT_STATUS, value);
}

uint8_t TSPicoBus::in(uint8_t port) {
    if (port == 0x0F) {
        poll();
        return y;
    }
    if (port != 0x0E) return 0xFF;

    poll();
    if (st != CONNECTED) return 0x00;

    if (count == 0) {
        // The Pico hasn't sent it yet: wait, as the bridge does.
        s.stalls++;
        int64_t t0 = link->nowUs();
        while (count == 0 && st == CONNECTED && link->nowUs() - t0 < TX_WAIT_US)
            poll();
    }
    if (st != CONNECTED) return 0x00;

    if (count == 0) {
        s.underruns++;
        sendFrame(OP_IN_UNDERRUN, 0);
        return 0x00;
    }
    uint8_t b = fifo[head];
    head = (head + 1) % FIFO_SIZE;
    count--;
    s.reads++;
    sendFrame(OP_IN_DATA, b);   // lets the Pico count what's left, for tx_fifo()
    return b;
}
