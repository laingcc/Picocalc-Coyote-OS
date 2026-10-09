#include "ai/http_stream.h"

#include <stdio.h>
#include <string.h>

static bool is_active_state(http_stream_state_t state) {
    return state != HTTP_STREAM_IDLE && state != HTTP_STREAM_DONE && state != HTTP_STREAM_ERROR;
}

static bool is_receiving(const http_stream_t *stream) {
    return stream->state == HTTP_STREAM_RECEIVING_HEAD || stream->state == HTTP_STREAM_RECEIVING_BODY;
}

static uint32_t now_ms(const http_stream_t *stream) {
    return stream->ops->clock_ms(stream->ops_context);
}

/* Give up the connection, if the stream still owns one.  Ownership is dropped
 * before calling out so a reentrant call cannot release it twice. */
static void release_connection(http_stream_t *stream, bool graceful) {
    if (!stream->connection_open) {
        return;
    }
    stream->connection_open = false;
    if (graceful) {
        stream->ops->tcp_close(stream->ops_context);
    } else {
        stream->ops->tcp_abort(stream->ops_context);
    }
}

/* Latch the terminal state before releasing the connection so nothing the
 * adapter does on the way out can be mistaken for progress. */
static void fail(http_stream_t *stream, http_stream_error_t error) {
    if (!is_active_state(stream->state)) {
        return;
    }
    stream->state = HTTP_STREAM_ERROR;
    stream->error = error;
    release_connection(stream, false);
}

/* The HTTP message is complete: let the sink judge the body, then settle. */
static void complete(http_stream_t *stream) {
    int finished = stream->sink.finish != NULL ? stream->sink.finish(stream->sink.context) : 0;

    if (!is_active_state(stream->state)) {
        return; /* cancelled from inside the sink */
    }
    if (stream->status_code < 200 || stream->status_code > 299) {
        fail(stream, HTTP_STREAM_ERROR_HTTP_STATUS);
    } else if (finished != 0) {
        fail(stream, HTTP_STREAM_ERROR_SINK);
    } else {
        stream->state = HTTP_STREAM_DONE;
        release_connection(stream, true);
    }
}

static void parser_on_head(void *context, const http_response_head_t *head) {
    http_stream_t *stream = (http_stream_t *)context;
    if (stream->state == HTTP_STREAM_RECEIVING_HEAD) {
        stream->status_code = head->status_code;
        stream->state = HTTP_STREAM_RECEIVING_BODY;
    }
}

static void parser_on_body(void *context, const char *data, size_t length) {
    http_stream_t *stream = (http_stream_t *)context;
    /* One segment can carry several body spans; once the stream has left
     * RECEIVING_BODY (cancel or failure mid-segment) the rest are dropped. */
    if (stream->state != HTTP_STREAM_RECEIVING_BODY) {
        return;
    }
    if (stream->sink.feed(stream->sink.context, data, length) != 0) {
        fail(stream, HTTP_STREAM_ERROR_SINK);
    }
}

/* Offer unsent request bytes until the request is queued or the stack stops
 * accepting them. */
static void pump_send(http_stream_t *stream) {
    size_t total = stream->head_length + stream->body_length;

    while (stream->state == HTTP_STREAM_SENDING && stream->queued < total) {
        const char *data;
        size_t length;
        int written;

        if (stream->queued < stream->head_length) {
            data = stream->head + stream->queued;
            length = stream->head_length - stream->queued;
        } else {
            size_t want = total - stream->queued;
            if (want > sizeof(stream->window)) {
                want = sizeof(stream->window);
            }
            length = stream->body.read(stream->body.context, stream->queued - stream->head_length,
                                       stream->window, want);
            if (stream->state != HTTP_STREAM_SENDING) {
                return; /* cancelled or failed during the read call-out */
            }
            if (length == 0u || length > want) {
                /* The body no longer matches the advertised Content-Length. */
                fail(stream, HTTP_STREAM_ERROR_SEND);
                return;
            }
            data = stream->window;
        }

        written = stream->ops->tcp_write(stream->ops_context, data, length);
        if (written < 0 || (size_t)written > length) {
            fail(stream, HTTP_STREAM_ERROR_SEND);
            return;
        }
        if (written == 0) {
            return; /* send buffer full: wait for an ack or the next poll */
        }
        stream->queued += (size_t)written;
    }
}

static void begin_connect(http_stream_t *stream) {
    if (stream->ops->tcp_connect(stream->ops_context, stream->port, stream->use_tls) != HTTP_STREAM_IO_PENDING) {
        fail(stream, HTTP_STREAM_ERROR_CONNECT);
        return;
    }
    stream->connection_open = true;
    stream->state = HTTP_STREAM_CONNECTING;
}

/* Reject anything that could break out of a request line or header value. */
static bool is_header_safe(const char *text, bool allow_space) {
    const unsigned char *p = (const unsigned char *)text;
    if (text == NULL || *p == '\0') {
        return false;
    }
    for (; *p != '\0'; p++) {
        if (*p < 0x20u || *p == 0x7fu || (*p == ' ' && !allow_space)) {
            return false;
        }
    }
    return true;
}

static bool ops_valid(const http_stream_ops_t *ops) {
    return ops != NULL && ops->clock_ms != NULL && ops->dns_resolve != NULL && ops->tcp_connect != NULL &&
           ops->tcp_write != NULL && ops->tcp_close != NULL && ops->tcp_abort != NULL;
}

void http_stream_init(http_stream_t *stream, const http_stream_ops_t *ops, void *ops_context) {
    if (stream == NULL) {
        return;
    }
    memset(stream, 0, sizeof(*stream));
    stream->state = HTTP_STREAM_IDLE;
    stream->error = HTTP_STREAM_ERROR_NONE;
    stream->ops = ops;
    stream->ops_context = ops_context;
}

int http_stream_start(http_stream_t *stream, const http_stream_request_t *request) {
    size_t body_length;
    int head_length;
    int resolved;

    if (stream == NULL || request == NULL || !ops_valid(stream->ops) || is_active_state(stream->state)) {
        return -1;
    }
    if (!is_header_safe(request->host, false) || !is_header_safe(request->method, false) ||
        !is_header_safe(request->path, false) || !is_header_safe(request->content_type, true) ||
        request->port == 0u || request->body.length == NULL || request->body.read == NULL ||
        request->sink.feed == NULL) {
        return -1;
    }

    body_length = request->body.length(request->body.context);
    if (request->bearer_token != NULL && request->bearer_token[0] != '\0') {
        if (!is_header_safe(request->bearer_token, true)) {
            return -1;
        }
        head_length = snprintf(stream->head, sizeof(stream->head),
                               "%s %s HTTP/1.1\r\n"
                               "Host: %s:%u\r\n"
                               "Authorization: Bearer %s\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %lu\r\n"
                               "Connection: close\r\n"
                               "\r\n",
                               request->method, request->path, request->host, (unsigned)request->port,
                               request->bearer_token, request->content_type, (unsigned long)body_length);
    } else {
        head_length = snprintf(stream->head, sizeof(stream->head),
                               "%s %s HTTP/1.1\r\n"
                               "Host: %s:%u\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %lu\r\n"
                               "Connection: close\r\n"
                               "\r\n",
                               request->method, request->path, request->host, (unsigned)request->port,
                               request->content_type, (unsigned long)body_length);
    }
    if (head_length <= 0 || (size_t)head_length >= sizeof(stream->head)) {
        return -1;
    }

    stream->body = request->body;
    stream->sink = request->sink;
    stream->port = request->port;
    stream->use_tls = request->use_tls;
    stream->connect_timeout_ms = request->connect_timeout_ms;
    stream->idle_timeout_ms = request->idle_timeout_ms;
    stream->request_timeout_ms = request->request_timeout_ms;
    stream->head_length = (size_t)head_length;
    stream->body_length = body_length;
    stream->queued = 0u;
    stream->acked = 0u;
    stream->status_code = 0;
    stream->connection_open = false;
    stream->error = HTTP_STREAM_ERROR_NONE;
    stream->started_ms = now_ms(stream);
    stream->activity_ms = stream->started_ms;
    http_parser_init(&stream->parser, parser_on_head, parser_on_body, stream);
    stream->state = HTTP_STREAM_RESOLVING;

    if (stream->ops->link_up != NULL && !stream->ops->link_up(stream->ops_context)) {
        fail(stream, HTTP_STREAM_ERROR_NETWORK_DOWN);
        return -1;
    }

    resolved = stream->ops->dns_resolve(stream->ops_context, request->host);
    if (stream->state != HTTP_STREAM_RESOLVING) {
        /* The adapter already reported the outcome through an event. */
        return stream->state == HTTP_STREAM_ERROR ? -1 : 0;
    }
    if (resolved == HTTP_STREAM_IO_DONE) {
        begin_connect(stream);
    } else if (resolved != HTTP_STREAM_IO_PENDING) {
        fail(stream, HTTP_STREAM_ERROR_DNS);
        return -1;
    }
    return 0;
}

void http_stream_poll(http_stream_t *stream) {
    uint32_t now;

    if (stream == NULL || !is_active_state(stream->state)) {
        return;
    }
    if (stream->ops->link_up != NULL && !stream->ops->link_up(stream->ops_context)) {
        fail(stream, HTTP_STREAM_ERROR_NETWORK_DOWN);
        return;
    }

    /* Unsigned subtraction keeps the elapsed times correct across a wrap. */
    now = now_ms(stream);
    if (stream->request_timeout_ms != 0u && now - stream->started_ms >= stream->request_timeout_ms) {
        fail(stream, HTTP_STREAM_ERROR_TIMEOUT);
        return;
    }
    if (stream->state == HTTP_STREAM_RESOLVING || stream->state == HTTP_STREAM_CONNECTING) {
        if (stream->connect_timeout_ms != 0u && now - stream->started_ms >= stream->connect_timeout_ms) {
            fail(stream, HTTP_STREAM_ERROR_CONNECT_TIMEOUT);
        }
        return;
    }
    if (stream->idle_timeout_ms != 0u && now - stream->activity_ms >= stream->idle_timeout_ms) {
        fail(stream, HTTP_STREAM_ERROR_IDLE_TIMEOUT);
        return;
    }
    if (stream->state == HTTP_STREAM_SENDING) {
        pump_send(stream);
    }
}

void http_stream_cancel(http_stream_t *stream) {
    if (stream == NULL) {
        return;
    }
    fail(stream, HTTP_STREAM_ERROR_CANCELLED);
}

void http_stream_on_dns(http_stream_t *stream, bool resolved) {
    if (stream == NULL || stream->state != HTTP_STREAM_RESOLVING) {
        return;
    }
    if (!resolved) {
        fail(stream, HTTP_STREAM_ERROR_DNS);
        return;
    }
    begin_connect(stream);
}

void http_stream_on_connected(http_stream_t *stream, bool connected) {
    if (stream == NULL || stream->state != HTTP_STREAM_CONNECTING) {
        return;
    }
    if (!connected) {
        fail(stream, HTTP_STREAM_ERROR_CONNECT);
        return;
    }
    stream->state = HTTP_STREAM_SENDING;
    stream->activity_ms = now_ms(stream);
    pump_send(stream);
}

void http_stream_on_sent(http_stream_t *stream, size_t length) {
    if (stream == NULL || stream->state != HTTP_STREAM_SENDING) {
        return;
    }
    if (length > stream->queued - stream->acked) {
        length = stream->queued - stream->acked;
    }
    stream->acked += length;
    stream->activity_ms = now_ms(stream);
    if (stream->acked >= stream->head_length + stream->body_length) {
        stream->state = HTTP_STREAM_RECEIVING_HEAD;
        return;
    }
    pump_send(stream);
}

void http_stream_on_recv(http_stream_t *stream, const char *data, size_t length) {
    int fed;

    if (stream == NULL || data == NULL || length == 0u) {
        return;
    }
    if (stream->state == HTTP_STREAM_SENDING) {
        /* The server answered without waiting for the whole request; whatever
         * is still unsent is abandoned. */
        stream->state = HTTP_STREAM_RECEIVING_HEAD;
    }
    if (!is_receiving(stream)) {
        return;
    }
    stream->activity_ms = now_ms(stream);

    fed = http_parser_feed(&stream->parser, data, length);
    if (!is_receiving(stream)) {
        return; /* a sink callback cancelled or failed the stream */
    }
    if (fed != 0) {
        fail(stream, HTTP_STREAM_ERROR_PROTOCOL);
        return;
    }
    if (http_parser_complete(&stream->parser)) {
        complete(stream);
    }
}

void http_stream_on_closed(http_stream_t *stream) {
    if (stream == NULL || !is_active_state(stream->state)) {
        return;
    }
    /* Only a close-delimited body is legitimately ended by the peer closing. */
    if (stream->state == HTTP_STREAM_RECEIVING_BODY && http_parser_finish(&stream->parser) == 0) {
        complete(stream);
        return;
    }
    fail(stream, HTTP_STREAM_ERROR_REMOTE_CLOSED);
}

void http_stream_on_error(http_stream_t *stream) {
    if (stream == NULL || !stream->connection_open) {
        return;
    }
    /* The stack has already freed the connection: there is nothing to abort. */
    stream->connection_open = false;
    fail(stream, stream->state == HTTP_STREAM_CONNECTING ? HTTP_STREAM_ERROR_CONNECT
                                                         : HTTP_STREAM_ERROR_REMOTE_CLOSED);
}

http_stream_state_t http_stream_state(const http_stream_t *stream) {
    return stream != NULL ? stream->state : HTTP_STREAM_IDLE;
}

http_stream_error_t http_stream_error(const http_stream_t *stream) {
    return stream != NULL ? stream->error : HTTP_STREAM_ERROR_NONE;
}

bool http_stream_is_active(const http_stream_t *stream) {
    return stream != NULL && is_active_state(stream->state);
}

int http_stream_status_code(const http_stream_t *stream) {
    return stream != NULL ? stream->status_code : 0;
}
