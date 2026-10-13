/*
 * The card, handed to a host over USB.
 *
 * The work here is turning what SCSI asks for - a run of blocks, or the tail
 * of one - into the block device's moves, and keeping the host's writes out of
 * the firmware's way.  See storage/usb_disk.h for the contract.
 */

#include <string.h>

#include "storage/usb_disk.h"

static blockdevice_t *card;
static uint32_t block_count;
static bool written;
static bool attached;

/* The card moves in whole blocks, so a part-block write goes through here. */
static uint8_t scratch[USB_DISK_BLOCK_SIZE];

/* The byte address a request starts at, when the request is one this module
 * can answer without a buffer as big as the request. */
static bool request_range(uint32_t lba, uint32_t offset, uint32_t size, bd_size_t *address) {
    uint64_t first = (uint64_t)lba * USB_DISK_BLOCK_SIZE + offset;
    uint64_t last = first + size;

    if (card == NULL || size == 0u || offset >= USB_DISK_BLOCK_SIZE) {
        return false;
    }
    if (lba >= block_count || last > (uint64_t)block_count * USB_DISK_BLOCK_SIZE) {
        return false;
    }
    /* A request may start part way into a block only if it ends in that same
     * block: a run of blocks starts at one of their beginnings. */
    if (size > USB_DISK_BLOCK_SIZE - offset && (offset != 0u || size % USB_DISK_BLOCK_SIZE != 0u)) {
        return false;
    }
    *address = (bd_size_t)first;
    return true;
}

static int32_t transfer(bool write, uint32_t lba, uint32_t offset, void *buffer, uint32_t size) {
    bd_size_t address;
    bd_size_t block_address;

    if (buffer == NULL || !request_range(lba, offset, size, &address)) {
        return -1;
    }
    if (offset == 0u && size % USB_DISK_BLOCK_SIZE == 0u) {
        int result = write ? card->program(card, buffer, address, size)
                           : card->read(card, buffer, address, size);
        if (result != BD_ERROR_OK) {
            return -1;
        }
        written = written || write;
        return (int32_t)size;
    }

    block_address = (bd_size_t)lba * USB_DISK_BLOCK_SIZE;
    if (card->read(card, scratch, block_address, USB_DISK_BLOCK_SIZE) != BD_ERROR_OK) {
        return -1;
    }
    if (write) {
        memcpy(scratch + offset, buffer, size);
        if (card->program(card, scratch, block_address, USB_DISK_BLOCK_SIZE) != BD_ERROR_OK) {
            return -1;
        }
        written = true;
    } else {
        memcpy(buffer, scratch + offset, size);
    }
    return (int32_t)size;
}

void usb_disk_begin(blockdevice_t *device) {
    if (device == NULL) {
        return;
    }
    card = NULL;
    written = false;
    attached = false;
    block_count = 0u;
    if (!device->is_initialized && (device->init == NULL || device->init(device) != BD_ERROR_OK)) {
        return;
    }
    if (device->size == NULL) {
        return;
    }
    card = device;
    block_count = (uint32_t)(device->size(device) / USB_DISK_BLOCK_SIZE);
}

void usb_disk_end(void) {
    if (card != NULL && card->sync != NULL) {
        card->sync(card);
    }
    card = NULL;
    block_count = 0u;
    attached = false;
    /* Whether the host wrote is kept: the caller has not read it yet. */
    memset(scratch, 0, sizeof(scratch));
}

bool usb_disk_active(void) {
    return card != NULL;
}

uint32_t usb_disk_block_count(void) {
    return block_count;
}

int32_t usb_disk_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t size) {
    return transfer(false, lba, offset, buffer, size);
}

int32_t usb_disk_write(uint32_t lba, uint32_t offset, const void *buffer, uint32_t size) {
    return transfer(true, lba, offset, (void *)buffer, size);
}

bool usb_disk_take_written(void) {
    bool was = written;
    written = false;
    return was;
}

void usb_disk_set_host_attached(bool is_attached) {
    attached = is_attached;
}

bool usb_disk_host_attached(void) {
    return attached;
}
