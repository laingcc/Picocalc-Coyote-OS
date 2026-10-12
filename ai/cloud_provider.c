#include "ai/cloud_provider.h"

#include <string.h>

#include "ai/claude_provider.h"
#include "ai/gemini_provider.h"
#include "ai/openai_compat.h"

typedef int (*build_fn)(sse_provider_t *provider, const char *model, const ollama_message_t *messages,
                        size_t message_count, int max_tokens, const char *system_prompt, int temperature);

typedef struct {
    const char *name;
    const char *path;         /* NULL: the path names the model (Gemini) */
    const char *key_header;   /* NULL: "Authorization: Bearer" */
    const char *extra_header; /* NULL for none */
    build_fn build;
} wire_t;

static int build_deepseek(sse_provider_t *provider, const char *model, const ollama_message_t *messages,
                          size_t message_count, int max_tokens, const char *system_prompt, int temperature) {
    return openai_compat_build_request(provider, &openai_compat_deepseek, model, messages, message_count,
                                       max_tokens, system_prompt, temperature);
}

static int build_openai(sse_provider_t *provider, const char *model, const ollama_message_t *messages,
                        size_t message_count, int max_tokens, const char *system_prompt, int temperature) {
    return openai_compat_build_request(provider, &openai_compat_openai, model, messages, message_count,
                                       max_tokens, system_prompt, temperature);
}

static int build_mistral(sse_provider_t *provider, const char *model, const ollama_message_t *messages,
                         size_t message_count, int max_tokens, const char *system_prompt, int temperature) {
    return openai_compat_build_request(provider, &openai_compat_mistral, model, messages, message_count,
                                       max_tokens, system_prompt, temperature);
}

static const wire_t wires[] = {
    {AI_CONFIG_PROVIDER_DEEPSEEK, "/chat/completions", NULL, NULL, build_deepseek},
    {AI_CONFIG_PROVIDER_OPENAI, "/v1/chat/completions", NULL, NULL, build_openai},
    {AI_CONFIG_PROVIDER_MISTRAL, "/v1/chat/completions", NULL, NULL, build_mistral},
    {AI_CONFIG_PROVIDER_GEMINI, NULL, GEMINI_API_KEY_HEADER, NULL, gemini_provider_build_request},
    {AI_CONFIG_PROVIDER_CLAUDE, CLAUDE_PATH, CLAUDE_API_KEY_HEADER, CLAUDE_API_VERSION_HEADER,
     claude_provider_build_request},
};

static const wire_t *wire_for(const char *provider) {
    size_t i;

    for (i = 0u; i < sizeof(wires) / sizeof(wires[0]); i++) {
        if (strcmp(provider, wires[i].name) == 0) {
            return &wires[i];
        }
    }
    return NULL;
}

cloud_start_t cloud_provider_start_check(const ai_config_t *config, bool clock_synced) {
    const ai_config_cloud_t *cloud = ai_config_cloud(config);

    if (cloud == NULL || wire_for(cloud->name) == NULL) {
        return CLOUD_START_NOT_CLOUD;
    }
    if (strcmp(config->host, cloud->host) != 0 || config->port != AI_CONFIG_CLOUD_PORT) {
        return CLOUD_START_BAD_ENDPOINT;
    }
    if (!ai_config_cloud_model_known(cloud, config->model)) {
        return CLOUD_START_BAD_MODEL;
    }
    if (config->bearer_token[0] == '\0') {
        return CLOUD_START_NO_KEY;
    }
    if (!ai_config_bearer_token_valid(config->bearer_token)) {
        return CLOUD_START_BAD_KEY;
    }
    if (!clock_synced) {
        return CLOUD_START_CLOCK_NOT_SYNCED;
    }
    return CLOUD_START_OK;
}

const char *cloud_provider_start_text(cloud_start_t status) {
    switch (status) {
        case CLOUD_START_OK: return "";
        case CLOUD_START_BAD_ENDPOINT: return "host is not the provider's";
        case CLOUD_START_BAD_MODEL: return "unknown model: pick one (F5)";
        case CLOUD_START_NO_KEY: return "set the API key (F5)";
        case CLOUD_START_BAD_KEY: return "API key has bad characters";
        case CLOUD_START_CLOCK_NOT_SYNCED: return "clock not synced: wait";
        default: return "not a cloud provider";
    }
}

cloud_prepare_t cloud_provider_prepare(const ai_config_t *config,
                                       bool clock_synced,
                                       const ollama_message_t *messages,
                                       size_t message_count,
                                       sse_provider_t *provider,
                                       char path[CLOUD_PROVIDER_PATH_CAPACITY],
                                       http_stream_request_t *request,
                                       cloud_start_t *status) {
    cloud_start_t verdict = cloud_provider_start_check(config, clock_synced);
    const ai_config_cloud_t *cloud;
    const wire_t *wire;

    if (status != NULL) {
        *status = verdict;
    }
    if (request != NULL) {
        memset(request, 0, sizeof(*request));
    }
    if (verdict != CLOUD_START_OK) {
        return CLOUD_PREPARE_NOT_READY;
    }
    if (provider == NULL || path == NULL || request == NULL) {
        return CLOUD_PREPARE_NO_BODY;
    }
    cloud = ai_config_cloud(config);
    wire = wire_for(cloud->name);

    if (wire->build(provider, config->model, messages, message_count, (int)config->max_predict,
                    config->system_prompt, (int)config->temperature) != 0) {
        return CLOUD_PREPARE_NO_BODY;
    }
    if (wire->path != NULL) {
        if (strlen(wire->path) >= CLOUD_PROVIDER_PATH_CAPACITY) {
            return CLOUD_PREPARE_NO_BODY;
        }
        memcpy(path, wire->path, strlen(wire->path) + 1u);
    } else if (gemini_provider_path(config->model, path, CLOUD_PROVIDER_PATH_CAPACITY) != 0) {
        return CLOUD_PREPARE_NO_BODY;
    }

    /* Always the provider's own host over TLS; the key only in its header. */
    request->host = cloud->host;
    request->port = AI_CONFIG_CLOUD_PORT;
    request->tls = true;
    if (wire->key_header != NULL) {
        request->api_key_header = wire->key_header;
        request->api_key = config->bearer_token;
    } else {
        request->bearer_token = config->bearer_token;
    }
    request->extra_header = wire->extra_header;
    request->method = "POST";
    request->path = path;
    request->content_type = "application/json";
    request->body = sse_provider_request(provider);
    return CLOUD_PREPARE_OK;
}

const char *cloud_provider_status_text(const char *provider, int status) {
    if (provider == NULL || ai_config_cloud_find(provider) == NULL) {
        return NULL;
    }
    switch (status) {
        case 400:
            /* Gemini answers a key it does not recognise with 400, not 401. */
            return strcmp(provider, AI_CONFIG_PROVIDER_GEMINI) == 0 ? "API key or request refused" : NULL;
        case 401: return "API key rejected";
        case 402:
            return strcmp(provider, AI_CONFIG_PROVIDER_DEEPSEEK) == 0 ? "no DeepSeek balance" : "billing problem";
        case 403: return "key not allowed to do this";
        case 404: return "model not available";
        case 429: return "rate limited: try later";
        case 500:
        case 502:
        case 503:
        case 504:
        case 529: return "provider busy: try later";
        default: return NULL;
    }
}
