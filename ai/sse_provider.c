#include "ai/sse_provider.h"

#include <string.h>

/*
 * No heap allocation and no variable length arrays.  The request is built into
 * the provider's fixed buffer and read back by offset; the reply is parsed a
 * line at a time out of the provider's line buffer.
 */

/* ---- Request ------------------------------------------------------------ */

static bool append_raw(sse_provider_t *provider, const char *text, size_t length) {
    if (provider->body_length > provider->body_max || length > provider->body_max - provider->body_length) {
        return false;
    }
    memcpy(provider->body + provider->body_length, text, length);
    provider->body_length += length;
    provider->body[provider->body_length] = '\0';
    return true;
}

bool sse_append(sse_provider_t *provider, const char *text) {
    return text != NULL && append_raw(provider, text, strlen(text));
}

bool sse_append_escaped(sse_provider_t *provider, const char *text) {
    size_t remaining;
    size_t written = 0u;

    if (text == NULL || provider->body_length > provider->body_max) {
        return false;
    }
    remaining = provider->body_max - provider->body_length;
    if (json_escape_string(text, strlen(text), provider->body + provider->body_length, remaining + 1u,
                           &written) != 0 || written > remaining) {
        provider->body[provider->body_length] = '\0';
        return false;
    }
    provider->body_length += written;
    provider->body[provider->body_length] = '\0';
    return true;
}

bool sse_append_uint(sse_provider_t *provider, unsigned value) {
    char digits[10];
    size_t count = 0u;
    char text[11];
    size_t length = 0u;

    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (count > 0u) {
        text[length++] = digits[--count];
    }
    return append_raw(provider, text, length);
}

/* Hundredths as a decimal (80 -> "0.80") without float printf. */
bool sse_append_hundredths(sse_provider_t *provider, unsigned value) {
    return sse_append_uint(provider, value / 100u) && sse_append(provider, ".") &&
           sse_append_uint(provider, value % 100u / 10u) && sse_append_uint(provider, value % 10u);
}

bool sse_role_known(const char *role) {
    return role != NULL &&
           (strcmp(role, "system") == 0 || strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0);
}

static void reset_reply(sse_provider_t *provider) {
    provider->failed = false;
    provider->done = false;
    provider->skipping = false;
    provider->line_length = 0u;
    provider->line[0] = '\0';
    provider->payload_length = 0u;
    provider->error_text[0] = '\0';
    provider->event[0] = '\0';
    provider->flags = 0u;
}

void sse_provider_init(sse_provider_t *provider, provider_event_callback_t callback, void *context) {
    if (provider == NULL) {
        return;
    }
    memset(provider, 0, sizeof(*provider));
    provider->callback = callback;
    provider->callback_context = context;
}

void sse_request_begin(sse_provider_t *provider, const sse_format_t *format, const void *variant, size_t body_max) {
    provider->body_length = 0u;
    provider->body[0] = '\0';
    provider->body_max = body_max < SSE_REQUEST_CAPACITY ? body_max : SSE_REQUEST_CAPACITY;
    provider->built = false;
    provider->format = format;
    provider->variant = variant;
    reset_reply(provider);
}

void sse_request_end(sse_provider_t *provider) {
    provider->built = provider->format != NULL && provider->format->line != NULL &&
                      provider->format->line_max <= SSE_LINE_CAPACITY;
}

size_t sse_provider_request_length(const sse_provider_t *provider) {
    return provider != NULL && provider->built ? provider->body_length : 0u;
}

size_t sse_provider_read(const sse_provider_t *provider, size_t offset, char *destination, size_t capacity) {
    size_t available;
    size_t take;

    if (provider == NULL || !provider->built || destination == NULL || offset >= provider->body_length) {
        return 0u;
    }
    available = provider->body_length - offset;
    take = capacity < available ? capacity : available;
    memcpy(destination, provider->body + offset, take);
    return take;
}

static size_t request_length_adapter(void *context) {
    return sse_provider_request_length((const sse_provider_t *)context);
}

static size_t request_read_adapter(void *context, size_t offset, char *destination, size_t capacity) {
    return sse_provider_read((const sse_provider_t *)context, offset, destination, capacity);
}

provider_request_t sse_provider_request(sse_provider_t *provider) {
    provider_request_t request;
    request.length = request_length_adapter;
    request.read = request_read_adapter;
    request.context = provider;
    return request;
}

/* ---- Events ------------------------------------------------------------- */

static void emit(sse_provider_t *provider, provider_event_type_t type, const char *data, size_t length) {
    provider_event_t event;

    if (provider->callback == NULL) {
        return;
    }
    event.type = type;
    event.data = data;
    event.length = length;
    provider->callback(provider->callback_context, &event);
}

void sse_emit_content(sse_provider_t *provider, const char *data, size_t length) {
    if (length > 0u && !provider->failed && !provider->done) {
        emit(provider, PROVIDER_EVENT_CONTENT, data, length);
    }
}

void sse_done(sse_provider_t *provider) {
    if (!provider->failed && !provider->done) {
        provider->done = true;
        emit(provider, PROVIDER_EVENT_DONE, NULL, 0u);
    }
}

/* Latch failure before calling out, so a callback that feeds or finishes the
 * provider again cannot produce a second event. */
int sse_fail(sse_provider_t *provider, const char *text) {
    if (!provider->failed) {
        /* A later framing fault (for example, data after an end marker) makes
         * the whole reply invalid; it cannot remain a completed reply. */
        provider->done = false;
        provider->failed = true;
        emit(provider, PROVIDER_EVENT_ERROR, text, strlen(text));
    }
    return -1;
}

/* ---- JSON walking ------------------------------------------------------- */

bool sse_json_is_object(const char *text) {
    const char *p = json_scan_whitespace(text);
    return *p == '{' && json_scan_skip_value(&p) == 0 && *json_scan_whitespace(p) == '\0';
}

bool sse_json_at_null(const char *cursor) {
    return strncmp(cursor, "null", 4u) == 0;
}

int sse_json_key(const char **cursor, char *key, size_t capacity) {
    const char *p = json_scan_whitespace(*cursor);

    if (json_scan_string(&p, key, capacity, NULL, NULL) != 0) {
        return -1;
    }
    p = json_scan_whitespace(p);
    if (*p != ':') {
        return -1;
    }
    *cursor = json_scan_whitespace(p + 1);
    return 0;
}

int sse_json_object(sse_provider_t *provider, const char **cursor, sse_member_fn visit) {
    const char *p = json_scan_whitespace(*cursor);
    char key[SSE_KEY_MAX];

    if (*p != '{') {
        return -1;
    }
    p = json_scan_whitespace(p + 1);
    if (*p != '}') {
        for (;;) {
            if (sse_json_key(&p, key, sizeof(key)) != 0 || visit(provider, key, &p) != 0) {
                return -1;
            }
            p = json_scan_whitespace(p);
            if (*p == '}') {
                break;
            }
            if (*p != ',') {
                return -1;
            }
            p++;
        }
    }
    *cursor = p + 1;
    return 0;
}

int sse_json_array(sse_provider_t *provider, const char **cursor, sse_element_fn visit) {
    const char *p = json_scan_whitespace(*cursor);

    if (*p != '[') {
        return -1;
    }
    p = json_scan_whitespace(p + 1);
    if (*p != ']') {
        for (;;) {
            if (visit(provider, &p) != 0) {
                return -1;
            }
            /* The callback may have cancelled the request and reused us. */
            if (provider->failed) {
                return -1;
            }
            p = json_scan_whitespace(p);
            if (*p == ']') {
                break;
            }
            if (*p != ',') {
                return -1;
            }
            p = json_scan_whitespace(p + 1);
        }
    }
    *cursor = p + 1;
    return 0;
}

int sse_json_payload(sse_provider_t *provider, const char **cursor) {
    size_t length = 0u;
    bool overflow = false;

    provider->payload_length = 0u;
    if (json_scan_string(cursor, provider->payload, sizeof(provider->payload), &length, &overflow) != 0 ||
        overflow) {
        return -1;
    }
    provider->payload_length = length;
    return 0;
}

int sse_json_name(const char **cursor, char *out, size_t capacity) {
    return json_scan_string(cursor, out, capacity, NULL, NULL);
}

/* ---- Line framing ------------------------------------------------------- */

/* One line, NUL-terminated in provider->line with its line ending removed. */
static int process_line(sse_provider_t *provider) {
    char *line = provider->line;
    size_t length = provider->line_length;

    provider->line_length = 0u;
    if (length > 0u && line[length - 1u] == '\r') {
        length--;
    }
    while (length > 0u && (line[length - 1u] == ' ' || line[length - 1u] == '\t')) {
        length--;
    }
    line[length] = '\0';

    if (length == 0u) {
        return provider->format->blank != NULL && !provider->done ? provider->format->blank(provider) : 0;
    }
    if (provider->done) {
        return sse_fail(provider, SSE_ERROR_TRAILING);
    }
    if (line[0] == ':') {
        return 0;
    }
    return provider->format->line(provider, line);
}

int sse_provider_feed(sse_provider_t *provider, const char *data, size_t length) {
    size_t i;

    if (provider == NULL || (data == NULL && length > 0u) || !provider->built || provider->failed) {
        return -1;
    }
    for (i = 0u; i < length; i++) {
        char c = data[i];

        if (c == '\n') {
            if (provider->skipping) {
                provider->skipping = false;
                continue;
            }
            if (process_line(provider) != 0) {
                return -1;
            }
            if (provider->failed) {
                /* A reported server error ends the stream; the rest of this
                 * segment is dropped and later feeds are refused. */
                return 0;
            }
            continue;
        }
        if (c == '\0') {
            return sse_fail(provider, SSE_ERROR_MALFORMED);
        }
        if (provider->skipping) {
            continue;
        }
        if (provider->done && c != ' ' && c != '\t' && c != '\r') {
            return sse_fail(provider, SSE_ERROR_TRAILING);
        }
        if (provider->line_length == provider->format->line_max) {
            provider->line[provider->line_length] = '\0';
            if (provider->format->oversized == NULL || !provider->format->oversized(provider, provider->line)) {
                return sse_fail(provider, SSE_ERROR_OVERSIZED);
            }
            provider->line_length = 0u;
            provider->skipping = true;
            continue;
        }
        provider->line[provider->line_length++] = c;
    }
    return 0;
}

int sse_provider_finish(sse_provider_t *provider) {
    if (provider == NULL || !provider->built || provider->failed) {
        return -1;
    }
    if (!provider->skipping && provider->line_length > 0u && process_line(provider) != 0) {
        return -1;
    }
    if (provider->failed) {
        return -1;
    }
    if (!provider->done) {
        return sse_fail(provider, SSE_ERROR_TRUNCATED);
    }
    return 0;
}

bool sse_provider_is_done(const sse_provider_t *provider) {
    return provider != NULL && provider->done && !provider->failed;
}

bool sse_provider_failed(const sse_provider_t *provider) {
    return provider != NULL && provider->failed;
}
