# TS-Pico link: bridge v1.1

How ESP2068 talks to a real TS-Pico firmware running on a separate Pico over a serial link. It extends bridge v1 (`tspico-firmware-build/docs/EMULATOR_BRIDGE.md`). In bridge v1 every Z80 access to ports 0Eh/0Fh is a round trip. That is too slow over a UART, so v1.1 keeps a **mirror** of the bus in the emulator:

- The emulator holds the status byte (what `IN 0Fh` reads) and a FIFO of bytes for `IN 0Eh`.
- The Pico pushes status changes and fills the FIFO ahead of demand.
- The Z80 only waits when it reads `0Eh` and the FIFO is empty.

Code:

- `include/TSPicoBus.h`, `src/TSPicoBus.cpp`: the mirror. Pure C++, host-tested by `test/host/tspico_test.cpp`.
- `include/TSPico.h`, `src/TSPicoUart.cpp`: UART1 on the board.
- `src/Ports.cpp`: the port hook.

Everything is built only with `-D ESP2068_TSPICO`, which the `fabgl2068` env sets.

## Wiring (ESP32-SBC-FabGL AccessBus)

| ESP32 | Signal | Pico |
|---|---|---|
| GPIO2 | U1TXD | UART RX |
| GPI34 | U1RXD | UART TX |
| GPIO0 | U1RTS | UART CTS |
| GPI39 | U1CTS | UART RTS |
| GND, +5V | | GND, VSYS |

The link runs at 3 Mbaud, 8N1.

- **Flow control:** RTS/CTS is on only while a Pico answers. GPI34 and GPI39 have no pull-ups and float when nothing is plugged in.
- **Pins the Pico drives:** only GPI34 and GPI39. Both are input-only and neither is a strapping pin.

## The Z80's view

| Access | With a Pico | With no Pico |
|---|---|---|
| `OUT (0Eh),v` / `OUT (0Fh),v` | status goes to `00h` (BUSY) at once; the byte goes to the Pico | nothing |
| `IN (0Fh)` | the mirrored status byte | `FFh` |
| `IN (0Eh)` | the next byte from the FIFO. If the FIFO is empty, it waits up to 50 ms for one, then reads `00h` (as `pico_host` and the P4 bridge do) | `00h` |

Ports are decoded on the low address byte only, as on the board.

## Emulator to Pico

Each frame is two bytes, `[op, value]`. No frame gets a reply.

| Op | Meaning | Value |
|---|---|---|
| 0 | `OUT (0Eh)` | the byte written |
| 1 | `IN (0Eh)` took a byte from the FIFO | the byte |
| 3 | `OUT (0Fh)` | the byte written |
| 4 | HELLO: start over | link version, `11h` |
| 5 | `IN (0Eh)` found the FIFO empty and read `00h` | 0 |

- Ops 0, 1, 3 and 4 are bridge v1's.
- `IN (0Fh)` (op 2) is never sent, because polling loops would flood the link.
- Op 1 tells the Pico how much of what it sent is still unread. That is what the firmware's `tx_fifo()` needs.

## Pico to emulator

| Bytes | Meaning |
|---|---|
| `80 y s` | STATUS: the firmware set the status byte to `y`, after taking `s` Z80 writes (mod 256) since the last HELLO |
| `81 n d1 … dn` | DATA: `n` bytes (0–255) for `IN (0Eh)`, in order |
| `82 'T' 'P' v` | HELLO: link version `v`. The Pico has started over: its queues are empty and its write count is 0 |

### Rules for the Pico

1. **Auto-busy:** model it as the PIO does. Every Z80 write sets the status to `00h` on the Pico side too. Do **not** send that change, because the emulator has already made it.
2. **Status messages:** send STATUS every time the firmware sets the status byte, with the count of writes taken so far.
   - The emulator drops a STATUS whose count isn't the number of writes it has sent (`staleStatus`). Such a STATUS answers an older write; the Z80 has written again since.
   - A later STATUS always follows. Rule 1 means the firmware sets the status again after taking the newer write.
3. **FIFO credit:** never have more than **4096** bytes sent and not yet taken (counting op 1 frames). The emulator's FIFO holds 4096 and drops anything beyond that (`fifoOverruns`).
4. **Answering HELLO:**
   - Clear both queues and set the write count to 0.
   - Reply with `82 'T' 'P' 11`, then a STATUS (`80 y 00`).
   - After it reboots, the Pico may also send this pair unasked.
5. **Ordering:** the link is ordered, so put DATA before the STATUS that tells the Z80 to read it. The firmware already does that.

## Connecting

**With no Pico,** the emulator sends HELLO once a second and the ports read as an empty socket. Garbage from a floating RX pin is ignored. Only a complete `82 'T' 'P' 1x` connects.

**On a 2068 reset,** the emulator sends HELLO, clears the FIFO and reads BUSY. Until the Pico's HELLO arrives, it drops everything, because all of it was sent before the reset.
- No answer within 500 ms means no Pico.
- Z80 writes made while it waits are sent, and they count: the Pico takes them after the HELLO.

**When the Pico is lost,** the emulator goes back to "no device". A send that can't go out within 20 ms (CTS held) counts as lost, and so does a reset with no answer.

## Not yet in v1.1

The ROM and DOCK image messages for `tpi:boot`, `tpi:dock` and `tpi:zx48` (PLAN.md §3.1, memory hooks) belong to Phase 3. Their tags will start at `83h`. The ROM comes from `/sd/2068/tspico.rom` on the emulator's own card until then.

## Tests

`test/host/tspico_test.cpp` runs unit tests against a scripted link. It also replays the two reference traces in `test/traces/`, recorded from the real firmware under `pico_host` with Fuse and ZEsarUX (see esp32-2068's `test/traces/README.md`):

- **The fake Pico:** plays the firmware's side over v1.1. It answers 200 µs late, a few bytes at a time.
- **The test:** plays the Z80's side through the mirror.
- **What passes:** all 2,485 and 2,191 frames, every reply matching, with no underruns.

```bash
g++ -std=c++17 -Wall -I include -DESP2068_HOST_TEST \
    test/host/tspico_test.cpp src/TSPicoBus.cpp -o /tmp/tspico_test && /tmp/tspico_test
```
