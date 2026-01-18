#include <stdio.h>
#include <stdint.h>
#include "utils.h"
#include "lcdspi/lcdspi.h"
#include "pwm_sound/pwm_sound.h"

static unsigned char scr_buffer[LCD_WIDTH * 3];

void take_screenshot(const char* directory) {
    char filename[128];
    static int screen_count = 0;
    FILE *f = NULL;

    for (; screen_count < 1000; screen_count++) {
        sprintf(filename, "%s/scr_%03d.bmp", directory, screen_count);
        f = fopen(filename, "r");
        if (f) {
            fclose(f);
            continue;
        }
        break;
    }

    if (screen_count >= 1000) return;

    f = fopen(filename, "wb");
    if (!f) return;

    uint32_t width = LCD_WIDTH;
    uint32_t height = LCD_HEIGHT;
    int padding = (4 - (width * 3) % 4) % 4;
    uint32_t image_size = (width * 3 + padding) * height;
    uint32_t file_size = 54 + image_size;

    unsigned char header[54] = {
        0x42, 0x4D,             // Signature 'BM'
        (uint8_t)(file_size), (uint8_t)(file_size >> 8), (uint8_t)(file_size >> 16), (uint8_t)(file_size >> 24),
        0, 0, 0, 0,             // Reserved
        54, 0, 0, 0,            // Offset to pixel data
        40, 0, 0, 0,            // Header size
        (uint8_t)(width), (uint8_t)(width >> 8), (uint8_t)(width >> 16), (uint8_t)(width >> 24),
        (uint8_t)(height), (uint8_t)(height >> 8), (uint8_t)(height >> 16), (uint8_t)(height >> 24),
        1, 0,                   // Planes
        24, 0,                  // Bits per pixel
        0, 0, 0, 0,             // Compression (none)
        (uint8_t)(image_size), (uint8_t)(image_size >> 8), (uint8_t)(image_size >> 16), (uint8_t)(image_size >> 24),
        0, 0, 0, 0,             // X pixels per meter
        0, 0, 0, 0,             // Y pixels per meter
        0, 0, 0, 0,             // Total colors
        0, 0, 0, 0              // Important colors
    };

    fwrite(header, 1, 54, f);

    uint8_t padding_bytes[3] = {0, 0, 0};
    for (int y = height - 1; y >= 0; y--) {
        read_buffer_spi(0, y, width - 1, y, scr_buffer);
        fwrite(scr_buffer, 1, width * 3, f);
        if (padding > 0) {
            fwrite(padding_bytes, 1, padding, f);
        }
    }

    fclose(f);
}
