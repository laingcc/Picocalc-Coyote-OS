#include "ai/deepseek_provider.h"

#include <string.h>

/*
 * No heap allocation and no variable length arrays.  The request is built into
 * the provider's fixed buffer and read back by offset; the reply is parsed a
 * line at a time out of the provider's line buffer.
 */

#define DEEPSEEK_ERROR_OVERSIZED "reply line too long"
#define DEEPSEEK_ERROR_MALFORMED "malformed reply stream"
#define DEEPSEEK_ERROR_TRUNCATED "reply ended early"
#define DEEPSEEK_ERROR_TRAILING "data after end of reply"
#define DEEPSEEK_ERROR_SERVER "DeepSeek error"

#define DEEPSEEK_KEY_MAX 24u

/* ---- Request ------------------------------------------------------------ */

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

static bool append_escaped(deepseek_provider_t *provider, const char *text) {
    size_t remaining = DEEPSEEK_REQUEST_MAX - provider->body_length;
    size_t written = 0u;

    if (json_escape_string(text, strlen(text), provider->body + provider->body_length, remaining + 1u,
                           &written) != 0 || written > remaining) {
        provider->body[provider->body_length] = '\0';
        return false;
    }
    provider->body_length += written;
    provider->body[provider->body_length] = '\0';
    return true;
}

static bool append_uint(deepseek_provider_t *provider, unsigned value) {
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

static bool append_message(deepseek_provider_t *provider, const char *role, const char *content, bool first) {
    return (first || append_literal(provider, ",")) && append_literal(provider, "{\"role\":\"") &&
           append_literal(provider, role) && append_literal(provider, "\",\"content\":\"") &&
           append_escaped(provider, content) && append_literal(provider, "\"}");
}

static bool known_role(const char *role) {
    return role != NULL &&
           (strcmp(role, "system") == 0 || strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0);
}

static void reset_reply(deepseek_provider_t *provider) {
    provider->failed = false;
    provider->done = false;
    provider->line_length = 0u;
    provider->line[0] = '\0';
    provider->error_text[0] = '\0';
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
                                    const ollama_message_t *messages,
                                    size_t message_count,
                                    int max_tokens,
                                    const char *system_prompt,
                                    int temperature) {
    bool first = true;
    size_t i;

    if (provider == NULL) {
        return -1;
    }
    provider->body_length = 0u;
    provider->body[0] = '\0';
    provider->built = false;
    reset_reply(provider);

    if (!ai_config_deepseek_model_known(model) || message_count > OLLAMA_MAX_MESSAGES ||
        (message_count > 0u && messages == NULL) || max_tokens <= 0 || temperature < 0 || temperature > 200) {
        return -1;
    }

    /* The model is one of two fixed literals, so it needs no escaping. */
    if (!append_literal(provider, "{\"model\":\"") || !append_literal(provider, model) ||
        !append_literal(provider, "\",\"messages\":[")) {
        return -1;
    }
    if (system_prompt != NULL && system_prompt[0] != '\0') {
        if (!append_message(provider, "system", system_prompt, first)) {
            return -1;
        }
        first = false;
    }
    for (i = 0u; i < message_count; i++) {
        if (!known_role(messages[i].role) ||
            !append_message(provider, messages[i].role, messages[i].content != NULL ? messages[i].content : "",
                            first)) {
            return -1;
        }
        first = false;
    }
    /* Hundredths as a decimal (80 -> "0.80") without float printf. */
    if (!append_literal(provider, "],\"stream\":true,\"max_tokens\":") ||
        !append_uint(provider, (unsigned)max_tokens) || !append_literal(provider, ",\"temperature\":") ||
        !append_uint(provider, (unsigned)temperature / 100u) || !append_literal(provider, ".") ||
        !append_uint(provider, (unsigned)temperature % 100u / 10u) ||
        !append_uint(provider, (unsigned)temperature % 10u) || !append_literal(provider, "}")) {
        return -1;
    }
    provider->built = true;
    return 0;
}

size_t deepseek_provider_request_length(const deepseek_provider_t *provider) {
    return provider != NULL && provider->built ? provider->body_length : 0u;
}

size_t deepseek_provider_read(const deepseek_provider_t *provider, size_t offset, char *destination,
                              size_t capacity) {
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

/* ---- Reply -------------------------------------------------------------- */

static void emit(deepseek_provider_t *provider, provider_event_type_t type, const char *data, size_t length) {
    provider_event_t event;

    if (provider->callback == NULL) {
        return;
    }
    event.type = type;
    event.data = data;
    event.length = length;
    provider->callback(provider->callback_context, &event);
}

/* Latch failure before calling out, so a callback that feeds or finishes the
 * provider again cannot produce a second event. */
static int fail(deepseek_provider_t *provider, const char *text) {
    if (!provider->failed) {
        provider->failed = true;
        emit(provider, PROVIDER_EVENT_ERROR, text, strlen(text));
    }
    return -1;
}

static bool is_blank(const char *text) {
    return *json_scan_whitespace(text) == '\0';
}

/* Read an object key and the colon after it.  A key too long for the buffer
 * is truncated, which cannot match any name looked for here. */
static int scan_key(const char **cursor, char *key, size_t capacity) {
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

typedef int (*member_fn)(deepseek_provider_t *provider, const char *key, const char **cursor);

/* Walk the members of the object at *cursor, handing each value to visit,
 * which must consume it. */
static int scan_object(deepseek_provider_t *provider, const char **cursor, member_fn visit) {
    const char *p = json_scan_whitespace(*cursor);
    char key[DEEPSEEK_KEY_MAX];

    if (*p != '{') {
        return -1;
    }
    p = json_scan_whitespace(p + 1);
    if (*p != '}') {
        for (;;) {
            if (scan_key(&p, key, sizeof(key)) != 0 || visit(provider, key, &p) != 0) {
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

static bool at_null(const char *p) {
    return strncmp(p, "null", 4u) == 0;
}

static int visit_delta(deepseek_provider_t *provider, const char *key, const char **cursor) {
    size_t length = 0u;
    bool overflow = false;

    /* Everything but a string content is skipped: the role, a null content
     * and the reasoner's reasoning_content are not part of the reply text. */
    if (strcmp(key, "content") != 0 || at_null(*cursor)) {
        return json_scan_skip_value(cursor);
    }
    if (json_scan_string(cursor, provider->payload, sizeof(provider->payload), &length, &overflow) != 0 ||
        overflow) {
        return -1;
    }
    if (length > 0u) {
        emit(provider, PROVIDER_EVENT_CONTENT, provider->payload, length);
    }
    return 0;
}

static int visit_choice(deepseek_provider_t *provider, const char *key, const char **cursor) {
    if (strcmp(key, "delta") != 0) {
        return json_scan_skip_value(cursor);
    }
    return scan_object(provider, cursor, visit_delta);
}

static int scan_choices(deepseek_provider_t *provider, const char **cursor) {
    const char *p = *cursor;

    if (*p != '[') {
        return -1;
    }
    p = json_scan_whitespace(p + 1);
    if (*p != ']') {
        for (;;) {
            if (scan_object(provider, &p, visit_choice) != 0) {
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

static int visit_error(deepseek_provider_t *provider, const char *key, const char **cursor) {
    if (strcmp(key, "message") != 0 || **cursor != '"') {
        return json_scan_skip_value(cursor);
    }
    /* A message too long for the buffer is cut, not refused. */
    return json_scan_string(cursor, provider->error_text, sizeof(provider->error_text), NULL, NULL);
}

static int visit_chunk(deepseek_provider_t *provider, const char *key, const char **cursor) {
    if (strcmp(key, "choices") == 0 && !at_null(*cursor)) {
        return scan_choices(provider, cursor);
    }
    if (strcmp(key, "error") == 0 && !at_null(*cursor)) {
        /* Set first so an error without a usable message still reports. */
        memcpy(provider->error_text, DEEPSEEK_ERROR_SERVER, sizeof(DEEPSEEK_ERROR_SERVER));
        if (**cursor == '"') {
            return json_scan_string(cursor, provider->error_text, sizeof(provider->error_text), NULL, NULL);
        }
        if (**cursor == '{') {
            return scan_object(provider, cursor, visit_error);
        }
    }
    return json_scan_skip_value(cursor);
}

/* One "data:" payload.  It is validated as a whole before anything is taken
 * from it, so no content is ever delivered out of a malformed record. */
static int process_chunk(deepseek_provider_t *provider, const char *json) {
    const char *p = json_scan_whitespace(json);

    if (*p != '{' || json_scan_skip_value(&p) != 0 || !is_blank(p)) {
        return fail(provider, DEEPSEEK_ERROR_MALFORMED);
    }
    p = json;
    provider->error_text[0] = '\0';
    if (scan_object(provider, &p, visit_chunk) != 0) {
        return fail(provider, DEEPSEEK_ERROR_MALFORMED);
    }
    if (provider->error_text[0] != '\0') {
        /* A valid record reporting a server-side error: the feed succeeds. */
        fail(provider, provider->error_text);
    }
    return 0;
}

/* One line, NUL-terminated in provider->line with its line ending removed. */
static int process_line(deepseek_provider_t *provider) {
    char *line = provider->line;
    size_t length = provider->line_length;
    const char *value;

    provider->line_length = 0u;
    if (length > 0u && line[length - 1u] == '\r') {
        length--;
    }
    while (length > 0u && (line[length - 1u] == ' ' || line[length - 1u] == '\t')) {
        length--;
    }
    line[length] = '\0';

    if (length == 0u) {
        return 0;
    }
    if (provider->done) {
        return fail(provider, DEEPSEEK_ERROR_TRAILING);
    }
    if (line[0] == ':') {
        return 0;
    }
    if (strncmp(line, "data:", 5u) != 0) {
        return fail(provider, DEEPSEEK_ERROR_MALFORMED);
    }
    value = line + 5;
    if (*value == ' ') {
        value++;
    }
    if (strcmp(value, "[DONE]") == 0) {
        provider->done = true;
        emit(provider, PROVIDER_EVENT_DONE, NULL, 0u);
        return 0;
    }
    return process_chunk(provider, value);
}

int deepseek_provider_feed(deepseek_provider_t *provider, const char *data, size_t length) {
    size_t i;

    if (provider == NULL || (data == NULL && length > 0u) || !provider->built || provider->failed) {
        return -1;
    }
    for (i = 0u; i < length; i++) {
        char c = data[i];

        if (c == '\n') {
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
            return fail(provider, DEEPSEEK_ERROR_MALFORMED);
        }
        if (provider->done && c != ' ' && c != '\t' && c != '\r') {
            return fail(provider, DEEPSEEK_ERROR_TRAILING);
        }
        if (provider->line_length == DEEPSEEK_LINE_MAX) {
            return fail(provider, DEEPSEEK_ERROR_OVERSIZED);
        }
        provider->line[provider->line_length++] = c;
    }
    return 0;
}

int deepseek_provider_finish(deepseek_provider_t *provider) {
    if (provider == NULL || !provider->built || provider->failed) {
        return -1;
    }
    if (provider->line_length > 0u && process_line(provider) != 0) {
        return -1;
    }
    if (provider->failed) {
        return -1;
    }
    if (!provider->done) {
        return fail(provider, DEEPSEEK_ERROR_TRUNCATED);
    }
    return 0;
}

bool deepseek_provider_is_done(const deepseek_provider_t *provider) {
    return provider != NULL && provider->done && !provider->failed;
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

/* ---- Start policy ------------------------------------------------------- */

deepseek_start_t deepseek_provider_start_check(const ai_config_t *config, bool clock_synced) {
    if (!ai_config_is_deepseek(config)) {
        return DEEPSEEK_START_NOT_DEEPSEEK;
    }
    if (strcmp(config->host, AI_CONFIG_DEEPSEEK_HOST) != 0 || config->port != AI_CONFIG_DEEPSEEK_PORT) {
        return DEEPSEEK_START_BAD_ENDPOINT;
    }
    if (!ai_config_deepseek_model_known(config->model)) {
        return DEEPSEEK_START_BAD_MODEL;
    }
    if (config->bearer_token[0] == '\0') {
        return DEEPSEEK_START_NO_KEY;
    }
    if (!ai_config_bearer_token_valid(config->bearer_token)) {
        return DEEPSEEK_START_BAD_KEY;
    }
    if (!clock_synced) {
        return DEEPSEEK_START_CLOCK_NOT_SYNCED;
    }
    return DEEPSEEK_START_OK;
}

const char *deepseek_provider_start_text(deepseek_start_t status) {
    switch (status) {
        case DEEPSEEK_START_OK: return "";
        case DEEPSEEK_START_BAD_ENDPOINT: return "host is not DeepSeek";
        case DEEPSEEK_START_BAD_MODEL: return "unknown DeepSeek model";
        case DEEPSEEK_START_NO_KEY: return "set the API key (F5)";
        case DEEPSEEK_START_BAD_KEY: return "API key has bad characters";
        case DEEPSEEK_START_CLOCK_NOT_SYNCED: return "clock not synced: wait";
        default: return "provider is not DeepSeek";
    }
}
