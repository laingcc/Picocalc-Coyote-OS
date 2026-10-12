#include "ai/gemini_provider.h"

#include <string.h>

#include "ai/openai_compat.h"

/* All cleared before each record is read. */
#define FLAG_FINISHED 0x01u       /* a candidate carried a finishReason */
#define FLAG_REPLY_BLOCKED 0x02u  /* ... other than STOP or MAX_TOKENS */
#define FLAG_PROMPT_BLOCKED 0x04u
#define FLAG_ERROR 0x08u
#define FLAG_PART_THOUGHT 0x10u   /* the part being read is the model's thinking */

/* ---- Reply -------------------------------------------------------------- */

static int visit_part(sse_provider_t *provider, const char *key, const char **cursor) {
    if (strcmp(key, "text") == 0 && **cursor == '"') {
        return sse_json_payload(provider, cursor);
    }
    if (strcmp(key, "thought") == 0 && strncmp(*cursor, "true", 4u) == 0) {
        provider->flags |= FLAG_PART_THOUGHT;
    }
    return json_scan_skip_value(cursor);
}

/* A part's text and its thought mark may come in either order, so the text is
 * only delivered once the whole part has been read. */
static int scan_part(sse_provider_t *provider, const char **cursor) {
    if (**cursor != '{') {
        return -1;
    }
    provider->flags &= ~FLAG_PART_THOUGHT;
    provider->payload_length = 0u;
    if (sse_json_object(provider, cursor, visit_part) != 0) {
        return -1;
    }
    if ((provider->flags & FLAG_PART_THOUGHT) == 0u) {
        sse_emit_content(provider, provider->payload, provider->payload_length);
    }
    return 0;
}

static int visit_content(sse_provider_t *provider, const char *key, const char **cursor) {
    if (strcmp(key, "parts") == 0 && **cursor == '[') {
        return sse_json_array(provider, cursor, scan_part);
    }
    return json_scan_skip_value(cursor);
}

static int visit_candidate(sse_provider_t *provider, const char *key, const char **cursor) {
    char reason[SSE_KEY_MAX];

    if (strcmp(key, "content") == 0 && **cursor == '{') {
        return sse_json_object(provider, cursor, visit_content);
    }
    if (strcmp(key, "finishReason") == 0 && **cursor == '"') {
        if (sse_json_name(cursor, reason, sizeof(reason)) != 0) {
            return -1;
        }
        provider->flags |= FLAG_FINISHED;
        if (strcmp(reason, "STOP") != 0 && strcmp(reason, "MAX_TOKENS") != 0) {
            provider->flags |= FLAG_REPLY_BLOCKED;
        }
        return 0;
    }
    return json_scan_skip_value(cursor);
}

static int scan_candidate(sse_provider_t *provider, const char **cursor) {
    return sse_json_object(provider, cursor, visit_candidate);
}

static int visit_feedback(sse_provider_t *provider, const char *key, const char **cursor) {
    if (strcmp(key, "blockReason") == 0 && **cursor == '"') {
        provider->flags |= FLAG_PROMPT_BLOCKED;
    }
    return json_scan_skip_value(cursor);
}

static int visit_response(sse_provider_t *provider, const char *key, const char **cursor) {
    if (strcmp(key, "candidates") == 0 && !sse_json_at_null(*cursor)) {
        return **cursor == '[' ? sse_json_array(provider, cursor, scan_candidate) : -1;
    }
    if (strcmp(key, "promptFeedback") == 0 && **cursor == '{') {
        return sse_json_object(provider, cursor, visit_feedback);
    }
    if (strcmp(key, "error") == 0 && !sse_json_at_null(*cursor)) {
        provider->flags |= FLAG_ERROR;
    }
    return json_scan_skip_value(cursor);
}

/* One "data:" payload.  It is validated as a whole before anything is taken
 * from it, so no content is ever delivered out of a malformed record. */
static int on_line(sse_provider_t *provider, char *line) {
    const char *json;
    const char *p;
    unsigned flags;

    if (strncmp(line, "data:", 5u) != 0) {
        return sse_fail(provider, SSE_ERROR_MALFORMED);
    }
    json = line[5] == ' ' ? line + 6 : line + 5;
    if (!sse_json_is_object(json)) {
        return sse_fail(provider, SSE_ERROR_MALFORMED);
    }
    provider->flags = 0u;
    p = json;
    if (sse_json_object(provider, &p, visit_response) != 0) {
        return sse_fail(provider, SSE_ERROR_MALFORMED);
    }
    flags = provider->flags;
    /* A valid record that ends the reply badly: the feed succeeds. */
    if ((flags & FLAG_ERROR) != 0u) {
        sse_fail(provider, GEMINI_ERROR_SERVER);
    } else if ((flags & FLAG_PROMPT_BLOCKED) != 0u) {
        sse_fail(provider, GEMINI_ERROR_PROMPT_BLOCKED);
    } else if ((flags & FLAG_REPLY_BLOCKED) != 0u) {
        sse_fail(provider, GEMINI_ERROR_REPLY_BLOCKED);
    } else if ((flags & FLAG_FINISHED) != 0u) {
        sse_done(provider);
    }
    return 0;
}

static const sse_format_t gemini_format = {GEMINI_LINE_MAX, on_line, NULL, NULL};

/* ---- Request ------------------------------------------------------------ */

static const char *gemini_role(const char *role) {
    if (role != NULL && strcmp(role, "user") == 0) {
        return "user";
    }
    if (role != NULL && strcmp(role, "assistant") == 0) {
        return "model";
    }
    return NULL;
}

int gemini_provider_build_request(sse_provider_t *provider,
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
    sse_request_begin(provider, &gemini_format, NULL, SSE_REQUEST_CAPACITY);

    if (model == NULL || model[0] == '\0' || message_count > OLLAMA_MAX_MESSAGES ||
        (message_count > 0u && messages == NULL) || max_tokens <= 0) {
        return -1;
    }
    if (!sse_append(provider, "{")) {
        return -1;
    }
    if (system_prompt != NULL && system_prompt[0] != '\0') {
        if (!sse_append(provider, "\"systemInstruction\":{\"parts\":[{\"text\":\"") ||
            !sse_append_escaped(provider, system_prompt) || !sse_append(provider, "\"}]},")) {
            return -1;
        }
    }
    if (!sse_append(provider, "\"contents\":[")) {
        return -1;
    }
    for (i = 0u; i < message_count; i++) {
        const char *role = gemini_role(messages[i].role);

        if (role == NULL || (i > 0u && !sse_append(provider, ",")) || !sse_append(provider, "{\"role\":\"") ||
            !sse_append(provider, role) || !sse_append(provider, "\",\"parts\":[{\"text\":\"") ||
            !sse_append_escaped(provider, messages[i].content != NULL ? messages[i].content : "") ||
            !sse_append(provider, "\"}]}")) {
            return -1;
        }
    }
    if (!sse_append(provider, "],\"generationConfig\":{\"maxOutputTokens\":") ||
        !sse_append_uint(provider, (unsigned)max_tokens + PROVIDER_REASONING_HEADROOM_TOKENS) ||
        !sse_append(provider, ",\"thinkingConfig\":{\"thinkingLevel\":\"LOW\"}}}")) {
        return -1;
    }
    sse_request_end(provider);
    return 0;
}

int gemini_provider_path(const char *model, char *out, size_t capacity) {
    size_t length;
    size_t i;

    if (model == NULL || out == NULL || model[0] == '\0') {
        return -1;
    }
    length = strlen(model);
    for (i = 0u; i < length; i++) {
        char c = model[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
              c == '.' || c == '_')) {
            return -1;
        }
    }
    if (sizeof(GEMINI_PATH_PREFIX) - 1u + length + sizeof(GEMINI_PATH_SUFFIX) > capacity) {
        return -1;
    }
    memcpy(out, GEMINI_PATH_PREFIX, sizeof(GEMINI_PATH_PREFIX) - 1u);
    memcpy(out + sizeof(GEMINI_PATH_PREFIX) - 1u, model, length);
    memcpy(out + sizeof(GEMINI_PATH_PREFIX) - 1u + length, GEMINI_PATH_SUFFIX, sizeof(GEMINI_PATH_SUFFIX));
    return 0;
}
