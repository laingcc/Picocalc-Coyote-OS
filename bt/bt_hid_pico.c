#include "bt/bt_hid.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bt/bt_hid_keycode.h"
#include "bt_hid_keyboard.h"
#include "btstack.h"
#include "btstack_tlv_flash_bank.h"
#include "ble/gatt-service/hids_device.h"
#include "pico/cyw43_arch.h"
#include "pico/btstack_flash_bank.h"
#include "pico/time.h"

static const uint8_t hid_descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xe0,
    0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x03,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
    0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03,
    0x91, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0xff,
    0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x81, 0x00,
    0xc0
};

static const uint8_t advertising_data[] = {
    0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
    0x12, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME,
    'P', 'i', 'c', 'o', 'C', 'a', 'l', 'c', ' ', 'K', 'e', 'y', 'b', 'o', 'a', 'r', 'd',
    0x03, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS,
    ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE & 0xff,
    ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE >> 8,
    0x03, BLUETOOTH_DATA_TYPE_APPEARANCE, 0xc1, 0x03
};

static btstack_packet_callback_registration_t hci_registration;
static btstack_packet_callback_registration_t sm_registration;
static btstack_tlv_flash_bank_t tlv_context;
static bt_hid_state_t state = BT_HID_STATE_STARTING;
static hci_con_handle_t connection_handle = HCI_CON_HANDLE_INVALID;
static hci_con_handle_t confirm_handle = HCI_CON_HANDLE_INVALID;
static bd_addr_t peer_address;
static char peer_text[18] = "00:00:00:00:00:00";
static bt_hid_report_t pending_report;
static bt_hid_report_t report_queue[8];
static uint32_t release_at;
static uint8_t protocol_mode = 1;
static uint8_t send_phase;
static uint8_t queue_head;
static uint8_t queue_tail;
static uint8_t queue_count;
static bool active;
static bool advertise_pending;
static bool confirm_pending;
static bool can_send;
static bool input_enabled;

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    uint8_t event;
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    event = hci_event_packet_get_type(packet);
    if (event == BTSTACK_EVENT_STATE) {
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            state = BT_HID_STATE_ADVERTISING;
            advertise_pending = active;
        }
        return;
    }
    if (event == HCI_EVENT_LE_META &&
        hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
        if (hci_subevent_le_connection_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
            connection_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
            hci_subevent_le_connection_complete_get_peer_address(packet, peer_address);
            state = BT_HID_STATE_CONNECTED;
            advertise_pending = false;
        }
        return;
    }
    if (event == HCI_EVENT_DISCONNECTION_COMPLETE) {
        connection_handle = HCI_CON_HANDLE_INVALID;
        input_enabled = false;
        can_send = false;
        send_phase = 0;
        queue_head = queue_tail = queue_count = 0;
        state = BT_HID_STATE_ADVERTISING;
        advertise_pending = active;
        return;
    }
    if (event == SM_EVENT_JUST_WORKS_REQUEST) {
        confirm_handle = sm_event_just_works_request_get_handle(packet);
        confirm_pending = true;
        return;
    }
    if (event == SM_EVENT_PAIRING_COMPLETE) {
        if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
            sm_event_pairing_complete_get_address(packet, peer_address);
            state = BT_HID_STATE_BONDED;
        }
        return;
    }
    if (event == SM_EVENT_REENCRYPTION_COMPLETE) {
        if (sm_event_reencryption_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
            sm_event_reencryption_complete_get_address(packet, peer_address);
            state = BT_HID_STATE_BONDED;
        }
        return;
    }
    if (event == HCI_EVENT_HIDS_META) {
        switch (hci_event_hids_meta_get_subevent_code(packet)) {
            case HIDS_SUBEVENT_INPUT_REPORT_ENABLE:
                input_enabled = hids_subevent_input_report_enable_get_enable(packet) != 0;
                break;
            case HIDS_SUBEVENT_BOOT_KEYBOARD_INPUT_REPORT_ENABLE:
                input_enabled = hids_subevent_boot_keyboard_input_report_enable_get_enable(packet) != 0;
                break;
            case HIDS_SUBEVENT_PROTOCOL_MODE:
                protocol_mode = hids_subevent_protocol_mode_get_protocol_mode(packet);
                break;
            case HIDS_SUBEVENT_CAN_SEND_NOW:
                can_send = true;
                break;
            default:
                break;
        }
    }
}

static void set_advertising(bool enabled) {
    gap_advertisements_enable(enabled ? 1 : 0);
}

static void format_peer(void) {
    static const char hex[] = "0123456789ABCDEF";
    int i;
    for (i = 0; i < 6; i++) {
        peer_text[i * 3] = hex[peer_address[i] >> 4];
        peer_text[i * 3 + 1] = hex[peer_address[i] & 0x0f];
        if (i != 5) peer_text[i * 3 + 2] = ':';
    }
    peer_text[17] = '\0';
}

static void send_report(const bt_hid_report_t *report) {
    if (protocol_mode == 0) {
        hids_device_send_boot_keyboard_input_report(connection_handle,
                                                    (const uint8_t *)report, sizeof(*report));
    } else {
        hids_device_send_input_report_for_id(connection_handle, 1,
                                             (const uint8_t *)report, sizeof(*report));
    }
}

int bt_hid_pico_init(void) {
    bd_addr_t null_address = {0};
    uint16_t interval = 0x0030;

    /* Bonds live in the two flash sectors the SDK reserves for BTstack; without
     * this the host pairs again after every power cycle. */
    btstack_tlv_set_instance(btstack_tlv_flash_bank_init_instance(&tlv_context,
                                                                pico_flash_bank_instance(), NULL),
                             &tlv_context);
    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);
    att_server_init(profile_data, NULL, NULL);
    hids_device_init(0, hid_descriptor, sizeof(hid_descriptor));
    hids_device_register_packet_handler(packet_handler);
    gap_advertisements_set_params(interval, interval, 0, 0, null_address, 0x07, 0x00);
    gap_advertisements_set_data(sizeof(advertising_data), (uint8_t *)advertising_data);
    hci_registration.callback = packet_handler;
    hci_add_event_handler(&hci_registration);
    sm_registration.callback = packet_handler;
    sm_add_event_handler(&sm_registration);
    state = BT_HID_STATE_STARTING;
    if (hci_power_control(HCI_POWER_ON) != 0) {
        state = BT_HID_STATE_FAILED;
        return -1;
    }
    return 0;
}

void bt_hid_pico_fail(void) {
    state = BT_HID_STATE_FAILED;
}

void bt_hid_pico_enter(void) {
    active = true;
    if (state == BT_HID_STATE_ADVERTISING) advertise_pending = true;
}

void bt_hid_pico_leave(void) {
    active = false;
    advertise_pending = false;
    confirm_pending = false;
    can_send = false;
    set_advertising(false);
    if (connection_handle != HCI_CON_HANDLE_INVALID) gap_disconnect(connection_handle);
    send_phase = 0;
    queue_head = queue_tail = queue_count = 0;
}

void bt_hid_pico_send(const bt_hid_report_t *report) {
    if (!active || !input_enabled || queue_count == 8 || report == NULL) return;
    report_queue[queue_tail] = *report;
    queue_tail = (uint8_t)((queue_tail + 1) % 8);
    queue_count++;
}

void bt_hid_pico_poll(void) {
    static const bt_hid_report_t release_report;
    uint32_t now;

    cyw43_arch_poll();
    if (advertise_pending && connection_handle == HCI_CON_HANDLE_INVALID) {
        advertise_pending = false;
        set_advertising(true);
    }
    if (confirm_pending) {
        confirm_pending = false;
        sm_just_works_confirm(confirm_handle);
    }
    if (state == BT_HID_STATE_BONDED) format_peer();
    if (connection_handle == HCI_CON_HANDLE_INVALID) return;
    now = to_ms_since_boot(get_absolute_time());
    if (send_phase == 0 && queue_count != 0) {
        pending_report = report_queue[queue_head];
        queue_head = (uint8_t)((queue_head + 1) % 8);
        queue_count--;
        send_phase = 1;
    }
    if (send_phase == 1) {
        if (hids_device_request_can_send_now_event(connection_handle) == ERROR_CODE_SUCCESS) send_phase = 2;
    }
    if (send_phase == 2 && can_send) {
        can_send = false;
        send_report(&pending_report);
        release_at = now + 5u;
        send_phase = 3;
    }
    if (send_phase == 3 && (int32_t)(now - release_at) >= 0) {
        if (hids_device_request_can_send_now_event(connection_handle) == ERROR_CODE_SUCCESS) send_phase = 5;
    }
    if (send_phase == 5 && can_send) {
        can_send = false;
        send_report(&release_report);
        send_phase = 0;
    }
}

bt_hid_state_t bt_hid_pico_state(void) {
    return state;
}

const char *bt_hid_pico_peer(void) {
    return peer_text;
}
