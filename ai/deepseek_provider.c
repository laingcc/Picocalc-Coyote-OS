#include "ai/deepseek_provider.h"

#include <stdio.h>
#include <string.h>

#include "ai/json_stream.h"

static void emit_event(deepseek_provider_t *provider, provider_event_type_t type, const char *data, size_t length) {
    if (provider->callback == NULL) {
        return;
    }
    provider_event_t event;
    event.type = type;
    event.data = data;
    event.length = length;
    provider->callback(provider->callback_context, &event);
}

static bool append_raw(deepseek_provider_t *provider, const char *text, size_t length) {
    if (length > DEEPSEEK_REQUEST_MAX - provider->body_length) {
        return false;
    }
    memcpy(provider->body + provider->body_length, text, length);
    provider->body_length += length;
    provider->body[provider->body_length] = '\0';
    return true;
}

static bool append_literal(deepseek_provider_t *provider, const char *text) {
    return append_raw(provider, text, strlen(text));
}

static bool append_escaped(deepseek_provider_t *provider, const char *text, size_t length) {
    size_t remaining = DEEPSEEK_REQUEST_MAX - provider->body_length;
    if (remaining == 0u) {
        return false;
    }
    size_t written = 0;
    if (json_escape_string(text, length, provider->body + provider->body_length, remaining + 1u, &written) != 0) {
        return false;
    }
    if (written > remaining) {
        return false;
    }
    provider->body_length += written;
    provider->body[provider->body_length] = '\0';
    return true;
}

static bool append_int(deepseek_provider_t *provider, int value) {
    if (value < 0) {
        return false;
    }
    char digits[12];
    size_t length = 0u;
    unsigned int magnitude = (unsigned int)value;
    do {
        digits[length++] = (char)('0' + (magnitude % 10u));
        magnitude /= 10u;
    } while (magnitude != 0u);
    for (size_t i = 0; i < length / 2u; i++) {
        char swap = digits[i];
        digits[i] = digits[length - 1u - i];
        digits[length - 1u - i] = swap;
    }
    return append_raw(provider, digits, length);
}

void deepseek_provider_init(deepseek_provider_t *provider, provider_event_callback_t callback, void *context) {
    if (provider == NULL) {
        return;
    }
    memset(provider, 0, sizeof(*provider));
    provider->callback = callback;
    provider->callback_context = context;
}

int deepseek_provider_build_request(deepseek_provider_t *provider,
                                    const char *model,
                                    const deepseek_message_t *messages,
                                    size_t message_count,
                                    int max_tokens) {
    if (provider == NULL) {
        return -1;
    }
    provider->body_length = 0u;
    provider->body[0] = '\0';
    provider->built = false;
    provider->failed = false;
    provider->done = false;
    provider->line_length = 0u;
    provider->overflow = false;

    if (model == NULL || model[0] == '\0' || strlen(model) > DEEPSEEK_MODEL_MAX) {
        return -1;
    }
    if (message_count > DEEPSEEK_MAX_MESSAGES) {
        return -1;
    }
    if (message_count > 0u && messages == NULL) {
        return -1;
    }
    if (max_tokens < 0) {
        return -1;
    }

    if (!append_literal(provider, "{\"model\":\"")) {
        return -1;
    }
    if (!append_escaped(provider, model, strlen(model))) {
        return -1;
    }
    if (!append_literal(provider, "\",\"messages\":[")) {
        return -1;
    }
    for (size_t i = 0; i < message_count; i++) {
        const char *role = messages[i].role != NULL ? messages[i].role : "";
        const char *content = messages[i].content != NULL ? messages[i].content : "";
        if (role[0] == '\0') {
            return -1;
        }
        if (i > 0u && !append_literal(provider, ",")) {
            return -1;
        }
        if (!append_literal(provider, "{\"role\":\"")) {
            return -1;
        }
        if (!append_escaped(provider, role, strlen(role))) {
            return -1;
        }
        if (!append_literal(provider, "\",\"content\":\"")) {
            return -1;
        }
        if (!append_escaped(provider, content, strlen(content))) {
            return -1;
        }
        if (!append_literal(provider, "\"}")) {
            return -1;
        }
    }
    if (!append_literal(provider, "],\"stream\":true,\"max_tokens\":")) {
        return -1;
    }
    if (!append_int(provider, max_tokens)) {
        return -1;
    }
    if (!append_literal(provider, "}")) {
        return -1;
    }
    provider->built = true;
    return 0;
}

size_t deepseek_provider_request_length(const deepseek_provider_t *provider) {
    return provider != NULL ? provider->body_length : 0u;
}

size_t deepseek_provider_read(const deepseek_provider_t *provider, size_t offset, char *destination, size_t capacity) {
    if (provider == NULL) {
        return 0u;
    }
    if (destination == NULL && capacity > 0u) {
        return 0u;
    }
    if (offset >= provider->body_length) {
        return 0u;
    }
    size_t available = provider->body_length - offset;
    size_t take = capacity < available ? capacity : available;
    if (take > 0u) {
        memcpy(destination, provider->body + offset, take);
    }
    return take;
}

static size_t request_length_adapter(void *context) {
    return deepseek_provider_request_length((const deepseek_provider_t *)context);
}

static size_t request_read_adapter(void *context, size_t offset, char *destination, size_t capacity) {
    return deepseek_provider_read((const deepseek_provider_t *)context, offset, destination, capacity);
}

provider_request_t deepseek_provider_request(deepseek_provider_t *provider) {
    provider_request_t request;
    request.length = request_length_adapter;
    request.read = request_read_adapter;
    request.context = provider;
    return request;
}

/* Helper to parse JSON strings and extract fields from SSE json payload */
static const char *skip_spaces(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r') {
        p++;
    }
    return p;
}

/* Find key in JSON object and return pointer to its value */
static const char *find_json_key(const char *json, const char *key) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    const char *p = strstr(json, search_key);
    if (p == NULL) {
        return NULL;
    }
    p += strlen(search_key);
    p = skip_spaces(p);
    if (*p == ':') {
        p++;
        return skip_spaces(p);
    }
    return NULL;
}

/* Extract string value from cursor at quoted string: "value" */
static int extract_json_string(const char *val_start, char *out, size_t out_cap, size_t *out_len) {
    if (val_start == NULL || *val_start != '"') {
        return -1;
    }
    val_start++;
    const char *end = val_start;
    while (*end != '\0') {
        if (*end == '"' && *(end - 1) != '\\') {
            break;
        }
        end++;
    }
    if (*end != '"') {
        return -1;
    }
    size_t src_len = (size_t)(end - val_start);
    if (json_unescape_string(val_start, src_len, out, out_cap, out_len) != 0) {
        return -1;
    }
    return 0;
}

static int parse_deepseek_line(deepseek_provider_t *provider, const char *line) {
    const char *p = skip_spaces(line);
    if (*p == '\0') {
        return 0; /* empty line */
    }

    if (strncmp(p, "data:", 5) == 0) {
        p += 5;
        p = skip_spaces(p);
    }

    if (strcmp(p, "[DONE]") == 0) {
        if (!provider->done) {
            provider->done = true;
            emit_event(provider, PROVIDER_EVENT_DONE, NULL, 0u);
        }
        return 0;
    }

    if (*p != '{') {
        return 0; /* non-JSON comment or SSE line */
    }

    /* Check for error object */
    const char *err_val = find_json_key(p, "error");
    if (err_val != NULL) {
        size_t err_len = 0;
        if (*err_val == '{') {
            const char *msg_val = find_json_key(err_val, "message");
            if (msg_val != NULL) {
                extract_json_string(msg_val, provider->error_text, sizeof(provider->error_text), &err_len);
            }
        } else if (*err_val == '"') {
            extract_json_string(err_val, provider->error_text, sizeof(provider->error_text), &err_len);
        }
        if (err_len == 0u) {
            snprintf(provider->error_text, sizeof(provider->error_text), "DeepSeek error");
            err_len = strlen(provider->error_text);
        }
        provider->failed = true;
        emit_event(provider, PROVIDER_EVENT_ERROR, provider->error_text, err_len);
        return 0;
    }

    /* Check choices array */
    const char *choices_val = find_json_key(p, "choices");
    if (choices_val != NULL) {
        /* Check finish_reason */
        const char *finish_val = find_json_key(choices_val, "finish_reason");
        char finish_reason[32];
        size_t finish_len = 0;
        if (finish_val != NULL && extract_json_string(finish_val, finish_reason, sizeof(finish_reason), &finish_len) == 0) {
            if (finish_len > 0u && strcmp(finish_reason, "null") != 0) {
                /* Non-null finish_reason (e.g. "stop", "length") */
                /* Extract delta content first if any */
                const char *delta_val = find_json_key(choices_val, "delta");
                if (delta_val != NULL) {
                    const char *content_val = find_json_key(delta_val, "content");
                    if (content_val != NULL) {
                        size_t content_len = 0;
                        if (extract_json_string(content_val, provider->payload, sizeof(provider->payload), &content_len) == 0) {
                            if (content_len > 0u) {
                                emit_event(provider, PROVIDER_EVENT_CONTENT, provider->payload, content_len);
                            }
                        }
                    }
                }
                if (!provider->done) {
                    provider->done = true;
                    emit_event(provider, PROVIDER_EVENT_DONE, NULL, 0u);
                }
                return 0;
            }
        }

        /* Check delta content */
        const char *delta_val = find_json_key(choices_val, "delta");
        if (delta_val != NULL) {
            const char *content_val = find_json_key(delta_val, "content");
            if (content_val != NULL) {
                size_t content_len = 0;
                if (extract_json_string(content_val, provider->payload, sizeof(provider->payload), &content_len) == 0) {
                    if (content_len > 0u) {
                        emit_event(provider, PROVIDER_EVENT_CONTENT, provider->payload, content_len);
                    }
                }
            }
        }
    }

    return 0;
}

int deepseek_provider_feed(deepseek_provider_t *provider, const char *data, size_t length) {
    if (provider == NULL) {
        return -1;
    }
    if (data == NULL && length > 0u) {
        return -1;
    }
    if (!provider->built || provider->failed) {
        return -1;
    }
    if (provider->done) {
        if (length == 0u) {
            return 0;
        }
        provider->failed = true;
        snprintf(provider->error_text, sizeof(provider->error_text), "data after done");
        emit_event(provider, PROVIDER_EVENT_ERROR, provider->error_text, strlen(provider->error_text));
        return -1;
    }

    for (size_t i = 0; i < length; i++) {
        char c = data[i];
        if (provider->done) {
            provider->failed = true;
            snprintf(provider->error_text, sizeof(provider->error_text), "data after done");
            emit_event(provider, PROVIDER_EVENT_ERROR, provider->error_text, strlen(provider->error_text));
            return -1;
        }
        if (c == '\n') {
            if (provider->overflow) {
                provider->failed = true;
                snprintf(provider->error_text, sizeof(provider->error_text), "line exceeds maximum length");
                emit_event(provider, PROVIDER_EVENT_ERROR, provider->error_text, strlen(provider->error_text));
                return -1;
            }
            provider->line[provider->line_length] = '\0';
            parse_deepseek_line(provider, provider->line);
            provider->line_length = 0u;
            provider->overflow = false;
            if (provider->failed) {
                if (i + 1u < length) {
                    return -1;
                }
                return 0;
            }
        } else if (provider->line_length < DEEPSEEK_RECORD_MAX) {
            provider->line[provider->line_length++] = c;
        } else {
            provider->overflow = true;
        }
    }
    return 0;
}

int deepseek_provider_finish(deepseek_provider_t *provider) {
    if (provider == NULL || !provider->built) {
        return -1;
    }
    if (provider->failed) {
        return -1;
    }
    if (provider->line_length > 0u) {
        if (provider->overflow) {
            provider->failed = true;
            snprintf(provider->error_text, sizeof(provider->error_text), "line exceeds maximum length");
            emit_event(provider, PROVIDER_EVENT_ERROR, provider->error_text, strlen(provider->error_text));
            return -1;
        }
        provider->line[provider->line_length] = '\0';
        parse_deepseek_line(provider, provider->line);
        provider->line_length = 0u;
    }
    if (!provider->done && !provider->failed) {
        provider->failed = true;
        snprintf(provider->error_text, sizeof(provider->error_text), "truncated response");
        emit_event(provider, PROVIDER_EVENT_ERROR, provider->error_text, strlen(provider->error_text));
        return -1;
    }
    return provider->failed ? -1 : 0;
}

bool deepseek_provider_is_done(const deepseek_provider_t *provider) {
    return provider != NULL && provider->done;
}

bool deepseek_provider_failed(const deepseek_provider_t *provider) {
    return provider != NULL && provider->failed;
}

const char *deepseek_provider_method(void) {
    return "POST";
}

const char *deepseek_provider_path(void) {
    return "/chat/completions";
}

const char *deepseek_provider_content_type(void) {
    return "application/json";
}
