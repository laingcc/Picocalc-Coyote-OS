#ifndef COYOTE_AI_CHAT_REQUEST_H
#define COYOTE_AI_CHAT_REQUEST_H

#include <stddef.h>

#include "ai/ai_config.h"
#include "ai/chat_model.h"
#include "ai/ollama_provider.h"

/*
 * Glue between the chat transcript and the Ollama request.
 *
 * chat_request_convert maps stored chat messages onto the ollama_message_t
 * array the provider serialises.  Nothing is copied: every content pointer
 * refers to the caller's message text, so the transcript must stay unchanged
 * until the request has been built.
 *
 * chat_request_measure is a chat_request_measure_fn.  It builds the real
 * request into a scratch provider and reports its length, so the chat model
 * evicts turns against exactly the bytes that would be sent.
 */

/* Returned when the messages cannot form a request at all. */
#define CHAT_REQUEST_UNMEASURABLE ((size_t)-1)

const char *chat_request_role_name(chat_role_t role); /* "user" / "assistant" */

/*
 * Fill out with one entry per message, followed by pending_user (when not
 * NULL) as a final user message.  Assistant messages with no text (the reply
 * still in flight, or one that failed before any content) are skipped.
 * Returns the number of entries written, or CHAT_REQUEST_UNMEASURABLE if they
 * do not fit in capacity or message_count is non-zero with NULL messages.
 */
size_t chat_request_convert(const chat_message_t *messages,
                            size_t message_count,
                            const char *pending_user,
                            ollama_message_t *out,
                            size_t capacity);

/*
 * Measure context.  It embeds a scratch ollama_provider_t, so firmware must
 * declare it as a static or global object, never as a stack local.  config
 * supplies the model name and max_predict at the time of each measurement.
 */
typedef struct {
    const ai_config_t *config;
    ollama_provider_t scratch;
} chat_request_measure_t;

void chat_request_measure_init(chat_request_measure_t *measure, const ai_config_t *config);

/*
 * context is a chat_request_measure_t.  pending_user, when not NULL, must be
 * NUL-terminated at pending_user_length (the chat model's composer is).
 * Returns the request body length in bytes, or CHAT_REQUEST_UNMEASURABLE when
 * no request can be built (no model, too many messages, or larger than
 * OLLAMA_REQUEST_MAX), which always exceeds the model's request bound.
 */
size_t chat_request_measure(void *context,
                            const chat_message_t *messages,
                            size_t message_count,
                            const char *pending_user,
                            size_t pending_user_length);

#endif
