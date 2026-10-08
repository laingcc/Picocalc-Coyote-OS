#ifndef COYOTE_AI_JSON_STREAM_H
#define COYOTE_AI_JSON_STREAM_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Bounded, allocation-free JSON helpers for the Coyote AI chat path.
 *
 * json_stream_t incrementally consumes newline-delimited JSON (NDJSON) as
 * produced by an Ollama /api/chat streaming response.  It accepts arbitrarily
 * fragmented input (a record may be split at any byte boundary, including
 * inside an escape sequence) and delivers events through a caller-supplied
 * callback:
 *
 *   JSON_STREAM_EVENT_CONTENT - a non-empty message.content string
 *   JSON_STREAM_EVENT_DONE    - the record carried "done": true
 *   JSON_STREAM_EVENT_ERROR   - a record carried an "error", or the record was
 *                               malformed/oversized and the stream gave up
 *
 * The event payload pointer is owned by the stream and is only valid for the
 * duration of the callback.  On a malformed or oversized record the stream is
 * latched into a failed state, an ERROR event is delivered, and every later
 * call returns -1.
 */

#define JSON_STREAM_RECORD_MAX 2048u
#define JSON_STREAM_ERROR_MAX 256u

typedef enum {
    JSON_STREAM_EVENT_CONTENT = 0,
    JSON_STREAM_EVENT_DONE,
    JSON_STREAM_EVENT_ERROR
} json_stream_event_type_t;

typedef struct {
    json_stream_event_type_t type;
    const char *data; /* CONTENT/ERROR payload (NUL terminated); NULL for DONE */
    size_t length;    /* payload length in bytes, excluding the NUL terminator */
} json_stream_event_t;

typedef void (*json_stream_callback_t)(void *context, const json_stream_event_t *event);

typedef struct {
    char record[JSON_STREAM_RECORD_MAX + 1u];
    size_t record_length;
    bool overflow;
    bool failed;
    char payload[JSON_STREAM_RECORD_MAX + 1u];
    size_t payload_length;
    char error_text[JSON_STREAM_ERROR_MAX];
    size_t error_length;
    json_stream_callback_t callback;
    void *callback_context;
} json_stream_t;

void json_stream_init(json_stream_t *stream, json_stream_callback_t callback, void *context);

/* Feed a chunk of NDJSON bytes.  Returns 0 on success (including partial
 * input), or -1 once the stream has failed. */
int json_stream_feed(json_stream_t *stream, const char *data, size_t length);

/* Signal end of the transport.  A trailing record without a newline is parsed,
 * so a truncated record fails here with an ERROR event.  Returns 0 if clean. */
int json_stream_finish(json_stream_t *stream);

bool json_stream_failed(const json_stream_t *stream);

/*
 * Escape src into a JSON string body (no surrounding quotes).  Writes a NUL
 * terminator when dst is non-NULL.  Returns 0 on success and stores the number
 * of body bytes in *out_length (excluding the NUL).  Returns -1 on buffer
 * overflow; *out_length is then set to the required size.  dst may be NULL to
 * measure only.
 */
int json_escape_string(const char *src, size_t src_length, char *dst, size_t dst_size, size_t *out_length);

/*
 * Decode a JSON string body (no surrounding quotes) into dst, handling the
 * standard short escapes and \uXXXX including valid UTF-16 surrogate pairs.
 * Returns 0 on success, -1 on malformed input or buffer overflow.
 */
int json_unescape_string(const char *src, size_t src_length, char *dst, size_t dst_size, size_t *out_length);

#endif
