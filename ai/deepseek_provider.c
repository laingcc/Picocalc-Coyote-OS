#include "ai/deepseek_provider.h"

#include <string.h>

/*
 * DeepSeek speaks the OpenAI-style protocol, so the request builder and the
 * reply parser are the shared ones in ai/openai_compat; this file binds them
 * to DeepSeek's dialect and keeps the DeepSeek-named interface.
 */

void deepseek_provider_init(deepseek_provider_t *provider, provider_event_callback_t callback, void *context) {
    sse_provider_init(provider, callback, context);
}

int deepseek_provider_build_request(deepseek_provider_t *provider,
                                    const char *model,
                                    const ollama_message_t *messages,
                                    size_t message_count,
                                    int max_tokens,
                                    const char *system_prompt,
                                    int temperature) {
    if (provider == NULL) {
        return -1;
    }
    if (!ai_config_deepseek_model_known(model)) {
        /* Leave no request behind, as a failed build always does. */
        sse_request_begin(provider, NULL, NULL, 0u);
        return -1;
    }
    return openai_compat_build_request(provider, &openai_compat_deepseek, model, messages, message_count,
                                       max_tokens, system_prompt, temperature);
}

size_t deepseek_provider_request_length(const deepseek_provider_t *provider) {
    return sse_provider_request_length(provider);
}

size_t deepseek_provider_read(const deepseek_provider_t *provider, size_t offset, char *destination,
                              size_t capacity) {
    return sse_provider_read(provider, offset, destination, capacity);
}

provider_request_t deepseek_provider_request(deepseek_provider_t *provider) {
    return sse_provider_request(provider);
}

int deepseek_provider_feed(deepseek_provider_t *provider, const char *data, size_t length) {
    return sse_provider_feed(provider, data, length);
}

int deepseek_provider_finish(deepseek_provider_t *provider) {
    return sse_provider_finish(provider);
}

bool deepseek_provider_is_done(const deepseek_provider_t *provider) {
    return sse_provider_is_done(provider);
}

bool deepseek_provider_failed(const deepseek_provider_t *provider) {
    return sse_provider_failed(provider);
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
    return cloud_provider_start_check(config, clock_synced);
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
