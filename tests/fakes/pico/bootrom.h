#ifndef COYOTE_TEST_FAKE_PICO_BOOTROM_H
#define COYOTE_TEST_FAKE_PICO_BOOTROM_H

#include <stdint.h>

void reset_usb_boot(uint32_t gpio_activity_pin_mask, uint32_t disable_interface_mask);

#endif
