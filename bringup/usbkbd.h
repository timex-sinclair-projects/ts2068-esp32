#pragma once

#include <stdint.h>

// USB host through the board's hub: reports devices as they come and go,
// and turns boot-protocol keyboard reports into characters.
void usbkbd_init(void);
void usbkbd_task(void);

// The next typed character, 0 if none. Function keys come back as
// KEY_F1.. (0x80 + n); arrows as KEY_UP etc.
int usbkbd_getc(void);

#define KEY_F1    0x81
#define KEY_F2    0x82
#define KEY_F3    0x83
#define KEY_F4    0x84
#define KEY_UP    0x90
#define KEY_DOWN  0x91
#define KEY_LEFT  0x92
#define KEY_RIGHT 0x93
