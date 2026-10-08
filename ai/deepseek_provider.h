#ifndef COYOTE_AI_DEEPSEEK_PROVIDER_H
#define COYOTE_AI_DEEPSEEK_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

#include "ai/provider.h"

/*
 * DeepSeek / OpenAI-compatible /chat/completions streaming provider.
 *
 * deepseek_provider_build_request serialises model, messages, stream and
 * max_tokens into a fixed-capacity buffer. The buffer is exposed through
 * provider_request_t for offset-based reading.
 * Response bytes (SSE formatted lines `data: {...}`) are fed to
 * deepseek_provider_feed and translated into provider events.
 */

#define DEEPSEEK_REQUEST_MAX 2048u
#define DEEPSEEK_MODEL_MAX 63u
#define DEEPSEEK_MAX_MESSAGES 8u
#define DEEPSEEK_RECORD_MAX 2048u
#define DEEPSEEK_ERROR_MAX 256u

/* Worst-case static storage for one deepseek_provider_t. */
#define DEEPSEEK_PROVIDER_STORAGE_MAX_BYTES 8192u

typedef struct {
    const char *role;
    const char *content;
} deepseek_message_t;

typedef struct {
    char body[DEEPSEEK_REQUEST_MAX + 1u];
    size_t body_length;
    bool built;
    bool failed;
    bool done;

    char line[DEEPSEEK_RECORD_MAX + 1u];
    size_t line_length;
    bool overflow;

    char payload[DEEPSEEK_RECORD_MAX + 1u];
    char error_text[DEEPSEEK_ERROR_MAX];

    provider_event_callback_t callback;
    void *callback_context;
} deepseek_provider_t;

_Static_assert(sizeof(deepseek_provider_t) <= DEEPSEEK_PROVIDER_STORAGE_MAX_BYTES,
               "deepseek_provider_t exceeds its static RAM budget");

void deepseek_provider_init(deepseek_provider_t *provider, provider_event_callback_t callback, void *context);

/*
 * Serialise a request into DeepSeek chat completion JSON format.
 * Returns 0 on success, -1 on invalid argument or if the request body exceeds capacity.
 */
int deepseek_provider_build_request(deepseek_provider_t *provider,
                                    const char *model,
                                    const deepseek_message_t *messages,
                                    size_t message_count,
                                    int max_tokens);

/* Measured length, in bytes, of the built request body. */
size_t deepseek_provider_request_length(const deepseek_provider_t *provider);

/* Read up to capacity bytes of the request body starting at offset. */
size_t deepseek_provider_read(const deepseek_provider_t *provider, size_t offset, char *destination, size_t capacity);

/* Offset-readable view over the built request. */
provider_request_t deepseek_provider_request(deepseek_provider_t *provider);

/* Feed response bytes. Transformed into CONTENT, DONE, or ERROR events. */
int deepseek_provider_feed(deepseek_provider_t *provider, const char *data, size_t length);
int deepseek_provider_finish(deepseek_provider_t *provider);

bool deepseek_provider_is_done(const deepseek_provider_t *provider);
bool deepseek_provider_failed(const deepseek_provider_t *provider);

const char *deepseek_provider_method(void);       /* "POST" */
const char *deepseek_provider_path(void);         /* "/chat/completions" */
const char *deepseek_provider_content_type(void); /* "application/json" */

#endif
