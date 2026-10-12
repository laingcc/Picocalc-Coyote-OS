#include <stdio.h>
#include <string.h>

#include "ai/deepseek_provider.h"
#include "ai/http_stream.h"
#include "test_util.h"

/*
 * The transport's TLS policy, driven against a fake network that counts every
 * adapter call: a TLS request is connected through tls_connect or not at all,
 * and a bearer token never leaves except inside one.
 */

#define WIRE_MAX 4096u

typedef struct {
    uint32_t now;
    int dns_result;
    int dns_calls;
    int tcp_connect_calls; /* plain connections asked for */
    int tls_connect_calls;
    int tls_connect_result;
    char tls_host[64];
    uint16_t tls_port;
    int live;
    int closes;
    int aborts;
    int write_calls;
    char wire[WIRE_MAX];
    size_t wire_length;
    size_t unacked;
    size_t sndbuf; /* 0 = unlimited */
} fake_t;

static uint32_t fake_clock_ms(void *context) { return ((fake_t *)context)->now; }

static int fake_dns_resolve(void *context, const char *host) {
    fake_t *net = (fake_t *)context;
    (void)host;
    net->dns_calls++;
    return net->dns_result;
}

static int fake_tcp_connect(void *context, uint16_t port) {
    fake_t *net = (fake_t *)context;
    (void)port;
    net->tcp_connect_calls++;
    net->live++;
    return HTTP_STREAM_IO_PENDING;
}

static int fake_tls_connect(void *context, const char *host, uint16_t port) {
    fake_t *net = (fake_t *)context;
    net->tls_connect_calls++;
    snprintf(net->tls_host, sizeof(net->tls_host), "%s", host);
    net->tls_port = port;
    if (net->tls_connect_result != HTTP_STREAM_IO_PENDING) {
        return net->tls_connect_result;
    }
    net->live++;
    return HTTP_STREAM_IO_PENDING;
}

static int fake_write(void *context, const char *data, size_t length) {
    fake_t *net = (fake_t *)context;
    net->write_calls++;
    if (net->sndbuf != 0u && length > net->sndbuf - net->unacked) {
        length = net->sndbuf - net->unacked;
    }
    if (length > WIRE_MAX - net->wire_length) {
        length = WIRE_MAX - net->wire_length;
    }
    memcpy(net->wire + net->wire_length, data, length);
    net->wire_length += length;
    net->unacked += length;
    return (int)length;
}

static void fake_close(void *context) {
    fake_t *net = (fake_t *)context;
    net->live--;
    net->closes++;
}

static void fake_abort(void *context) {
    fake_t *net = (fake_t *)context;
    net->live--;
    net->aborts++;
}

static const http_stream_ops_t tls_ops = {
    fake_clock_ms, NULL, fake_dns_resolve, fake_tcp_connect, fake_write, fake_close, fake_abort, fake_tls_connect,
};

/* A platform without TLS has no tls_connect adapter. */
static const http_stream_ops_t plain_ops = {
    fake_clock_ms, NULL, fake_dns_resolve, fake_tcp_connect, fake_write, fake_close, fake_abort, NULL,
};

/* Static: these are too large for stack locals. */
static fake_t net;
static http_stream_t stream;
static deepseek_provider_t provider;
static char content[256];
static int done_events, error_events;

static void on_event(void *context, const provider_event_t *event) {
    (void)context;
    if (event->type == PROVIDER_EVENT_CONTENT) {
        size_t used = strlen(content);
        memcpy(content + used, event->data, event->length);
        content[used + event->length] = '\0';
    } else if (event->type == PROVIDER_EVENT_DONE) {
        done_events++;
    } else {
        error_events++;
    }
}

/* The firmware's sink: an error status has no event stream to parse. */
static bool status_ok(void) {
    int status = http_stream_status_code(&stream);
    return status >= 200 && status <= 299;
}

static int sink_feed(void *context, const char *data, size_t length) {
    return status_ok() ? deepseek_provider_feed((deepseek_provider_t *)context, data, length) : 0;
}

static int sink_finish(void *context) {
    return status_ok() ? deepseek_provider_finish((deepseek_provider_t *)context) : 0;
}

/* Not a real key: a made-up value in the shape of one. */
#define TEST_KEY "sk-0000000000000000000000000000test"

#define EXPECTED_BODY \
    "{\"model\":\"deepseek-chat\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]," \
    "\"stream\":true,\"max_tokens\":8,\"temperature\":0.80}"

/* Written out byte for byte rather than assembled from the code's constants. */
#define EXPECTED_HEAD                                                   \
    "POST /chat/completions HTTP/1.1\r\n"                               \
    "Host: api.deepseek.com\r\n"                                        \
    "Authorization: Bearer sk-0000000000000000000000000000test\r\n"     \
    "Content-Type: application/json\r\n"                                \
    "Content-Length: 117\r\n"                                           \
    "Connection: close\r\n"                                             \
    "\r\n"

static http_stream_request_t request;

static void setup(const http_stream_ops_t *ops) {
    static const ollama_message_t hello[] = {{"user", "hi"}};

    memset(&net, 0, sizeof(net));
    net.dns_result = HTTP_STREAM_IO_DONE;
    net.tls_connect_result = HTTP_STREAM_IO_PENDING;
    content[0] = '\0';
    done_events = error_events = 0;
    http_stream_init(&stream, ops, &net);
    deepseek_provider_init(&provider, on_event, NULL);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", hello, 1u, 8, NULL, 80) == 0);

    memset(&request, 0, sizeof(request));
    request.host = AI_CONFIG_DEEPSEEK_HOST;
    request.port = AI_CONFIG_DEEPSEEK_PORT;
    request.tls = true;
    request.bearer_token = TEST_KEY;
    request.method = deepseek_provider_method();
    request.path = deepseek_provider_path();
    request.content_type = deepseek_provider_content_type();
    request.body = deepseek_provider_request(&provider);
    request.sink.feed = sink_feed;
    request.sink.finish = sink_finish;
    request.sink.context = &provider;
    request.connect_timeout_ms = 1000u;
    request.idle_timeout_ms = 1000u;
    request.request_timeout_ms = 5000u;
}

static bool head_is_wiped(void) {
    size_t i;
    for (i = 0u; i < sizeof(stream.head); i++) {
        if (stream.head[i] != '\0') {
            return false;
        }
    }
    return true;
}

static void ack_all(void) {
    size_t n = net.unacked;
    net.unacked = 0u;
    http_stream_on_sent(&stream, n);
}

static void recv_text(const char *text) {
    http_stream_on_recv(&stream, text, strlen(text));
}

/* The invariant behind every failure test: the request never reached a plain
 * connection, the stream holds no connection, and the key is not in memory. */
static void check_failed_closed(http_stream_error_t error) {
    CHECK(http_stream_state(&stream) == HTTP_STREAM_ERROR);
    CHECK(http_stream_error(&stream) == error);
    CHECK(net.tcp_connect_calls == 0);
    CHECK(net.live == 0);
    CHECK(head_is_wiped());
}

/* ---- Success ------------------------------------------------------------- */

static void test_tls_request_on_the_wire(void) {
    setup(&tls_ops);
    CHECK(strlen(EXPECTED_BODY) == 117u);
    CHECK(http_stream_start(&stream, &request) == 0);
    CHECK(http_stream_state(&stream) == HTTP_STREAM_CONNECTING);
    CHECK(net.tls_connect_calls == 1);
    CHECK(net.tcp_connect_calls == 0);
    CHECK_STR_EQ(net.tls_host, "api.deepseek.com");
    CHECK(net.tls_port == 443u);
    /* Nothing is written until the adapter reports the verified handshake. */
    http_stream_poll(&stream);
    http_stream_on_sent(&stream, 10u);
    CHECK(net.write_calls == 0);
    CHECK(net.wire_length == 0u);

    http_stream_on_connected(&stream, true);
    net.wire[net.wire_length] = '\0';
    CHECK_STR_EQ(net.wire, EXPECTED_HEAD EXPECTED_BODY);
    CHECK(strstr(net.wire, "\r\nAuthorization: Bearer " TEST_KEY "\r\n") != NULL);
    /* Once handed to the stack the head, and the key in it, is gone. */
    CHECK(head_is_wiped());
    ack_all();
    CHECK(http_stream_state(&stream) == HTTP_STREAM_RECEIVING_HEAD);

    recv_text("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n");
    recv_text("e\r\n: keep-alive\n\n\r\n");
    recv_text("34\r\ndata: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n\r\n");
    CHECK(content[0] == '\0');
    recv_text("30\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n\r\n");
    recv_text("32\r\ndata: {\"choices\":[{\"delta\":{\"content\":\" you\"}}]}\n\n\r\n");
    CHECK_STR_EQ(content, "Hi you");
    CHECK(done_events == 0);
    recv_text("e\r\ndata: [DONE]\n\n\r\n0\r\n\r\n");
    CHECK(done_events == 1);
    CHECK(error_events == 0);
    CHECK(http_stream_state(&stream) == HTTP_STREAM_DONE);
    CHECK(net.closes == 1 && net.aborts == 0 && net.live == 0);
    CHECK(net.tcp_connect_calls == 0);
}

static void test_tls_request_with_backpressure(void) {
    setup(&tls_ops);
    net.sndbuf = 50u;
    CHECK(http_stream_start(&stream, &request) == 0);
    http_stream_on_connected(&stream, true);
    while (http_stream_state(&stream) == HTTP_STREAM_SENDING) {
        /* The key is still needed until the last of the head is queued. */
        CHECK(net.wire_length >= strlen(EXPECTED_HEAD) ? head_is_wiped() : !head_is_wiped());
        ack_all();
    }
    net.wire[net.wire_length] = '\0';
    CHECK_STR_EQ(net.wire, EXPECTED_HEAD EXPECTED_BODY);
    CHECK(head_is_wiped());
}

static void test_dns_pending_then_tls(void) {
    setup(&tls_ops);
    net.dns_result = HTTP_STREAM_IO_PENDING;
    CHECK(http_stream_start(&stream, &request) == 0);
    CHECK(net.tls_connect_calls == 0);
    http_stream_on_dns(&stream, true);
    CHECK(net.tls_connect_calls == 1);
    CHECK(net.tcp_connect_calls == 0);
    CHECK_STR_EQ(net.tls_host, "api.deepseek.com");
    http_stream_cancel(&stream);
    check_failed_closed(HTTP_STREAM_ERROR_CANCELLED);
    CHECK(net.aborts == 1);
}

/* ---- No fallback --------------------------------------------------------- */

static void test_no_tls_adapter(void) {
    /* Without a TLS adapter the request is refused outright: no lookup, no
     * connection of any kind, nothing written. */
    setup(&plain_ops);
    CHECK(http_stream_start(&stream, &request) == -1);
    CHECK(http_stream_state(&stream) == HTTP_STREAM_IDLE);
    CHECK(net.dns_calls == 0);
    CHECK(net.tcp_connect_calls == 0);
    CHECK(net.write_calls == 0);
    CHECK(net.live == 0);
    /* The same adapters do carry a plain request. */
    request.tls = false;
    request.bearer_token = NULL;
    CHECK(http_stream_start(&stream, &request) == 0);
    CHECK(net.tcp_connect_calls == 1);
    http_stream_cancel(&stream);
}

static void test_tls_setup_failure(void) {
    /* The adapter could not create the TLS configuration, the PCB or set the
     * host name: terminal, and no plain connection is tried instead. */
    setup(&tls_ops);
    net.tls_connect_result = HTTP_STREAM_IO_ERROR;
    CHECK(http_stream_start(&stream, &request) == 0);
    CHECK(net.tls_connect_calls == 1);
    check_failed_closed(HTTP_STREAM_ERROR_TLS);
    http_stream_poll(&stream);
    http_stream_on_connected(&stream, true);
    http_stream_on_dns(&stream, true);
    CHECK(net.tls_connect_calls == 1);
    check_failed_closed(HTTP_STREAM_ERROR_TLS);
    CHECK(net.write_calls == 0);

    /* An adapter claiming to have connected at once is not believed either. */
    setup(&tls_ops);
    net.tls_connect_result = HTTP_STREAM_IO_DONE;
    CHECK(http_stream_start(&stream, &request) == 0);
    check_failed_closed(HTTP_STREAM_ERROR_TLS);
    CHECK(net.write_calls == 0);
}

static void test_tls_handshake_failure(void) {
    /* Handshake or verification failed after the connection was opened: the
     * stack reports it as a failed connect... */
    setup(&tls_ops);
    CHECK(http_stream_start(&stream, &request) == 0);
    http_stream_on_connected(&stream, false);
    check_failed_closed(HTTP_STREAM_ERROR_TLS);
    CHECK(net.aborts == 1);
    CHECK(net.write_calls == 0);

    /* ...or by destroying the connection (lwIP's error callback). */
    setup(&tls_ops);
    CHECK(http_stream_start(&stream, &request) == 0);
    net.live--; /* the stack freed it */
    http_stream_on_error(&stream);
    check_failed_closed(HTTP_STREAM_ERROR_TLS);
    CHECK(net.aborts == 0 && net.closes == 0);
    CHECK(net.write_calls == 0);
    /* Nothing afterwards revives the request or retries it another way. */
    http_stream_poll(&stream);
    http_stream_on_connected(&stream, true);
    http_stream_on_sent(&stream, 1u);
    CHECK(net.tls_connect_calls == 1);
    CHECK(net.write_calls == 0);
    check_failed_closed(HTTP_STREAM_ERROR_TLS);

    /* The peer closing during the handshake. */
    setup(&tls_ops);
    CHECK(http_stream_start(&stream, &request) == 0);
    http_stream_on_closed(&stream);
    check_failed_closed(HTTP_STREAM_ERROR_REMOTE_CLOSED);
    CHECK(net.write_calls == 0);
}

static void test_tls_dns_and_timeout_failures(void) {
    setup(&tls_ops);
    net.dns_result = HTTP_STREAM_IO_ERROR;
    CHECK(http_stream_start(&stream, &request) == -1);
    check_failed_closed(HTTP_STREAM_ERROR_DNS);
    CHECK(net.tls_connect_calls == 0);

    setup(&tls_ops);
    net.dns_result = HTTP_STREAM_IO_PENDING;
    CHECK(http_stream_start(&stream, &request) == 0);
    http_stream_on_dns(&stream, false);
    check_failed_closed(HTTP_STREAM_ERROR_DNS);
    CHECK(net.tls_connect_calls == 0);

    /* A handshake that never finishes is bounded by the connect timeout. */
    setup(&tls_ops);
    CHECK(http_stream_start(&stream, &request) == 0);
    net.now = 999u;
    http_stream_poll(&stream);
    CHECK(http_stream_is_active(&stream));
    net.now = 1000u;
    http_stream_poll(&stream);
    check_failed_closed(HTTP_STREAM_ERROR_CONNECT_TIMEOUT);
    CHECK(net.aborts == 1);
    CHECK(net.write_calls == 0);
}

static void test_tls_mid_stream_failures(void) {
    /* Connection lost after the request went out. */
    setup(&tls_ops);
    CHECK(http_stream_start(&stream, &request) == 0);
    http_stream_on_connected(&stream, true);
    ack_all();
    recv_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    recv_text("30\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n\r\n");
    net.live--;
    http_stream_on_error(&stream);
    check_failed_closed(HTTP_STREAM_ERROR_REMOTE_CLOSED);

    /* The peer closes cleanly at a chunk boundary but before [DONE]: a
     * truncated reply is never reported as complete. */
    setup(&tls_ops);
    CHECK(http_stream_start(&stream, &request) == 0);
    http_stream_on_connected(&stream, true);
    ack_all();
    recv_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    recv_text("30\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n\r\n");
    http_stream_on_closed(&stream);
    check_failed_closed(HTTP_STREAM_ERROR_REMOTE_CLOSED);
    CHECK(done_events == 0);

    /* The HTTP body ends properly but the event stream never said [DONE]. */
    setup(&tls_ops);
    CHECK(http_stream_start(&stream, &request) == 0);
    http_stream_on_connected(&stream, true);
    ack_all();
    recv_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    recv_text("30\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n\r\n0\r\n\r\n");
    check_failed_closed(HTTP_STREAM_ERROR_SINK);
    CHECK(done_events == 0 && error_events == 1);

    /* Bytes after [DONE] inside the body. */
    setup(&tls_ops);
    CHECK(http_stream_start(&stream, &request) == 0);
    http_stream_on_connected(&stream, true);
    ack_all();
    recv_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    recv_text("14\r\ndata: [DONE]\n\nextra\n\r\n0\r\n\r\n");
    check_failed_closed(HTTP_STREAM_ERROR_SINK);
    CHECK(error_events == 1);
}

static void test_error_status(void) {
    /* DeepSeek's answer to a bad key is a 401 with a plain-text body.  It is
     * reported as the status; the body is not parsed and raises no event. */
    setup(&tls_ops);
    CHECK(http_stream_start(&stream, &request) == 0);
    http_stream_on_connected(&stream, true);
    ack_all();
    recv_text("HTTP/1.1 401 Unauthorized\r\nContent-Length: 31\r\n\r\nAuthentication Fails (governor)");
    check_failed_closed(HTTP_STREAM_ERROR_HTTP_STATUS);
    CHECK(http_stream_status_code(&stream) == 401);
    CHECK(error_events == 0 && done_events == 0 && content[0] == '\0');
}

/* ---- The credential ------------------------------------------------------ */

static void test_bearer_token_needs_tls(void) {
    /* A token on a plain request is refused before anything happens. */
    setup(&tls_ops);
    request.tls = false;
    CHECK(http_stream_start(&stream, &request) == -1);
    CHECK(http_stream_state(&stream) == HTTP_STREAM_IDLE);
    CHECK(net.dns_calls == 0 && net.tcp_connect_calls == 0 && net.tls_connect_calls == 0);
    CHECK(net.write_calls == 0);
    CHECK(head_is_wiped());
}

static void test_bearer_token_must_be_header_safe(void) {
    static const char *const bad[] = {
        "",
        "sk-abc def",
        "sk-abc\r\nX-Injected: 1",
        "sk-abc\nHost: evil.example",
        "sk-abc\tdef",
        "sk-\"quoted\"",
        "sk-abc,def",
        "sk-abc:def",
        "sk-abc=def", /* '=' is padding: only at the end */
        "=",
        "sk-\x7f",
        "sk-\xc3\xa9",
    };
    static const char *const good[] = {"sk-0123456789abcdef", "A-Za-z0-9._~+/", "dGVzdA==", "a"};
    size_t i;

    for (i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
        setup(&tls_ops);
        request.bearer_token = bad[i];
        CHECK(http_stream_start(&stream, &request) == -1);
        CHECK(net.dns_calls == 0 && net.tls_connect_calls == 0 && net.tcp_connect_calls == 0);
        CHECK(head_is_wiped());
        CHECK(!ai_config_bearer_token_valid(bad[i]));
    }
    for (i = 0u; i < sizeof(good) / sizeof(good[0]); i++) {
        setup(&tls_ops);
        request.bearer_token = good[i];
        CHECK(http_stream_start(&stream, &request) == 0);
        CHECK(ai_config_bearer_token_valid(good[i]));
        http_stream_cancel(&stream);
        CHECK(head_is_wiped());
    }
    CHECK(!ai_config_bearer_token_valid(NULL));

    /* A head too long for its buffer is refused, and the partial copy of the
     * token is not left behind. */
    {
        static char long_token[HTTP_STREAM_REQUEST_HEAD_MAX + 1u];
        memset(long_token, 'k', HTTP_STREAM_REQUEST_HEAD_MAX);
        setup(&tls_ops);
        request.bearer_token = long_token;
        CHECK(http_stream_start(&stream, &request) == -1);
        CHECK(net.dns_calls == 0);
        CHECK(head_is_wiped());
    }
}

static void test_host_header_port(void) {
    /* The port is left out only for TLS on 443. */
    setup(&tls_ops);
    request.port = 8443u;
    request.bearer_token = NULL;
    CHECK(http_stream_start(&stream, &request) == 0);
    http_stream_on_connected(&stream, true);
    net.wire[net.wire_length] = '\0';
    CHECK(strstr(net.wire, "\r\nHost: api.deepseek.com:8443\r\n") != NULL);
    CHECK(strstr(net.wire, "Authorization") == NULL);
    CHECK(net.tls_port == 8443u);
    http_stream_cancel(&stream);

    setup(&tls_ops);
    request.tls = false;
    request.bearer_token = NULL;
    request.port = 443u;
    CHECK(http_stream_start(&stream, &request) == 0);
    CHECK(net.tls_connect_calls == 0 && net.tcp_connect_calls == 1);
    http_stream_on_connected(&stream, true);
    net.wire[net.wire_length] = '\0';
    CHECK(strstr(net.wire, "\r\nHost: api.deepseek.com:443\r\n") != NULL);
    http_stream_cancel(&stream);
}

void test_http_tls(void);
void test_http_tls(void) {
    test_tls_request_on_the_wire();
    test_tls_request_with_backpressure();
    test_dns_pending_then_tls();
    test_no_tls_adapter();
    test_tls_setup_failure();
    test_tls_handshake_failure();
    test_tls_dns_and_timeout_failures();
    test_tls_mid_stream_failures();
    test_error_status();
    test_bearer_token_needs_tls();
    test_bearer_token_must_be_header_safe();
    test_host_header_port();
}
