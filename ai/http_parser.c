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

/* RFC 7230 tchar. */
static bool is_tchar(char c) {
    if (c >= '0' && c <= '9') {
        return true;
    }
    if (c >= 'a' && c <= 'z') {
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        return true;
    }
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

static bool is_token(const char *s, size_t length) {
    if (length == 0u) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (!is_tchar(s[i])) {
            return false;
        }
    }
    return true;
}

/*
 * Split a comma-delimited header value into OWS-trimmed tokens and invoke
 * visit() on each.  Rejects empty tokens (including a leading or trailing
 * comma) and tokens containing non-tchar bytes, so a token must match in full
 * rather than as a substring ("notchunked" and "disclose" are not "chunked" or
 * "close").  Returns 0 if every token was well formed and visit() returned 0.
 */
static int for_each_token(const char *value,
                          size_t length,
                          int (*visit)(const char *token, size_t token_length, void *context),
                          void *context) {
    if (length == 0u || value[0] == ',' || value[length - 1u] == ',') {
        return -1;
    }
    size_t i = 0u;
    while (i < length) {
        size_t comma = i;
        while (comma < length && value[comma] != ',') {
            comma++;
        }
        size_t start = i;
        size_t stop = comma;
        while (start < stop && (value[start] == ' ' || value[start] == '\t')) {
            start++;
        }
        while (stop > start && (value[stop - 1u] == ' ' || value[stop - 1u] == '\t')) {
            stop--;
        }
        if (stop == start || !is_token(value + start, stop - start)) {
            return -1;
        }
        if (visit(value + start, stop - start, context) != 0) {
            return -1;
        }
        i = comma + 1u;
    }
    return 0;
}

typedef struct {
    size_t count;
    bool last_chunked;
} transfer_encoding_t;

static int visit_transfer_encoding(const char *token, size_t token_length, void *context) {
    transfer_encoding_t *state = (transfer_encoding_t *)context;
    state->count++;
    state->last_chunked = ci_equal(token, token_length, "chunked");
    return 0;
}

static int visit_connection(const char *token, size_t token_length, void *context) {
    http_parser_t *parser = (http_parser_t *)context;
    if (ci_equal(token, token_length, "close")) {
        parser->head.connection_close = true;
    }
    return 0;
}

/*
 * Only "Transfer-Encoding: chunked" is understood.  Any other coding, an
 * unsupported sequence (for example "gzip, chunked"), or chunked followed by
 * another coding is rejected rather than mis-framed.
 */
static int apply_transfer_encoding(http_parser_t *parser, const char *value, size_t length) {
    transfer_encoding_t state = {0u, false};
    if (for_each_token(value, length, visit_transfer_encoding, &state) != 0) {
        return -1;
    }
    if (state.count != 1u || !state.last_chunked) {
        return -1;
    }
    parser->head.chunked = true;
    return 0;
}

static int apply_connection(http_parser_t *parser, const char *value, size_t length) {
    return for_each_token(value, length, visit_connection, parser);
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

/*
 * Parse a chunk-size line (the trailing CRLF already removed).  Requires one or
 * more hex digits with no surrounding whitespace, optionally followed by a
 * non-empty chunk extension introduced by ';'.  Any other byte is rejected, so
 * LF-only, leading/trailing whitespace and repeated-CR lines cannot slip
 * through as valid framing.
 */
static int parse_chunk_size(const char *s, size_t length, size_t *out) {
    size_t i = 0u;
    size_t value = 0u;
    while (i < length && hex_value(s[i]) >= 0) {
        int digit = hex_value(s[i]);
        if (value > (SIZE_MAX - (size_t)digit) / 16u) {
            return -1;
        }
        value = value * 16u + (size_t)digit;
        i++;
    }
    if (i == 0u) {
        return -1; /* no size digits */
    }
    if (i < length) {
        if (s[i] != ';' || i + 1u >= length) {
            return -1; /* trailing junk or an empty extension */
        }
        for (size_t k = i + 1u; k < length; k++) {
            unsigned char c = (unsigned char)s[k];
            if (c < 0x20u || c == 0x7Fu) {
                return -1; /* control bytes are not valid extension text */
            }
        }
    }
    *out = value;
    return 0;
}

/*
 * Validate a header or trailer field line (trailing CRLF already removed) and
 * optionally split it into a name/value pair.  Any output pointer may be NULL.
 * Rejects obs-fold, missing or non-token field names, and control bytes in the
 * value.
 */
static int parse_field_line(const char *line,
                            size_t length,
                            const char **name,
                            size_t *name_length,
                            const char **value,
                            size_t *value_length) {
    if (length == 0u || line[0] == ' ' || line[0] == '\t') {
        return -1;
    }
    const char *colon = memchr(line, ':', length);
    if (colon == NULL) {
        return -1;
    }
    size_t name_len = (size_t)(colon - line);
    if (!is_token(line, name_len)) {
        return -1;
    }
    const char *val = colon + 1;
    size_t val_len = length - name_len - 1u;
    while (val_len > 0u && (*val == ' ' || *val == '\t')) {
        val++;
        val_len--;
    }
    while (val_len > 0u && (val[val_len - 1u] == ' ' || val[val_len - 1u] == '\t')) {
        val_len--;
    }
    for (size_t k = 0u; k < val_len; k++) {
        unsigned char c = (unsigned char)val[k];
        if ((c < 0x20u && c != '\t') || c == 0x7Fu) {
            return -1;
        }
    }
    if (name != NULL) {
        *name = line;
    }
    if (name_length != NULL) {
        *name_length = name_len;
    }
    if (value != NULL) {
        *value = val;
    }
    if (value_length != NULL) {
        *value_length = val_len;
    }
    return 0;
}

static int parse_head(http_parser_t *parser) {
    const char *base = parser->header;
    size_t total = parser->header_length;

    const char *nl = memchr(base, '\n', total);
    if (nl == NULL || nl == base || nl[-1] != '\r') {
        return -1; /* status line must end with CRLF */
    }
    size_t line_length = (size_t)(nl - base) - 1u;
    for (size_t k = 0u; k < line_length; k++) {
        if (base[k] == '\r' || base[k] == '\n') {
            return -1; /* repeated CR or embedded newline */
        }
    }
    if (line_length < 13u ||
        (memcmp(base, "HTTP/1.0 ", 9u) != 0 && memcmp(base, "HTTP/1.1 ", 9u) != 0)) {
        return -1; /* only HTTP/1.0 and HTTP/1.1 are understood */
    }
    const char *code = base + 9u;
    if (!is_digit(code[0]) || !is_digit(code[1]) || !is_digit(code[2])) {
        return -1;
    }
    int status = (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
    if (status < 100 || status > 599) {
        return -1;
    }
    if (code[3] != ' ') {
        return -1;
    }
    parser->head.status_code = status;

    const char *cursor = nl + 1;
    const char *end = base + total;
    bool saw_content_length = false;
    bool saw_transfer_encoding = false;
    while (cursor < end) {
        const char *line_nl = memchr(cursor, '\n', (size_t)(end - cursor));
        if (line_nl == NULL || line_nl == cursor || line_nl[-1] != '\r') {
            return -1; /* every header line must end with CRLF */
        }
        size_t length = (size_t)(line_nl - cursor) - 1u;
        if (length == 0u) {
            break; /* blank line terminates the header block */
        }
        const char *name;
        size_t name_length;
        const char *value;
        size_t value_length;
        if (parse_field_line(cursor, length, &name, &name_length, &value, &value_length) != 0) {
            return -1;
        }
        if (ci_equal(name, name_length, "content-length")) {
            size_t parsed;
            if (saw_content_length || parse_decimal(value, value_length, &parsed) != 0) {
                return -1;
            }
            parser->head.content_length = parsed;
            parser->head.has_content_length = true;
            saw_content_length = true;
        } else if (ci_equal(name, name_length, "transfer-encoding")) {
            if (saw_transfer_encoding) {
                return -1; /* duplicate Transfer-Encoding is ambiguous framing */
            }
            saw_transfer_encoding = true;
            if (apply_transfer_encoding(parser, value, value_length) != 0) {
                return -1;
            }
        } else if (ci_equal(name, name_length, "connection")) {
            if (apply_connection(parser, value, value_length) != 0) {
                return -1;
            }
        }
        cursor = line_nl + 1;
    }
    if (parser->head.chunked && parser->head.has_content_length) {
        return -1; /* Content-Length and chunked framing are mutually exclusive */
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
    if (parser == NULL) {
        return;
    }
    memset(parser, 0, sizeof(*parser));
    parser->state = HTTP_PARSER_STATE_HEADERS;
    parser->on_head = on_head;
    parser->on_body = on_body;
    parser->context = context;
}

int http_parser_feed(http_parser_t *parser, const char *data, size_t length) {
    if (parser == NULL) {
        return -1;
    }
    if (data == NULL && length > 0u) {
        return -1;
    }
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
                    if (parser->chunk_line_length == 0u ||
                        parser->chunk_line[parser->chunk_line_length - 1u] != '\r') {
                        return parser_fail(parser); /* LF-only chunk-size line */
                    }
                    size_t size;
                    size_t line_length = parser->chunk_line_length - 1u;
                    if (parse_chunk_size(parser->chunk_line, line_length, &size) != 0) {
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
                if (parser->chunk_line_length == 0u) {
                    if (c != '\r') {
                        return parser_fail(parser); /* chunk data must end with CRLF */
                    }
                    parser->chunk_line_length = 1u;
                } else {
                    if (c != '\n') {
                        return parser_fail(parser); /* repeated CR or missing LF */
                    }
                    parser->chunk_line_length = 0u;
                    parser->state = HTTP_PARSER_STATE_CHUNK_SIZE;
                }
                break;
            }
            case HTTP_PARSER_STATE_TRAILER: {
                char c = data[i++];
                if (c == '\n') {
                    if (parser->chunk_line_length == 0u ||
                        parser->chunk_line[parser->chunk_line_length - 1u] != '\r') {
                        return parser_fail(parser); /* LF-only trailer line */
                    }
                    size_t line_length = parser->chunk_line_length - 1u;
                    parser->chunk_line_length = 0u;
                    if (line_length == 0u) {
                        parser_complete(parser);
                    } else if (parse_field_line(parser->chunk_line, line_length, NULL, NULL, NULL, NULL) != 0) {
                        return parser_fail(parser); /* malformed trailer field */
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
                if (i < length) {
                    return parser_fail(parser); /* bytes after a fully framed body */
                }
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
    if (parser == NULL || parser->failed) {
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
    return parser != NULL && parser->complete;
}

bool http_parser_failed(const http_parser_t *parser) {
    return parser != NULL && parser->failed;
}
