#include "bt/bt_hid_keycode.h"

#include <string.h>

#define BT_HID_LEFT_CTRL  0x01u
#define BT_HID_LEFT_SHIFT 0x02u

static int map_printable(int key, uint8_t *usage, uint8_t *modifier) {
    static const char unshifted[] = "1234567890-=[]\\;'/`,.";
    static const char shifted[] = "!@#$%^&*()_+{}|:\"?~<>";
    size_t i;

    if (key >= 'a' && key <= 'z') {
        *usage = (uint8_t)(0x04 + key - 'a');
        return 1;
    }
    if (key >= 'A' && key <= 'Z') {
        *usage = (uint8_t)(0x04 + key - 'A');
        *modifier = BT_HID_LEFT_SHIFT;
        return 1;
    }
    if (key == ' ') {
        *usage = 0x2c;
        return 1;
    }
    for (i = 0; i < sizeof(unshifted) - 1; i++) {
        if (key == unshifted[i]) {
            static const uint8_t usages[] = {
                0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x33, 0x34, 0x38, 0x35, 0x36, 0x37
            };
            *usage = usages[i];
            return 1;
        }
        if (key == shifted[i]) {
            static const uint8_t usages[] = {
                0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x33, 0x34, 0x38, 0x35, 0x36, 0x37
            };
            *usage = usages[i];
            *modifier = BT_HID_LEFT_SHIFT;
            return 1;
        }
    }
    return 0;
}

int bt_hid_keycode_map(int key, int raw_key, bt_hid_report_t *out) {
    uint8_t usage = 0;
    uint8_t modifier = 0;

    if (out == NULL) return BT_HID_UNMAPPED;
    if (key >= 0x01 && key <= 0x1a && raw_key >= 'a' && raw_key <= 'z') {
        usage = (uint8_t)(0x04 + key - 1);
        modifier = BT_HID_LEFT_CTRL;
    } else if (key >= 0x20 && key <= 0x7e) {
        if (!map_printable(key, &usage, &modifier)) return BT_HID_UNMAPPED;
    } else {
        switch (key) {
            case 0x08: usage = 0x2a; break;
            case 0x09: usage = 0x2b; break;
            case 0x0a:
            case 0x0d: usage = 0x28; break;
            case 0xd4: usage = 0x4c; break;
            case 0xd1: usage = 0x49; break;
            case 0xd2: usage = 0x4a; break;
            case 0xb5: usage = 0x52; break;
            case 0xb6: usage = 0x51; break;
            case 0xb4: usage = 0x50; break;
            case 0xb7: usage = 0x4f; break;
            case 0xc1: usage = 0x39; break;
            default:
                if (key >= 0x81 && key <= 0x86) usage = (uint8_t)(0x3a + key - 0x81);
                else return BT_HID_UNMAPPED;
                break;
        }
    }

    memset(out, 0, sizeof(*out));
    out->modifiers = modifier;
    out->keycode = usage;
    return BT_HID_MAPPED;
}
