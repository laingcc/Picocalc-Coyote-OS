#include "bt/bt_hid.h"

#include <string.h>

#include "UI/ui.h"
#include "ai/app_services.h"
#include "bt/bt_hid_keycode.h"

int bt_hid_pico_init(void);
void bt_hid_pico_fail(void);
void bt_hid_pico_poll(void);
void bt_hid_pico_enter(void);
void bt_hid_pico_leave(void);
void bt_hid_pico_send(const bt_hid_report_t *report);
bt_hid_state_t bt_hid_pico_state(void);
const char *bt_hid_pico_peer(void);

static bool initialised;
static bt_hid_state_t shown_state = (bt_hid_state_t)-1;
static char shown_peer[18];

static void draw_state(void) {
    bt_hid_state_t state = bt_hid_pico_state();
    const char *peer = bt_hid_pico_peer();
    char text[48];

    if (state == shown_state && (state != BT_HID_STATE_BONDED || strcmp(peer, shown_peer) == 0)) return;
    shown_state = state;
    if (state == BT_HID_STATE_BONDED) {
        static const char prefix[] = " Connected to ";
        size_t i;
        memcpy(text, prefix, sizeof(prefix) - 1);
        for (i = 0; i < 17; i++) text[sizeof(prefix) - 1 + i] = peer[i];
        text[sizeof(prefix) - 1 + 17] = ' ';
        text[sizeof(prefix) + 17] = '\0';
        memcpy(shown_peer, peer, sizeof(shown_peer));
    } else if (state == BT_HID_STATE_STARTING) strcpy(text, " Starting Bluetooth ");
    else if (state == BT_HID_STATE_ADVERTISING) strcpy(text, " Pair from your device: PicoCalc Keyboard ");
    else if (state == BT_HID_STATE_CONNECTED) strcpy(text, " Connected - finishing setup ");
    else strcpy(text, " Bluetooth failed ");
    ui_bt_keyboard_show(text);
}

int bt_hid_init(void) {
    shown_peer[0] = '\0';
    if (!initialised) {
        shown_state = BT_HID_STATE_STARTING;
        ui_bt_keyboard_show(" Starting Bluetooth ");
        if (app_services_radio_ensure() != 0 || bt_hid_pico_init() != 0) {
            bt_hid_pico_fail();
            initialised = true;
            draw_state();
            return -1;
        }
        initialised = true;
    } else {
        shown_state = (bt_hid_state_t)-1;
    }
    bt_hid_pico_enter();
    draw_state();
    return bt_hid_pico_state() == BT_HID_STATE_FAILED ? -1 : 0;
}

void bt_hid_poll(void) {
    if (!initialised) return;
    bt_hid_pico_poll();
    draw_state();
}

void bt_hid_key(int key, int raw_key) {
    bt_hid_report_t report;
    if (initialised && bt_hid_keycode_map(key, raw_key, &report) == BT_HID_MAPPED) {
        bt_hid_pico_send(&report);
    }
}

void bt_hid_leave(void) {
    if (initialised) bt_hid_pico_leave();
}

bt_hid_state_t bt_hid_state(void) {
    return initialised ? bt_hid_pico_state() : BT_HID_STATE_STARTING;
}
