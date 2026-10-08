#include <string.h>

#include "ai/http_parser.h"
#include "test_util.h"

typedef struct {
    int head_count;
    http_response_head_t head;
    size_t body_length;
    char body[4096];
    bool complete;
} response_t;

static void on_head(void *context, const http_response_head_t *head) {
    response_t *response = (response_t *)context;
    response->head_count++;
    response->head = *head;
}

static void on_body(void *context, const char *data, size_t length) {
    response_t *response = (response_t *)context;
    if (response->body_length + length <= sizeof(response->body)) {
        memcpy(response->body + response->body_length, data, length);
        response->body_length += length;
    }
}

static int run(const char *input, size_t length, size_t chunk, response_t *response) {
    memset(response, 0, sizeof(*response));
    http_parser_t parser;
    http_parser_init(&parser, on_head, on_body, response);
    size_t offset = 0;
    while (offset < length) {
        size_t take = length - offset < chunk ? length - offset : chunk;
        if (http_parser_feed(&parser, input + offset, take) != 0) {
            response->complete = false;
            return -1;
        }
        offset += take;
    }
    int rc = http_parser_finish(&parser);
    response->complete = http_parser_complete(&parser);
    return rc;
}

static void check_content_length(void) {
    const char *input = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    size_t length = strlen(input);
    for (size_t chunk = 1; chunk <= length; chunk++) {
        response_t response;
        CHECK(run(input, length, chunk, &response) == 0);
        CHECK(response.head_count == 1);
        CHECK(response.head.status_code == 200);
        CHECK(response.head.has_content_length);
        CHECK(response.head.content_length == 5);
        CHECK(response.head.chunked == false);
        CHECK(response.complete);
        CHECK(response.body_length == 5);
        CHECK_BYTES_EQ(response.body, "hello", 5);
    }

    /* Extra bytes past Content-Length are rejected, not silently discarded:
     * this parser is not a pipeline. */
    const char *extra = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcXYZ";
    for (size_t chunk = 1; chunk <= strlen(extra); chunk++) {
        response_t response;
        CHECK(run(extra, strlen(extra), chunk, &response) == -1);
        CHECK(response.complete == false);
    }

    /* A Content-Length body split across feeds stays accepted. */
    response_t response;
    const char *exact = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
    CHECK(run(exact, strlen(exact), 1, &response) == 0);
    CHECK(response.complete);
    CHECK(response.body_length == 3);

    /* HTTP/1.0 is accepted. */
    const char *http10 = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok";
    CHECK(run(http10, strlen(http10), 3, &response) == 0);
    CHECK(response.head.status_code == 200);
    CHECK(response.body_length == 2);

    /* Zero-length body completes immediately. */
    const char *zero = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
    CHECK(run(zero, strlen(zero), 1, &response) == 0);
    CHECK(response.head.status_code == 204);
    CHECK(response.complete);
    CHECK(response.body_length == 0);

    /* Connection header is captured. */
    const char *close_header = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok";
    CHECK(run(close_header, strlen(close_header), 4, &response) == 0);
    CHECK(response.head.connection_close);
    CHECK(response.body_length == 2);
    CHECK_BYTES_EQ(response.body, "ok", 2);

    /* Connection is tokenised: "close" inside a comma list matches, but a
     * token that merely contains the substring "close" ("disclose") does not. */
    const char *close_list = "HTTP/1.1 200 OK\r\nConnection: keep-alive, close\r\nContent-Length: 2\r\n\r\nok";
    CHECK(run(close_list, strlen(close_list), 4, &response) == 0);
    CHECK(response.head.connection_close);
    CHECK(response.body_length == 2);

    const char *disclose = "HTTP/1.1 200 OK\r\nConnection: disclose\r\nContent-Length: 2\r\n\r\nok";
    CHECK(run(disclose, strlen(disclose), 4, &response) == 0);
    CHECK(response.head.connection_close == false);
    CHECK(response.body_length == 2);
}

static void check_chunked(void) {
    const char *input = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                        "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    size_t length = strlen(input);
    for (size_t chunk = 1; chunk <= length; chunk++) {
        response_t response;
        CHECK(run(input, length, chunk, &response) == 0);
        CHECK(response.head_count == 1);
        CHECK(response.head.status_code == 200);
        CHECK(response.head.chunked);
        CHECK(response.complete);
        CHECK(response.body_length == 11);
        CHECK_BYTES_EQ(response.body, "hello world", 11);
    }

    /* Chunk extensions are ignored. */
    const char *extension = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                            "5;name=value\r\nhello\r\n0\r\n\r\n";
    response_t response;
    CHECK(run(extension, strlen(extension), 1, &response) == 0);
    CHECK(response.body_length == 5);
    CHECK_BYTES_EQ(response.body, "hello", 5);

    /* Trailer headers after the last chunk. */
    const char *trailer = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                          "3\r\nabc\r\n0\r\nX-Trailer: value\r\n\r\n";
    CHECK(run(trailer, strlen(trailer), 1, &response) == 0);
    CHECK(response.complete);
    CHECK(response.body_length == 3);
    CHECK_BYTES_EQ(response.body, "abc", 3);

    /* Transfer-Encoding is matched case-insensitively as a whole token. */
    const char *mixed = "HTTP/1.1 200 OK\r\nTransfer-Encoding: ChUnKeD\r\n\r\n2\r\nok\r\n0\r\n\r\n";
    CHECK(run(mixed, strlen(mixed), 1, &response) == 0);
    CHECK(response.head.chunked);
    CHECK(response.body_length == 2);
    CHECK_BYTES_EQ(response.body, "ok", 2);

    /* Bytes after the chunked terminator are rejected. */
    const char *trailing = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\nJUNK";
    for (size_t chunk = 1; chunk <= strlen(trailing); chunk++) {
        CHECK(run(trailing, strlen(trailing), chunk, &response) == -1);
        CHECK(response.complete == false);
    }
}

static void check_close_delimited(void) {
    const char *input = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nabc";
    size_t length = strlen(input);
    for (size_t chunk = 1; chunk <= length; chunk++) {
        response_t response;
        CHECK(run(input, length, chunk, &response) == 0);
        CHECK(response.head_count == 1);
        CHECK(response.head.chunked == false);
        CHECK(response.head.has_content_length == false);
        CHECK(response.complete);
        CHECK(response.body_length == 3);
        CHECK_BYTES_EQ(response.body, "abc", 3);
    }
}

static void check_malformed(void) {
    static const char *cases[] = {
        "NOTHTTP 200 OK\r\n\r\n",
        "HTTP/1.1 2x0 OK\r\n\r\n",
        "HTTP/1.1 200 OK\r\nBadHeader\r\n\r\n",
        "HTTP/1.1 200 OK\r\nX: 1\r\n continuation\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10001\r\n",
        /* Status line: version and 3-digit status are validated. */
        "HTTP/2.0 200 OK\r\n\r\n",
        "HTTP/1.1 20 OK\r\n\r\n",
        "HTTP/1.1 2000 OK\r\n\r\n",
        "HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 999 OK\r\n\r\n",
        "HTTP/1.1 099 OK\r\n\r\n",
        /* LF-only status line and LF-only header line. */
        "HTTP/1.1 200 OK\nContent-Length: 2\r\n\r\nok",
        "HTTP/1.1 200 OK\r\nContent-Length: 2\n\r\n\r\n",
        /* Transfer-Encoding is a token, not a substring. */
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: notchunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: \r\n\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked,\r\n\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: ,chunked\r\n\r\n0\r\n\r\n",
        /* Ambiguous framing. */
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        /* Chunk framing must use exact CRLF and strict hex syntax. */
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\nhello\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\r\nhello\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n 5\r\nhello\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;\r\nhello\r\n0\r\n\r\n",
        /* Trailer fields are validated. */
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX-Trailer value\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX: 1\r\n continuation\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX: 1\n\r\n",
        /* Bytes after a zero-length body. */
        "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\nX",
    };
    static const size_t chunks[] = {1, 2, 3, 7};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
            response_t response;
            CHECK(run(cases[i], strlen(cases[i]), chunks[c], &response) == -1);
            CHECK(response.complete == false);
        }
    }

    /* Truncated Content-Length body fails at finish. */
    const char *truncated = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhel";
    response_t response;
    CHECK(run(truncated, strlen(truncated), 1, &response) == -1);

    /* Truncated chunk data fails at finish. */
    const char *truncated_chunk = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel";
    CHECK(run(truncated_chunk, strlen(truncated_chunk), 1, &response) == -1);

    /* Header block larger than HTTP_PARSER_HEADER_MAX fails. */
    char oversized[2100];
    size_t length = 0;
    const char *prefix = "HTTP/1.1 200 OK\r\nX-Big: ";
    memcpy(oversized, prefix, strlen(prefix));
    length = strlen(prefix);
    memset(oversized + length, 'a', 2000);
    length += 2000;
    memcpy(oversized + length, "\r\n\r\n", 4);
    length += 4;
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        CHECK(run(oversized, length, chunks[c], &response) == -1);
    }

    /* Chunk size line larger than HTTP_PARSER_CHUNK_LINE_MAX fails. */
    const char *long_chunk = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n";
    CHECK(run(long_chunk, strlen(long_chunk), 1, &response) == -1);
}

static void check_null_preconditions(void) {
    response_t response;
    http_parser_t parser;
    memset(&response, 0, sizeof(response));
    http_parser_init(NULL, on_head, on_body, &response);
    http_parser_init(&parser, on_head, on_body, &response);

    CHECK(http_parser_feed(NULL, "x", 1u) == -1);
    CHECK(http_parser_feed(&parser, NULL, 1u) == -1);
    CHECK(http_parser_feed(&parser, NULL, 0u) == 0);
    CHECK(http_parser_finish(NULL) == -1);
    CHECK(http_parser_complete(NULL) == false);
    CHECK(http_parser_failed(NULL) == false);
}

void test_http_parser(void) {
    check_content_length();
    check_chunked();
    check_close_delimited();
    check_malformed();
    check_null_preconditions();
}
