#include "ai/chat_layout.h"

size_t chat_layout_next_line(const char *text, size_t length, size_t offset, size_t columns,
                             size_t *line_length) {
    size_t limit;
    size_t last_space = 0u;
    int have_space = 0;
    size_t i;

    if (line_length != NULL) {
        *line_length = 0u;
    }
    if (text == NULL || offset >= length || columns == 0u) {
        return offset;
    }

    limit = length - offset < columns ? length : offset + columns;
    for (i = offset; i < limit; i++) {
        if (text[i] == '\n') {
            if (line_length != NULL) {
                *line_length = i - offset;
            }
            return i + 1u;
        }
        if (text[i] == ' ') {
            last_space = i;
            have_space = 1;
        }
    }

    if (limit == length) {
        /* The rest fits on this line. */
        if (line_length != NULL) {
            *line_length = length - offset;
        }
        return length;
    }
    if (text[limit] == '\n' || text[limit] == ' ') {
        /* A full line that ends exactly at a break. */
        if (line_length != NULL) {
            *line_length = columns;
        }
        return limit + 1u;
    }
    if (have_space && last_space > offset) {
        if (line_length != NULL) {
            *line_length = last_space - offset;
        }
        return last_space + 1u;
    }
    /* One unbroken word wider than the line: split it. */
    if (line_length != NULL) {
        *line_length = columns;
    }
    return limit;
}

size_t chat_layout_count_lines(const char *text, size_t length, size_t columns) {
    size_t offset = 0u;
    size_t lines = 0u;

    if (text == NULL || columns == 0u) {
        return 0u;
    }
    while (offset < length) {
        offset = chat_layout_next_line(text, length, offset, columns, NULL);
        lines++;
    }
    return lines;
}
