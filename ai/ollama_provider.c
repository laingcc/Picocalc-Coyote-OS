#include "ai/ollama_provider.h"

#include <string.h>

/*
 * No heap allocation and no variable length arrays.  The request is built into
 * the provider's fixed buffer and read back by offset.
 */

static void json_bridge(void *context, const json_stream_event_t *event) {
    ollama_provider_t *provider = (ollama_provider_t *)context;
    provider_event_t out;
    out.data = event->data;
    out.length = event->length;
    switch (event->type) {
        case JSON_STREAM_EVENT_CONTENT:
            out.type = PROVIDER_EVENT_CONTENT;
            break;
        case JSON_STREAM_EVENT_DONE:
            out.type = PROVIDER_EVENT_DONE;
            provider->done = true;
            break;
        case JSON_STREAM_EVENT_ERROR:
            out.type = PROVIDER_EVENT_ERROR;
            provider->failed = true;
            break;
        default:
            return;
    }
    if (provider->callback != NULL) {
        provider->callback(provider->callback_context, &out);
    }
}

static bool append_raw(ollama_provider_t *provider, const char *text, size_t length) {
    if (length > OLLAMA_REQUEST_MAX - provider->body_length) {
        return false;
    }
    memcpy(provider->body + provider->body_length, text, length);
    provider->body_length += length;
    provider->body[provider->body_length] = '\0';
    return true;
}

static bool append_literal(ollama_provider_t *provider, const char *text) {
    return append_raw(provider, text, strlen(text));
}

static bool append_escaped(ollama_provider_t *provider, const char *text, size_t length) {
    size_t remaining = OLLAMA_REQUEST_MAX - provider->body_length;
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

static bool append_int(ollama_provider_t *provider, int value) {
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

void ollama_provider_init(ollama_provider_t *provider, provider_event_callback_t callback, void *context) {
    memset(provider, 0, sizeof(*provider));
    provider->callback = callback;
    provider->callback_context = context;
    json_stream_init(&provider->stream, json_bridge, provider);
}

int ollama_provider_build_request(ollama_provider_t *provider,
                                  const char *model,
                                  const ollama_message_t *messages,
                                  size_t message_count,
                                  int num_predict) {
    provider->body_length = 0u;
    provider->body[0] = '\0';
    provider->built = false;
    provider->failed = false;
    provider->done = false;
    json_stream_init(&provider->stream, json_bridge, provider);

    if (model == NULL || model[0] == '\0' || strlen(model) > OLLAMA_MODEL_MAX) {
        return -1;
    }
    if (message_count > OLLAMA_MAX_MESSAGES) {
        return -1;
    }
    if (num_predict < 0) {
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
    if (!append_literal(provider, "],\"stream\":true,\"options\":{\"num_predict\":")) {
        return -1;
    }
    if (!append_int(provider, num_predict)) {
        return -1;
    }
    if (!append_literal(provider, "}}")) {
        return -1;
    }
    provider->built = true;
    return 0;
}

size_t ollama_provider_request_length(const ollama_provider_t *provider) {
    return provider->body_length;
}

size_t ollama_provider_read(const ollama_provider_t *provider, size_t offset, char *destination, size_t capacity) {
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
    return ollama_provider_request_length((const ollama_provider_t *)context);
}

static size_t request_read_adapter(void *context, size_t offset, char *destination, size_t capacity) {
    return ollama_provider_read((const ollama_provider_t *)context, offset, destination, capacity);
}

provider_request_t ollama_provider_request(ollama_provider_t *provider) {
    provider_request_t request;
    request.length = request_length_adapter;
    request.read = request_read_adapter;
    request.context = provider;
    return request;
}

int ollama_provider_feed(ollama_provider_t *provider, const char *data, size_t length) {
    if (!provider->built || provider->failed) {
        return -1;
    }
    return json_stream_feed(&provider->stream, data, length);
}

int ollama_provider_finish(ollama_provider_t *provider) {
    if (!provider->built) {
        return -1;
    }
    return json_stream_finish(&provider->stream);
}

bool ollama_provider_is_done(const ollama_provider_t *provider) {
    return provider->done;
}

bool ollama_provider_failed(const ollama_provider_t *provider) {
    return provider->failed;
}

const char *ollama_provider_method(void) {
    return "POST";
}

const char *ollama_provider_path(void) {
    return "/api/chat";
}

const char *ollama_provider_content_type(void) {
    return "application/json";
}
