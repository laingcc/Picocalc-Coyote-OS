#include "storage/screenshot.h"
#include "storage/file_store.h"

#include <stdio.h>
#include <string.h>

static unsigned char row_buffer[SCREENSHOT_MAX_WIDTH * 3u];
static char shot_path[FILE_STORE_PATH_CAPACITY];

uint32_t screenshot_bmp_row_size(uint32_t width) {
    return (width * 3u + 3u) & ~3u;
}

static void put_le32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

void screenshot_bmp_header(uint8_t header[SCREENSHOT_BMP_HEADER_SIZE], uint32_t width, uint32_t height) {
    uint32_t image_size = screenshot_bmp_row_size(width) * height;

    memset(header, 0, SCREENSHOT_BMP_HEADER_SIZE);
    header[0] = 'B';
    header[1] = 'M';
    put_le32(header + 2, SCREENSHOT_BMP_HEADER_SIZE + image_size); /* file size */
    put_le32(header + 10, SCREENSHOT_BMP_HEADER_SIZE);             /* offset to pixel data */
    put_le32(header + 14, 40u);                                    /* info header size */
    put_le32(header + 18, width);
    put_le32(header + 22, height);
    header[26] = 1;                                                /* planes */
    header[28] = 24;                                               /* bits per pixel */
    put_le32(header + 34, image_size);
}

/* Aims shot_path at the first unused name; the search resumes where it left off. */
static int next_free_path(const char *dir, char *name) {
    static int next;

    for (; next < SCREENSHOT_MAX_FILES; next++) {
        FILE *taken;

        snprintf(name, SCREENSHOT_NAME_CAPACITY, "scr_%03d.bmp", next);
        if (file_store_path(shot_path, dir, name) != 0) {
            return -1;
        }
        taken = file_store_open(shot_path);
        if (taken == NULL) {
            return 0;
        }
        fclose(taken);
    }
    return -1;
}

int screenshot_save(const char *dir,
                    uint32_t width,
                    uint32_t height,
                    screenshot_read_row_t read_row,
                    void *context,
                    char *name) {
    static const uint8_t padding_bytes[3] = {0, 0, 0};
    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    char shot_name[SCREENSHOT_NAME_CAPACITY];
    size_t row_length = (size_t)width * 3u;
    size_t padding;
    int failed;
    FILE *f;

    if (read_row == NULL || width == 0u || width > SCREENSHOT_MAX_WIDTH || height == 0u ||
        height > INT32_MAX || next_free_path(dir, shot_name) != 0) {
        return -1;
    }
    padding = screenshot_bmp_row_size(width) - row_length;

    f = file_store_open_temp(shot_path);
    if (f == NULL) {
        return -1;
    }

    screenshot_bmp_header(header, width, height);
    failed = fwrite(header, 1u, sizeof(header), f) != sizeof(header);
    for (uint32_t y = height; y-- > 0u && !failed;) {
        read_row((int)y, row_buffer, context);
        failed = fwrite(row_buffer, 1u, row_length, f) != row_length ||
                 (padding > 0u && fwrite(padding_bytes, 1u, padding, f) != padding);
    }

    if (file_store_commit(shot_path, f, failed) != 0) {
        return -1;
    }
    if (name != NULL) {
        memcpy(name, shot_name, sizeof(shot_name));
    }
    return 0;
}
