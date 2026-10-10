#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "storage/screenshot.h"
#include "test_util.h"

static const char *const test_dir = ".";

static unsigned char file_bytes[4096];
static int rows_read;

static void clean(void) {
    char path[32];

    reset_rename();
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "./scr_%03d.bmp", i);
        remove(path);
        snprintf(path, sizeof(path), "./scr_%03d.bmp.tmp", i);
        remove(path);
    }
}

static size_t read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    size_t length;

    if (file == NULL) {
        return 0u;
    }
    length = fread(file_bytes, 1u, sizeof(file_bytes), file);
    fclose(file);
    return length;
}

static uint32_t le32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* Every byte names its row, column and channel, so misplaced data shows. */
static void pattern_row(int y, unsigned char *pixels, void *context) {
    uint32_t width = *(const uint32_t *)context;

    rows_read++;
    for (uint32_t x = 0u; x < width; x++) {
        pixels[x * 3u] = (unsigned char)(y * 16);
        pixels[x * 3u + 1u] = (unsigned char)x;
        pixels[x * 3u + 2u] = 0xc0u;
    }
}

static void check_row_size(void) {
    CHECK(screenshot_bmp_row_size(1u) == 4u);
    CHECK(screenshot_bmp_row_size(2u) == 8u);
    CHECK(screenshot_bmp_row_size(3u) == 12u);
    CHECK(screenshot_bmp_row_size(4u) == 12u);
    CHECK(screenshot_bmp_row_size(5u) == 16u);
    CHECK(screenshot_bmp_row_size(320u) == 960u);
}

static void check_header(void) {
    /* The 320x320 header the device writes, as the old take_screenshot laid it out. */
    static const uint8_t expected[SCREENSHOT_BMP_HEADER_SIZE] = {
        0x42, 0x4d,             /* 'BM' */
        0x36, 0xb0, 0x04, 0x00, /* file size 307254 */
        0, 0, 0, 0,
        54, 0, 0, 0,            /* offset to pixel data */
        40, 0, 0, 0,            /* info header size */
        0x40, 0x01, 0, 0,       /* width 320 */
        0x40, 0x01, 0, 0,       /* height 320 */
        1, 0,                   /* planes */
        24, 0,                  /* bits per pixel */
        0, 0, 0, 0,             /* no compression */
        0x00, 0xb0, 0x04, 0x00, /* image size 307200 */
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];

    memset(header, 0xaa, sizeof(header));
    screenshot_bmp_header(header, 320u, 320u);
    CHECK_BYTES_EQ(header, expected, sizeof(expected));

    /* Padding counts toward the sizes: 5 pixels make a 16-byte row. */
    screenshot_bmp_header(header, 5u, 3u);
    CHECK(le32(header + 2) == 54u + 48u);
    CHECK(le32(header + 18) == 5u);
    CHECK(le32(header + 22) == 3u);
    CHECK(le32(header + 34) == 48u);
}

static void check_arguments(void) {
    uint32_t width = 4u;

    clean();
    CHECK(screenshot_save(NULL, width, 2u, pattern_row, &width, NULL) == -1);
    CHECK(screenshot_save("", width, 2u, pattern_row, &width, NULL) == -1);
    CHECK(screenshot_save(test_dir, width, 2u, NULL, &width, NULL) == -1);
    CHECK(screenshot_save(test_dir, 0u, 2u, pattern_row, &width, NULL) == -1);
    CHECK(screenshot_save(test_dir, width, 0u, pattern_row, &width, NULL) == -1);
    CHECK(screenshot_save(test_dir, SCREENSHOT_MAX_WIDTH + 1u, 2u, pattern_row, &width, NULL) == -1);
    CHECK(!exists("./scr_000.bmp"));
    CHECK(!exists("./scr_000.bmp.tmp"));
}

/* The free-name search keeps its place between calls, so these run in order. */
static void check_save(void) {
    uint32_t width = 5u;
    char name[SCREENSHOT_NAME_CAPACITY] = "";
    FILE *taken;

    clean();

    /* A failed write leaves neither an image nor a temp file, and the name stays free. */
    rename_fail_all = 1;
    CHECK(screenshot_save(test_dir, width, 3u, pattern_row, &width, name) == -1);
    CHECK_STR_EQ(name, "");
    CHECK(!exists("./scr_000.bmp"));
    CHECK(!exists("./scr_000.bmp.tmp"));
    reset_rename();

    rows_read = 0;
    CHECK(screenshot_save(test_dir, width, 3u, pattern_row, &width, name) == 0);
    CHECK_STR_EQ(name, "scr_000.bmp");
    CHECK(rows_read == 3);
    CHECK(!exists("./scr_000.bmp.tmp"));
    CHECK(read_file("./scr_000.bmp") == 54u + 48u);
    CHECK(file_bytes[0] == 'B' && file_bytes[1] == 'M');
    CHECK(le32(file_bytes + 2) == 54u + 48u);
    CHECK(le32(file_bytes + 18) == 5u);
    CHECK(le32(file_bytes + 22) == 3u);

    /* Rows are stored bottom-up, each padded with zeros to 16 bytes. */
    for (uint32_t row = 0u; row < 3u; row++) {
        const unsigned char *bytes = file_bytes + 54u + row * 16u;
        uint32_t y = 2u - row;

        for (uint32_t x = 0u; x < width; x++) {
            CHECK(bytes[x * 3u] == y * 16u);
            CHECK(bytes[x * 3u + 1u] == x);
            CHECK(bytes[x * 3u + 2u] == 0xc0u);
        }
        CHECK(bytes[15] == 0u);
    }

    /* Names already on the card are skipped; a width of 4 needs no padding. */
    taken = fopen("./scr_001.bmp", "wb");
    CHECK(taken != NULL);
    if (taken != NULL) {
        fputs("mine", taken);
        fclose(taken);
    }
    width = 4u;
    rename_like_fat = 1;
    CHECK(screenshot_save(test_dir, width, 2u, pattern_row, &width, name) == 0);
    CHECK_STR_EQ(name, "scr_002.bmp");
    CHECK(read_file("./scr_002.bmp") == 54u + 24u);
    CHECK(file_bytes[54] == 16u);
    CHECK(file_bytes[54 + 12] == 0u);
    CHECK(read_file("./scr_001.bmp") == 4u);
    CHECK(read_file("./scr_000.bmp") == 54u + 48u);

    CHECK(screenshot_save(test_dir, width, 2u, pattern_row, &width, NULL) == 0);
    CHECK(exists("./scr_003.bmp"));
}

void test_screenshot(void) {
    check_row_size();
    check_header();
    check_arguments();
    check_save();
    clean();
}
