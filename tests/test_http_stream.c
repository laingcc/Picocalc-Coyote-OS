#include <stdio.h>
#include <string.h>

#include "ai/http_stream.h"
#include "ai/json_stream.h"
#include "ai/ollama_provider.h"
#include "ai/wifi_manager.h"
#include "test_util.h"

/* ------------------------------------------------------------------------ */
/* Fake network: DNS, one TCP connection with a bounded send buffer, a clock */
/* and a link flag.  No lwIP, no CYW43.                                      */
/* ------------------------------------------------------------------------ */

#define FAKE_WIRE_MAX 4096u
#define FAKE_SNDBUF_UNLIMITED ((size_t)-1)

typedef struct {
    uint32_t now;
    bool link_up;
    wifi_manager_t *wifi; /* when set, link state comes from the manager */

    int dns_result; /* HTTP_STREAM_IO_* returned by dns_resolve */
    int dns_calls;
    char dns_host[64];

    int connect_result;
    int connect_calls;
    uint16_t connect_port;

    int live; /* connections opened and not yet closed/aborted */
    int closes;
    int aborts;

    size_t sndbuf;  /* send buffer capacity */
    size_t unacked; /* bytes written and not yet acknowledged */
    bool write_fails;
    int write_calls;
    char wire[FAKE_WIRE_MAX];
    size_t wire_length;
} fake_net_t;

static uint32_t fake_clock_ms(void *context) {
    return ((fake_net_t *)context)->now;
}

static bool fake_link_up(void *context) {
    fake_net_t *net = (fake_net_t *)context;
    if (net->wifi != NULL) {
        return wifi_manager_is_online(net->wifi);
    }
    return net->link_up;
}

static int fake_dns_resolve(void *context, const char *host) {
    fake_net_t *net = (fake_net_t *)context;
    net->dns_calls++;
    snprintf(net->dns_host, sizeof(net->dns_host), "%s", host);
    return net->dns_result;
}

static int fake_tcp_connect(void *context, uint16_t port) {
    fake_net_t *net = (fake_net_t *)context;
    net->connect_calls++;
    net->connect_port = port;
    if (net->connect_result != HTTP_STREAM_IO_PENDING) {
        return net->connect_result;
    }
    net->live++;
    return HTTP_STREAM_IO_PENDING;
}

static int fake_tcp_write(void *context, const char *data, size_t length) {
    fake_net_t *net = (fake_net_t *)context;
    size_t room;
    net->write_calls++;
    if (net->write_fails) {
        return HTTP_STREAM_IO_ERROR;
    }
    room = net->sndbuf == FAKE_SNDBUF_UNLIMITED ? length : net->sndbuf - net->unacked;
    if (room > length) {
        room = length;
    }
    if (room > FAKE_WIRE_MAX - net->wire_length) {
        room = FAKE_WIRE_MAX - net->wire_length;
    }
    memcpy(net->wire + net->wire_length, data, room);
    net->wire_length += room;
    net->unacked += room;
    return (int)room;
}

static void fake_tcp_close(void *context) {
    fake_net_t *net = (fake_net_t *)context;
    net->live--;
    net->closes++;
}

static void fake_tcp_abort(void *context) {
    fake_net_t *net = (fake_net_t *)context;
    net->live--;
    net->aborts++;
}

static const http_stream_ops_t fake_ops = {
    fake_clock_ms, fake_link_up, fake_dns_resolve, fake_tcp_connect,
    fake_tcp_write, fake_tcp_close, fake_tcp_abort,
};

/* ------------------------------------------------------------------------ */
/* Harness: a stream wired to a real ollama_provider sink.                   */
/* ------------------------------------------------------------------------ */

#define LOG_MAX 32

typedef struct harness {
    fake_net_t net;
    http_stream_t stream;
    ollama_provider_t provider;
    int event_count;
    int event_type[LOG_MAX];
    char content[1024];
    char error_text[JSON_STREAM_ERROR_MAX + 1u];
    bool cancel_on_content; /* cancel the stream from inside the callback */
} harness_t;

/* Static: ollama_provider_t and http_stream_t are too large for stack locals
 * by the project's own rule. */
static harness_t g_harness;

static void on_event(void *context, const provider_event_t *event) {
    harness_t *h = (harness_t *)context;
    if (h->event_count < LOG_MAX) {
        h->event_type[h->event_count] = (int)event->type;
    }
    h->event_count++;
    if (event->type == PROVIDER_EVENT_CONTENT) {
        size_t used = strlen(h->content);
        if (used + event->length < sizeof(h->content)) {
            memcpy(h->content + used, event->data, event->length);
            h->content[used + event->length] = '\0';
        }
        if (h->cancel_on_content) {
            http_stream_cancel(&h->stream);
        }
    } else if (event->type == PROVIDER_EVENT_ERROR && event->data != NULL &&
               event->length < sizeof(h->error_text)) {
        memcpy(h->error_text, event->data, event->length);
        h->error_text[event->length] = '\0';
    }
}

static int sink_feed(void *context, const char *data, size_t length) {
    return ollama_provider_feed((ollama_provider_t *)context, data, length);
}

static int sink_finish(void *context) {
    return ollama_provider_finish((ollama_provider_t *)context);
}

#define REQUEST_BODY                                                        \
    "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]," \
    "\"stream\":true,\"options\":{\"num_predict\":8,\"temperature\":0.80}}"

#define REQUEST_HEAD                       \
    "POST /api/chat HTTP/1.1\r\n"          \
    "Host: wang.local:11434\r\n"           \
    "Content-Type: application/json\r\n"   \
    "Content-Length: 118\r\n"               \
    "Connection: close\r\n"                \
    "\r\n"

#define NDJSON_BODY                                                          \
    "{\"message\":{\"role\":\"assistant\",\"content\":\"Hel\"},\"done\":false}\n" \
    "{\"message\":{\"role\":\"assistant\",\"content\":\"lo\"},\"done\":false}\n"  \
    "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"done\":true}\n"

#define CONNECT_TIMEOUT_MS 5000u
#define IDLE_TIMEOUT_MS 3000u
#define REQUEST_TIMEOUT_MS 20000u

static void harness_reset(harness_t *h) {
    memset(&h->net, 0, sizeof(h->net));
    h->net.link_up = true;
    h->net.sndbuf = FAKE_SNDBUF_UNLIMITED;
    h->net.dns_result = HTTP_STREAM_IO_PENDING;
    h->net.connect_result = HTTP_STREAM_IO_PENDING;
    h->net.now = 1000u;
    h->event_count = 0;
    h->content[0] = '\0';
    h->error_text[0] = '\0';
    h->cancel_on_content = false;
    http_stream_init(&h->stream, &fake_ops, &h->net);
}

static void clear_events(harness_t *h) {
    h->event_count = 0;
    h->content[0] = '\0';
    h->error_text[0] = '\0';
}

static int harness_start(harness_t *h) {
    static const ollama_message_t messages[1] = {{"user", "hi"}};
    http_stream_request_t request;

    clear_events(h);
    h->net.wire_length = 0;
    h->net.unacked = 0;
    ollama_provider_init(&h->provider, on_event, h);
    CHECK(ollama_provider_build_request(&h->provider, "m", messages, 1, 8, "", 80) == 0);

    memset(&request, 0, sizeof(request));
    request.host = "wang.local";
    request.port = 11434u;
    request.method = ollama_provider_method();
    request.path = ollama_provider_path();
    request.content_type = ollama_provider_content_type();
    request.body = ollama_provider_request(&h->provider);
    request.sink.feed = sink_feed;
    request.sink.finish = sink_finish;
    request.sink.context = &h->provider;
    request.connect_timeout_ms = CONNECT_TIMEOUT_MS;
    request.idle_timeout_ms = IDLE_TIMEOUT_MS;
    request.request_timeout_ms = REQUEST_TIMEOUT_MS;
    return http_stream_start(&h->stream, &request);
}

static void ack_all(harness_t *h) {
    size_t acked = h->net.unacked;
    h->net.unacked = 0;
    http_stream_on_sent(&h->stream, acked);
}

/* Start a request and drive it until the whole request has been acked. */
static void start_to_receiving(harness_t *h) {
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    http_stream_on_connected(&h->stream, true);
    ack_all(h);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_HEAD);
}

static void recv_text(harness_t *h, const char *text) {
    http_stream_on_recv(&h->stream, text, strlen(text));
}

static void check_done_hello(harness_t *h) {
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_DONE);
    CHECK(http_stream_error(&h->stream) == HTTP_STREAM_ERROR_NONE);
    CHECK(http_stream_status_code(&h->stream) == 200);
    CHECK_STR_EQ(h->content, "Hello");
    CHECK(h->event_count == 3);
    CHECK(h->event_type[0] == PROVIDER_EVENT_CONTENT);
    CHECK(h->event_type[1] == PROVIDER_EVENT_CONTENT);
    CHECK(h->event_type[2] == PROVIDER_EVENT_DONE);
    CHECK(ollama_provider_is_done(&h->provider));
    CHECK(h->net.live == 0);
}

static void check_failed(harness_t *h, http_stream_error_t error) {
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_ERROR);
    CHECK(http_stream_error(&h->stream) == error);
    CHECK(!http_stream_is_active(&h->stream));
    CHECK(h->net.live == 0);
}

/* Build "HTTP/1.1 200 OK" + chunked framing around NDJSON_BODY, one chunk per
 * NDJSON line. */
static size_t build_chunked_response(char *out, size_t capacity) {
    const char *body = NDJSON_BODY;
    size_t used = (size_t)snprintf(out, capacity,
                                   "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: application/x-ndjson\r\n"
                                   "Transfer-Encoding: chunked\r\n"
                                   "\r\n");
    while (*body != '\0') {
        size_t line = (size_t)(strchr(body, '\n') - body) + 1u;
        used += (size_t)snprintf(out + used, capacity - used, "%zx\r\n%.*s\r\n", line, (int)line, body);
        body += line;
    }
    used += (size_t)snprintf(out + used, capacity - used, "0\r\n\r\n");
    return used;
}

/* ------------------------------------------------------------------------ */
/* HTTP stream tests                                                         */
/* ------------------------------------------------------------------------ */

static void check_happy_path_content_length(void) {
    harness_t *h = &g_harness;
    char response[512];
    harness_reset(h);

    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_IDLE);
    CHECK(!http_stream_is_active(&h->stream));

    CHECK(harness_start(h) == 0);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RESOLVING);
    CHECK(http_stream_is_active(&h->stream));
    CHECK(h->net.dns_calls == 1);
    CHECK_STR_EQ(h->net.dns_host, "wang.local");
    CHECK(h->net.connect_calls == 0);

    http_stream_on_dns(&h->stream, true);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_CONNECTING);
    CHECK(h->net.connect_calls == 1);
    CHECK(h->net.connect_port == 11434u);
    CHECK(h->net.live == 1);
    CHECK(h->net.wire_length == 0);

    http_stream_on_connected(&h->stream, true);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_SENDING);
    CHECK(strlen(REQUEST_BODY) == 118u);
    CHECK(h->net.wire_length == strlen(REQUEST_HEAD REQUEST_BODY));
    CHECK_BYTES_EQ(h->net.wire, REQUEST_HEAD REQUEST_BODY, strlen(REQUEST_HEAD REQUEST_BODY));

    ack_all(h);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_HEAD);

    snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(NDJSON_BODY), NDJSON_BODY);
    recv_text(h, response);
    check_done_hello(h);
    CHECK(h->net.closes == 1);
    CHECK(h->net.aborts == 0);

    /* Late transport events after completion are ignored. */
    http_stream_on_recv(&h->stream, "x", 1);
    http_stream_on_closed(&h->stream);
    http_stream_on_error(&h->stream);
    http_stream_poll(&h->stream);
    check_done_hello(h);
    CHECK(h->net.closes == 1);
}

static void check_dns_resolved_immediately(void) {
    harness_t *h = &g_harness;
    harness_reset(h);
    h->net.dns_result = HTTP_STREAM_IO_DONE;
    CHECK(harness_start(h) == 0);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_CONNECTING);
    CHECK(h->net.connect_calls == 1);
    http_stream_cancel(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_CANCELLED);
}

static void check_dns_failure(void) {
    harness_t *h = &g_harness;

    /* Asynchronous failure. */
    harness_reset(h);
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, false);
    check_failed(h, HTTP_STREAM_ERROR_DNS);
    CHECK(h->net.connect_calls == 0);
    CHECK(h->net.aborts == 0);
    CHECK(h->event_count == 0);

    /* Synchronous failure from the resolver. */
    harness_reset(h);
    h->net.dns_result = HTTP_STREAM_IO_ERROR;
    CHECK(harness_start(h) == -1);
    check_failed(h, HTTP_STREAM_ERROR_DNS);
    CHECK(h->net.connect_calls == 0);

    /* A late resolver answer after the failure does not open a connection. */
    http_stream_on_dns(&h->stream, true);
    check_failed(h, HTTP_STREAM_ERROR_DNS);
    CHECK(h->net.connect_calls == 0);
}

static void check_connection_refused(void) {
    harness_t *h = &g_harness;

    /* Refusal reported through the error callback: the PCB is already gone,
     * so the stream must not abort it a second time. */
    harness_reset(h);
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    h->net.live--; /* the stack freed the connection itself */
    http_stream_on_error(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_CONNECT);
    CHECK(h->net.aborts == 0);
    CHECK(h->net.closes == 0);

    /* Refusal reported through the connected callback: the stream owns the
     * connection and must abort it. */
    harness_reset(h);
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    http_stream_on_connected(&h->stream, false);
    check_failed(h, HTTP_STREAM_ERROR_CONNECT);
    CHECK(h->net.aborts == 1);

    /* The connect call itself fails: nothing was opened. */
    harness_reset(h);
    h->net.connect_result = HTTP_STREAM_IO_ERROR;
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    check_failed(h, HTTP_STREAM_ERROR_CONNECT);
    CHECK(h->net.aborts == 0);
    CHECK(h->net.wire_length == 0);
}

static void check_backpressure(void) {
    harness_t *h = &g_harness;
    const char *expected = REQUEST_HEAD REQUEST_BODY;
    size_t total = strlen(expected);
    size_t sent = 0;
    int guard = 0;
    char response[512];

    harness_reset(h);
    h->net.sndbuf = 16u;
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    http_stream_on_connected(&h->stream, true);

    while (sent + 16u < total && guard++ < 100) {
        /* The send buffer is full: exactly one window is on the wire. */
        CHECK(h->net.wire_length == sent + 16u);
        CHECK(h->net.unacked == 16u);
        CHECK(http_stream_state(&h->stream) == HTTP_STREAM_SENDING);

        /* Polling without an ack must not push more bytes. */
        http_stream_poll(&h->stream);
        http_stream_poll(&h->stream);
        CHECK(h->net.wire_length == sent + 16u);

        /* A partial ack opens exactly that much room. */
        h->net.unacked -= 5u;
        http_stream_on_sent(&h->stream, 5u);
        CHECK(h->net.wire_length == sent + 21u || h->net.wire_length == total);

        h->net.unacked -= 11u;
        http_stream_on_sent(&h->stream, 11u);
        sent += 16u;
    }
    CHECK(guard < 100);
    CHECK(h->net.wire_length == total);
    CHECK_BYTES_EQ(h->net.wire, expected, total);

    /* Everything is queued but not yet acknowledged: still sending. */
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_SENDING);
    ack_all(h);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_HEAD);
    CHECK(h->net.wire_length == total);

    snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(NDJSON_BODY), NDJSON_BODY);
    recv_text(h, response);
    check_done_hello(h);
}

static void check_send_buffer_full_then_poll_retries(void) {
    harness_t *h = &g_harness;
    harness_reset(h);

    /* The stack refuses every byte (no buffer memory) with nothing in flight,
     * so no ack will ever arrive; poll must retry the write. */
    h->net.sndbuf = 0u;
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    http_stream_on_connected(&h->stream, true);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_SENDING);
    CHECK(h->net.wire_length == 0);

    h->net.sndbuf = FAKE_SNDBUF_UNLIMITED;
    http_stream_poll(&h->stream);
    CHECK(h->net.wire_length == strlen(REQUEST_HEAD REQUEST_BODY));
    CHECK_BYTES_EQ(h->net.wire, REQUEST_HEAD REQUEST_BODY, h->net.wire_length);
    http_stream_cancel(&h->stream);
    CHECK(h->net.live == 0);
}

static void check_write_error(void) {
    harness_t *h = &g_harness;
    harness_reset(h);
    h->net.write_fails = true;
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    http_stream_on_connected(&h->stream, true);
    check_failed(h, HTTP_STREAM_ERROR_SEND);
    CHECK(h->net.aborts == 1);
}

static void check_header_fragmentation(void) {
    harness_t *h = &g_harness;
    char response[512];
    size_t length;
    size_t i;
    bool saw_body_state = false;

    harness_reset(h);
    start_to_receiving(h);
    length = (size_t)snprintf(response, sizeof(response),
                              "HTTP/1.1 200 OK\r\n"
                              "Server: fake\r\n"
                              "Content-Type: application/x-ndjson\r\n"
                              "Content-Length: %zu\r\n"
                              "\r\n%s",
                              strlen(NDJSON_BODY), NDJSON_BODY);
    for (i = 0; i < length; i++) {
        if (response[i] == '{' && !saw_body_state) {
            CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_BODY);
            CHECK(http_stream_status_code(&h->stream) == 200);
            saw_body_state = true;
        } else if (!saw_body_state) {
            CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_HEAD);
        }
        http_stream_on_recv(&h->stream, response + i, 1u);
    }
    CHECK(saw_body_state);
    check_done_hello(h);

    /* Fragments of 3 and 7 bytes as well. */
    for (size_t step = 3u; step <= 7u; step += 4u) {
        harness_reset(h);
        start_to_receiving(h);
        for (i = 0; i < length; i += step) {
            size_t n = length - i < step ? length - i : step;
            http_stream_on_recv(&h->stream, response + i, n);
        }
        check_done_hello(h);
    }
}

static void check_chunked_every_split(void) {
    harness_t *h = &g_harness;
    char response[1024];
    size_t length = build_chunked_response(response, sizeof(response));
    size_t split;
    size_t i;

    /* Two segments, split at every possible boundary. */
    for (split = 1u; split < length; split++) {
        harness_reset(h);
        start_to_receiving(h);
        http_stream_on_recv(&h->stream, response, split);
        CHECK(http_stream_is_active(&h->stream));
        http_stream_on_recv(&h->stream, response + split, length - split);
        check_done_hello(h);
        CHECK(h->net.closes == 1);
    }

    /* One byte per segment. */
    harness_reset(h);
    start_to_receiving(h);
    for (i = 0; i < length; i++) {
        http_stream_on_recv(&h->stream, response + i, 1u);
    }
    check_done_hello(h);

    /* A single segment. */
    harness_reset(h);
    start_to_receiving(h);
    http_stream_on_recv(&h->stream, response, length);
    check_done_hello(h);
}

static void check_close_delimited(void) {
    harness_t *h = &g_harness;
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.0 200 OK\r\nContent-Type: application/x-ndjson\r\n\r\n");
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_BODY);
    recv_text(h, NDJSON_BODY);
    /* The body is only complete once the peer closes. */
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_BODY);
    CHECK(h->net.live == 1);
    http_stream_on_closed(&h->stream);
    check_done_hello(h);
    CHECK(h->net.closes == 1);

    /* Close-delimited body that ends before the terminal record. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.0 200 OK\r\n\r\n");
    recv_text(h, "{\"message\":{\"content\":\"Hel\"},\"done\":false}\n");
    http_stream_on_closed(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_SINK);
    CHECK_STR_EQ(h->content, "Hel");
}

static void check_idle_timeout(void) {
    harness_t *h = &g_harness;

    /* Idle while receiving the body; received bytes restart the idle clock. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    h->net.now += IDLE_TIMEOUT_MS - 1u;
    http_stream_poll(&h->stream);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_BODY);
    recv_text(h, "1\r\n");
    h->net.now += IDLE_TIMEOUT_MS - 1u;
    http_stream_poll(&h->stream);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_BODY);
    h->net.now += 1u;
    http_stream_poll(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_IDLE_TIMEOUT);
    CHECK(h->net.aborts == 1);

    /* Idle while waiting for the head. */
    harness_reset(h);
    start_to_receiving(h);
    h->net.now += IDLE_TIMEOUT_MS;
    http_stream_poll(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_IDLE_TIMEOUT);

    /* Idle while sending: the peer never acks. */
    harness_reset(h);
    h->net.sndbuf = 16u;
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    h->net.now += CONNECT_TIMEOUT_MS - 1u; /* connect time is not idle time */
    http_stream_on_connected(&h->stream, true);
    h->net.now += IDLE_TIMEOUT_MS - 1u;
    http_stream_poll(&h->stream);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_SENDING);
    h->net.now += 1u;
    http_stream_poll(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_IDLE_TIMEOUT);
    CHECK(h->net.aborts == 1);

    /* The clock wrapping around does not fire a spurious timeout. */
    harness_reset(h);
    h->net.now = 0xFFFFFF00u;
    start_to_receiving(h);
    h->net.now += 0x200u;
    http_stream_poll(&h->stream);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_HEAD);
    h->net.now += IDLE_TIMEOUT_MS;
    http_stream_poll(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_IDLE_TIMEOUT);
}

static void check_absolute_timeout(void) {
    harness_t *h = &g_harness;
    uint32_t elapsed = 0;

    /* A peer that keeps trickling bytes never trips the idle timeout but must
     * still hit the absolute deadline. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.1 200 OK\r\n\r\n");
    while (elapsed + 1000u < REQUEST_TIMEOUT_MS) {
        h->net.now += 1000u;
        elapsed += 1000u;
        recv_text(h, " ");
        http_stream_poll(&h->stream);
        CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_BODY);
    }
    h->net.now += 1000u;
    recv_text(h, " ");
    http_stream_poll(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_TIMEOUT);
    CHECK(h->net.aborts == 1);
    CHECK(h->net.closes == 0);
}

static void check_connect_timeout(void) {
    harness_t *h = &g_harness;

    /* Resolver never answers. */
    harness_reset(h);
    CHECK(harness_start(h) == 0);
    h->net.now += CONNECT_TIMEOUT_MS - 1u;
    http_stream_poll(&h->stream);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RESOLVING);
    h->net.now += 1u;
    http_stream_poll(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_CONNECT_TIMEOUT);
    CHECK(h->net.aborts == 0);
    /* The answer finally arrives: it must not open a connection. */
    http_stream_on_dns(&h->stream, true);
    CHECK(h->net.connect_calls == 0);
    check_failed(h, HTTP_STREAM_ERROR_CONNECT_TIMEOUT);

    /* SYN never answered: the half-open connection is aborted. */
    harness_reset(h);
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    h->net.now += CONNECT_TIMEOUT_MS;
    http_stream_poll(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_CONNECT_TIMEOUT);
    CHECK(h->net.aborts == 1);
}

static void check_remote_close_mid_body(void) {
    harness_t *h = &g_harness;
    char response[1024];
    size_t length;

    /* Content-Length body cut short. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.1 200 OK\r\nContent-Length: 500\r\n\r\n");
    recv_text(h, "{\"message\":{\"content\":\"Hel\"},\"done\":false}\n");
    http_stream_on_closed(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_REMOTE_CLOSED);
    CHECK_STR_EQ(h->content, "Hel");
    CHECK(h->net.live == 0);

    /* Chunked body cut short before the terminal chunk. */
    length = build_chunked_response(response, sizeof(response));
    harness_reset(h);
    start_to_receiving(h);
    http_stream_on_recv(&h->stream, response, length - 5u);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_BODY);
    http_stream_on_closed(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_REMOTE_CLOSED);

    /* Connection reset mid-body: the stack already freed the connection. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.1 200 OK\r\nContent-Length: 500\r\n\r\n{");
    h->net.live--;
    http_stream_on_error(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_REMOTE_CLOSED);
    CHECK(h->net.aborts == 0);
    CHECK(h->net.closes == 0);

    /* Closed while the head is still incomplete. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.1 200 OK\r\nContent-Le");
    http_stream_on_closed(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_REMOTE_CLOSED);

    /* Closed while the request is still being sent. */
    harness_reset(h);
    h->net.sndbuf = 16u;
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    http_stream_on_connected(&h->stream, true);
    http_stream_on_closed(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_REMOTE_CLOSED);
}

static void check_cancellation(void) {
    harness_t *h = &g_harness;
    size_t wire;

    /* Idle: nothing to cancel. */
    harness_reset(h);
    http_stream_cancel(&h->stream);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_IDLE);
    CHECK(h->net.aborts == 0);

    /* While resolving: no connection exists; a late answer is ignored. */
    harness_reset(h);
    CHECK(harness_start(h) == 0);
    http_stream_cancel(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_CANCELLED);
    CHECK(h->net.aborts == 0);
    http_stream_on_dns(&h->stream, true);
    CHECK(h->net.connect_calls == 0);
    check_failed(h, HTTP_STREAM_ERROR_CANCELLED);

    /* While connecting: the half-open connection is aborted; a late connected
     * callback sends nothing. */
    harness_reset(h);
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    http_stream_cancel(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_CANCELLED);
    CHECK(h->net.aborts == 1);
    http_stream_on_connected(&h->stream, true);
    CHECK(h->net.wire_length == 0);

    /* During send, with part of the request still unsent. */
    harness_reset(h);
    h->net.sndbuf = 16u;
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    http_stream_on_connected(&h->stream, true);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_SENDING);
    wire = h->net.wire_length;
    http_stream_cancel(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_CANCELLED);
    CHECK(h->net.aborts == 1);
    CHECK(h->net.closes == 0);
    /* Late acks and polls must not resume the send. */
    h->net.unacked = 0;
    http_stream_on_sent(&h->stream, 16u);
    http_stream_poll(&h->stream);
    CHECK(h->net.wire_length == wire);
    CHECK(h->net.aborts == 1);

    /* During receive of the head. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.1 200 OK\r\nContent-");
    http_stream_cancel(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_CANCELLED);
    CHECK(h->net.aborts == 1);

    /* During receive of the body: later bytes never reach the provider. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.1 200 OK\r\n\r\n");
    recv_text(h, "{\"message\":{\"content\":\"Hel\"},\"done\":false}\n");
    CHECK(h->event_count == 1);
    http_stream_cancel(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_CANCELLED);
    CHECK(h->net.aborts == 1);
    recv_text(h, "{\"message\":{\"content\":\"lo\"},\"done\":false}\n");
    http_stream_on_closed(&h->stream);
    CHECK(h->event_count == 1);
    CHECK_STR_EQ(h->content, "Hel");
    check_failed(h, HTTP_STREAM_ERROR_CANCELLED);

    /* Cancelling twice, or after an error, changes nothing. */
    http_stream_cancel(&h->stream);
    CHECK(h->net.aborts == 1);
    CHECK(h->net.live == 0);

    /* After success the result is kept. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.0 200 OK\r\n\r\n" NDJSON_BODY);
    http_stream_on_closed(&h->stream);
    http_stream_cancel(&h->stream);
    check_done_hello(h);
    CHECK(h->net.aborts == 0);
}

static void check_cancel_from_event_callback(void) {
    harness_t *h = &g_harness;
    char response[512];

    /* The provider callback cancels the stream while the transport is in the
     * middle of delivering a segment holding three records. */
    harness_reset(h);
    start_to_receiving(h);
    h->cancel_on_content = true;
    snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(NDJSON_BODY), NDJSON_BODY);
    recv_text(h, response);
    check_failed(h, HTTP_STREAM_ERROR_CANCELLED);
    CHECK(h->net.aborts == 1);
    CHECK(h->net.closes == 0);

    /* Same, with every record in its own chunk of one segment: chunks after
     * the cancel never reach the provider. */
    harness_reset(h);
    start_to_receiving(h);
    h->cancel_on_content = true;
    http_stream_on_recv(&h->stream, response, build_chunked_response(response, sizeof(response)));
    check_failed(h, HTTP_STREAM_ERROR_CANCELLED);
    CHECK(h->net.aborts == 1);
    CHECK(h->event_count == 1);
    CHECK_STR_EQ(h->content, "Hel");
}

static void check_protocol_errors(void) {
    harness_t *h = &g_harness;
    char big[JSON_STREAM_RECORD_MAX + 64u];
    size_t i;

    /* Not HTTP. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "SSH-2.0-nope\r\n\r\n");
    check_failed(h, HTTP_STREAM_ERROR_PROTOCOL);
    CHECK(h->net.aborts == 1);

    /* Response head larger than the 1024-byte cap. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.1 200 OK\r\n");
    for (i = 0; i < 64u && http_stream_is_active(&h->stream); i++) {
        recv_text(h, "X-Padding: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n");
    }
    CHECK(i < 64u);
    check_failed(h, HTTP_STREAM_ERROR_PROTOCOL);

    /* NDJSON record longer than JSON_STREAM_RECORD_MAX. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.1 200 OK\r\n\r\n");
    memset(big, 'a', sizeof(big));
    memcpy(big, "{\"message\":{\"content\":\"", 23u);
    http_stream_on_recv(&h->stream, big, JSON_STREAM_RECORD_MAX);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_BODY);
    CHECK(h->event_count == 0);
    http_stream_on_recv(&h->stream, big + JSON_STREAM_RECORD_MAX, 64u);
    http_stream_on_recv(&h->stream, "\"},\"done\":false}\n", 17u);
    check_failed(h, HTTP_STREAM_ERROR_SINK);
    CHECK(h->net.aborts == 1);
    CHECK(h->event_count == 1);
    CHECK(h->event_type[0] == PROVIDER_EVENT_ERROR);

    /* Body complete by Content-Length, but no terminal record. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}");
    check_failed(h, HTTP_STREAM_ERROR_SINK);
}

static void check_http_error_status(void) {
    harness_t *h = &g_harness;
    const char *body = "{\"error\":\"model 'm' not found\"}";
    char response[256];

    /* Ollama reports errors as a JSON body on a non-2xx status: the provider
     * still surfaces the text, and the stream reports the status. */
    harness_reset(h);
    start_to_receiving(h);
    snprintf(response, sizeof(response), "HTTP/1.1 404 Not Found\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(body), body);
    recv_text(h, response);
    check_failed(h, HTTP_STREAM_ERROR_HTTP_STATUS);
    CHECK(http_stream_status_code(&h->stream) == 404);
    CHECK(h->event_count == 1);
    CHECK(h->event_type[0] == PROVIDER_EVENT_ERROR);
    CHECK_STR_EQ(h->error_text, "model 'm' not found");

    /* Non-JSON error page. */
    harness_reset(h);
    start_to_receiving(h);
    recv_text(h, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 4\r\n\r\noops");
    check_failed(h, HTTP_STREAM_ERROR_HTTP_STATUS);
    CHECK(http_stream_status_code(&h->stream) == 502);
}

static void check_early_response_while_sending(void) {
    harness_t *h = &g_harness;
    size_t wire;

    /* The server answers before reading the whole request. */
    harness_reset(h);
    h->net.sndbuf = 16u;
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    http_stream_on_connected(&h->stream, true);
    wire = h->net.wire_length;
    recv_text(h, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
    check_failed(h, HTTP_STREAM_ERROR_HTTP_STATUS);
    CHECK(http_stream_status_code(&h->stream) == 400);
    CHECK(h->net.wire_length == wire);
}

static void check_start_validation_and_reuse(void) {
    harness_t *h = &g_harness;
    http_stream_request_t request;
    char response[512];
    int round;

    harness_reset(h);
    CHECK(http_stream_start(NULL, NULL) == -1);
    CHECK(http_stream_start(&h->stream, NULL) == -1);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_IDLE);

    /* A host that could inject header lines is rejected before any I/O. */
    memset(&request, 0, sizeof(request));
    ollama_provider_init(&h->provider, on_event, h);
    request.host = "wang.local\r\nX-Evil: 1";
    request.port = 11434u;
    request.method = "POST";
    request.path = "/api/chat";
    request.content_type = "application/json";
    request.body = ollama_provider_request(&h->provider);
    request.sink.feed = sink_feed;
    request.sink.finish = sink_finish;
    request.sink.context = &h->provider;
    CHECK(http_stream_start(&h->stream, &request) == -1);
    CHECK(h->net.dns_calls == 0);
    request.host = "";
    CHECK(http_stream_start(&h->stream, &request) == -1);
    request.host = "wang.local";
    request.port = 0u;
    CHECK(http_stream_start(&h->stream, &request) == -1);
    request.port = 11434u;
    request.sink.feed = NULL;
    CHECK(http_stream_start(&h->stream, &request) == -1);
    request.sink.feed = sink_feed;
    CHECK(h->net.dns_calls == 0);
    CHECK(!http_stream_is_active(&h->stream));

    /* A second start while a request is active is refused and harmless. */
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    CHECK(http_stream_start(&h->stream, &request) == -1);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_CONNECTING);
    CHECK(h->net.live == 1);
    http_stream_cancel(&h->stream);
    CHECK(h->net.live == 0);

    /* The same stream is reusable after an error and after success. */
    snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s",
             strlen(NDJSON_BODY), NDJSON_BODY);
    for (round = 0; round < 3; round++) {
        start_to_receiving(h);
        CHECK(http_stream_error(&h->stream) == HTTP_STREAM_ERROR_NONE);
        CHECK(http_stream_status_code(&h->stream) == 0);
        CHECK(h->net.wire_length == strlen(REQUEST_HEAD REQUEST_BODY));
        recv_text(h, response);
        check_done_hello(h);
    }
}

/* A body whose read cancels the request: the stream must not write after. */
typedef struct {
    http_stream_t *stream;
    int calls;
} cancel_on_read_t;

static size_t cancel_on_read(void *context, size_t offset, char *destination, size_t capacity) {
    cancel_on_read_t *c = (cancel_on_read_t *)context;
    (void)offset;
    c->calls++;
    if (capacity > 0) {
        destination[0] = 'x'; /* a valid byte the stream would otherwise write */
    }
    http_stream_cancel(c->stream);
    return capacity > 0 ? 1u : 0u;
}

static size_t body_length_99(void *context) {
    (void)context;
    return 118u; /* strlen(REQUEST_BODY) */
}

static void check_cancel_from_body_read(void) {
    harness_t *h = &g_harness;
    cancel_on_read_t cancel;
    http_stream_request_t request;
    provider_request_t body;

    harness_reset(h);
    cancel.stream = &h->stream;
    cancel.calls = 0;

    body.length = body_length_99;
    body.read = cancel_on_read;
    body.context = &cancel;

    memset(&request, 0, sizeof(request));
    request.host = "wang.local";
    request.port = 11434u;
    request.method = ollama_provider_method();
    request.path = ollama_provider_path();
    request.content_type = ollama_provider_content_type();
    request.body = body;
    request.sink.feed = sink_feed;
    request.sink.finish = sink_finish;
    request.sink.context = &h->provider;
    request.connect_timeout_ms = CONNECT_TIMEOUT_MS;
    request.idle_timeout_ms = IDLE_TIMEOUT_MS;
    request.request_timeout_ms = REQUEST_TIMEOUT_MS;

    ollama_provider_init(&h->provider, on_event, h);
    CHECK(http_stream_start(&h->stream, &request) == 0);
    http_stream_on_dns(&h->stream, true);
    http_stream_on_connected(&h->stream, true);

    /* The read cancelled the request mid-send: the body byte must not have
     * been written, and the error is CANCELLED, not SEND. */
    CHECK(cancel.calls >= 1);
    CHECK(h->net.write_calls == 1); /* request head only, no body byte */
    CHECK(http_stream_error(&h->stream) == HTTP_STREAM_ERROR_CANCELLED);
    CHECK(h->net.live == 0);
}

/* ------------------------------------------------------------------------ */
/* Wi-Fi manager                                                             */
/* ------------------------------------------------------------------------ */
typedef struct {
    uint32_t now;
    wifi_link_t link;
    int radio_on_result;
    int radio_on_calls;
    int radio_off_calls;
    int join_result;
    int join_calls;
    int leave_calls;
    char ssid[64];
    char password[80];
} fake_radio_t;

static uint32_t radio_clock_ms(void *context) {
    return ((fake_radio_t *)context)->now;
}

static int radio_on(void *context) {
    fake_radio_t *radio = (fake_radio_t *)context;
    radio->radio_on_calls++;
    return radio->radio_on_result;
}

static void radio_off(void *context) {
    ((fake_radio_t *)context)->radio_off_calls++;
}

static int radio_join(void *context, const char *ssid, const char *password) {
    fake_radio_t *radio = (fake_radio_t *)context;
    radio->join_calls++;
    snprintf(radio->ssid, sizeof(radio->ssid), "%s", ssid);
    snprintf(radio->password, sizeof(radio->password), "%s", password);
    return radio->join_result;
}

static void radio_leave(void *context) {
    ((fake_radio_t *)context)->leave_calls++;
}

static wifi_link_t radio_link(void *context) {
    return ((fake_radio_t *)context)->link;
}

static const wifi_manager_ops_t radio_ops = {
    radio_clock_ms, radio_on, radio_off, radio_join, radio_leave, radio_link,
};

static void radio_reset(fake_radio_t *radio, wifi_manager_t *wifi) {
    memset(radio, 0, sizeof(*radio));
    radio->now = 500u;
    radio->link = WIFI_LINK_DOWN;
    wifi_manager_init(wifi, &radio_ops, radio);
}

static void check_wifi_needs_config(void) {
    fake_radio_t radio;
    wifi_manager_t wifi;
    char too_long[80];

    radio_reset(&radio, &wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_OFF);
    CHECK(!wifi_manager_is_online(&wifi));
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_OFF);
    CHECK(radio.radio_on_calls == 0);

    /* Enabled with no SSID: the radio is never powered. */
    wifi_manager_enable(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_NEEDS_CONFIG);
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_NEEDS_CONFIG);
    CHECK(radio.radio_on_calls == 0);
    CHECK(radio.join_calls == 0);
    CHECK_STR_EQ(wifi_manager_ssid(&wifi), "");

    /* Oversized credentials are rejected and leave the manager unchanged. */
    memset(too_long, 'a', sizeof(too_long) - 1u);
    too_long[sizeof(too_long) - 1u] = '\0';
    CHECK(wifi_manager_set_credentials(&wifi, too_long, "pw") == -1);
    CHECK(wifi_manager_set_credentials(&wifi, "net", too_long) == -1);
    CHECK(wifi_manager_set_credentials(&wifi, NULL, "pw") == -1);
    CHECK(wifi_manager_set_credentials(NULL, "net", "pw") == -1);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_NEEDS_CONFIG);
    CHECK_STR_EQ(wifi_manager_ssid(&wifi), "");

    /* Supplying credentials while enabled starts the join. */
    CHECK(wifi_manager_set_credentials(&wifi, "net", "hunter2hunter2") == 0);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_CONNECTING);
    CHECK(radio.radio_on_calls == 1);
    CHECK(radio.join_calls == 1);
    CHECK_STR_EQ(radio.ssid, "net");
    CHECK_STR_EQ(radio.password, "hunter2hunter2");
    CHECK_STR_EQ(wifi_manager_ssid(&wifi), "net");

    /* Clearing the SSID drops back to NEEDS_CONFIG and leaves the network. */
    CHECK(wifi_manager_set_credentials(&wifi, "", "") == 0);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_NEEDS_CONFIG);
    CHECK(radio.leave_calls == 1);
}

static void check_wifi_clear_credentials_powers_off(void) {
    fake_radio_t radio;
    wifi_manager_t wifi;

    radio_reset(&radio, &wifi);
    CHECK(wifi_manager_set_credentials(&wifi, "net", "pw") == 0);
    wifi_manager_enable(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_CONNECTING);
    CHECK(radio.radio_on_calls == 1);
    radio.link = WIFI_LINK_UP;
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_ONLINE);
    CHECK(wifi_manager_radio_powered(&wifi));

    /* Clearing the SSID while enabled drops to NEEDS_CONFIG and powers the
     * radio off, so an unconfigured unit does not idle in station mode. */
    CHECK(wifi_manager_set_credentials(&wifi, "", "") == 0);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_NEEDS_CONFIG);
    CHECK(!wifi_manager_radio_powered(&wifi));
    CHECK(radio.radio_off_calls == 1);
}

static void check_wifi_connect_and_disable(void) {
    fake_radio_t radio;
    wifi_manager_t wifi;

    radio_reset(&radio, &wifi);
    CHECK(wifi_manager_set_credentials(&wifi, "net", "") == 0);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_OFF); /* not enabled yet */
    CHECK(radio.join_calls == 0);

    wifi_manager_enable(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_CONNECTING);
    CHECK_STR_EQ(radio.password, "");
    wifi_manager_enable(&wifi); /* idempotent */
    CHECK(radio.join_calls == 1);

    radio.link = WIFI_LINK_JOINING;
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_CONNECTING);
    radio.link = WIFI_LINK_UP;
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_ONLINE);
    CHECK(wifi_manager_is_online(&wifi));
    CHECK(wifi_manager_error(&wifi) == WIFI_ERROR_NONE);

    wifi_manager_disable(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_OFF);
    CHECK(!wifi_manager_is_online(&wifi));
    CHECK(radio.leave_calls == 1);
    CHECK(radio.radio_off_calls == 1);
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_OFF);
    CHECK(radio.join_calls == 1);
}

static void check_wifi_backoff_is_bounded(void) {
    fake_radio_t radio;
    wifi_manager_t wifi;
    uint32_t expected = WIFI_BACKOFF_BASE_MS;
    int attempt;

    radio_reset(&radio, &wifi);
    CHECK(wifi_manager_set_credentials(&wifi, "net", "pw") == 0);
    wifi_manager_enable(&wifi);

    /* Every failed join doubles the wait up to the cap, and never beyond. */
    for (attempt = 0; attempt < 40; attempt++) {
        int joins = radio.join_calls;
        radio.link = attempt % 2 == 0 ? WIFI_LINK_FAILED : WIFI_LINK_NO_NETWORK;
        wifi_manager_poll(&wifi);
        CHECK(wifi_manager_state(&wifi) == WIFI_STATE_BACKOFF);
        CHECK(wifi_manager_backoff_ms(&wifi) == expected);
        CHECK(wifi_manager_backoff_ms(&wifi) <= WIFI_BACKOFF_MAX_MS);

        radio.link = WIFI_LINK_DOWN;
        radio.now += expected - 1u;
        wifi_manager_poll(&wifi);
        CHECK(wifi_manager_state(&wifi) == WIFI_STATE_BACKOFF);
        CHECK(radio.join_calls == joins);
        radio.now += 1u;
        wifi_manager_poll(&wifi);
        CHECK(wifi_manager_state(&wifi) == WIFI_STATE_CONNECTING);
        CHECK(radio.join_calls == joins + 1);

        expected = expected >= WIFI_BACKOFF_MAX_MS / 2u ? WIFI_BACKOFF_MAX_MS : expected * 2u;
    }
    CHECK(expected == WIFI_BACKOFF_MAX_MS);

    /* Success resets the backoff. */
    radio.link = WIFI_LINK_UP;
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_ONLINE);
    radio.link = WIFI_LINK_DOWN;
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_BACKOFF);
    CHECK(wifi_manager_backoff_ms(&wifi) == WIFI_BACKOFF_BASE_MS);
}

static void check_wifi_join_timeout_and_errors(void) {
    fake_radio_t radio;
    wifi_manager_t wifi;

    /* The join never resolves either way. */
    radio_reset(&radio, &wifi);
    CHECK(wifi_manager_set_credentials(&wifi, "net", "pw") == 0);
    wifi_manager_enable(&wifi);
    radio.link = WIFI_LINK_JOINING;
    radio.now += WIFI_JOIN_TIMEOUT_MS - 1u;
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_CONNECTING);
    radio.now += 1u;
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_BACKOFF);
    CHECK(radio.leave_calls == 1);

    /* The join call itself fails: back off rather than spin. */
    radio_reset(&radio, &wifi);
    radio.join_result = -1;
    CHECK(wifi_manager_set_credentials(&wifi, "net", "pw") == 0);
    wifi_manager_enable(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_BACKOFF);
    wifi_manager_poll(&wifi);
    CHECK(radio.join_calls == 1);

    /* Wrong password: retrying cannot help, so stop until reconfigured. */
    radio_reset(&radio, &wifi);
    CHECK(wifi_manager_set_credentials(&wifi, "net", "wrong") == 0);
    wifi_manager_enable(&wifi);
    radio.link = WIFI_LINK_BAD_AUTH;
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_ERROR);
    CHECK(wifi_manager_error(&wifi) == WIFI_ERROR_AUTH);
    radio.now += 10u * WIFI_BACKOFF_MAX_MS;
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_ERROR);
    CHECK(radio.join_calls == 1);
    /* New credentials restart the join. */
    radio.link = WIFI_LINK_DOWN;
    CHECK(wifi_manager_set_credentials(&wifi, "net", "right") == 0);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_CONNECTING);
    CHECK(wifi_manager_error(&wifi) == WIFI_ERROR_NONE);
    CHECK(radio.join_calls == 2);

    /* The radio will not power up. */
    radio_reset(&radio, &wifi);
    radio.radio_on_result = -1;
    CHECK(wifi_manager_set_credentials(&wifi, "net", "pw") == 0);
    wifi_manager_enable(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_ERROR);
    CHECK(wifi_manager_error(&wifi) == WIFI_ERROR_RADIO);
    CHECK(radio.join_calls == 0);
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_ERROR);
    /* An explicit re-enable tries again. */
    radio.radio_on_result = 0;
    wifi_manager_enable(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_CONNECTING);
}

static void check_wifi_loss_mid_request(void) {
    harness_t *h = &g_harness;
    fake_radio_t radio;
    wifi_manager_t wifi;

    radio_reset(&radio, &wifi);
    CHECK(wifi_manager_set_credentials(&wifi, "net", "pw") == 0);
    wifi_manager_enable(&wifi);

    /* A request cannot start before the station is online. */
    harness_reset(h);
    h->net.wifi = &wifi;
    CHECK(harness_start(h) == -1);
    check_failed(h, HTTP_STREAM_ERROR_NETWORK_DOWN);
    CHECK(h->net.dns_calls == 0);

    radio.link = WIFI_LINK_UP;
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_is_online(&wifi));

    /* Link drops while the body is streaming. */
    start_to_receiving(h);
    recv_text(h, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    recv_text(h, "2b\r\n{\"message\":{\"content\":\"Hel\"},\"done\":false}\n\r\n");
    CHECK_STR_EQ(h->content, "Hel");
    http_stream_poll(&h->stream);
    CHECK(http_stream_state(&h->stream) == HTTP_STREAM_RECEIVING_BODY);

    radio.link = WIFI_LINK_DOWN;
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_state(&wifi) == WIFI_STATE_BACKOFF);
    http_stream_poll(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_NETWORK_DOWN);
    CHECK(h->net.aborts == 1);
    CHECK(h->net.closes == 0);
    CHECK_STR_EQ(h->content, "Hel");

    /* Link drops while the request is still being sent. */
    radio.link = WIFI_LINK_UP;
    radio.now += WIFI_BACKOFF_BASE_MS;
    wifi_manager_poll(&wifi);
    wifi_manager_poll(&wifi);
    CHECK(wifi_manager_is_online(&wifi));
    harness_reset(h);
    h->net.wifi = &wifi;
    h->net.sndbuf = 16u;
    CHECK(harness_start(h) == 0);
    http_stream_on_dns(&h->stream, true);
    http_stream_on_connected(&h->stream, true);
    radio.link = WIFI_LINK_DOWN;
    wifi_manager_poll(&wifi);
    http_stream_poll(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_NETWORK_DOWN);
    CHECK(h->net.aborts == 1);

    /* And while resolving, where no connection exists yet. */
    radio.link = WIFI_LINK_UP;
    radio.now += WIFI_BACKOFF_BASE_MS;
    wifi_manager_poll(&wifi);
    wifi_manager_poll(&wifi);
    harness_reset(h);
    h->net.wifi = &wifi;
    CHECK(harness_start(h) == 0);
    wifi_manager_disable(&wifi);
    http_stream_poll(&h->stream);
    check_failed(h, HTTP_STREAM_ERROR_NETWORK_DOWN);
    CHECK(h->net.aborts == 0);
}

void test_http_stream(void) {
    check_happy_path_content_length();
    check_dns_resolved_immediately();
    check_dns_failure();
    check_connection_refused();
    check_backpressure();
    check_send_buffer_full_then_poll_retries();
    check_write_error();
    check_header_fragmentation();
    check_chunked_every_split();
    check_close_delimited();
    check_idle_timeout();
    check_absolute_timeout();
    check_connect_timeout();
    check_remote_close_mid_body();
    check_cancellation();
    check_cancel_from_event_callback();
    check_cancel_from_body_read();
    check_protocol_errors();
    check_http_error_status();
    check_early_response_while_sending();
    check_start_validation_and_reuse();

    check_wifi_needs_config();
    check_wifi_clear_credentials_powers_off();
    check_wifi_connect_and_disable();
    check_wifi_backoff_is_bounded();
    check_wifi_join_timeout_and_errors();
    check_wifi_loss_mid_request();
}
