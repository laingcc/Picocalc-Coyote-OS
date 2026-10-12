#ifndef COYOTE_AI_SSE_PROVIDER_H
#define COYOTE_AI_SSE_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

#include "ai/json_stream.h"
#include "ai/ollama_provider.h"
#include "ai/provider.h"

/*
 * What the cloud providers share: a fixed-capacity request body the transport
 * reads by offset, and a strict reader for a server-sent event reply.
 *
 * Each provider (ai/openai_compat, ai/claude_provider, ai/gemini_provider)
 * builds its own JSON into the body with the sse_append_* functions and
 * supplies an sse_format_t that says what its reply lines mean.  Everything
 * that must be the same for all of them lives here once: the line framing,
 * the size limits, the terminal rules and the failure latch.
 *
 * Reply stream
 * ------------
 * Lines end in LF or CRLF.  Blank lines and ": comment" lines are consumed
 * here; every other line goes to the format, which either accepts it or fails
 * the stream.  The stream succeeds only if the format reported the end of the
 * reply (sse_done) and nothing but whitespace followed it.  A line longer
 * than the format allows, a NUL byte, input after the end, or the end of
 * input before it emits one ERROR event and latches failure: every later
 * feed is refused and no further event is produced.
 *
 * A format may let an over-long line through unread (sse_format_t.oversized)
 * when what was kept of it proves it carries no reply text.  Nothing is ever
 * buffered beyond SSE_LINE_CAPACITY.
 *
 * Storage
 * -------
 * sse_provider_t embeds the request, line and decoded-text buffers.  Firmware
 * must declare it as a static or global object, never as a stack local.
 * Nothing here allocates.  Only one cloud request is in flight at a time, so
 * the firmware keeps a single one for every cloud provider.
 */

/* The chat model sizes requests with the Ollama builder (chat_request.h).  The
 * cloud bodies wrap the same messages a little differently, so the buffer is
 * larger than that bound by more than any of them can add: whatever the
 * Ollama builder accepts, each cloud builder can also serialise. */
#define SSE_REQUEST_CAPACITY (OLLAMA_REQUEST_MAX + 512u)
/* Gemini sends a reply in few, large records; the others stay under 2 KB. */
#define SSE_LINE_CAPACITY 8192u
#define SSE_ERROR_MAX 96u
#define SSE_KEY_MAX 24u

/* Worst-case static storage for one sse_provider_t. */
#define SSE_PROVIDER_STORAGE_MAX_BYTES 20480u

#define SSE_ERROR_OVERSIZED "reply line too long"
#define SSE_ERROR_MALFORMED "malformed reply stream"
#define SSE_ERROR_TRUNCATED "reply ended early"
#define SSE_ERROR_TRAILING "data after end of reply"

typedef struct sse_provider sse_provider_t;

typedef struct {
    /* Longest line accepted, at most SSE_LINE_CAPACITY. */
    size_t line_max;
    /* One line that is neither blank nor a comment, NUL-terminated with its
     * line ending and trailing blanks removed.  Returns 0, or -1 after
     * sse_fail. */
    int (*line)(sse_provider_t *provider, char *line);
    /* A blank line: the end of one event.  Returns as line does.  May be
     * NULL. */
    int (*blank)(sse_provider_t *provider);
    /* A line outgrew line_max; prefix holds its first line_max bytes.  Return
     * true to drop the rest of the line unparsed, false to fail the stream.
     * NULL means every over-long line fails. */
    bool (*oversized)(sse_provider_t *provider, const char *prefix);
} sse_format_t;

struct sse_provider {
    char body[SSE_REQUEST_CAPACITY + 1u];
    size_t body_length;
    size_t body_max;
    bool built;
    bool failed;
    bool done;
    bool skipping; /* inside an over-long line the format let through */
    const sse_format_t *format;
    const void *variant; /* the format's own constant settings */
    char line[SSE_LINE_CAPACITY + 1u];
    size_t line_length;
    char payload[SSE_LINE_CAPACITY + 1u];
    size_t payload_length;
    char error_text[SSE_ERROR_MAX];
    char event[SSE_KEY_MAX]; /* scratch for the format: an event or type name */
    unsigned flags;          /* scratch for the format */
    provider_event_callback_t callback;
    void *callback_context;
};

_Static_assert(sizeof(sse_provider_t) <= SSE_PROVIDER_STORAGE_MAX_BYTES,
               "sse_provider_t exceeds its static RAM budget");

void sse_provider_init(sse_provider_t *provider, provider_event_callback_t callback, void *context);

/* ---- Request ------------------------------------------------------------ */

/* Start a new request body of at most body_max bytes whose reply will be read
 * with format.  Clears any earlier request and reply state. */
void sse_request_begin(sse_provider_t *provider, const sse_format_t *format, const void *variant, size_t body_max);
/* Each returns false, leaving the body as it was, if the text does not fit. */
bool sse_append(sse_provider_t *provider, const char *text);
bool sse_append_escaped(sse_provider_t *provider, const char *text); /* as a JSON string body */
bool sse_append_uint(sse_provider_t *provider, unsigned value);
bool sse_append_hundredths(sse_provider_t *provider, unsigned value); /* 80 -> "0.80" */
/* The body is complete; until this the provider has no request and reads no
 * reply. */
void sse_request_end(sse_provider_t *provider);

/* The roles a chat message may carry: "system", "user" or "assistant". */
bool sse_role_known(const char *role);

size_t sse_provider_request_length(const sse_provider_t *provider);
size_t sse_provider_read(const sse_provider_t *provider, size_t offset, char *destination, size_t capacity);
provider_request_t sse_provider_request(sse_provider_t *provider);

/* ---- Reply -------------------------------------------------------------- */

/* Feed reply bytes.  Returns -1 if no request was built, the stream already
 * failed, data is NULL with a non-zero length, or the input is malformed
 * (after an ERROR event).  A well-formed record that reports an error from
 * the server returns 0 with an ERROR event and latches failure. */
int sse_provider_feed(sse_provider_t *provider, const char *data, size_t length);
/* End of the reply.  A final line without a line ending is parsed first.
 * Returns 0 only if the reply ended properly and the stream never failed. */
int sse_provider_finish(sse_provider_t *provider);
bool sse_provider_is_done(const sse_provider_t *provider);
bool sse_provider_failed(const sse_provider_t *provider);

/* ---- For the formats ---------------------------------------------------- */

void sse_emit_content(sse_provider_t *provider, const char *data, size_t length);
/* The reply is complete: emits DONE.  Only whitespace may follow. */
void sse_done(sse_provider_t *provider);
/* Emit ERROR (once) and latch failure.  Always returns -1. */
int sse_fail(sse_provider_t *provider, const char *text);

/*
 * JSON walking over one NUL-terminated record, built on the json_scan_*
 * primitives.  A visitor is handed each member's key with *cursor at its
 * value, which it must consume; it returns 0 or -1.
 */
typedef int (*sse_member_fn)(sse_provider_t *provider, const char *key, const char **cursor);
typedef int (*sse_element_fn)(sse_provider_t *provider, const char **cursor);

/* True if text is one JSON object and nothing else.  Records are checked with
 * this before anything is taken from them. */
bool sse_json_is_object(const char *text);
int sse_json_object(sse_provider_t *provider, const char **cursor, sse_member_fn visit);
/* Walk an array, handing each element to visit.  Stops with -1 as soon as
 * the provider has failed (an event callback may cancel the request). */
int sse_json_array(sse_provider_t *provider, const char **cursor, sse_element_fn visit);
bool sse_json_at_null(const char *cursor);
/* Decode the string at *cursor into the payload buffer and set
 * payload_length.  Fails if it is not a string or does not fit. */
int sse_json_payload(sse_provider_t *provider, const char **cursor);
/* Decode a short string (a type or reason name) into out.  One too long for
 * out is cut, which cannot match any name looked for. */
int sse_json_name(const char **cursor, char *out, size_t capacity);
/* Read an object key and the colon after it, leaving *cursor at the value.  A
 * key too long for the buffer is cut, which cannot match any name looked for. */
int sse_json_key(const char **cursor, char *key, size_t capacity);

#endif
