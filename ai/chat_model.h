#ifndef COYOTE_AI_CHAT_MODEL_H
#define COYOTE_AI_CHAT_MODEL_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Fixed-capacity chat transcript model.
 *
 * Storage is entirely static: at most CHAT_MAX_MESSAGES messages of
 * CHAT_MESSAGE_MAX bytes (plus NUL) each, and a CHAT_COMPOSER_MAX byte (plus
 * NUL) composer.  Messages are stored as complete user/assistant pairs.  When a
 * new turn is submitted the model evicts the oldest complete turn until the
 * projected request (as measured by the caller-supplied callback) fits within
 * max_request_bytes and there is room for the new pair.
 *
 * A submission is rejected when the composer is empty, when the turn cannot fit
 * the request bound, or when a response is already streaming.  A response that
 * is abandoned with fail/cancel is retained and flagged partial.
 *
 * Storage
 * -------
 * chat_model_t is large: it embeds every message and the composer.  Firmware
 * must declare it as a static or global object, never as a stack local, so the
 * linker can account for it.  sizeof(chat_model_t) is bounded by
 * CHAT_MODEL_STORAGE_MAX_BYTES on both the 64-bit host tests and the 32-bit
 * RP2350 target.
 *
 * All entry points tolerate a NULL model (returning -1, 0, NULL or false as
 * appropriate) and the append/submit entry points reject a length that exceeds
 * the remaining capacity or a NULL buffer with a non-zero length.
 */

#define CHAT_MAX_MESSAGES 8u
#define CHAT_MAX_TURNS 4u
#define CHAT_MESSAGE_MAX 4095u
#define CHAT_COMPOSER_MAX 511u

/* Worst-case static storage for one chat_model_t.  Generous enough for the
 * 64-bit host build while still proving the firmware budget on the target. */
#define CHAT_MODEL_STORAGE_MAX_BYTES 36864u

typedef enum {
    CHAT_ROLE_USER = 0,
    CHAT_ROLE_ASSISTANT = 1
} chat_role_t;

typedef enum {
    CHAT_STATE_IDLE = 0,
    CHAT_STATE_STREAMING = 1
} chat_state_t;

typedef struct {
    chat_role_t role;
    char text[CHAT_MESSAGE_MAX + 1u];
    size_t length;
    bool partial;
} chat_message_t;

/*
 * Measures the request size, in bytes, that would be produced for the stored
 * messages plus a pending user message.  messages may be NULL when
 * message_count is 0.  The callback must not allocate.
 */
typedef size_t (*chat_request_measure_fn)(void *context,
                                           const chat_message_t *messages,
                                           size_t message_count,
                                           const char *pending_user,
                                           size_t pending_user_length);

typedef struct {
    chat_message_t messages[CHAT_MAX_MESSAGES];
    size_t message_count;
    size_t in_flight_index;
    chat_state_t state;
    char composer[CHAT_COMPOSER_MAX + 1u];
    size_t composer_length;
    size_t max_request_bytes;
    chat_request_measure_fn measure;
    void *measure_context;
} chat_model_t;

_Static_assert(sizeof(chat_model_t) <= CHAT_MODEL_STORAGE_MAX_BYTES,
               "chat_model_t exceeds its static RAM budget");
_Static_assert(CHAT_MAX_MESSAGES == CHAT_MAX_TURNS * 2u,
               "CHAT_MAX_MESSAGES must fit whole user/assistant turns");

void chat_model_init(chat_model_t *model,
                     chat_request_measure_fn measure,
                     void *measure_context,
                     size_t max_request_bytes);

/* Clear the transcript and composer while preserving the measure configuration. */
void chat_model_reset(chat_model_t *model);

/* Composer editing.  Append returns -1 (and leaves the composer unchanged) if
 * the text would exceed CHAT_COMPOSER_MAX. */
int chat_model_composer_append(chat_model_t *model, const char *text, size_t length);
int chat_model_composer_backspace(chat_model_t *model);
void chat_model_composer_clear(chat_model_t *model);
const char *chat_model_composer_text(const chat_model_t *model);
size_t chat_model_composer_length(const chat_model_t *model);

/* Submit the composer as a user turn.  Returns 0 on success, -1 on rejection. */
int chat_model_submit(chat_model_t *model);

/* Append streaming assistant text.  Returns -1 if not streaming or oversized. */
int chat_model_append_response(chat_model_t *model, const char *text, size_t length);
void chat_model_complete_response(chat_model_t *model);
void chat_model_fail_response(chat_model_t *model);
void chat_model_cancel_response(chat_model_t *model);

size_t chat_model_message_count(const chat_model_t *model);
const chat_message_t *chat_model_message_at(const chat_model_t *model, size_t index);
bool chat_model_is_streaming(const chat_model_t *model);

#endif
