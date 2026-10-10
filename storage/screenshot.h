#ifndef COYOTE_STORAGE_SCREENSHOT_H
#define COYOTE_STORAGE_SCREENSHOT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Screen capture to <dir>/scr_NNN.bmp as an uncompressed 24-bit BMP.
 *
 * Pixels come from a row callback rather than the LCD driver, so the module
 * builds for the host tests as well as the firmware.  The file is written
 * through storage/file_store: a failed capture leaves no partial image.
 * Scratch state is static: nothing allocates, nothing is reentrant.
 */

#define SCREENSHOT_BMP_HEADER_SIZE 54u
/* Widest capture; one row of it is the only pixel buffer. */
#define SCREENSHOT_MAX_WIDTH 320u
/* Names run scr_000.bmp .. scr_999.bmp. */
#define SCREENSHOT_MAX_FILES 1000
/* "scr_NNN.bmp" with its NUL. */
#define SCREENSHOT_NAME_CAPACITY 12u

/*
 * Fill pixels with row y (0 = top) as width 3-byte pixels in the byte order
 * the file stores them: blue, green, red.
 */
typedef void (*screenshot_read_row_t)(int y, unsigned char *pixels, void *context);

/* Bytes one row takes in the file: 3 per pixel, padded to a multiple of 4. */
uint32_t screenshot_bmp_row_size(uint32_t width);

/* The file and info headers of a width x height image, rows stored bottom-up. */
void screenshot_bmp_header(uint8_t header[SCREENSHOT_BMP_HEADER_SIZE], uint32_t width, uint32_t height);

/*
 * Capture into the first free scr_NNN.bmp in dir.  name (optional,
 * SCREENSHOT_NAME_CAPACITY bytes) receives the file name.  Returns 0, or -1
 * for a bad argument, no free name, or a failed write.
 */
int screenshot_save(const char *dir,
                    uint32_t width,
                    uint32_t height,
                    screenshot_read_row_t read_row,
                    void *context,
                    char *name);

#endif
