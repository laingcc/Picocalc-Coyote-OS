#ifndef COYOTE_AI_HTTP_PARSER_H
#define COYOTE_AI_HTTP_PARSER_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Bounded, allocation-free incremental HTTP/1.x response parser.
 *
 * The parser accepts arbitrarily fragmented input: the status line, headers,
 * chunk-size lines and body bytes may all be split across feed() calls at any
 * offset.  It understands three body framings:
 *
 *   - Content-Length delimited
 *   - Transfer-Encoding: chunked (chunk extensions are ignored)
 *   - connection close delimited (completed by http_parser_finish)
 *
 * The head callback fires exactly once, as soon as the header block is
 * complete.  Body callbacks receive spans that point directly into the caller's
 * input buffer and are only valid for the duration of the call.
 *
 * Malformed headers, oversized headers, malformed/oversized chunk framing and
 * truncated bodies all latch the parser into a failed state and return -1.
 */

#define HTTP_PARSER_HEADER_MAX 1024u
#define HTTP_PARSER_CHUNK_LINE_MAX 32u
#define HTTP_PARSER_CHUNK_MAX 65536u

typedef struct {
    int status_code;         /* 100..999, or 0 before the head is parsed */
    bool has_content_length;
    size_t content_length;
    bool chunked;
    bool connection_close;
} http_response_head_t;

typedef void (*http_head_callback_t)(void *context, const http_response_head_t *head);
typedef void (*http_body_callback_t)(void *context, const char *data, size_t length);

typedef enum {
    HTTP_PARSER_STATE_HEADERS = 0,
    HTTP_PARSER_STATE_BODY_LENGTH,
    HTTP_PARSER_STATE_CHUNK_SIZE,
    HTTP_PARSER_STATE_CHUNK_DATA,
    HTTP_PARSER_STATE_CHUNK_END,
    HTTP_PARSER_STATE_TRAILER,
    HTTP_PARSER_STATE_CLOSE,
    HTTP_PARSER_STATE_COMPLETE,
    HTTP_PARSER_STATE_ERROR
} http_parser_state_t;

typedef struct {
    http_parser_state_t state;
    char header[HTTP_PARSER_HEADER_MAX + 1u];
    size_t header_length;
    char chunk_line[HTTP_PARSER_CHUNK_LINE_MAX + 1u];
    size_t chunk_line_length;
    size_t chunk_remaining;
    size_t body_remaining;
    http_response_head_t head;
    http_head_callback_t on_head;
    http_body_callback_t on_body;
    void *context;
    bool complete;
    bool failed;
} http_parser_t;

void http_parser_init(http_parser_t *parser, http_head_callback_t on_head, http_body_callback_t on_body, void *context);

/* Feed a chunk of response bytes.  Returns 0 on success, -1 on failure. */
int http_parser_feed(http_parser_t *parser, const char *data, size_t length);

/* Signal end of the transport (remote close).  Completes a close-delimited
 * body; returns -1 if the response is incomplete or malformed. */
int http_parser_finish(http_parser_t *parser);

bool http_parser_complete(const http_parser_t *parser);
bool http_parser_failed(const http_parser_t *parser);

#endif
