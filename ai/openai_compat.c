#include "ai/openai_compat.h"

#include <string.h>

const openai_compat_dialect_t openai_compat_deepseek = {
    "system", "max_tokens", 0u, 200, "", "DeepSeek error", true, false, OLLAMA_REQUEST_MAX,
};

/* The GPT-6 models reason before answering: the limit is
 * max_completion_tokens, which counts reasoning; temperature is not accepted
 * while reasoning is on; and "low" is the least effort all of them take. */
const openai_compat_dialect_t openai_compat_openai = {
    "developer", "max_completion_tokens", PROVIDER_REASONING_HEADROOM_TOKENS, -1,
    ",\"reasoning_effort\":\"low\"", "OpenAI error", false, false, SSE_REQUEST_CAPACITY,
};

/* Mistral takes a temperature of at most 1.5, and its reasoning models send
 * content as an array of typed chunks. */
const openai_compat_dialect_t openai_compat_mistral = {
    "system", "max_tokens", 0u, 150, "", "Mistral error", false, true, SSE_REQUEST_CAPACITY,
};

#define FLAG_PART_NOT_TEXT 1u /* the content chunk being read has a type other than "text" */

/* ---- Request ------------------------------------------------------------ */

static bool append_message(sse_provider_t *provider, const char *role, const char *content, bool first) {
    return (first || sse_append(provider, ",")) && sse_append(provider, "{\"role\":\"") &&
           sse_append(provider, role) && sse_append(provider, "\",\"content\":\"") &&
           sse_append_escaped(provider, content) && sse_append(provider, "\"}");
}

/* ---- Reply -------------------------------------------------------------- */

static int visit_part(sse_provider_t *provider, const char *key, const char **cursor) {
    char type[SSE_KEY_MAX];

    if (strcmp(key, "type") == 0 && **cursor == '"') {
        if (sse_json_name(cursor, type, sizeof(type)) != 0) {
            return -1;
        }
        if (strcmp(type, "text") != 0) {
            provider->flags |= FLAG_PART_NOT_TEXT;
        }
        return 0;
    }
    if (strcmp(key, "text") == 0 && **cursor == '"') {
        return sse_json_payload(provider, cursor);
    }
    return json_scan_skip_value(cursor);
}

/* One entry of an array content.  Its type and text may come in either
 * order, so the text is only delivered once the whole entry has been read. */
static int scan_part(sse_provider_t *provider, const char **cursor) {
    if (**cursor != '{') {
        return -1;
    }
    provider->flags &= ~FLAG_PART_NOT_TEXT;
    provider->payload_length = 0u;
    if (sse_json_object(provider, cursor, visit_part) != 0) {
        return -1;
    }
    if ((provider->flags & FLAG_PART_NOT_TEXT) == 0u) {
        sse_emit_content(provider, provider->payload, provider->payload_length);
    }
    return 0;
}

static int visit_delta(sse_provider_t *provider, const char *key, const char **cursor) {
    /* Everything but the content is skipped: the role, a null content, a
     * refusal and the reasoner's reasoning_content are not reply text. */
    if (strcmp(key, "content") != 0 || sse_json_at_null(*cursor)) {
        return json_scan_skip_value(cursor);
    }
    if (**cursor == '[' && ((const openai_compat_dialect_t *)provider->variant)->content_parts) {
        return sse_json_array(provider, cursor, scan_part);
    }
    if (sse_json_payload(provider, cursor) != 0) {
        return -1;
    }
    sse_emit_content(provider, provider->payload, provider->payload_length);
    return 0;
}

static int visit_choice(sse_provider_t *provider, const char *key, const char **cursor) {
    if (strcmp(key, "delta") != 0) {
        return json_scan_skip_value(cursor);
    }
    return sse_json_object(provider, cursor, visit_delta);
}

static int scan_choice(sse_provider_t *provider, const char **cursor) {
    return sse_json_object(provider, cursor, visit_choice);
}

static int visit_error(sse_provider_t *provider, const char *key, const char **cursor) {
    const openai_compat_dialect_t *dialect = (const openai_compat_dialect_t *)provider->variant;

    if (!dialect->show_error_message || strcmp(key, "message") != 0 || **cursor != '"') {
        return json_scan_skip_value(cursor);
    }
    /* A message too long for the buffer is cut, not refused. */
    return json_scan_string(cursor, provider->error_text, sizeof(provider->error_text), NULL, NULL);
}

static int visit_chunk(sse_provider_t *provider, const char *key, const char **cursor) {
    const openai_compat_dialect_t *dialect = (const openai_compat_dialect_t *)provider->variant;

    if (strcmp(key, "choices") == 0 && !sse_json_at_null(*cursor)) {
        return **cursor == '[' ? sse_json_array(provider, cursor, scan_choice) : -1;
    }
    if (strcmp(key, "error") == 0 && !sse_json_at_null(*cursor)) {
        /* Set first so an error without a usable message still reports. */
        memcpy(provider->error_text, dialect->error_text, strlen(dialect->error_text) + 1u);
        if (**cursor == '"' && dialect->show_error_message) {
            return json_scan_string(cursor, provider->error_text, sizeof(provider->error_text), NULL, NULL);
        }
        if (**cursor == '{') {
            return sse_json_object(provider, cursor, visit_error);
        }
    }
    return json_scan_skip_value(cursor);
}

/* One "data:" payload. */
static int process_chunk(sse_provider_t *provider, const char *json) {
    const char *p = json;

    if (!sse_json_is_object(json)) {
        return sse_fail(provider, SSE_ERROR_MALFORMED);
    }
    provider->error_text[0] = '\0';
    if (sse_json_object(provider, &p, visit_chunk) != 0) {
        return sse_fail(provider, SSE_ERROR_MALFORMED);
    }
    if (provider->error_text[0] != '\0') {
        /* A valid record reporting a server-side error: the feed succeeds. */
        sse_fail(provider, provider->error_text);
    }
    return 0;
}

static int on_line(sse_provider_t *provider, char *line) {
    const char *value;

    if (strncmp(line, "data:", 5u) != 0) {
        return sse_fail(provider, SSE_ERROR_MALFORMED);
    }
    value = line + 5;
    if (*value == ' ') {
        value++;
    }
    if (strcmp(value, "[DONE]") == 0) {
        sse_done(provider);
        return 0;
    }
    return process_chunk(provider, value);
}

static const sse_format_t openai_compat_format = {OPENAI_COMPAT_LINE_MAX, on_line, NULL, NULL};

int openai_compat_build_request(sse_provider_t *provider,
                                const openai_compat_dialect_t *dialect,
                                const char *model,
                                const ollama_message_t *messages,
                                size_t message_count,
                                int max_tokens,
                                const char *system_prompt,
                                int temperature) {
    bool first = true;
    size_t i;

    if (provider == NULL || dialect == NULL) {
        return -1;
    }
    sse_request_begin(provider, &openai_compat_format, dialect, dialect->request_max);

    if (model == NULL || model[0] == '\0' || message_count > OLLAMA_MAX_MESSAGES ||
        (message_count > 0u && messages == NULL) || max_tokens <= 0 || temperature < 0 || temperature > 200) {
        return -1;
    }

    if (!sse_append(provider, "{\"model\":\"") || !sse_append_escaped(provider, model) ||
        !sse_append(provider, "\",\"messages\":[")) {
        return -1;
    }
    if (system_prompt != NULL && system_prompt[0] != '\0') {
        if (!append_message(provider, dialect->system_role, system_prompt, first)) {
            return -1;
        }
        first = false;
    }
    for (i = 0u; i < message_count; i++) {
        if (!sse_role_known(messages[i].role) ||
            !append_message(provider, messages[i].role, messages[i].content != NULL ? messages[i].content : "",
                            first)) {
            return -1;
        }
        first = false;
    }
    if (!sse_append(provider, "],\"stream\":true,\"") || !sse_append(provider, dialect->limit_key) ||
        !sse_append(provider, "\":") ||
        !sse_append_uint(provider, (unsigned)max_tokens + dialect->limit_headroom)) {
        return -1;
    }
    if (dialect->temperature_max >= 0) {
        if (temperature > dialect->temperature_max) {
            temperature = dialect->temperature_max;
        }
        if (!sse_append(provider, ",\"temperature\":") || !sse_append_hundredths(provider, (unsigned)temperature)) {
            return -1;
        }
    }
    if (!sse_append(provider, dialect->tail) || !sse_append(provider, "}")) {
        return -1;
    }
    sse_request_end(provider);
    return 0;
}
