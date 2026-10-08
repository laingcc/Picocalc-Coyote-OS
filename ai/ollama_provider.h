#ifndef COYOTE_AI_OLLAMA_PROVIDER_H
#define COYOTE_AI_OLLAMA_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

#include "ai/json_stream.h"
#include "ai/provider.h"

/*
 * Direct Ollama /api/chat provider.
 *
 * ollama_provider_build_request serialises model, messages, stream and
 * options.num_predict into a fixed-capacity buffer.  The buffer is exposed
 * through a provider_request_t so the transport can measure it and read it by
 * offset; no full-request copy is ever required.  Response bytes are fed to
 * ollama_provider_feed and translated into provider events.
 *
 * The request cap is deliberately small to respect the firmware's static
 * storage budget; the chat model evicts turns until the measured request fits.
 *
 * Storage
 * -------
 * ollama_provider_t embeds the request buffer and a json_stream_t.  Firmware
 * must declare it as a static or global object, never as a stack local.
 * sizeof(ollama_provider_t) is bounded by OLLAMA_PROVIDER_STORAGE_MAX_BYTES on
 * both the 64-bit host tests and the 32-bit RP2350 target.
 */

#define OLLAMA_REQUEST_MAX 2048u
#define OLLAMA_MODEL_MAX 63u
#define OLLAMA_MAX_MESSAGES 8u

/* Worst-case static storage for one ollama_provider_t. */
#define OLLAMA_PROVIDER_STORAGE_MAX_BYTES 8192u

typedef struct {
    const char *role;
    const char *content;
} ollama_message_t;

typedef struct {
    char body[OLLAMA_REQUEST_MAX + 1u];
    size_t body_length;
    bool built;
    bool failed;
    bool done;
    json_stream_t stream;
    provider_event_callback_t callback;
    void *callback_context;
} ollama_provider_t;

_Static_assert(sizeof(ollama_provider_t) <= OLLAMA_PROVIDER_STORAGE_MAX_BYTES,
               "ollama_provider_t exceeds its static RAM budget");

void ollama_provider_init(ollama_provider_t *provider, provider_event_callback_t callback, void *context);

/*
 * Serialise a request.  Returns 0 on success, -1 if the model is empty, the
 * message count exceeds OLLAMA_MAX_MESSAGES, message_count is non-zero with a
 * NULL messages array, or the request does not fit in OLLAMA_REQUEST_MAX bytes.
 * num_predict must be non-negative.
 */
int ollama_provider_build_request(ollama_provider_t *provider,
                                  const char *model,
                                  const ollama_message_t *messages,
                                  size_t message_count,
                                  int num_predict);

/* Measured length, in bytes, of the built request body. */
size_t ollama_provider_request_length(const ollama_provider_t *provider);

/* Read up to capacity bytes of the request body starting at offset.  Returns 0
 * when the provider is NULL, when destination is NULL with a non-zero capacity,
 * or once offset has reached the end of the body. */
size_t ollama_provider_read(const ollama_provider_t *provider, size_t offset, char *destination, size_t capacity);

/* Offset-readable view over the built request. */
provider_request_t ollama_provider_request(ollama_provider_t *provider);

/* Feed response bytes.  A syntactically valid record carrying an error still
 * returns 0 with an ERROR event; the provider then latches failed and rejects
 * further feeds.  A DONE event latches the stream as complete, and every feed
 * after either terminal returns -1.  Returns -1 if not built, already failed,
 * already done, or fed a NULL buffer with a non-zero length. */
int ollama_provider_feed(ollama_provider_t *provider, const char *data, size_t length);
int ollama_provider_finish(ollama_provider_t *provider);

bool ollama_provider_is_done(const ollama_provider_t *provider);
bool ollama_provider_failed(const ollama_provider_t *provider);

const char *ollama_provider_method(void);       /* "POST" */
const char *ollama_provider_path(void);         /* "/api/chat" */
const char *ollama_provider_content_type(void); /* "application/json" */

#endif
