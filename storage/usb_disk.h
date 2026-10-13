#ifndef COYOTE_STORAGE_USB_DISK_H
#define COYOTE_STORAGE_USB_DISK_H

#include <stdbool.h>
#include <stdint.h>

#include "blockdevice/blockdevice.h"

/*
 * The card, handed to a host over USB.
 *
 * While this is active the host owns the card's sectors: the firmware must not
 * touch its filesystem, and the caller unmounts before usb_disk_begin and
 * mounts again after usb_disk_end.  Nothing here blocks, allocates or prints,
 * and only the block device interface is used, so the module builds for the
 * host tests as well as the firmware with a test double for the card.
 *
 * The read and write calls take the byte range a SCSI READ10 or WRITE10 asks
 * for: a whole number of blocks from the start of one, or part of a single
 * block.  Anything else is refused, since answering it would need a buffer the
 * size of the request.
 */

/* SD and MMC cards move in 512-byte blocks, and so does USB mass storage. */
#define USB_DISK_BLOCK_SIZE 512u

/* Hand the card over.  A device that is not initialized is initialized here.
 * Calling it again while the mode is active changes nothing. */
void usb_disk_begin(blockdevice_t *device);

/* Take the card back and flush what the host wrote.  The caller initializes
 * and mounts it again: the host may have swapped the card out. */
void usb_disk_end(void);

bool usb_disk_active(void);

/* Blocks the host may address, or 0 when no card is in the mode. */
uint32_t usb_disk_block_count(void);

/*
 * Read or write up to one block's worth of bytes at block lba, starting
 * offset bytes into it.  Returns the number of bytes moved, or -1 when the
 * range is outside the card, is longer than the contract above allows, or the
 * card failed.
 */
int32_t usb_disk_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t size);
int32_t usb_disk_write(uint32_t lba, uint32_t offset, const void *buffer, uint32_t size);

/* True once after the host has written to the card, so the caller knows the
 * filesystem it left behind on it is stale. */
bool usb_disk_take_written(void);

/* What the host is doing with the card, for the screen that shows it. */
void usb_disk_set_host_attached(bool attached);
bool usb_disk_host_attached(void);

#endif
