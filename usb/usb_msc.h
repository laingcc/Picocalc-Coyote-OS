#ifndef COYOTE_USB_MSC_H
#define COYOTE_USB_MSC_H

#include <stdbool.h>

/*
 * USB mass storage, wrapped around storage/usb_disk.
 *
 * The host only ever sees the card while the disk mode is running:
 *
 *   usb_disk_begin(card);   the card is handed over
 *   usb_msc_begin();        the device appears, and the host mounts it
 *   ... usb_msc_poll() from the main loop for as long as the mode lasts ...
 *   usb_msc_end();          the device leaves, so the host drops its cache
 *   usb_disk_end();         the card is taken back
 *
 * The USB stack is brought up on the first use and then stays up, idle, so
 * entering the mode again is immediate.
 */

void usb_msc_begin(void);

/* Disconnect, so the host does not keep what it cached about the card. */
void usb_msc_end(void);

/* Run the USB stack.  Call it often while the mode is active: the transfers
 * are serviced from here. */
void usb_msc_poll(void);

bool usb_msc_started(void);

#endif
