/*
 * The USB side of the SD-over-USB mode.
 *
 * Every SCSI transfer the class driver receives is answered from
 * storage/usb_disk, which owns the card; this file only translates between the
 * two and reports what the host is doing.  A card that is not in the mode is
 * reported as no medium, which is how a host is told to stop using the volume
 * before the card is taken back.
 */

#include <string.h>

#include "tusb.h"

#include "storage/usb_disk.h"
#include "usb/usb_msc.h"

static bool started;

void usb_msc_begin(void) {
    if (!started) {
        started = tusb_init();
        return; /* tusb_init connects the device */
    }
    /* Connecting again makes the host read the card afresh rather than reuse
     * what it worked out last time the mode ran. */
    tud_connect();
}

void usb_msc_end(void) {
    if (started) {
        tud_disconnect();
    }
}

void usb_msc_poll(void) {
    if (started) {
        tud_task();
    }
}

bool usb_msc_started(void) {
    return started;
}

/* READ10 and WRITE10 are the card's own moves, offset and all. */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun;
    return usb_disk_read(lba, offset, buffer, bufsize);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    return usb_disk_write(lba, offset, buffer, bufsize);
}

/* What the host shows for the drive.  The fields are fixed width and space
 * padded, as SCSI expects, so they are set exactly. */
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, "CoyoteOS", 8);
    memcpy(product_id, "PicoCalc SD card", 16);
    memcpy(product_rev, "1.0 ", 4);
}

/* No card in the mode means no medium: the host is told to let go of it. */
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return usb_disk_active() && usb_disk_block_count() > 0u;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = usb_disk_block_count();
    *block_size = USB_DISK_BLOCK_SIZE;
}

/* Writes go straight to the card, so there is nothing to flush here. */
bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return true;
}

/* The cable, as opposed to the volume: what the screen reports. */
void tud_mount_cb(void) {
    usb_disk_set_host_attached(true);
}

void tud_umount_cb(void) {
    usb_disk_set_host_attached(false);
}
