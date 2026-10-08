#include "ai/chat_model.h"

#include <string.h>

/*
 * No heap allocation and no variable length arrays.  All transcript and
 * composer storage is embedded in chat_model_t.
 */

static void evict_oldest_turn(chat_model_t *model) {
    if (model->message_count <= 2u) {
        model->message_count = 0u;
        return;
    }
    size_t remaining = model->message_count - 2u;
    memmove(&model->messages[0], &model->messages[2], remaining * sizeof(chat_message_t));
    model->message_count = remaining;
}

static void mark_partial(chat_model_t *model) {
    if (model->state != CHAT_STATE_STREAMING) {
        return;
    }
    model->messages[model->in_flight_index].partial = true;
    model->state = CHAT_STATE_IDLE;
}

void chat_model_init(chat_model_t *model,
                     chat_request_measure_fn measure,
                     void *measure_context,
                     size_t max_request_bytes) {
    memset(model, 0, sizeof(*model));
    model->measure = measure;
    model->measure_context = measure_context;
    model->max_request_bytes = max_request_bytes;
    model->state = CHAT_STATE_IDLE;
}

void chat_model_reset(chat_model_t *model) {
    model->message_count = 0u;
    model->in_flight_index = 0u;
    model->state = CHAT_STATE_IDLE;
    model->composer_length = 0u;
    model->composer[0] = '\0';
}

int chat_model_composer_append(chat_model_t *model, const char *text, size_t length) {
    if (length > CHAT_COMPOSER_MAX - model->composer_length) {
        return -1;
    }
    memcpy(model->composer + model->composer_length, text, length);
    model->composer_length += length;
    model->composer[model->composer_length] = '\0';
    return 0;
}

int chat_model_composer_backspace(chat_model_t *model) {
    if (model->composer_length == 0u) {
        return -1;
    }
    model->composer_length--;
    model->composer[model->composer_length] = '\0';
    return 0;
}

void chat_model_composer_clear(chat_model_t *model) {
    model->composer_length = 0u;
    model->composer[0] = '\0';
}

const char *chat_model_composer_text(const chat_model_t *model) {
    return model->composer;
}

size_t chat_model_composer_length(const chat_model_t *model) {
    return model->composer_length;
}

int chat_model_submit(chat_model_t *model) {
    if (model->state != CHAT_STATE_IDLE) {
        return -1; /* concurrent submission while a response is streaming */
    }
    if (model->composer_length == 0u) {
        return -1; /* empty submission */
    }
    if (model->composer_length > CHAT_MESSAGE_MAX) {
        return -1;
    }

    if (model->measure != NULL) {
        size_t projected = model->measure(model->measure_context, NULL, 0u, model->composer, model->composer_length);
        if (projected > model->max_request_bytes) {
            return -1; /* the new turn alone cannot fit the bound */
        }
    }

    while (model->message_count + 2u > CHAT_MAX_MESSAGES) {
        evict_oldest_turn(model);
    }
    if (model->measure != NULL) {
        while (model->message_count >= 2u &&
               model->measure(model->measure_context, model->messages, model->message_count, model->composer,
                              model->composer_length) > model->max_request_bytes) {
            evict_oldest_turn(model);
        }
    }

    chat_message_t *user = &model->messages[model->message_count++];
    user->role = CHAT_ROLE_USER;
    memcpy(user->text, model->composer, model->composer_length);
    user->text[model->composer_length] = '\0';
    user->length = model->composer_length;
    user->partial = false;

    chat_message_t *assistant = &model->messages[model->message_count++];
    assistant->role = CHAT_ROLE_ASSISTANT;
    assistant->text[0] = '\0';
    assistant->length = 0u;
    assistant->partial = false;

    model->in_flight_index = model->message_count - 1u;
    model->state = CHAT_STATE_STREAMING;
    model->composer_length = 0u;
    model->composer[0] = '\0';
    return 0;
}

int chat_model_append_response(chat_model_t *model, const char *text, size_t length) {
    if (model->state != CHAT_STATE_STREAMING) {
        return -1;
    }
    chat_message_t *message = &model->messages[model->in_flight_index];
    if (length > CHAT_MESSAGE_MAX - message->length) {
        return -1; /* oversized response */
    }
    memcpy(message->text + message->length, text, length);
    message->length += length;
    message->text[message->length] = '\0';
    return 0;
}

void chat_model_complete_response(chat_model_t *model) {
    if (model->state != CHAT_STATE_STREAMING) {
        return;
    }
    model->messages[model->in_flight_index].partial = false;
    model->state = CHAT_STATE_IDLE;
}

void chat_model_fail_response(chat_model_t *model) {
    mark_partial(model);
}

void chat_model_cancel_response(chat_model_t *model) {
    mark_partial(model);
}

size_t chat_model_message_count(const chat_model_t *model) {
    return model->message_count;
}

const chat_message_t *chat_model_message_at(const chat_model_t *model, size_t index) {
    if (index >= model->message_count) {
        return NULL;
    }
    return &model->messages[index];
}

bool chat_model_is_streaming(const chat_model_t *model) {
    return model->state == CHAT_STATE_STREAMING;
}
