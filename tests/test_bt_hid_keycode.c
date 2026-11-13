#include <stdint.h>
#include <string.h>

#include "bt/bt_hid_keycode.h"
#include "keyboard_definition.h"
#include "test_util.h"

static void expected_ascii(int c, uint8_t *modifier, uint8_t *usage) {
    static const char plain[] = "1234567890-=[]\\;'/`,.";
    static const char shifted[] = "!@#$%^&*()_+{}|:\"?~<>";
    static const uint8_t usages[] = {
        0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x33, 0x34, 0x38, 0x35, 0x36, 0x37
    };
    const char *found;

    *modifier = 0;
    if (c >= 'a' && c <= 'z') {
        *usage = (uint8_t)(0x04 + c - 'a');
    } else if (c >= 'A' && c <= 'Z') {
        *modifier = 0x02;
        *usage = (uint8_t)(0x04 + c - 'A');
    } else if (c == ' ') {
        *usage = 0x2c;
    } else if ((found = strchr(plain, c)) != NULL) {
        *usage = usages[found - plain];
    } else {
        found = strchr(shifted, c);
        *modifier = 0x02;
        *usage = usages[found - shifted];
    }
}

static void check_mapping(int key, int raw_key, uint8_t modifier, uint8_t usage) {
    bt_hid_report_t report;
    const uint8_t *bytes = (const uint8_t *)&report;
    memset(&report, 0xa5, sizeof(report));
    CHECK(bt_hid_keycode_map(key, raw_key, &report) == BT_HID_MAPPED);
    CHECK(bytes[0] == modifier);
    CHECK(bytes[1] == 0);
    CHECK(bytes[2] == usage);
    for (int i = 3; i < 8; i++) CHECK(bytes[i] == 0);
}

void test_bt_hid_keycode(void) {
    static const struct {
        int key;
        uint8_t usage;
    } named[] = {
        {KEY_BACKSPACE, 0x2a}, {KEY_TAB, 0x2b}, {KEY_ENTER, 0x28}, {0x0d, 0x28},
        {KEY_DEL, 0x4c}, {KEY_INSERT, 0x49}, {KEY_HOME, 0x4a},
        {KEY_UP, 0x52}, {KEY_DOWN, 0x51}, {KEY_LEFT, 0x50}, {KEY_RIGHT, 0x4f},
        {KEY_F1, 0x3a}, {KEY_F2, 0x3b}, {KEY_F3, 0x3c},
        {KEY_F4, 0x3d}, {KEY_F5, 0x3e}, {KEY_F6, 0x3f}, {KEY_CAPS_LOCK, 0x39}
    };
    static const int unmapped[] = {
        KEY_MOD_ALT, KEY_MOD_SHL, KEY_MOD_SHR, KEY_MOD_SYM, KEY_MOD_CTRL,
        KEY_BREAK, KEY_NONE, 0xff, -1
    };

    for (int c = 0x20; c <= 0x7e; c++) {
        uint8_t modifier, usage;
        expected_ascii(c, &modifier, &usage);
        check_mapping(c, c, modifier, usage);
    }
    for (int c = 0x01; c <= 0x1a; c++) {
        check_mapping(c, 'a' + c - 1, 0x01, (uint8_t)(0x04 + c - 1));
    }

    check_mapping(0x08, 'h', 0x01, 0x0b);
    check_mapping(0x08, 0x08, 0, 0x2a);
    check_mapping(0x09, 'i', 0x01, 0x0c);
    check_mapping(0x09, 0x09, 0, 0x2b);
    check_mapping(0x0a, 'j', 0x01, 0x0d);
    check_mapping(0x0a, 0x0a, 0, 0x28);

    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        check_mapping(named[i].key, named[i].key, 0, named[i].usage);
    }
    for (size_t i = 0; i < sizeof(unmapped) / sizeof(unmapped[0]); i++) {
        bt_hid_report_t report;
        bt_hid_report_t original;
        memset(&report, 0x5a, sizeof(report));
        original = report;
        CHECK(bt_hid_keycode_map(unmapped[i], unmapped[i], &report) == BT_HID_UNMAPPED);
        CHECK(memcmp(&report, &original, sizeof(report)) == 0);
    }
}
