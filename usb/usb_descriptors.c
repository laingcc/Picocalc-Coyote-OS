/*
 * Descriptors for the USB disk the card is offered as.
 *
 * A mass-storage device with one interface and two bulk endpoints, which is
 * all a computer needs to mount the card as a drive.  The serial number is the
 * board's own, so two PicoCalcs plugged into the same machine are told apart.
 */

#include "tusb.h"
#include "pico/unique_id.h"

/* TinyUSB's example vendor id, used by hobby firmware that has none of its
 * own: a product would register one. */
#define USB_VID 0xCAFE
#define USB_PID 0x4D53 /* "MS" */
#define USB_BCD 0x0200

/* Strings are indexed from 1; 0 is the language list. */
enum {
    STRING_LANGUAGE = 0,
    STRING_MANUFACTURER,
    STRING_PRODUCT,
    STRING_SERIAL,
    STRING_COUNT
};

#define USB_MANUFACTURER "Coyote OS"
#define USB_PRODUCT "PicoCalc SD card"

/* The card is the only thing this device does, so its class lives there. */
static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRING_MANUFACTURER,
    .iProduct = STRING_PRODUCT,
    .iSerialNumber = STRING_SERIAL,
    .bNumConfigurations = 0x01
};

enum { ITF_NUM_MSC = 0, ITF_NUM_TOTAL };

#define EPNUM_MSC_OUT 0x03
#define EPNUM_MSC_IN 0x83

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t desc_configuration[] = {
    /* Config number, interface count, string index, total length, attributes,
     * power in mA. */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    /* Interface number, string index, endpoint addresses, endpoint size. */
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64)
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index; /* One configuration. */
    return desc_configuration;
}

/* UTF-16, as the USB string descriptors are. */
static uint16_t string_buffer[32];

/* Copy text into the string buffer and return it, with its header. */
static uint16_t const *describe(uint8_t index, const char *text) {
    size_t length = strlen(text);
    if (length > (sizeof(string_buffer) / sizeof(string_buffer[0])) - 1u) {
        length = (sizeof(string_buffer) / sizeof(string_buffer[0])) - 1u;
    }
    string_buffer[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * length + 2u));
    for (size_t i = 0u; i < length; i++) {
        string_buffer[1u + i] = (uint16_t)text[i];
    }
    return string_buffer;
}

/* The board's unique id, in hex, so a host can tell two of them apart. */
static uint16_t const *describe_serial(void) {
    static char hex[2u * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1u];
    static bool filled = false;

    if (!filled) {
        pico_unique_board_id_t id;
        static const char digits[] = "0123456789abcdef";
        pico_get_unique_board_id(&id);
        for (size_t i = 0u; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
            hex[2u * i] = digits[(id.id[i] >> 4) & 0x0fu];
            hex[2u * i + 1u] = digits[id.id[i] & 0x0fu];
        }
        hex[sizeof(hex) - 1u] = '\0';
        filled = true;
    }
    return describe(STRING_SERIAL, hex);
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    switch (index) {
        case STRING_LANGUAGE:
            string_buffer[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4u);
            string_buffer[1] = 0x0409u; /* English, United States. */
            return string_buffer;
        case STRING_MANUFACTURER:
            return describe(index, USB_MANUFACTURER);
        case STRING_PRODUCT:
            return describe(index, USB_PRODUCT);
        case STRING_SERIAL:
            return describe_serial();
        default:
            return NULL;
    }
}
