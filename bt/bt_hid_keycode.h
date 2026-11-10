#ifndef COYOTE_BT_HID_KEYCODE_H
#define COYOTE_BT_HID_KEYCODE_H

#include <stdint.h>

enum {
    BT_HID_UNMAPPED = 0,
    BT_HID_MAPPED = 1
};

typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keycode;
    uint8_t additional_keycodes[5];
} bt_hid_report_t;

int bt_hid_keycode_map(int key, int raw_key, bt_hid_report_t *out);

#endif
