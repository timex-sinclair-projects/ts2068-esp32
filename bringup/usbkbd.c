#include "usbkbd.h"

#include <string.h>

#include "console.h"
#include "tusb.h"

static const uint8_t keycode_to_ascii[128][2] = { HID_KEYCODE_TO_ASCII };

#define QSIZE 32
static uint8_t queue[QSIZE];
static uint q_head, q_tail;

static void push(int c) {
    uint next = (q_head + 1) % QSIZE;
    if (next != q_tail) {
        queue[q_head] = (uint8_t)c;
        q_head = next;
    }
}

int usbkbd_getc(void) {
    if (q_tail == q_head) return 0;
    int c = queue[q_tail];
    q_tail = (q_tail + 1) % QSIZE;
    return c;
}

void usbkbd_init(void) {
    tuh_init(BOARD_TUH_RHPORT);
}

void usbkbd_task(void) {
    tuh_task();
}

// ----------------------------------------------------------------------------
// TinyUSB callbacks

void tuh_mount_cb(uint8_t daddr) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);
    say_c(C_CYAN | C_BRIGHT, "USB: device %u up, %04x:%04x\n", daddr, vid, pid);
}

void tuh_umount_cb(uint8_t daddr) {
    say_c(C_CYAN, "USB: device %u gone\n", daddr);
}

void tuh_hid_mount_cb(uint8_t daddr, uint8_t instance, uint8_t const *desc, uint16_t len) {
    static const char *const names[] = {"other", "keyboard", "mouse"};
    uint8_t proto = tuh_hid_interface_protocol(daddr, instance);
    say_c(C_CYAN | C_BRIGHT, "USB: HID %u.%u %s\n", daddr, instance, names[proto < 3 ? proto : 0]);
    tuh_hid_receive_report(daddr, instance);
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t instance) {
}

static bool held(const hid_keyboard_report_t *r, uint8_t code) {
    for (int i = 0; i < 6; i++) {
        if (r->keycode[i] == code) return true;
    }
    return false;
}

static void keyboard_report(const hid_keyboard_report_t *r) {
    static hid_keyboard_report_t prev;
    bool shift = r->modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    for (int i = 0; i < 6; i++) {
        uint8_t code = r->keycode[i];
        if (code == 0 || held(&prev, code)) continue;
        if (code >= HID_KEY_F1 && code <= HID_KEY_F12) {
            push(0x81 + code - HID_KEY_F1);
        } else if (code == HID_KEY_ARROW_UP) {
            push(KEY_UP);
        } else if (code == HID_KEY_ARROW_DOWN) {
            push(KEY_DOWN);
        } else if (code == HID_KEY_ARROW_LEFT) {
            push(KEY_LEFT);
        } else if (code == HID_KEY_ARROW_RIGHT) {
            push(KEY_RIGHT);
        } else if (code < 128) {
            uint8_t ch = keycode_to_ascii[code][shift ? 1 : 0];
            if (ch) push(ch);
        }
    }
    prev = *r;
}

void tuh_hid_report_received_cb(uint8_t daddr, uint8_t instance, uint8_t const *report, uint16_t len) {
    uint8_t proto = tuh_hid_interface_protocol(daddr, instance);
    if (proto == HID_ITF_PROTOCOL_KEYBOARD && len >= sizeof(hid_keyboard_report_t)) {
        keyboard_report((const hid_keyboard_report_t *)report);
    }
    tuh_hid_receive_report(daddr, instance);
}
