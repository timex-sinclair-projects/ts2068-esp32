/*

ESP2068 — TS2068 port of ESPectrum

TSPico.h — the TS-Pico on the board: ports 0Eh/0Fh go to a TSPicoBus
(include/TSPicoBus.h) whose link is UART1 on the AccessBus header:

  GPIO2  U1TXD  to the Pico's RX
  GPI34  U1RXD  from the Pico's TX
  GPIO0  U1RTS  to the Pico's CTS
  GPI39  U1CTS  from the Pico's RTS

3 Mbaud, 8N1. Hardware flow control is on only while a Pico answers, since
GPI34/GPI39 have no pull-ups and float with nothing plugged in. The Pico
drives only the two input-only pins, neither a strapping pin, so it can't
put the ESP32 into download mode at reset.

Built only with -D ESP2068_TSPICO, which decodes ports 0Eh/0Fh (without
it they read FFh and ignore writes). See TSPICO-LINK.md.

*/

#ifndef TSPico_h
#define TSPico_h

#include <cstdint>

namespace TSPico {
    void init();                        // once, at boot
    void frame();                       // once a frame
    void machineReset();                // the 2068 was reset
    uint8_t in(uint8_t port);           // low address byte 0Eh or 0Fh
    void out(uint8_t port, uint8_t value);
}

#endif
