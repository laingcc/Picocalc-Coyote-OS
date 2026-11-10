#ifndef COYOTE_BT_HID_H
#define COYOTE_BT_HID_H

typedef enum {
    BT_HID_STATE_STARTING,
    BT_HID_STATE_ADVERTISING,
    BT_HID_STATE_CONNECTED,
    BT_HID_STATE_BONDED,
    BT_HID_STATE_FAILED
} bt_hid_state_t;

int bt_hid_init(void);
void bt_hid_poll(void);
void bt_hid_key(int key, int raw_key);
void bt_hid_leave(void);
bt_hid_state_t bt_hid_state(void);

#endif
