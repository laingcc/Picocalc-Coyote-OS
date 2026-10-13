/*
 * Host preview for the imported Muse avatar: renders each mood into a PPM so
 * the pixels can be eyeballed without flashing firmware.  Not part of the
 * firmware or the test suite; run it by hand:
 *
 *   cc -std=c11 -O1 -I avatars tools/avatar_preview.c avatars/muse_pixel.c -o /tmp/avatar_preview
 *   /tmp/avatar_preview > /tmp/avatar.ppm
 */
#include <stdio.h>
#include <stdint.h>

typedef struct {
    int mode;
    int mouth;
    int blink;
    uint32_t tick;
} preview_pose_t;

void muse_pixel_render(const void *pose);
void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);

#define SIZE 64
#define SCALE 4
#define MOODS 6

static const int mood_ids[MOODS] = {0, 1, 2, 3, 4, 5};
static const char *const mood_names[MOODS] = {"boot", "idle", "listening", "thinking", "speaking", "error"};

int main(void) {
    static uint16_t frame[MOODS][SIZE * SIZE];
    unsigned char row[MOODS * SCALE * SIZE * 3];
    const int width = SCALE * SIZE * MOODS;
    const int height = SCALE * SIZE;

    /* Render every mood once: the pose is per frame, so each is drawn in full
     * before any of it is written out. */
    for (int m = 0; m < MOODS; m++) {
        preview_pose_t pose = {mood_ids[m], mood_ids[m] == 4 ? 2 : 0, 0, (uint32_t)(m * 3)};
        muse_pixel_render(&pose);
        muse_pixel_scale(frame[m], SIZE, 0, SIZE, 0, SIZE);
    }

    printf("P6\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; y++) {
        for (int m = 0; m < MOODS; m++) {
            for (int x = 0; x < SCALE * SIZE; x++) {
                uint16_t pixel = frame[m][(y / SCALE) * SIZE + x / SCALE];
                unsigned int r = (pixel >> 11) & 0x1Fu;
                unsigned int g = (pixel >> 5) & 0x3Fu;
                unsigned int b = pixel & 0x1Fu;
                unsigned char *pixel_out = row + ((size_t)m * SCALE * SIZE + (size_t)x) * 3u;
                pixel_out[0] = (unsigned char)((r << 3) | (r >> 2));
                pixel_out[1] = (unsigned char)((g << 2) | (g >> 4));
                pixel_out[2] = (unsigned char)((b << 3) | (b >> 2));
            }
        }
        fwrite(row, 1, sizeof(row), stdout);
    }
    fprintf(stderr, "moods left to right: %s, %s, %s, %s, %s, %s\n", mood_names[0], mood_names[1],
            mood_names[2], mood_names[3], mood_names[4], mood_names[5]);
    return 0;
}
