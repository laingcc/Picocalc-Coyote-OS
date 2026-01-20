#ifndef GIF_UTILS_H
#define GIF_UTILS_H

#include <stdbool.h>

void gif_recording_init(const char* directory);
void gif_recording_toggle();
void gif_recording_update();
bool gif_is_recording();

#endif // GIF_UTILS_H
