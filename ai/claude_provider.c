#include "ai/claude_provider.h"

#include <string.h>

#include "ai/openai_compat.h"

/* Stream state, kept until the reply ends. */
#define FLAG_STARTED 0x01u /* message_start has been seen */
#define FLAG_REFUSED 0x02u /* a message_delta reported stop_reason "refusal" */
/* Event state, cleared by the blank line that ends each event. */
#define FLAG_HAVE_DATA 0x04u
/* Data state, cleared before each data line is read. */
#define FLAG_TYPE_MATCHES 0x10u
#define FLAG_TYPE_DIFFERS 0x20u
#define FLAG_HAVE_DELTA 0x40u
#define FLAG_TEXT_DELTA 0x80u
#define FLAG_HAVE_TEXT 0x100u
#define FLAG_OVERLOADED 0x200u
#define FLAG_RATE_LIMITED 0x400u
#define DATA_FLAGS 0x7f0u

/* ---- Request ------------------------------------------------------------ */

static bool claude_role(const char *role) {
    return role != NULL && (strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0);
}

/* ---- Reply -------------------------------------------------------------- */

static bool event_is(const sse_provider_t *provider, const char *name) {
    return strcmp(provider->event, name) == 0;
}

static int visit_content_delta(sse_provider_t *provider, const char *key, const char **cursor) {
    char type[SSE_KEY_MAX];

    if (strcmp(key, "type") == 0 && **cursor == '"') {
        if (sse_json_name(cursor, type, sizeof(type)) != 0) {
            return -1;
        }
        if (strcmp(type, "text_delta") == 0) {
            provider->flags |= FLAG_TEXT_DELTA;
        }
        return 0;
    }
    if (strcmp(key, "text") == 0 && **cursor == '"') {
        provider->flags |= FLAG_HAVE_TEXT;
        return sse_json_payload(provider, cursor);
    }
    return json_scan_skip_value(cursor);
}

static int visit_message_delta(sse_provider_t *provider, const char *key, const char **cursor) {
    char reason[SSE_KEY_MAX];

    if (strcmp(key, "stop_reason") != 0 || **cursor != '"') {
        return json_scan_skip_value(cursor);
    }
    if (sse_json_name(cursor, reason, sizeof(reason)) != 0) {
        return -1;
    }
    if (strcmp(reason, "refusal") == 0) {
        provider->flags |= FLAG_REFUSED;
    }
    return 0;
}

static int visit_error(sse_provider_t *provider, const char *key, const char **cursor) {
    char type[SSE_KEY_MAX];

    if (strcmp(key, "type") != 0 || **cursor != '"') {
        return json_scan_skip_value(cursor);
    }
    if (sse_json_name(cursor, type, sizeof(type)) != 0) {
        return -1;
    }
    if (strcmp(type, "overloaded_error") == 0) {
        provider->flags |= FLAG_OVERLOADED;
    } else if (strcmp(type, "rate_limit_error") == 0) {
        provider->flags |= FLAG_RATE_LIMITED;
    }
    return 0;
}

static int visit_data(sse_provider_t *provider, const char *key, const char **cursor) {
    char type[SSE_KEY_MAX];

    if (strcmp(key, "type") == 0) {
        if (**cursor != '"' || sse_json_name(cursor, type, sizeof(type)) != 0) {
            return -1;
        }
        provider->flags |= strcmp(type, provider->event) == 0 ? FLAG_TYPE_MATCHES : FLAG_TYPE_DIFFERS;
        return 0;
    }
    if (strcmp(key, "delta") == 0 && **cursor == '{') {
        if (event_is(provider, "content_block_delta")) {
            provider->flags |= FLAG_HAVE_DELTA;
            return sse_json_object(provider, cursor, visit_content_delta);
        }
        if (event_is(provider, "message_delta")) {
            return sse_json_object(provider, cursor, visit_message_delta);
        }
    }
    if (strcmp(key, "error") == 0 && **cursor == '{' && event_is(provider, "error")) {
        return sse_json_object(provider, cursor, visit_error);
    }
    return json_scan_skip_value(cursor);
}

/* An event's data.  It is validated as a whole, and its type checked against
 * the event's name, before anything is taken from it. */
static int process_data(sse_provider_t *provider, const char *json) {
    const char *p = json;
    unsigned flags;

    provider->flags &= ~DATA_FLAGS;
    provider->payload_length = 0u;
    if (!sse_json_is_object(json) || sse_json_object(provider, &p, visit_data) != 0) {
        return sse_fail(provider, SSE_ERROR_MALFORMED);
    }
    flags = provider->flags;
    if ((flags & FLAG_TYPE_MATCHES) == 0u || (flags & FLAG_TYPE_DIFFERS) != 0u) {
        return sse_fail(provider, SSE_ERROR_MALFORMED);
    }

    if (event_is(provider, "error")) {
        /* A valid record reporting a server-side error: the feed succeeds. */
        sse_fail(provider, (flags & FLAG_OVERLOADED) != 0u     ? CLAUDE_ERROR_OVERLOADED
                           : (flags & FLAG_RATE_LIMITED) != 0u ? CLAUDE_ERROR_RATE_LIMITED
                                                               : CLAUDE_ERROR_SERVER);
        return 0;
    }
    if (event_is(provider, "message_start")) {
        if ((flags & FLAG_STARTED) != 0u) {
            return sse_fail(provider, SSE_ERROR_MALFORMED);
        }
        provider->flags |= FLAG_STARTED;
        return 0;
    }
    if (event_is(provider, "ping")) {
        return 0;
    }
    if (event_is(provider, "content_block_start") || event_is(provider, "content_block_stop") ||
        event_is(provider, "content_block_delta") || event_is(provider, "message_delta") ||
        event_is(provider, "message_stop")) {
        if ((flags & FLAG_STARTED) == 0u) {
            return sse_fail(provider, SSE_ERROR_MALFORMED);
        }
    }
    if (event_is(provider, "content_block_delta")) {
        if ((flags & FLAG_HAVE_DELTA) == 0u ||
            ((flags & FLAG_TEXT_DELTA) != 0u && (flags & FLAG_HAVE_TEXT) == 0u)) {
            return sse_fail(provider, SSE_ERROR_MALFORMED);
        }
        if ((flags & FLAG_TEXT_DELTA) != 0u) {
            sse_emit_content(provider, provider->payload, provider->payload_length);
        }
        return 0;
    }
    if (event_is(provider, "message_stop")) {
        if ((flags & FLAG_REFUSED) != 0u) {
            sse_fail(provider, CLAUDE_ERROR_REFUSED);
        } else {
            sse_done(provider);
        }
    }
    /* Anything else, including events added to the API later, is ignored. */
    return 0;
}

/* The value of a "name:" field line, or NULL if line is not that field. */
static const char *field_value(const char *line, const char *name) {
    size_t length = strlen(name);

    if (strncmp(line, name, length) != 0 || line[length] != ':') {
        return NULL;
    }
    return line[length + 1u] == ' ' ? line + length + 2u : line + length + 1u;
}

static int on_line(sse_provider_t *provider, char *line) {
    const char *value;

    if ((value = field_value(line, "event")) != NULL) {
        size_t length = strlen(value);

        if (provider->event[0] != '\0' || length == 0u) {
            return sse_fail(provider, SSE_ERROR_MALFORMED);
        }
        /* A name too long to keep is cut, which matches no known event. */
        if (length >= sizeof(provider->event)) {
            length = sizeof(provider->event) - 1u;
        }
        memcpy(provider->event, value, length);
        provider->event[length] = '\0';
        return 0;
    }
    if ((value = field_value(line, "data")) != NULL) {
        if (provider->event[0] == '\0' || (provider->flags & FLAG_HAVE_DATA) != 0u) {
            return sse_fail(provider, SSE_ERROR_MALFORMED);
        }
        provider->flags |= FLAG_HAVE_DATA;
        return process_data(provider, value);
    }
    return sse_fail(provider, SSE_ERROR_MALFORMED);
}

static int on_blank(sse_provider_t *provider) {
    bool incomplete = provider->event[0] != '\0' && (provider->flags & FLAG_HAVE_DATA) == 0u;

    provider->event[0] = '\0';
    provider->flags &= ~(FLAG_HAVE_DATA | DATA_FLAGS);
    return incomplete ? sse_fail(provider, SSE_ERROR_MALFORMED) : 0;
}

/* True if what was kept of a content_block_delta data line shows a delta
 * whose type is something other than "text_delta".  The line is cut short, so
 * the scanners fail where it ends; anything not proven is answered false. */
static bool delta_is_not_text(const char *json) {
    char key[SSE_KEY_MAX];
    char type[SSE_KEY_MAX];
    const char *p = json_scan_whitespace(json);
    bool in_delta = false;

    if (*p != '{') {
        return false;
    }
    p++;
    for (;;) {
        if (sse_json_key(&p, key, sizeof(key)) != 0) {
            return false;
        }
        if (in_delta && strcmp(key, "type") == 0) {
            return *p == '"' && sse_json_name(&p, type, sizeof(type)) == 0 && strcmp(type, "text_delta") != 0;
        }
        if (!in_delta && strcmp(key, "delta") == 0) {
            if (*p != '{') {
                return false;
            }
            in_delta = true;
            p++;
            continue;
        }
        if (json_scan_skip_value(&p) != 0) {
            return false;
        }
        p = json_scan_whitespace(p);
        if (*p != ',') {
            return false;
        }
        p++;
    }
}

static bool on_oversized(sse_provider_t *provider, const char *prefix) {
    const char *value = field_value(prefix, "data");

    if (value == NULL || provider->event[0] == '\0' || (provider->flags & FLAG_HAVE_DATA) != 0u) {
        return false;
    }
    if (event_is(provider, "content_block_delta")) {
        if ((provider->flags & FLAG_STARTED) == 0u || !delta_is_not_text(value)) {
            return false;
        }
    } else if (event_is(provider, "message_start") || event_is(provider, "message_delta") ||
               event_is(provider, "message_stop") || event_is(provider, "error")) {
        return false;
    }
    provider->flags |= FLAG_HAVE_DATA;
    return true;
}

static const sse_format_t claude_format = {CLAUDE_LINE_MAX, on_line, on_blank, on_oversized};

int claude_provider_build_request(sse_provider_t *provider,
                                  const char *model,
                                  const ollama_message_t *messages,
                                  size_t message_count,
                                  int max_tokens,
                                  const char *system_prompt,
                                  int temperature) {
    size_t i;

    (void)temperature;
    if (provider == NULL) {
        return -1;
    }
    sse_request_begin(provider, &claude_format, NULL, SSE_REQUEST_CAPACITY);

    if (model == NULL || model[0] == '\0' || message_count > OLLAMA_MAX_MESSAGES ||
        (message_count > 0u && messages == NULL) || max_tokens <= 0) {
        return -1;
    }
    if (!sse_append(provider, "{\"model\":\"") || !sse_append_escaped(provider, model) ||
        !sse_append(provider, "\",\"max_tokens\":") ||
        !sse_append_uint(provider, (unsigned)max_tokens + PROVIDER_REASONING_HEADROOM_TOKENS) ||
        !sse_append(provider, ",\"stream\":true,\"output_config\":{\"effort\":\"low\"},")) {
        return -1;
    }
    if (system_prompt != NULL && system_prompt[0] != '\0') {
        if (!sse_append(provider, "\"system\":\"") || !sse_append_escaped(provider, system_prompt) ||
            !sse_append(provider, "\",")) {
            return -1;
        }
    }
    if (!sse_append(provider, "\"messages\":[")) {
        return -1;
    }
    for (i = 0u; i < message_count; i++) {
        if (!claude_role(messages[i].role) || (i > 0u && !sse_append(provider, ",")) ||
            !sse_append(provider, "{\"role\":\"") || !sse_append(provider, messages[i].role) ||
            !sse_append(provider, "\",\"content\":\"") ||
            !sse_append_escaped(provider, messages[i].content != NULL ? messages[i].content : "") ||
            !sse_append(provider, "\"}")) {
            return -1;
        }
    }
    if (!sse_append(provider, "]}")) {
        return -1;
    }
    sse_request_end(provider);
    return 0;
}
