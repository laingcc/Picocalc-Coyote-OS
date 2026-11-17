#ifndef COYOTE_AI_CHAT_LAYOUT_H
#define COYOTE_AI_CHAT_LAYOUT_H

#include <stddef.h>

/*
 * Word wrapping for the chat transcript.  Pure text arithmetic: no drawing,
 * no allocation, and the text is never modified or copied.
 *
 * A display line ends at a newline, or at the last space that keeps it within
 * the column limit; a word longer than a whole line is split.  The newline or
 * the space a line was broken at is consumed and not part of either line.
 */

/*
 * Lay out one display line starting at text[offset].  Stores the number of
 * bytes to draw (at most columns) in *line_length and returns the offset the
 * next line starts at, which is always greater than offset.  Returns offset
 * unchanged, with *line_length 0, when offset >= length or columns is 0.
 */
size_t chat_layout_next_line(const char *text, size_t length, size_t offset, size_t columns,
                             size_t *line_length);

/* Number of display lines text occupies; 0 for empty text. */
size_t chat_layout_count_lines(const char *text, size_t length, size_t columns);

/*
 * Columns a transcript line may use: the full width, less the cells the
 * imported Muse avatar occupies when it is drawn beside the chat.  Only the
 * "muse" provider narrows, and only when an avatar is compiled in.
 */
#define CHAT_LAYOUT_TEXT_COLUMNS 38u
#define CHAT_LAYOUT_AVATAR_CELLS 8u
#define CHAT_LAYOUT_AVATAR_FRAME_MS 100u /* at most ten frames a second */

size_t chat_layout_transcript_columns(const char *provider, int avatar_available);

/* 1 when the avatar's next animation frame is due, 0 when it should wait. */
int chat_layout_avatar_refresh_due(unsigned long now_ms, unsigned long last_draw_ms);

#endif
