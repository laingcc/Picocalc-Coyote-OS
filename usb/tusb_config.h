#ifndef COYOTE_USB_TUSB_CONFIG_H
#define COYOTE_USB_TUSB_CONFIG_H

/*
 * The USB device the PicoCalc's card is handed to a computer through: mass
 * storage only, full speed, one interface and two bulk endpoints.
 *
 * The SDK's TinyUSB integration supplies the MCU and OS definitions, so this
 * file configures the device side alone.  USB is not brought up at boot: it is
 * initialized the first time the disk mode is entered, and the host only ever
 * sees the card while it is in that mode.
 */

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE)

#define CFG_TUD_MSC 1

/* The class driver moves whole blocks through this buffer, so it must be a
 * multiple of the block size; more means fewer round trips per transfer.  The
 * endpoint itself is 64 bytes, which is a full-speed device's maximum. */
#define CFG_TUD_MSC_EP_BUFSIZE 1024

#endif
