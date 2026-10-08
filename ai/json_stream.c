#include "ai/json_stream.h"

#include <stdint.h>
#include <string.h>

/*
 * This file contains no heap allocation and no variable length arrays.  All
 * working storage lives inside json_stream_t or on the stack with a fixed
 * bound.
 */

static const char HEX_DIGITS[] = "0123456789abcdef";

/* Keep each payload in one place so the emitted length can never drift from
 * the literal it describes. */
#define JSON_STREAM_ERROR_OVERSIZED "record exceeds maximum length"
#define JSON_STREAM_ERROR_MALFORMED "malformed NDJSON record"
#define JSON_STREAM_ERROR_TRUNCATED "truncated NDJSON record"

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

static int hex4(const char *p, size_t available, uint32_t *value) {
    if (available < 4u) {
        return -1;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < 4u; i++) {
        int d = hex_value(p[i]);
        if (d < 0) {
            return -1;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *value = v;
    return 0;
}

static size_t encode_utf8(uint32_t cp, char out[4]) {
    if (cp <= 0x7Fu) {
        out[0] = (char)cp;
        return 1u;
    }
    if (cp <= 0x7FFu) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    if (cp <= 0xFFFFu) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3u;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4u;
}

/*
 * Decode one escape sequence.  *index points at the backslash; on success it is
 * advanced past the sequence and *codepoint holds the decoded scalar value.
 */
static int decode_escape(const char *src, size_t length, size_t *index, uint32_t *codepoint) {
    size_t p = *index + 1u;
    if (p >= length) {
        return -1;
    }
    char e = src[p];
    switch (e) {
        case '"': *codepoint = '"'; *index = p + 1u; return 0;
        case '\\': *codepoint = '\\'; *index = p + 1u; return 0;
        case '/': *codepoint = '/'; *index = p + 1u; return 0;
        case 'b': *codepoint = 0x08u; *index = p + 1u; return 0;
        case 'f': *codepoint = 0x0Cu; *index = p + 1u; return 0;
        case 'n': *codepoint = 0x0Au; *index = p + 1u; return 0;
        case 'r': *codepoint = 0x0Du; *index = p + 1u; return 0;
        case 't': *codepoint = 0x09u; *index = p + 1u; return 0;
        case 'u': {
            uint32_t hi;
            if (hex4(src + p + 1u, length - (p + 1u), &hi) != 0) {
                return -1;
            }
            size_t q = p + 5u;
            if (hi >= 0xD800u && hi <= 0xDBFFu) {
                uint32_t lo;
                if (q + 1u >= length || src[q] != '\\' || src[q + 1u] != 'u') {
                    return -1;
                }
                if (hex4(src + q + 2u, length - (q + 2u), &lo) != 0) {
                    return -1;
                }
                if (lo < 0xDC00u || lo > 0xDFFFu) {
                    return -1;
                }
                *codepoint = 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u);
                *index = q + 6u;
                return 0;
            }
            if (hi >= 0xDC00u && hi <= 0xDFFFu) {
                return -1; /* lone low surrogate */
            }
            *codepoint = hi;
            *index = q;
            return 0;
        }
        default:
            return -1;
    }
}

static void emit(json_stream_t *stream, json_stream_event_type_t type, const char *data, size_t length) {
    if (stream->callback == NULL) {
        return;
    }
    json_stream_event_t event;
    event.type = type;
    event.data = data;
    event.length = length;
    stream->callback(stream->callback_context, &event);
}

/*
 * Parse a quoted JSON string starting at *cursor into out (which may be NULL
 * for discard).  On success *cursor advances past the closing quote.  Stores
 * the decoded byte count in *out_length and sets *overflow when the decoded
 * value did not fit out_cap bytes (including the NUL).
 */
static int parse_string(const char **cursor, char *out, size_t out_cap, size_t *out_length, bool *overflow) {
    const char *body = *cursor;
    if (*body != '"') {
        return -1;
    }
    body++;
    size_t length = strlen(body);
    size_t i = 0;
    size_t written = 0;
    bool over = false;
    while (i < length) {
        unsigned char c = (unsigned char)body[i];
        if (c == '"') {
            if (out != NULL && out_cap > 0u) {
                out[written < out_cap ? written : out_cap - 1u] = '\0';
            }
            if (out_length != NULL) {
                *out_length = written;
            }
            if (overflow != NULL) {
                *overflow = over;
            }
            *cursor = body + i + 1u;
            return 0;
        }
        if (c < 0x20u) {
            return -1; /* raw control character */
        }
        char encoded[4];
        size_t encoded_len;
        if (c == '\\') {
            uint32_t cp;
            if (decode_escape(body, length, &i, &cp) != 0) {
                return -1;
            }
            if (cp == 0u) {
                return -1; /* embedded U+0000 would alias a C-string terminator */
            }
            encoded_len = encode_utf8(cp, encoded);
        } else {
            encoded[0] = (char)c;
            encoded_len = 1u;
            i++;
        }
        for (size_t k = 0; k < encoded_len; k++) {
            if (out != NULL && written + 1u < out_cap) {
                out[written] = encoded[k];
            } else {
                over = true;
            }
            written++;
        }
    }
    return -1; /* unterminated string */
}

static const char *skip_whitespace(const char *p) {
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static int skip_number(const char **cursor) {
    const char *p = *cursor;
    if (*p == '-') {
        p++;
    }
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    } else {
        return -1;
    }
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') {
            return -1;
        }
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') {
            p++;
        }
        if (*p < '0' || *p > '9') {
            return -1;
        }
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    }
    *cursor = p;
    return 0;
}

static int skip_literal(const char **cursor, const char *literal) {
    size_t n = strlen(literal);
    if (strncmp(*cursor, literal, n) != 0) {
        return -1;
    }
    *cursor += n;
    return 0;
}

static int skip_value(const char **cursor, int depth) {
    if (depth > 16) {
        return -1;
    }
    const char *p = skip_whitespace(*cursor);
    if (*p == '"') {
        if (parse_string(&p, NULL, 0u, NULL, NULL) != 0) {
            return -1;
        }
        *cursor = p;
        return 0;
    }
    if (*p == '{' || *p == '[') {
        char close = (*p == '{') ? '}' : ']';
        p++;
        p = skip_whitespace(p);
        if (*p == close) {
            *cursor = p + 1u;
            return 0;
        }
        for (;;) {
            if (close == '}') {
                p = skip_whitespace(p);
                if (parse_string(&p, NULL, 0u, NULL, NULL) != 0) {
                    return -1;
                }
                p = skip_whitespace(p);
                if (*p != ':') {
                    return -1;
                }
                p++;
            }
            if (skip_value(&p, depth + 1) != 0) {
                return -1;
            }
            p = skip_whitespace(p);
            if (*p == ',') {
                p++;
                continue;
            }
            if (*p == close) {
                p++;
                break;
            }
            return -1;
        }
        *cursor = p;
        return 0;
    }
    if (skip_literal(&p, "true") == 0 || skip_literal(&p, "false") == 0 || skip_literal(&p, "null") == 0) {
        *cursor = p;
        return 0;
    }
    if (skip_number(&p) == 0) {
        *cursor = p;
        return 0;
    }
    return -1;
}

/*
 * Parse the "message" object of an Ollama record, extracting an optional
 * "content" string.  Unknown members are skipped.
 */
static int parse_message_object(const char **cursor, json_stream_t *stream, bool *has_content) {
    const char *p = skip_whitespace(*cursor);
    if (*p != '{') {
        return -1;
    }
    p++;
    char key[64];
    p = skip_whitespace(p);
    if (*p == '}') {
        *cursor = p + 1u;
        return 0;
    }
    for (;;) {
        p = skip_whitespace(p);
        if (parse_string(&p, key, sizeof(key), NULL, NULL) != 0) {
            return -1;
        }
        p = skip_whitespace(p);
        if (*p != ':') {
            return -1;
        }
        p++;
        p = skip_whitespace(p);
        if (strcmp(key, "content") == 0) {
            bool over = false;
            size_t length = 0;
            if (parse_string(&p, stream->payload, sizeof(stream->payload), &length, &over) != 0) {
                return -1;
            }
            if (over) {
                return -1; /* oversized content */
            }
            stream->payload_length = length;
            *has_content = true;
        } else if (skip_value(&p, 0) != 0) {
            return -1;
        }
        p = skip_whitespace(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            p++;
            break;
        }
        return -1;
    }
    *cursor = p;
    return 0;
}

static int parse_record(json_stream_t *stream) {
    stream->payload_length = 0u;
    stream->error_length = 0u;
    bool has_content = false;
    bool has_done = false;
    bool done_value = false;
    bool has_error = false;

    const char *p = skip_whitespace(stream->record);
    if (*p != '{') {
        return -1;
    }
    p++;
    char key[64];
    p = skip_whitespace(p);
    if (*p == '}') {
        return -1; /* an empty object carries nothing useful */
    }
    for (;;) {
        p = skip_whitespace(p);
        if (parse_string(&p, key, sizeof(key), NULL, NULL) != 0) {
            return -1;
        }
        p = skip_whitespace(p);
        if (*p != ':') {
            return -1;
        }
        p++;
        p = skip_whitespace(p);
        if (strcmp(key, "error") == 0) {
            bool over = false;
            size_t length = 0;
            if (parse_string(&p, stream->error_text, sizeof(stream->error_text), &length, &over) != 0) {
                return -1;
            }
            if (over) {
                return -1; /* oversized error message */
            }
            stream->error_length = length;
            has_error = true;
        } else if (strcmp(key, "done") == 0) {
            if (skip_literal(&p, "true") == 0) {
                done_value = true;
            } else if (skip_literal(&p, "false") == 0) {
                done_value = false;
            } else {
                return -1;
            }
            has_done = true;
        } else if (strcmp(key, "message") == 0) {
            if (parse_message_object(&p, stream, &has_content) != 0) {
                return -1;
            }
        } else if (skip_value(&p, 0) != 0) {
            return -1;
        }
        p = skip_whitespace(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            p++;
            break;
        }
        return -1;
    }
    p = skip_whitespace(p);
    if (*p != '\0') {
        return -1; /* trailing garbage after the record */
    }

    if (has_error) {
        emit(stream, JSON_STREAM_EVENT_ERROR, stream->error_text, stream->error_length);
        stream->phase = JSON_STREAM_PHASE_ERROR;
        return 0;
    }
    if (!has_content && !has_done) {
        return -1; /* no field we understand */
    }
    if (has_content && stream->payload_length > 0u) {
        emit(stream, JSON_STREAM_EVENT_CONTENT, stream->payload, stream->payload_length);
    }
    if (has_done && done_value) {
        emit(stream, JSON_STREAM_EVENT_DONE, NULL, 0u);
        stream->phase = JSON_STREAM_PHASE_DONE;
    }
    return 0;
}

/* Process one accumulated record.  Returns 0 on success, -1 if malformed. */
static int process_record(json_stream_t *stream) {
    size_t length = stream->record_length;
    while (length > 0u && (stream->record[length - 1u] == '\r' || stream->record[length - 1u] == '\n')) {
        length--;
    }
    stream->record[length] = '\0';
    bool has_content = false;
    for (size_t i = 0; i < length; i++) {
        char c = stream->record[i];
        if (c != ' ' && c != '\t' && c != '\r') {
            has_content = true;
            break;
        }
    }
    if (!has_content) {
        return -1; /* blank record */
    }
    return parse_record(stream);
}

void json_stream_init(json_stream_t *stream, json_stream_callback_t callback, void *context) {
    memset(stream, 0, sizeof(*stream));
    stream->callback = callback;
    stream->callback_context = context;
}

int json_stream_feed(json_stream_t *stream, const char *data, size_t length) {
    if (stream == NULL) {
        return -1;
    }
    if (data == NULL && length > 0u) {
        return -1;
    }
    if (stream->failed || stream->phase == JSON_STREAM_PHASE_ERROR) {
        return -1;
    }
    if (stream->phase == JSON_STREAM_PHASE_DONE) {
        return 0; /* terminal success: discard any later bytes */
    }
    for (size_t i = 0; i < length; i++) {
        char c = data[i];
        if (c == '\n') {
            if (stream->overflow) {
                stream->failed = true;
                stream->phase = JSON_STREAM_PHASE_ERROR;
                emit(stream, JSON_STREAM_EVENT_ERROR, JSON_STREAM_ERROR_OVERSIZED,
                     sizeof(JSON_STREAM_ERROR_OVERSIZED) - 1u);
                return -1;
            }
            stream->record[stream->record_length] = '\0';
            int rc = process_record(stream);
            stream->record_length = 0u;
            stream->overflow = false;
            if (rc != 0) {
                stream->failed = true;
                stream->phase = JSON_STREAM_PHASE_ERROR;
                emit(stream, JSON_STREAM_EVENT_ERROR, JSON_STREAM_ERROR_MALFORMED,
                     sizeof(JSON_STREAM_ERROR_MALFORMED) - 1u);
                return -1;
            }
            if (stream->phase != JSON_STREAM_PHASE_OPEN) {
                return 0; /* terminal reached; ignore the rest of this chunk */
            }
        } else if (stream->record_length < JSON_STREAM_RECORD_MAX) {
            stream->record[stream->record_length++] = c;
        } else {
            stream->overflow = true;
        }
    }
    return 0;
}

int json_stream_finish(json_stream_t *stream) {
    if (stream == NULL) {
        return -1;
    }
    if (stream->failed || stream->phase == JSON_STREAM_PHASE_ERROR) {
        return -1;
    }
    if (stream->phase == JSON_STREAM_PHASE_DONE) {
        return 0;
    }
    if (stream->overflow) {
        stream->failed = true;
        stream->phase = JSON_STREAM_PHASE_ERROR;
        emit(stream, JSON_STREAM_EVENT_ERROR, JSON_STREAM_ERROR_OVERSIZED,
             sizeof(JSON_STREAM_ERROR_OVERSIZED) - 1u);
        return -1;
    }
    if (stream->record_length > 0u) {
        stream->record[stream->record_length] = '\0';
        int rc = process_record(stream);
        stream->record_length = 0u;
        if (rc != 0) {
            stream->failed = true;
            stream->phase = JSON_STREAM_PHASE_ERROR;
            emit(stream, JSON_STREAM_EVENT_ERROR, JSON_STREAM_ERROR_TRUNCATED,
                 sizeof(JSON_STREAM_ERROR_TRUNCATED) - 1u);
            return -1;
        }
    }
    if (stream->phase != JSON_STREAM_PHASE_DONE) {
        /* End of transport with no "done": true terminal: incomplete stream. */
        stream->failed = true;
        stream->phase = JSON_STREAM_PHASE_ERROR;
        return -1;
    }
    return 0;
}

bool json_stream_failed(const json_stream_t *stream) {
    return stream != NULL && stream->failed;
}

int json_escape_string(const char *src, size_t src_length, char *dst, size_t dst_size, size_t *out_length) {
    if (src == NULL && src_length > 0u) {
        if (out_length != NULL) {
            *out_length = 0u;
        }
        return -1;
    }
    size_t written = 0u;
    for (size_t i = 0; i < src_length; i++) {
        unsigned char c = (unsigned char)src[i];
        char scratch[6];
        const char *piece = scratch;
        size_t piece_len;
        switch (c) {
            case '"': piece = "\\\""; piece_len = 2u; break;
            case '\\': piece = "\\\\"; piece_len = 2u; break;
            case '\b': piece = "\\b"; piece_len = 2u; break;
            case '\f': piece = "\\f"; piece_len = 2u; break;
            case '\n': piece = "\\n"; piece_len = 2u; break;
            case '\r': piece = "\\r"; piece_len = 2u; break;
            case '\t': piece = "\\t"; piece_len = 2u; break;
            default:
                if (c < 0x20u) {
                    scratch[0] = '\\';
                    scratch[1] = 'u';
                    scratch[2] = '0';
                    scratch[3] = '0';
                    scratch[4] = HEX_DIGITS[(c >> 4) & 0x0Fu];
                    scratch[5] = HEX_DIGITS[c & 0x0Fu];
                    piece_len = 6u;
                } else {
                    scratch[0] = (char)c;
                    piece_len = 1u;
                }
                break;
        }
        if (dst != NULL && written + piece_len < dst_size) {
            memcpy(dst + written, piece, piece_len);
        }
        written += piece_len;
    }
    if (out_length != NULL) {
        *out_length = written;
    }
    if (dst == NULL) {
        return 0;
    }
    if (written >= dst_size) {
        if (dst_size > 0u) {
            dst[dst_size - 1u] = '\0';
        }
        return -1;
    }
    dst[written] = '\0';
    return 0;
}

int json_unescape_string(const char *src, size_t src_length, char *dst, size_t dst_size, size_t *out_length) {
    if (src == NULL && src_length > 0u) {
        if (out_length != NULL) {
            *out_length = 0u;
        }
        return -1;
    }
    size_t i = 0u;
    size_t written = 0u;
    while (i < src_length) {
        unsigned char c = (unsigned char)src[i];
        char encoded[4];
        size_t encoded_len;
        if (c == '\\') {
            uint32_t cp;
            if (decode_escape(src, src_length, &i, &cp) != 0) {
                return -1;
            }
            encoded_len = encode_utf8(cp, encoded);
        } else {
            if (c < 0x20u) {
                return -1;
            }
            encoded[0] = (char)c;
            encoded_len = 1u;
            i++;
        }
        for (size_t k = 0; k < encoded_len; k++) {
            if (dst != NULL && written < dst_size) {
                dst[written] = encoded[k];
            }
            written++;
        }
    }
    if (out_length != NULL) {
        *out_length = written;
    }
    if (dst == NULL) {
        return 0;
    }
    if (written >= dst_size) {
        if (dst_size > 0u) {
            dst[dst_size - 1u] = '\0';
        }
        return -1;
    }
    dst[written] = '\0';
    return 0;
}
