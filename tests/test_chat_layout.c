#include <string.h>

#include "ai/chat_layout.h"
#include "test_util.h"

/* Lay text out and join the lines with '|' so a case reads at a glance. */
static const char *wrap(const char *text, size_t columns) {
    static char out[512];
    size_t length = strlen(text), offset = 0u, used = 0u;
    int guard = 0;

    while (offset < length && guard++ < 100) {
        size_t line;
        size_t next = chat_layout_next_line(text, length, offset, columns, &line);
        if (next <= offset || line > columns || used + line + 2u > sizeof(out)) {
            return "<bad layout>";
        }
        if (used > 0u) {
            out[used++] = '|';
        }
        memcpy(out + used, text + offset, line);
        used += line;
        offset = next;
    }
    out[used] = '\0';
    return out;
}

static void check_wrapping(void) {
    CHECK_STR_EQ(wrap("hello", 10u), "hello");
    CHECK_STR_EQ(wrap("hello world", 11u), "hello world");
    CHECK_STR_EQ(wrap("hello world", 10u), "hello|world");
    CHECK_STR_EQ(wrap("hello world", 5u), "hello|world");
    CHECK_STR_EQ(wrap("hello world", 6u), "hello|world");
    CHECK_STR_EQ(wrap("the quick brown fox", 10u), "the quick|brown fox");
    CHECK_STR_EQ(wrap("the quick brown fox", 9u), "the quick|brown fox");
    CHECK_STR_EQ(wrap("the quick brown fox", 8u), "the|quick|brown|fox");

    /* A word longer than the line is split. */
    CHECK_STR_EQ(wrap("abcdefghij", 4u), "abcd|efgh|ij");
    CHECK_STR_EQ(wrap("ab cdefghij k", 4u), "ab|cdef|ghij|k");

    /* Newlines always break, and blank lines are kept. */
    CHECK_STR_EQ(wrap("a\nb", 10u), "a|b");
    CHECK_STR_EQ(wrap("a\n\nb", 10u), "a||b");
    CHECK_STR_EQ(wrap("abcd\nef", 4u), "abcd|ef");
    CHECK_STR_EQ(wrap("abc\n", 10u), "abc");
    CHECK_STR_EQ(wrap("x", 1u), "x");
    CHECK_STR_EQ(wrap("xy", 1u), "x|y");
}

static void check_counts(void) {
    CHECK(chat_layout_count_lines("", 0u, 10u) == 0u);
    CHECK(chat_layout_count_lines("hello", 5u, 10u) == 1u);
    CHECK(chat_layout_count_lines("hello world", 11u, 10u) == 2u);
    CHECK(chat_layout_count_lines("a\n\nb", 4u, 10u) == 3u);
    CHECK(chat_layout_count_lines("abc\n", 4u, 10u) == 1u);
    CHECK(chat_layout_count_lines("\n", 1u, 10u) == 1u);
    CHECK(chat_layout_count_lines("abcdefghij", 10u, 4u) == 3u);
    /* length, not the terminator, bounds the text. */
    CHECK(chat_layout_count_lines("abcdefghij", 4u, 4u) == 1u);
    CHECK(chat_layout_count_lines("abc", 3u, 0u) == 0u);
    CHECK(chat_layout_count_lines(NULL, 3u, 10u) == 0u);
}

static void check_edges(void) {
    size_t line = 99u;
    CHECK(chat_layout_next_line("abc", 3u, 3u, 10u, &line) == 3u);
    CHECK(line == 0u);
    line = 99u;
    CHECK(chat_layout_next_line("abc", 3u, 0u, 0u, &line) == 0u);
    CHECK(line == 0u);
    CHECK(chat_layout_next_line(NULL, 3u, 0u, 10u, &line) == 0u);
    /* line_length is optional. */
    CHECK(chat_layout_next_line("ab cd", 5u, 0u, 3u, NULL) == 3u);
}

/* Every byte is consumed exactly once and every line fits, for a long reply
 * at the transcript's real width. */
static void check_progress(void) {
    static char text[4096];
    for (size_t i = 0u; i < sizeof(text) - 1u; i++) {
        text[i] = (i % 97u) == 96u ? '\n' : (i % 7u) == 6u ? ' ' : (char)('a' + (i % 26u));
    }
    text[sizeof(text) - 1u] = '\0';

    for (size_t columns = 1u; columns <= 40u; columns += 13u) {
        size_t length = sizeof(text) - 1u, offset = 0u, lines = 0u;
        int ok = 1;
        while (offset < length) {
            size_t line;
            size_t next = chat_layout_next_line(text, length, offset, columns, &line);
            if (next <= offset || line > columns || next - offset > line + 1u || next > length) {
                ok = 0;
                break;
            }
            offset = next;
            lines++;
        }
        CHECK(ok == 1);
        CHECK(offset == length);
        CHECK(lines == chat_layout_count_lines(text, length, columns));
    }
}

void test_chat_layout(void) {
    check_wrapping();
    check_counts();
    check_edges();
    check_progress();
}
