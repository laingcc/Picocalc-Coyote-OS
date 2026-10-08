#include "ai/http_parser.h"

#include <stdint.h>
#include <string.h>

/*
 * No heap allocation and no variable length arrays.  Shared working storage
 * lives inside http_parser_t; helper scratch is fixed size on the stack.
 */

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool ci_equal(const char *s, size_t length, const char *literal) {
    size_t i = 0;
    for (; i < length; i++) {
        char b = literal[i];
        if (b == '\0') {
            return false;
        }
        if (ascii_lower(s[i]) != ascii_lower(b)) {
            return false;
        }
    }
    return literal[i] == '\0';
}

static bool ci_contains(const char *s, size_t length, const char *literal) {
    size_t n = strlen(literal);
    if (n == 0u || n > length) {
        return false;
    }
    for (size_t i = 0; i + n <= length; i++) {
        bool match = true;
        for (size_t j = 0; j < n; j++) {
            if (ascii_lower(s[i + j]) != ascii_lower(literal[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

static int parse_decimal(const char *s, size_t length, size_t *out) {
    if (length == 0u) {
        return -1;
    }
    size_t value = 0u;
    for (size_t i = 0; i < length; i++) {
        if (!is_digit(s[i])) {
            return -1;
        }
        size_t digit = (size_t)(s[i] - '0');
        if (value > (SIZE_MAX - digit) / 10u) {
            return -1;
        }
        value = value * 10u + digit;
    }
    *out = value;
    return 0;
}

/* Parse a chunk-size token, accepting and discarding an optional extension. */
static int parse_chunk_size(const char *s, size_t length, size_t *out) {
    size_t start = 0u;
    while (start < length && (s[start] == ' ' || s[start] == '\t')) {
        start++;
    }
    size_t end = length;
    for (size_t i = start; i < end; i++) {
        if (s[i] == ';') {
            end = i;
            break;
        }
    }
    while (end > start && (s[end - 1u] == ' ' || s[end - 1u] == '\t' || s[end - 1u] == '\r')) {
        end--;
    }
    if (end == start) {
        return -1;
    }
    size_t value = 0u;
    for (size_t i = start; i < end; i++) {
        int digit = hex_value(s[i]);
        if (digit < 0) {
            return -1;
        }
        if (value > (SIZE_MAX - (size_t)digit) / 16u) {
            return -1;
        }
        value = value * 16u + (size_t)digit;
    }
    *out = value;
    return 0;
}

static int parse_head(http_parser_t *parser) {
    const char *base = parser->header;
    size_t total = parser->header_length;

    const char *nl = memchr(base, '\n', total);
    if (nl == NULL) {
        return -1;
    }
    size_t line_length = (size_t)(nl - base);
    while (line_length > 0u && base[line_length - 1u] == '\r') {
        line_length--;
    }
    if (line_length < 12u || memcmp(base, "HTTP/", 5u) != 0) {
        return -1;
    }
    const char *space = memchr(base, ' ', line_length);
    if (space == NULL) {
        return -1;
    }
    size_t code_offset = (size_t)(space - base) + 1u;
    if (code_offset + 3u > line_length) {
        return -1;
    }
    if (!is_digit(base[code_offset]) || !is_digit(base[code_offset + 1u]) || !is_digit(base[code_offset + 2u])) {
        return -1;
    }
    if (code_offset + 3u < line_length && base[code_offset + 3u] != ' ') {
        return -1;
    }
    parser->head.status_code = (base[code_offset] - '0') * 100 + (base[code_offset + 1u] - '0') * 10 +
                               (base[code_offset + 2u] - '0');

    const char *cursor = nl + 1;
    const char *end = base + total;
    bool saw_content_length = false;
    while (cursor < end) {
        const char *line_nl = memchr(cursor, '\n', (size_t)(end - cursor));
        if (line_nl == NULL) {
            return -1;
        }
        size_t length = (size_t)(line_nl - cursor);
        while (length > 0u && cursor[length - 1u] == '\r') {
            length--;
        }
        if (length == 0u) {
            break; /* blank line terminates the header block */
        }
        if (cursor[0] == ' ' || cursor[0] == '\t') {
            return -1; /* obsolete line folding */
        }
        const char *colon = memchr(cursor, ':', length);
        if (colon == NULL) {
            return -1;
        }
        size_t name_length = (size_t)(colon - cursor);
        if (name_length == 0u) {
            return -1;
        }
        const char *value = colon + 1;
        size_t value_length = length - name_length - 1u;
        while (value_length > 0u && (*value == ' ' || *value == '\t')) {
            value++;
            value_length--;
        }
        while (value_length > 0u && (value[value_length - 1u] == ' ' || value[value_length - 1u] == '\t')) {
            value_length--;
        }
        if (ci_equal(cursor, name_length, "content-length")) {
            size_t parsed;
            if (saw_content_length || parse_decimal(value, value_length, &parsed) != 0) {
                return -1;
            }
            parser->head.content_length = parsed;
            parser->head.has_content_length = true;
            saw_content_length = true;
        } else if (ci_equal(cursor, name_length, "transfer-encoding")) {
            if (ci_contains(value, value_length, "chunked")) {
                parser->head.chunked = true;
            }
        } else if (ci_equal(cursor, name_length, "connection")) {
            if (ci_contains(value, value_length, "close")) {
                parser->head.connection_close = true;
            }
        }
        cursor = line_nl + 1;
    }
    return 0;
}

static void parser_complete(http_parser_t *parser) {
    parser->complete = true;
    parser->state = HTTP_PARSER_STATE_COMPLETE;
}

static int parser_fail(http_parser_t *parser) {
    parser->failed = true;
    parser->state = HTTP_PARSER_STATE_ERROR;
    return -1;
}

static void emit_body(http_parser_t *parser, const char *data, size_t length) {
    if (parser->on_body != NULL && length > 0u) {
        parser->on_body(parser->context, data, length);
    }
}

void http_parser_init(http_parser_t *parser, http_head_callback_t on_head, http_body_callback_t on_body, void *context) {
    memset(parser, 0, sizeof(*parser));
    parser->state = HTTP_PARSER_STATE_HEADERS;
    parser->on_head = on_head;
    parser->on_body = on_body;
    parser->context = context;
}

int http_parser_feed(http_parser_t *parser, const char *data, size_t length) {
    if (parser->failed) {
        return -1;
    }
    size_t i = 0u;
    while (i < length) {
        switch (parser->state) {
            case HTTP_PARSER_STATE_HEADERS: {
                char c = data[i++];
                if (parser->header_length >= HTTP_PARSER_HEADER_MAX) {
                    return parser_fail(parser);
                }
                parser->header[parser->header_length++] = c;
                if (parser->header_length >= 4u && parser->header[parser->header_length - 4u] == '\r' &&
                    parser->header[parser->header_length - 3u] == '\n' &&
                    parser->header[parser->header_length - 2u] == '\r' &&
                    parser->header[parser->header_length - 1u] == '\n') {
                    parser->header[parser->header_length] = '\0';
                    if (parse_head(parser) != 0) {
                        return parser_fail(parser);
                    }
                    if (parser->on_head != NULL) {
                        parser->on_head(parser->context, &parser->head);
                    }
                    if (parser->head.chunked) {
                        parser->state = HTTP_PARSER_STATE_CHUNK_SIZE;
                    } else if (parser->head.has_content_length) {
                        parser->body_remaining = parser->head.content_length;
                        if (parser->body_remaining == 0u) {
                            parser_complete(parser);
                        } else {
                            parser->state = HTTP_PARSER_STATE_BODY_LENGTH;
                        }
                    } else {
                        parser->state = HTTP_PARSER_STATE_CLOSE;
                    }
                }
                break;
            }
            case HTTP_PARSER_STATE_BODY_LENGTH: {
                size_t available = length - i;
                size_t take = available < parser->body_remaining ? available : parser->body_remaining;
                emit_body(parser, data + i, take);
                parser->body_remaining -= take;
                i += take;
                if (parser->body_remaining == 0u) {
                    parser_complete(parser);
                }
                break;
            }
            case HTTP_PARSER_STATE_CHUNK_SIZE: {
                char c = data[i++];
                if (c == '\n') {
                    size_t size;
                    if (parse_chunk_size(parser->chunk_line, parser->chunk_line_length, &size) != 0) {
                        return parser_fail(parser);
                    }
                    if (size > HTTP_PARSER_CHUNK_MAX) {
                        return parser_fail(parser);
                    }
                    parser->chunk_line_length = 0u;
                    if (size == 0u) {
                        parser->state = HTTP_PARSER_STATE_TRAILER;
                    } else {
                        parser->chunk_remaining = size;
                        parser->state = HTTP_PARSER_STATE_CHUNK_DATA;
                    }
                } else {
                    if (parser->chunk_line_length >= HTTP_PARSER_CHUNK_LINE_MAX) {
                        return parser_fail(parser);
                    }
                    parser->chunk_line[parser->chunk_line_length++] = c;
                }
                break;
            }
            case HTTP_PARSER_STATE_CHUNK_DATA: {
                size_t available = length - i;
                size_t take = available < parser->chunk_remaining ? available : parser->chunk_remaining;
                emit_body(parser, data + i, take);
                parser->chunk_remaining -= take;
                i += take;
                if (parser->chunk_remaining == 0u) {
                    parser->state = HTTP_PARSER_STATE_CHUNK_END;
                }
                break;
            }
            case HTTP_PARSER_STATE_CHUNK_END: {
                char c = data[i++];
                if (c == '\n') {
                    size_t length_seen = parser->chunk_line_length;
                    while (length_seen > 0u && parser->chunk_line[length_seen - 1u] == '\r') {
                        length_seen--;
                    }
                    if (length_seen != 0u) {
                        return parser_fail(parser);
                    }
                    parser->chunk_line_length = 0u;
                    parser->state = HTTP_PARSER_STATE_CHUNK_SIZE;
                } else {
                    if (parser->chunk_line_length >= HTTP_PARSER_CHUNK_LINE_MAX) {
                        return parser_fail(parser);
                    }
                    parser->chunk_line[parser->chunk_line_length++] = c;
                }
                break;
            }
            case HTTP_PARSER_STATE_TRAILER: {
                char c = data[i++];
                if (c == '\n') {
                    size_t length_seen = parser->chunk_line_length;
                    while (length_seen > 0u && parser->chunk_line[length_seen - 1u] == '\r') {
                        length_seen--;
                    }
                    parser->chunk_line_length = 0u;
                    if (length_seen == 0u) {
                        parser_complete(parser);
                    }
                } else {
                    if (parser->chunk_line_length >= HTTP_PARSER_CHUNK_LINE_MAX) {
                        return parser_fail(parser);
                    }
                    parser->chunk_line[parser->chunk_line_length++] = c;
                }
                break;
            }
            case HTTP_PARSER_STATE_CLOSE:
                emit_body(parser, data + i, length - i);
                i = length;
                break;
            case HTTP_PARSER_STATE_COMPLETE:
                i = length;
                break;
            case HTTP_PARSER_STATE_ERROR:
            default:
                return parser_fail(parser);
        }
    }
    return 0;
}

int http_parser_finish(http_parser_t *parser) {
    if (parser->failed) {
        return -1;
    }
    if (parser->state == HTTP_PARSER_STATE_COMPLETE) {
        return 0;
    }
    if (parser->state == HTTP_PARSER_STATE_CLOSE) {
        parser_complete(parser);
        return 0;
    }
    return parser_fail(parser);
}

bool http_parser_complete(const http_parser_t *parser) {
    return parser->complete;
}

bool http_parser_failed(const http_parser_t *parser) {
    return parser->failed;
}
