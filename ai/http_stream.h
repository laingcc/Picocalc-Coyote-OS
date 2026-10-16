#ifndef COYOTE_AI_HTTP_STREAM_H
#define COYOTE_AI_HTTP_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ai/http_parser.h"
#include "ai/provider.h"

/*
 * Asynchronous, allocation-free streaming HTTP/1.1 client.
 *
 * http_stream_t is a pure state machine: it never touches a socket, a resolver
 * or a clock directly.  The platform supplies those through http_stream_ops_t
 * and reports progress by calling the http_stream_on_* event functions, so the
 * same code runs against lwIP raw callbacks in firmware and against fakes in
 * the host tests.  This file and http_stream.c include no lwIP or Pico SDK
 * headers.
 *
 * TLS
 * ---
 * A request with tls set is carried over a verified TLS connection, opened
 * with the tls_connect adapter, and never over anything else.  For such a
 * request the stream does not call tcp_connect at all, so there is no path,
 * on any failure, by which its bytes could be sent in the clear: a missing
 * tls_connect adapter, or one that fails, is a terminal error.  The adapter
 * reports http_stream_on_connected only after the handshake has completed and
 * the peer's certificate and host name have been verified.
 *
 * A credential is either a bearer token, sent as "Authorization: Bearer
 * <token>", or an API key sent in a header of the provider's choosing (such as
 * "x-api-key: <key>").  Either is sent only on a TLS request:
 * http_stream_start refuses a credential on a plain request, and refuses one
 * that is not safe to put in a header.  The request head, which then holds
 * the credential, is wiped as soon as it has been handed to the stack or the
 * request ends.
 *
 * One request is in flight at a time:
 *
 *   IDLE -> RESOLVING -> CONNECTING -> SENDING -> RECEIVING_HEAD
 *        -> RECEIVING_BODY -> DONE
 *
 * Any active state may instead end in ERROR with an http_stream_error_t.  DONE
 * and ERROR are terminal until the next http_stream_start.
 *
 * Sending honours TCP backpressure: tcp_write may accept fewer bytes than
 * offered (or none), and the stream only offers more after http_stream_on_sent
 * or on the next poll.  The request body is read by offset from the provider
 * through a small window, so it is never copied whole.
 *
 * Receiving is incremental: response bytes go straight through http_parser
 * (response head capped at HTTP_PARSER_HEADER_MAX bytes) and decoded body spans
 * go straight to the sink.  The response body is never buffered here; the
 * NDJSON record cap (JSON_STREAM_RECORD_MAX) is enforced by the sink.
 *
 * Connection ownership
 * --------------------
 * The stream owns the connection from a successful tcp_connect until it calls
 * tcp_close or tcp_abort exactly once, or until the platform reports
 * http_stream_on_error (the stack has already freed the connection).  Every
 * path into DONE or ERROR releases the connection, so a terminal stream never
 * holds a live one.
 *
 * Reentrancy
 * ----------
 * The sink may call http_stream_cancel from inside feed/finish (for example
 * from a provider event callback).  The stream re-checks its state after every
 * call out and never resurrects a cancelled request.
 *
 * Storage
 * -------
 * http_stream_t embeds the response parser and the request head.  Firmware
 * must declare it as a static or global object, never as a stack local.
 */

/*
 * Room for the longest head any provider sends.  That is Gemini's, with a key
 * of the greatest length the configuration can hold (256 bytes):
 *
 *   request line, longest model path             80
 *   Host                                         41
 *   x-goog-api-key + 256-byte key               274
 *   Content-Type, Content-Length, Connection     73
 *   blank line                                    2
 *
 * 470 bytes; Claude's, with its version header, is 395.  The bridge's token is
 * shorter than a cloud key, so it fits here too.  Rounded up with room to
 * spare, and checked by the tests with a longest key for every provider.
 */
#define HTTP_STREAM_REQUEST_HEAD_MAX 768u
#define HTTP_STREAM_SEND_WINDOW 256u

/* Worst-case static storage for one http_stream_t. */
#define HTTP_STREAM_STORAGE_MAX_BYTES 3840u

/* Result codes for the dns_resolve / tcp_connect / tcp_write adapters. */
#define HTTP_STREAM_IO_ERROR (-1)
#define HTTP_STREAM_IO_PENDING 0
#define HTTP_STREAM_IO_DONE 1

typedef enum {
    HTTP_STREAM_IDLE = 0,
    HTTP_STREAM_RESOLVING,
    HTTP_STREAM_CONNECTING,
    HTTP_STREAM_SENDING,
    HTTP_STREAM_RECEIVING_HEAD,
    HTTP_STREAM_RECEIVING_BODY,
    HTTP_STREAM_DONE,
    HTTP_STREAM_ERROR
} http_stream_state_t;

typedef enum {
    HTTP_STREAM_ERROR_NONE = 0,
    HTTP_STREAM_ERROR_NETWORK_DOWN,    /* link lost or not up */
    HTTP_STREAM_ERROR_DNS,             /* host did not resolve */
    HTTP_STREAM_ERROR_CONNECT,         /* refused or could not be opened */
    HTTP_STREAM_ERROR_CONNECT_TIMEOUT, /* resolve + connect took too long */
    HTTP_STREAM_ERROR_SEND,            /* the stack rejected request bytes */
    HTTP_STREAM_ERROR_IDLE_TIMEOUT,    /* no progress for idle_timeout_ms */
    HTTP_STREAM_ERROR_TIMEOUT,         /* request_timeout_ms exceeded */
    HTTP_STREAM_ERROR_REMOTE_CLOSED,   /* closed or reset before completion */
    HTTP_STREAM_ERROR_PROTOCOL,        /* malformed or oversized HTTP */
    HTTP_STREAM_ERROR_HTTP_STATUS,     /* complete response, status not 2xx */
    HTTP_STREAM_ERROR_SINK,            /* the sink rejected the body */
    HTTP_STREAM_ERROR_CANCELLED,
    HTTP_STREAM_ERROR_TLS              /* TLS could not be set up or verified */
} http_stream_error_t;

/*
 * Platform adapters.  All are non-blocking and are called with the context
 * given to http_stream_init.  link_up may be NULL (the link is then assumed
 * up); every other entry is required.
 */
typedef struct {
    /* Monotonic milliseconds; may wrap. */
    uint32_t (*clock_ms)(void *context);

    /* True while the network link can carry traffic. */
    bool (*link_up)(void *context);

    /*
     * Begin resolving host (a name or an address literal).  The adapter keeps
     * the resulting address; the stream never sees it.  Returns
     * HTTP_STREAM_IO_DONE if resolved immediately, HTTP_STREAM_IO_PENDING if
     * http_stream_on_dns will follow, or HTTP_STREAM_IO_ERROR.  host is only
     * valid for the duration of the call.
     */
    int (*dns_resolve)(void *context, const char *host);

    /*
     * Begin connecting to the last resolved address.  Returns
     * HTTP_STREAM_IO_PENDING if a connection now exists and
     * http_stream_on_connected or http_stream_on_error will follow, or
     * HTTP_STREAM_IO_ERROR if no connection was created.
     */
    int (*tcp_connect)(void *context, uint16_t port);

    /*
     * Queue up to length bytes, copying them.  Returns the number of bytes
     * accepted, 0 when the send buffer is full, or HTTP_STREAM_IO_ERROR.
     */
    int (*tcp_write)(void *context, const char *data, size_t length);

    /* Gracefully close the connection.  No events may follow. */
    void (*tcp_close)(void *context);

    /* Abort the connection immediately.  No events may follow. */
    void (*tcp_abort)(void *context);

    /*
     * Begin connecting to the last resolved address over TLS, verifying the
     * peer's certificate chain and that it names host.  Same results as
     * tcp_connect, and the connection is then used through the same
     * tcp_write / tcp_close / tcp_abort.  http_stream_on_connected must only
     * be reported once the verified handshake is complete.  host is only
     * valid for the duration of the call.  May be NULL on a platform without
     * TLS, in which case TLS requests cannot be started.
     */
    int (*tls_connect)(void *context, const char *host, uint16_t port);
} http_stream_ops_t;

/* Receiver for decoded response body bytes.  Both return 0 on success. */
typedef struct {
    int (*feed)(void *context, const char *data, size_t length);
    /* End of body; returns 0 only if the body formed a complete response. */
    int (*finish)(void *context);
    void *context;
} http_stream_sink_t;

/*
 * One request.  The strings only need to live for the duration of
 * http_stream_start, except host on a TLS request, which must stay valid
 * until the connection attempt has begun; body and sink must stay valid until
 * the stream leaves its active states.  A timeout of 0 disables that timeout.
 *
 * bearer_token may be NULL for none.  When given it must be a non-empty
 * RFC 6750 b64token and tls must be set.
 *
 * api_key, with api_key_header, is the other way to carry a credential: it is
 * sent as "<api_key_header>: <api_key>".  The key is held to the same rules
 * as a bearer token, the header name must be an HTTP token (letters, digits
 * and "-"), and the two must be given together.  A request carries at most
 * one credential: bearer_token and api_key cannot both be set.
 *
 * extra_header may be NULL, or one more complete header line without its
 * line ending, such as "anthropic-version: 2023-06-01".  It is a
 * provider's header, so like a credential it is only accepted with tls set;
 * it must never carry a secret, since it is not checked as one.
 *
 * With tls set and port 443 the Host header carries no port.
 */
typedef struct {
    const char *host;
    uint16_t port;
    const char *method;
    const char *path;
    const char *content_type;
    /* Sent as "Authorization: Bearer ***" when not NULL or empty, and only
     * over TLS unless allow_plain_token says the host is the bridge on this
     * LAN; it must then be a valid bearer token, and is never logged. */
    const char *bearer_token;
    /* Set only for the Muse bridge, which speaks plain HTTP: the token above
     * may then travel in clear.  Never set for a cloud provider. */
    bool allow_plain_token;
    const char *api_key_header;
    const char *api_key;
    const char *extra_header;
    bool tls;
    provider_request_t body;
    http_stream_sink_t sink;
    uint32_t connect_timeout_ms; /* resolve + connect (+ TLS handshake) */
    uint32_t idle_timeout_ms;    /* no bytes acked or received */
    uint32_t request_timeout_ms; /* whole request, start to finish */
} http_stream_request_t;

typedef struct {
    http_stream_state_t state;
    http_stream_error_t error;
    const http_stream_ops_t *ops;
    void *ops_context;

    provider_request_t body;
    http_stream_sink_t sink;
    uint16_t port;
    uint32_t connect_timeout_ms;
    uint32_t idle_timeout_ms;
    uint32_t request_timeout_ms;

    uint32_t started_ms;
    uint32_t activity_ms;

    bool connection_open; /* the stream currently owns a connection */
    bool tls;             /* the request must travel over verified TLS */
    const char *tls_host; /* name to verify; only used to begin connecting */
    int status_code;

    size_t head_length;
    size_t body_length;
    size_t queued; /* request bytes accepted by tcp_write */
    size_t acked;  /* request bytes acknowledged by the peer */

    char head[HTTP_STREAM_REQUEST_HEAD_MAX];
    char window[HTTP_STREAM_SEND_WINDOW];
    http_parser_t parser;
} http_stream_t;

_Static_assert(sizeof(http_stream_t) <= HTTP_STREAM_STORAGE_MAX_BYTES,
               "http_stream_t exceeds its static RAM budget");

void http_stream_init(http_stream_t *stream, const http_stream_ops_t *ops, void *ops_context);

/*
 * Start a request.  Returns 0 once resolution has begun.  Returns -1 and
 * leaves the stream untouched if the arguments are invalid, the request head
 * does not fit, a request is already active, a TLS request is made without a
 * tls_connect adapter, or a credential is malformed or given twice; returns -1
 * with the stream in ERROR if the link is down or the resolver failed immediately.
 */
int http_stream_start(http_stream_t *stream, const http_stream_request_t *request);

/* Enforce timeouts and link state and retry a blocked send.  Cheap; call it
 * from the main loop. */
void http_stream_poll(http_stream_t *stream);

/* Abandon the active request, aborting its connection.  Safe in any state and
 * from inside a sink callback; a no-op unless a request is active. */
void http_stream_cancel(http_stream_t *stream);

/* Platform events.  Events that do not match the current state are ignored,
 * so late callbacks after cancel/timeout are harmless. */
void http_stream_on_dns(http_stream_t *stream, bool resolved);
void http_stream_on_connected(http_stream_t *stream, bool connected);
void http_stream_on_sent(http_stream_t *stream, size_t length);
/* data is only read during the call. */
void http_stream_on_recv(http_stream_t *stream, const char *data, size_t length);
/* The peer closed its side; the stream still owns the connection. */
void http_stream_on_closed(http_stream_t *stream);
/* The stack destroyed the connection (reset, refused, out of memory). */
void http_stream_on_error(http_stream_t *stream);

http_stream_state_t http_stream_state(const http_stream_t *stream);
http_stream_error_t http_stream_error(const http_stream_t *stream);
/* True from a successful start until DONE or ERROR. */
bool http_stream_is_active(const http_stream_t *stream);
/* HTTP status of the response, or 0 before the head has been parsed. */
int http_stream_status_code(const http_stream_t *stream);

#endif
