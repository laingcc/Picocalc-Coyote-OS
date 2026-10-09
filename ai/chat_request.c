#include "ai/chat_request.h"

const char *chat_request_role_name(chat_role_t role) {
    return role == CHAT_ROLE_ASSISTANT ? "assistant" : "user";
}

size_t chat_request_convert(const chat_message_t *messages,
                            size_t message_count,
                            const char *pending_user,
                            ollama_message_t *out,
                            size_t capacity) {
    size_t count = 0u;
    size_t i;

    if (out == NULL || (messages == NULL && message_count != 0u)) {
        return CHAT_REQUEST_UNMEASURABLE;
    }
    for (i = 0u; i < message_count; i++) {
        if (messages[i].role == CHAT_ROLE_ASSISTANT && messages[i].length == 0u) {
            continue;
        }
        if (count == capacity) {
            return CHAT_REQUEST_UNMEASURABLE;
        }
        out[count].role = chat_request_role_name(messages[i].role);
        out[count].content = messages[i].text;
        count++;
    }
    if (pending_user != NULL) {
        if (count == capacity) {
            return CHAT_REQUEST_UNMEASURABLE;
        }
        out[count].role = chat_request_role_name(CHAT_ROLE_USER);
        out[count].content = pending_user;
        count++;
    }
    return count;
}

void chat_request_measure_init(chat_request_measure_t *measure, const ai_config_t *config) {
    if (measure == NULL) {
        return;
    }
    measure->config = config;
    ollama_provider_init(&measure->scratch, NULL, NULL);
}

size_t chat_request_measure(void *context,
                            const chat_message_t *messages,
                            size_t message_count,
                            const char *pending_user,
                            size_t pending_user_length) {
    chat_request_measure_t *measure = (chat_request_measure_t *)context;
    ollama_message_t converted[OLLAMA_MAX_MESSAGES];
    size_t count;

    if (measure == NULL || measure->config == NULL) {
        return CHAT_REQUEST_UNMEASURABLE;
    }
    if (pending_user != NULL && pending_user[pending_user_length] != '\0') {
        return CHAT_REQUEST_UNMEASURABLE;
    }
    count = chat_request_convert(messages, message_count, pending_user, converted, OLLAMA_MAX_MESSAGES);
    if (count == CHAT_REQUEST_UNMEASURABLE) {
        return CHAT_REQUEST_UNMEASURABLE;
    }
    if (ollama_provider_build_request(&measure->scratch, measure->config->model, converted, count,
                                      (int)measure->config->max_predict,
                                      measure->config->system_prompt,
                                      (int)measure->config->temperature) != 0) {
        return CHAT_REQUEST_UNMEASURABLE;
    }
    return ollama_provider_request_length(&measure->scratch);
}
