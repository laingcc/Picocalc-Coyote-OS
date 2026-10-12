#ifndef COYOTE_AI_DEEPSEEK_PROVIDER_H
#define COYOTE_AI_DEEPSEEK_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

#include "ai/ai_config.h"
#include "ai/json_stream.h"
#include "ai/ollama_provider.h"
#include "ai/provider.h"

/*
 * Direct DeepSeek provider: the OpenAI-compatible streaming chat completion
 * endpoint, POST https://api.deepseek.com/chat/completions.
 *
 * deepseek_provider_build_request serialises model, messages, stream,
 * max_tokens and temperature into a fixed-capacity buffer that the transport
 * reads by offset.  The reply is a server-sent event stream; its bytes are fed
 * to deepseek_provider_feed and translated into provider events.
 *
 * The request takes the same messages, and has the same size cap, as the
 * Ollama one, and for the same input it is never longer.  The chat model can
 * therefore keep measuring requests with the Ollama builder (chat_request.h)
 * whichever provider is in use.
 *
 * Reply stream
 * ------------
 * Lines end in LF or CRLF.  The only lines accepted are:
 *
 *   (blank)             event separator
 *   : anything          comment (DeepSeek sends ": keep-alive")
 *   data: {json}        one chat.completion.chunk object
 *   data: [DONE]        the terminal marker
 *
 * A chunk's choices[].delta.content, when it is a non-empty string, becomes a
 * CONTENT event.  A delta carrying only a role, a null or empty content, or
 * reasoning_content (deepseek-reasoner's thinking) produces no event.  A chunk
 * with an "error" member becomes an ERROR event carrying its message.
 *
 * The stream succeeds only if "data: [DONE]" arrives and nothing but
 * whitespace follows it.  Any other line, a line that is not valid JSON of the
 * expected shape, a line longer than DEEPSEEK_LINE_MAX, a NUL byte, or the end
 * of input before [DONE] emits one ERROR event and latches failure.
 *
 * Storage
 * -------
 * deepseek_provider_t embeds the request, line and content buffers.  Firmware
 * must declare it as a static or global object, never as a stack local.
 * Nothing here allocates.
 */

#define DEEPSEEK_REQUEST_MAX OLLAMA_REQUEST_MAX
#define DEEPSEEK_LINE_MAX 2048u
#define DEEPSEEK_ERROR_MAX 96u

/* Worst-case static storage for one deepseek_provider_t. */
#define DEEPSEEK_PROVIDER_STORAGE_MAX_BYTES 8192u

typedef struct {
    char body[DEEPSEEK_REQUEST_MAX + 1u];
    size_t body_length;
    bool built;
    bool failed;
    bool done;
    char line[DEEPSEEK_LINE_MAX + 1u];
    size_t line_length;
    char payload[DEEPSEEK_LINE_MAX + 1u];
    char error_text[DEEPSEEK_ERROR_MAX];
    provider_event_callback_t callback;
    void *callback_context;
} deepseek_provider_t;

_Static_assert(sizeof(deepseek_provider_t) <= DEEPSEEK_PROVIDER_STORAGE_MAX_BYTES,
               "deepseek_provider_t exceeds its static RAM budget");

void deepseek_provider_init(deepseek_provider_t *provider, provider_event_callback_t callback, void *context);

/*
 * Serialise a request.  Returns 0 on success, -1 if the model is not one of
 * the known DeepSeek models, a role is not "system", "user" or "assistant",
 * the message count exceeds OLLAMA_MAX_MESSAGES, message_count is non-zero
 * with a NULL messages array, max_tokens is not positive, temperature is
 * outside 0..200 (hundredths), or the request does not fit in
 * DEEPSEEK_REQUEST_MAX bytes.  system_prompt may be NULL or empty to omit the
 * leading system message.
 */
int deepseek_provider_build_request(deepseek_provider_t *provider,
                                    const char *model,
                                    const ollama_message_t *messages,
                                    size_t message_count,
                                    int max_tokens,
                                    const char *system_prompt,
                                    int temperature);

size_t deepseek_provider_request_length(const deepseek_provider_t *provider);
size_t deepseek_provider_read(const deepseek_provider_t *provider, size_t offset, char *destination, size_t capacity);
provider_request_t deepseek_provider_request(deepseek_provider_t *provider);

/* Feed reply bytes.  A well-formed chunk carrying an "error" still returns 0
 * with an ERROR event; the provider then latches failed and rejects further
 * feeds.  Returns -1 if not built, already failed, fed a NULL buffer with a
 * non-zero length, or the input is malformed (after an ERROR event). */
int deepseek_provider_feed(deepseek_provider_t *provider, const char *data, size_t length);

/* End of the reply.  A final line without a line ending is parsed first.
 * Returns 0 only if [DONE] was seen and the stream never failed. */
int deepseek_provider_finish(deepseek_provider_t *provider);

bool deepseek_provider_is_done(const deepseek_provider_t *provider);
bool deepseek_provider_failed(const deepseek_provider_t *provider);

const char *deepseek_provider_method(void);       /* "POST" */
const char *deepseek_provider_path(void);         /* "/chat/completions" */
const char *deepseek_provider_content_type(void); /* "application/json" */

/*
 * Whether a DeepSeek request may be started, decided before anything touches
 * the network.  clock_synced must be true only once SNTP has set the clock
 * since boot (time_service_trusted_now): certificate validity cannot be
 * judged without the real date, so a request made earlier is refused here
 * rather than attempted.
 */
typedef enum {
    DEEPSEEK_START_OK = 0,
    DEEPSEEK_START_NOT_DEEPSEEK, /* the configuration names another provider */
    DEEPSEEK_START_BAD_ENDPOINT, /* host or port is not DeepSeek's */
    DEEPSEEK_START_BAD_MODEL,
    DEEPSEEK_START_NO_KEY,
    DEEPSEEK_START_BAD_KEY,      /* not a valid bearer token */
    DEEPSEEK_START_CLOCK_NOT_SYNCED
} deepseek_start_t;

deepseek_start_t deepseek_provider_start_check(const ai_config_t *config, bool clock_synced);

/* A short message for a status line; "" for DEEPSEEK_START_OK. */
const char *deepseek_provider_start_text(deepseek_start_t status);

#endif
