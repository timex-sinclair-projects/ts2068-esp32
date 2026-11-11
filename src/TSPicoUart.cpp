// TSPico: ports 0Eh/0Fh over UART1 to a Pico. See TSPico.h.

#ifdef TS2068_TSPICO

#include "TSPico.h"
#include "TSPicoBus.h"

#include <cstdio>

#include "driver/uart.h"
#include "esp_timer.h"

namespace {

const uart_port_t PORT = UART_NUM_1;
const int TX_PIN = 2, RX_PIN = 34, RTS_PIN = 0, CTS_PIN = 39;
const int BAUD = 3000000;
// The driver's RX buffer. When it fills, the driver stops emptying the
// hardware FIFO, and RTS then holds the Pico off.
const int RX_BUFFER = 2048;
const int RTS_THRESHOLD = 100;          // of the 128-byte hardware FIFO
const int64_t SEND_TIMEOUT_US = 20000;  // CTS held this long: the Pico is gone

class UartLink : public TSPicoLink {
public:
    bool send(const uint8_t* p, int n) override {
        int64_t t0 = esp_timer_get_time();
        while (n > 0) {
            // Straight into the hardware FIFO, never blocking.
            int k = uart_tx_chars(PORT, (const char*) p, n);
            if (k < 0) return false;
            p += k;
            n -= k;
            if (n > 0 && esp_timer_get_time() - t0 > SEND_TIMEOUT_US) return false;
        }
        return true;
    }

    int receive(uint8_t* p, int max) override {
        size_t avail = 0;
        if (uart_get_buffered_data_len(PORT, &avail) != ESP_OK || avail == 0) return 0;
        int n = uart_read_bytes(PORT, p, (uint32_t) (avail < (size_t) max ? avail : max), 0);
        return n < 0 ? 0 : n;
    }

    int64_t nowUs() override { return esp_timer_get_time(); }

    void connectedChanged(bool connected) override {
        uart_set_hw_flow_ctrl(PORT, connected ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE, RTS_THRESHOLD);
        printf("TS-Pico: %s\n", connected ? "connected" : "no Pico");
    }
};

UartLink uartLink;
TSPicoBus bus;
bool ready = false;
int framesSinceReport = 0;
TSPicoBus::Stats lastReported = {};

} // namespace

void TSPico::init() {
    uart_config_t cfg = {};
    cfg.baud_rate = BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.rx_flow_ctrl_thresh = RTS_THRESHOLD;
    cfg.source_clk = UART_SCLK_APB;

    esp_err_t err = uart_driver_install(PORT, RX_BUFFER, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(PORT, &cfg);
    if (err == ESP_OK) err = uart_set_pin(PORT, TX_PIN, RX_PIN, RTS_PIN, CTS_PIN);
    if (err != ESP_OK) {
        printf("TS-Pico: UART1 setup failed (%d); ports 0Eh/0Fh read as no device\n", err);
        return;
    }
    bus.setLink(&uartLink);
    ready = true;
    printf("TS-Pico: link on UART1 at %d baud, looking for a Pico\n", BAUD);
}

void TSPico::frame() {
    if (!ready) return;
    bus.poll();

    // Every 10 s while a Pico is there, report what changed.
    if (++framesSinceReport < 600) return;
    framesSinceReport = 0;
    const TSPicoBus::Stats& s = bus.stats();
    if (!bus.connected() || (s.writes == lastReported.writes && s.reads == lastReported.reads)) return;
    printf("TS-Pico: %u writes, %u reads, %u stalls, %u underruns, %u stale status, %u overruns, %u link errors\n",
           (unsigned) s.writes, (unsigned) s.reads, (unsigned) s.stalls, (unsigned) s.underruns,
           (unsigned) s.staleStatus, (unsigned) s.fifoOverruns, (unsigned) s.linkErrors);
    lastReported = s;
}

void TSPico::machineReset() {
    if (ready) bus.machineReset();
}

uint8_t TSPico::in(uint8_t port) {
    return bus.in(port);    // with no link, an empty socket
}

void TSPico::out(uint8_t port, uint8_t value) {
    bus.out(port, value);
}

#endif
