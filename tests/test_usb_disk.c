/*
 * The card handed to a host over USB, driven against a card that is a buffer.
 *
 * The SCSI requests a host makes are what the callbacks see, so they are what
 * these checks use: whole blocks, several at once, and part of one block.
 */

#include <string.h>

#include "storage/usb_disk.h"
#include "test_util.h"

#define FAKE_BLOCKS 64u

typedef struct {
    blockdevice_t device;
    uint8_t bytes[FAKE_BLOCKS * USB_DISK_BLOCK_SIZE];
    unsigned reads, writes, syncs, inits;
    bool fail_init, fail_read, fail_write;
} fake_card_t;

static int fake_init(blockdevice_t *device) {
    fake_card_t *card = (fake_card_t *)device;
    card->inits++;
    if (card->fail_init) {
        return BD_ERROR_DEVICE_ERROR;
    }
    device->is_initialized = true;
    return BD_ERROR_OK;
}

static int fake_deinit(blockdevice_t *device) {
    device->is_initialized = false;
    return BD_ERROR_OK;
}

static int fake_sync(blockdevice_t *device) {
    ((fake_card_t *)device)->syncs++;
    return BD_ERROR_OK;
}

static int fake_read(blockdevice_t *device, const void *buffer, bd_size_t addr, bd_size_t size) {
    fake_card_t *card = (fake_card_t *)device;
    card->reads++;
    if (card->fail_read || size == 0u || addr + size > sizeof(card->bytes)) {
        return BD_ERROR_DEVICE_ERROR;
    }
    memcpy((void *)buffer, card->bytes + addr, (size_t)size);
    return BD_ERROR_OK;
}

static int fake_program(blockdevice_t *device, const void *buffer, bd_size_t addr, bd_size_t size) {
    fake_card_t *card = (fake_card_t *)device;
    card->writes++;
    if (card->fail_write || size == 0u || addr + size > sizeof(card->bytes)) {
        return BD_ERROR_DEVICE_ERROR;
    }
    memcpy(card->bytes + addr, buffer, (size_t)size);
    return BD_ERROR_OK;
}

static bd_size_t fake_size(blockdevice_t *device) {
    return sizeof(((fake_card_t *)device)->bytes);
}

static void card_make(fake_card_t *card) {
    memset(card, 0, sizeof(*card));
    card->device.init = fake_init;
    card->device.deinit = fake_deinit;
    card->device.sync = fake_sync;
    card->device.read = fake_read;
    card->device.program = fake_program;
    card->device.size = fake_size;
    card->device.read_size = USB_DISK_BLOCK_SIZE;
    card->device.program_size = USB_DISK_BLOCK_SIZE;
    card->device.name = "fake";
}

static void fill_block(uint8_t *block, uint8_t seed) {
    for (size_t i = 0u; i < USB_DISK_BLOCK_SIZE; i++) {
        block[i] = (uint8_t)(seed + i);
    }
}

/* Every block with the same contents, so any offset in any block is known. */
static void fill_card(fake_card_t *card, uint8_t seed) {
    for (size_t block = 0u; block < FAKE_BLOCKS; block++) {
        fill_block(card->bytes + block * USB_DISK_BLOCK_SIZE, seed);
    }
}

static void check_handover(void) {
    fake_card_t card;
    uint8_t buffer[USB_DISK_BLOCK_SIZE];

    card_make(&card);
    usb_disk_end();
    CHECK(!usb_disk_active());
    CHECK(usb_disk_block_count() == 0u);

    /* A card that was never initialized is brought up for the mode. */
    usb_disk_begin(&card.device);
    CHECK(card.inits == 1);
    CHECK(card.device.is_initialized);
    CHECK(usb_disk_active());
    CHECK(usb_disk_block_count() == FAKE_BLOCKS);
    CHECK(!usb_disk_host_attached());

    usb_disk_set_host_attached(true);
    CHECK(usb_disk_host_attached());

    usb_disk_end();
    CHECK(!usb_disk_active());
    CHECK(!usb_disk_host_attached());
    CHECK(card.syncs == 1); /* what the host wrote is on the card */
    CHECK(usb_disk_block_count() == 0u);

    /* A card that cannot be brought up leaves the mode off. */
    card.fail_init = true;
    card.device.is_initialized = false;
    usb_disk_begin(&card.device);
    CHECK(!usb_disk_active());
    CHECK(usb_disk_block_count() == 0u);

    /* Nothing is answered while no card is in the mode. */
    CHECK(usb_disk_read(0u, 0u, buffer, sizeof(buffer)) == -1);
    CHECK(usb_disk_write(0u, 0u, buffer, sizeof(buffer)) == -1);
}

static void check_whole_blocks(void) {
    fake_card_t card;
    uint8_t written[3u * USB_DISK_BLOCK_SIZE];
    uint8_t read_back[3u * USB_DISK_BLOCK_SIZE];

    card_make(&card);
    usb_disk_begin(&card.device);

    fill_block(written, 0x10);
    fill_block(written + USB_DISK_BLOCK_SIZE, 0x20);
    fill_block(written + 2u * USB_DISK_BLOCK_SIZE, 0x30);

    /* Three blocks in one request is three blocks in one card move. */
    CHECK(usb_disk_write(4u, 0u, written, sizeof(written)) == (int32_t)sizeof(written));
    CHECK(card.writes == 1u);
    CHECK(memcmp(card.bytes + 4u * USB_DISK_BLOCK_SIZE, written, sizeof(written)) == 0);
    CHECK(usb_disk_take_written());

    CHECK(usb_disk_read(4u, 0u, read_back, sizeof(read_back)) == (int32_t)sizeof(read_back));
    CHECK(card.reads == 1u);
    CHECK(memcmp(read_back, written, sizeof(written)) == 0);

    /* The last block is a block like any other. */
    fill_block(written, 0x40);
    CHECK(usb_disk_write(FAKE_BLOCKS - 1u, 0u, written, USB_DISK_BLOCK_SIZE) ==
          USB_DISK_BLOCK_SIZE);
    CHECK(memcmp(card.bytes + (FAKE_BLOCKS - 1u) * USB_DISK_BLOCK_SIZE, written,
                 USB_DISK_BLOCK_SIZE) == 0);

    usb_disk_end();
}

static void check_part_of_a_block(void) {
    fake_card_t card;
    uint8_t block[USB_DISK_BLOCK_SIZE];
    uint8_t piece[16];
    uint8_t read_back[16];

    card_make(&card);
    fill_card(&card, 0x55); /* the whole card has known contents */
    usb_disk_begin(&card.device);

    /* A read that starts and ends inside one block. */
    memset(read_back, 0, sizeof(read_back));
    CHECK(usb_disk_read(2u, 100u, read_back, sizeof(read_back)) == (int32_t)sizeof(read_back));
    CHECK(read_back[0] == (uint8_t)(0x55 + 100u));
    CHECK(read_back[15] == (uint8_t)(0x55 + 115u));

    /* A write of the same shape changes those bytes and nothing else: the rest
     * of the block is read back and written with them. */
    memset(piece, 0xA5, sizeof(piece));
    CHECK(usb_disk_write(2u, 496u, piece, sizeof(piece)) == (int32_t)sizeof(piece));
    CHECK(memcmp(card.bytes + 2u * USB_DISK_BLOCK_SIZE + 496u, piece, sizeof(piece)) == 0);
    CHECK(card.bytes[2u * USB_DISK_BLOCK_SIZE + 495u] == (uint8_t)(0x55 + 495u));
    CHECK(card.bytes[3u * USB_DISK_BLOCK_SIZE] == (uint8_t)(0x55 + 0u));

    /* The whole of one block still goes straight through. */
    fill_block(block, 0x77);
    CHECK(usb_disk_write(6u, 0u, block, USB_DISK_BLOCK_SIZE) == USB_DISK_BLOCK_SIZE);
    CHECK(usb_disk_read(6u, 0u, block, USB_DISK_BLOCK_SIZE) == USB_DISK_BLOCK_SIZE);
    CHECK(card.bytes[6u * USB_DISK_BLOCK_SIZE] == 0x77);

    usb_disk_end();
}

static void check_refusals(void) {
    fake_card_t card;
    uint8_t buffer[2u * USB_DISK_BLOCK_SIZE];

    card_make(&card);
    usb_disk_begin(&card.device);
    memset(buffer, 0x11, sizeof(buffer));

    /* Past the end of the card, in blocks and part way through the last one. */
    CHECK(usb_disk_read(FAKE_BLOCKS, 0u, buffer, USB_DISK_BLOCK_SIZE) == -1);
    CHECK(usb_disk_write(FAKE_BLOCKS - 1u, 0u, buffer, sizeof(buffer)) == -1);
    CHECK(usb_disk_read(FAKE_BLOCKS - 1u, 100u, buffer, 500u) == -1);

    /* A request that starts part way in and runs into the next block would
     * need a buffer this size of the request; the class driver never asks for
     * one, and it is refused rather than answered wrongly. */
    CHECK(usb_disk_read(1u, 200u, buffer, 400u) == -1);
    CHECK(usb_disk_write(1u, 1u, buffer, USB_DISK_BLOCK_SIZE) == -1);

    /* Empty, out of the block, and no buffer at all. */
    CHECK(usb_disk_read(1u, 0u, buffer, 0u) == -1);
    CHECK(usb_disk_read(1u, USB_DISK_BLOCK_SIZE, buffer, 1u) == -1);
    CHECK(usb_disk_read(1u, 0u, NULL, 1u) == -1);

    /* A card that fails leaves the mode alone and says so.  The failed write
     * was attempted, and changed nothing. */
    card.fail_read = true;
    CHECK(usb_disk_read(1u, 0u, buffer, USB_DISK_BLOCK_SIZE) == -1);
    card.fail_read = false;
    card.fail_write = true;
    CHECK(usb_disk_write(1u, 0u, buffer, USB_DISK_BLOCK_SIZE) == -1);
    CHECK(!usb_disk_take_written());
    card.fail_write = false;

    CHECK(card.writes == 1u); /* the attempt, not a write */
    CHECK(card.bytes[USB_DISK_BLOCK_SIZE] == 0x00u); /* the card is as it was */
    usb_disk_end();
}

static void check_written_flag(void) {
    fake_card_t card;
    uint8_t block[USB_DISK_BLOCK_SIZE];

    card_make(&card);
    memset(block, 0x33, sizeof(block));
    usb_disk_begin(&card.device);

    /* Reading is not writing. */
    CHECK(usb_disk_read(0u, 0u, block, USB_DISK_BLOCK_SIZE) == USB_DISK_BLOCK_SIZE);
    CHECK(!usb_disk_take_written());
    CHECK(!usb_disk_take_written());

    CHECK(usb_disk_write(0u, 0u, block, USB_DISK_BLOCK_SIZE) == USB_DISK_BLOCK_SIZE);
    CHECK(usb_disk_take_written());
    CHECK(!usb_disk_take_written()); /* read once, and once only */

    /* Ending the mode keeps the answer for the caller that ends it. */
    CHECK(usb_disk_write(1u, 0u, block, USB_DISK_BLOCK_SIZE) == USB_DISK_BLOCK_SIZE);
    usb_disk_end();
    CHECK(usb_disk_take_written());

    /* A card put in again starts out unwritten. */
    usb_disk_begin(&card.device);
    CHECK(!usb_disk_take_written());
    usb_disk_end();
}

static void check_another_card(void) {
    fake_card_t first;
    fake_card_t second;
    uint8_t buffer[USB_DISK_BLOCK_SIZE];

    card_make(&first);
    card_make(&second);
    memset(second.bytes, 0x99, sizeof(second.bytes));

    usb_disk_begin(&first.device);
    usb_disk_end();
    /* The host may have swapped the card while it had it: the next one in is
     * the one the mode answers from. */
    usb_disk_begin(&second.device);
    CHECK(usb_disk_block_count() == FAKE_BLOCKS);
    CHECK(usb_disk_read(FAKE_BLOCKS - 1u, 0u, buffer, USB_DISK_BLOCK_SIZE) ==
          USB_DISK_BLOCK_SIZE);
    CHECK(buffer[0] == 0x99);
    CHECK(first.reads == 0u);
    usb_disk_end();
}

void test_usb_disk(void) {
    check_handover();
    check_whole_blocks();
    check_part_of_a_block();
    check_refusals();
    check_written_flag();
    check_another_card();
}
