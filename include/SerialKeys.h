/*
SerialKeys: a TS2068 keyboard over the USB serial port (UART0).

For bring-up without a PS/2 keyboard. Bytes arriving on UART0 become
key presses, injected into fabgl's keyboard as if typed on a PS/2
keyboard, one key per few frames so the ROM's once-a-frame scan sees each
press and release. Keys are 2068 keystrokes, not text: in K mode "p" gives
PRINT, as on the real keyboard.

  a-z 0-9 space    the key
  A-Z              CAPS SHIFT + letter
  ! @ # $ % & ' ( ) _ < > ; " ^ - + = : ? / * , .
                   SYMBOL SHIFT + the key that carries it
  CR or LF         ENTER (LF right after CR is ignored)
  BS or DEL        DELETE (CAPS SHIFT + 0)
  Ctrl-A           SYMBOL SHIFT with the next key
  Ctrl-B           CAPS SHIFT with the next key
  Ctrl-C           BREAK (CAPS SHIFT + SPACE)
  Ctrl-E           CAPS SHIFT + SYMBOL SHIFT (extended mode)
  Ctrl-V           send the screen: screen memory and the VGA frame
                   (tools/sendkeys.py {SHOT}, tools/check_screen.py)

Every 300 frames it also prints the emulation time per frame, against the
2068's 16639 us frame, to show whether the emulator keeps full speed.

tools/sendkeys.py is the host side. Built only with -D TS2068_SERIAL_KEYS.
*/

#ifndef SerialKeys_h
#define SerialKeys_h

#include <cstdint>

namespace fabgl { class Keyboard; }

class SerialKeys {
public:
    static void init();
    // Once per frame, before the keyboard queue is read.
    static void frame(fabgl::Keyboard* kbd, int64_t elapsedUs);
    // Time spent in CPU::loop() this frame, set by ESPectrum::loop().
    static int64_t cpuUs;
};

#endif
