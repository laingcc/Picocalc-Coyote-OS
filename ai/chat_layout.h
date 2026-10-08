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

#endif
