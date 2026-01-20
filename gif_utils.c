#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "gif_utils.h"
#include "gifenc/gifenc.h"
#include "lcdspi/lcdspi.h"
#include "pwm_sound/pwm_sound.h"

// Compatibility layer for gifenc which uses POSIX IO
#include <fcntl.h>
#include <unistd.h>

static FILE* gif_file = NULL;

int creat(const char *path, mode_t mode) {
    gif_file = fopen(path, "wb");
    if (gif_file) return 42; // Return a dummy file descriptor
    return -1;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (fd == 42 && gif_file) {
        return fwrite(buf, 1, count, gif_file);
    }
    return -1;
}

int close(int fd) {
    if (fd == 42 && gif_file) {
        fclose(gif_file);
        gif_file = NULL;
        return 0;
    }
    return -1;
}

static bool recording = false;
static ge_GIF *gif = NULL;
static char base_directory[64];
static uint32_t last_frame_time_ms = 0;
static uint8_t line_buffer[LCD_WIDTH * 3];

// Coyote OS-inspired 16-color palette
static uint8_t coyote_palette[] = {
    0x00, 0x00, 0x00, // BLACK
    0xFF, 0xFF, 0xFF, // WHITE
    0x80, 0x80, 0x80, // GRAY
    0xFF, 0x00, 0x00, // RED
    0x00, 0xFF, 0x00, // GREEN
    0x00, 0x00, 0xFF, // BLUE
    0xFF, 0xFF, 0x00, // YELLOW
    0xFF, 0x00, 0xFF, // MAGENTA
    0x00, 0xFF, 0xFF, // CYAN
    0xFF, 0xA5, 0x00, // ORANGE
    0xFF, 0xA0, 0xAB, // PINK
    0xFF, 0xD7, 0x00, // GOLD
    0xA5, 0x2A, 0x2A, // BROWN
    0x80, 0x00, 0x80, // PURPLE
    0xD2, 0xD2, 0xD2, // LITEGRAY
    0x00, 0x80, 0x00  // MIDGREEN
};

static uint8_t find_nearest_color(uint8_t r, uint8_t g, uint8_t b) {
    int min_dist = 0x7FFFFFFF;
    uint8_t best_idx = 0;
    for (int i = 0; i < 16; i++) {
        int dr = (int)r - coyote_palette[i*3];
        int dg = (int)g - coyote_palette[i*3+1];
        int db = (int)b - coyote_palette[i*3+2];
        int dist = dr*dr + dg*dg + db*db;
        if (dist < min_dist) {
            min_dist = dist;
            best_idx = i;
        }
    }
    return best_idx;
}

void gif_recording_init(const char* directory) {
    strncpy(base_directory, directory, sizeof(base_directory) - 1);
}

void gif_recording_toggle() {
    if (!recording) {
        char filename[128];
        static int gif_count = 0;

        for (; gif_count < 100; gif_count++) {
            sprintf(filename, "%s/rec_%02d.gif", base_directory, gif_count);
            FILE *f = fopen(filename, "r");
            if (f) {
                fclose(f);
                continue;
            }
            break;
        }

        if (gif_count >= 100) return;

        gif = ge_new_gif(filename, LCD_WIDTH, LCD_HEIGHT, coyote_palette, 4, -1, 0);
        if (gif) {
            recording = true;
            last_frame_time_ms = to_ms_since_boot(get_absolute_time());
            sound_play(SND_BEEP);
        }
    } else {
        if (gif) {
            ge_close_gif(gif);
            gif = NULL;
        }
        recording = false;
        sound_play(SND_BEEP);
    }
}

void gif_recording_update() {
    if (!recording || !gif) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_frame_time_ms < 200) return; // 5 FPS max

    // Capture frame
    for (int y = 0; y < LCD_HEIGHT; y++) {
        read_buffer_spi(0, y, LCD_WIDTH - 1, y, line_buffer);
        for (int x = 0; x < LCD_WIDTH; x++) {
            uint8_t r = line_buffer[x*3];
            uint8_t g = line_buffer[x*3+1];
            uint8_t b = line_buffer[x*3+2];
            gif->frame[y * LCD_WIDTH + x] = find_nearest_color(r, g, b);
        }
    }

    ge_add_frame(gif, (now - last_frame_time_ms) / 10);
    last_frame_time_ms = now;
}

bool gif_is_recording() {
    return recording;
}
