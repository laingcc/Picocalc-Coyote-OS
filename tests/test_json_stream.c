#include <string.h>

#include "ai/json_stream.h"
#include "test_util.h"

typedef struct {
    int count;
    int type[16];
    char data[16][256];
    size_t length[16];
} event_log_t;

typedef struct {
    json_stream_t *stream;
    size_t events;
    size_t done_events;
    size_t error_events;
    int nested_finish;
    int nested_error_finish;
} reentrant_log_t;

static void collect(void *context, const json_stream_event_t *event) {
    event_log_t *log = (event_log_t *)context;
    if (log->count < 16) {
        log->type[log->count] = (int)event->type;
        log->length[log->count] = event->length;
        if (event->data != NULL && event->length < sizeof(log->data[0])) {
            memcpy(log->data[log->count], event->data, event->length);
            log->data[log->count][event->length] = '\0';
        } else {
            log->data[log->count][0] = '\0';
        }
    }
    log->count++;
}

static void collect_reentrant(void *context, const json_stream_event_t *event) {
    reentrant_log_t *log = (reentrant_log_t *)context;
    log->events++;
    if (event->type == JSON_STREAM_EVENT_DONE) {
        log->done_events++;
        if (log->done_events == 1u) {
            log->nested_finish = json_stream_finish(log->stream);
        }
    } else if (event->type == JSON_STREAM_EVENT_ERROR) {
        log->error_events++;
        if (log->error_events == 1u) {
            log->nested_error_finish = json_stream_finish(log->stream);
        }
    }
}

/* Feed the whole input in fixed-size chunks; returns -1 if any feed fails. */
static int feed_all(json_stream_t *stream, const char *input, size_t length, size_t chunk) {
    size_t offset = 0;
    while (offset < length) {
        size_t take = length - offset < chunk ? length - offset : chunk;
        if (json_stream_feed(stream, input + offset, take) != 0) {
            return -1;
        }
        offset += take;
    }
    return 0;
}

/* Feed the whole input in fixed-size chunks, then finish. */
static int run(const char *input, size_t length, size_t chunk, event_log_t *log) {
    memset(log, 0, sizeof(*log));
    json_stream_t stream;
    json_stream_init(&stream, collect, log);
    if (feed_all(&stream, input, length, chunk) != 0) {
        return -1;
    }
    return json_stream_finish(&stream);
}

static void check_hello_at_every_split(void) {
    const char *input = "{\"message\":{\"role\":\"assistant\",\"content\":\"Hello\"},\"done\":false}\n";
    size_t length = strlen(input);
    for (size_t chunk = 1; chunk <= length; chunk++) {
        event_log_t log;
        memset(&log, 0, sizeof(log));
        json_stream_t stream;
        json_stream_init(&stream, collect, &log);
        CHECK(feed_all(&stream, input, length, chunk) == 0);
        CHECK(log.count == 1);
        CHECK(log.type[0] == JSON_STREAM_EVENT_CONTENT);
        CHECK(log.length[0] == 5);
        CHECK_STR_EQ(log.data[0], "Hello");
        /* End of transport without "done": true is not a success. */
        CHECK(json_stream_finish(&stream) == -1);
        CHECK(log.count == 1);
    }
}

static void check_escapes(void) {
    char buffer[64];
    size_t written = 0;

    CHECK(json_escape_string("a\"b\\c\n", 6, buffer, sizeof(buffer), &written) == 0);
    CHECK(written == 9);
    CHECK_STR_EQ(buffer, "a\\\"b\\\\c\\n");

    CHECK(json_escape_string("\x01", 1, buffer, sizeof(buffer), &written) == 0);
    CHECK(written == 6);
    CHECK_STR_EQ(buffer, "\\u0001");

    CHECK(json_escape_string("\t\r\b\f", 4, buffer, sizeof(buffer), &written) == 0);
    CHECK_STR_EQ(buffer, "\\t\\r\\b\\f");

    /* Non-ASCII UTF-8 passes through unchanged. */
    CHECK(json_escape_string("caf\xc3\xa9", 5, buffer, sizeof(buffer), &written) == 0);
    CHECK(written == 5);
    CHECK_STR_EQ(buffer, "caf\xc3\xa9");

    /* Overflow is reported with the required length. */
    CHECK(json_escape_string("abcd", 4, buffer, 3, &written) == -1);
    CHECK(written == 4);

    /* Measurement only (dst == NULL). */
    CHECK(json_escape_string("\n", 1, NULL, 0, &written) == 0);
    CHECK(written == 2);

    /* Unescaping the standard short escapes. */
    CHECK(json_unescape_string("a\\nb", 4, buffer, sizeof(buffer), &written) == 0);
    CHECK(written == 3);
    CHECK_STR_EQ(buffer, "a\nb");

    CHECK(json_unescape_string("\\\\\\\"\\/", 6, buffer, sizeof(buffer), &written) == 0);
    CHECK(written == 3);
    CHECK_STR_EQ(buffer, "\\\"/");

    /* \u00e9 -> U+00E9 -> C3 A9. */
    CHECK(json_unescape_string("\\u00e9", 6, buffer, sizeof(buffer), &written) == 0);
    CHECK(written == 2);
    CHECK_BYTES_EQ(buffer, "\xc3\xa9", 2);

    /* Surrogate pair -> U+1F600 -> F0 9F 98 80. */
    CHECK(json_unescape_string("\\uD83D\\uDE00", 12, buffer, sizeof(buffer), &written) == 0);
    CHECK(written == 4);
    CHECK_BYTES_EQ(buffer, "\xf0\x9f\x98\x80", 4);

    /* Malformed escapes fail explicitly. */
    CHECK(json_unescape_string("\\uD83D", 6, buffer, sizeof(buffer), &written) == -1);
    CHECK(json_unescape_string("\\uDE00", 6, buffer, sizeof(buffer), &written) == -1);
    CHECK(json_unescape_string("\\uD83Dx", 7, buffer, sizeof(buffer), &written) == -1);
    CHECK(json_unescape_string("\\x", 2, buffer, sizeof(buffer), &written) == -1);
    CHECK(json_unescape_string("abcd", 4, buffer, 3, &written) == -1);
}

static void check_records(void) {
    event_log_t log;

    const char *done_record = "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"done\":true}\n";
    size_t done_length = strlen(done_record);
    for (size_t chunk = 1; chunk <= done_length; chunk++) {
        CHECK(run(done_record, done_length, chunk, &log) == 0);
        CHECK(log.count == 1);
        CHECK(log.type[0] == JSON_STREAM_EVENT_DONE);
        CHECK(log.length[0] == 0);
    }

    /* An error record is delivered, but the stream is terminally failed. */
    const char *error_record = "{\"error\":\"model not found\"}\n";
    size_t error_length = strlen(error_record);
    for (size_t chunk = 1; chunk <= error_length; chunk++) {
        json_stream_t stream;
        memset(&log, 0, sizeof(log));
        json_stream_init(&stream, collect, &log);
        CHECK(feed_all(&stream, error_record, error_length, chunk) == 0);
        CHECK(log.count == 1);
        CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);
        CHECK_STR_EQ(log.data[0], "model not found");
        CHECK(json_stream_failed(&stream));
        CHECK(json_stream_finish(&stream) == -1);
    }

    /* Two records in one stream: content then done. */
    const char *two = "{\"message\":{\"content\":\"Hi\"},\"done\":false}\n"
                      "{\"message\":{\"content\":\"\"},\"done\":true}\n";
    size_t two_length = strlen(two);
    for (size_t chunk = 1; chunk <= two_length; chunk++) {
        CHECK(run(two, two_length, chunk, &log) == 0);
        CHECK(log.count == 2);
        CHECK(log.type[0] == JSON_STREAM_EVENT_CONTENT);
        CHECK_STR_EQ(log.data[0], "Hi");
        CHECK(log.type[1] == JSON_STREAM_EVENT_DONE);
    }

    /* Escapes and a surrogate pair inside message.content. */
    const char *escaped = "{\"message\":{\"content\":\"a\\nb\\\"c\"},\"done\":false}\n";
    json_stream_t stream;
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(feed_all(&stream, escaped, strlen(escaped), 1) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_CONTENT);
    CHECK_STR_EQ(log.data[0], "a\nb\"c");
    CHECK(json_stream_finish(&stream) == -1); /* no done terminal */

    const char *unicode = "{\"message\":{\"content\":\"\\uD83D\\uDE00\"},\"done\":false}\n";
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(feed_all(&stream, unicode, strlen(unicode), 1) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_CONTENT);
    CHECK(log.length[0] == 4);
    CHECK_BYTES_EQ(log.data[0], "\xf0\x9f\x98\x80", 4);

    /* Unknown members are skipped, including nested objects and numbers. */
    const char *noise = "{\"model\":\"m\",\"created_at\":\"2026-01-01T00:00:00Z\","
                        "\"message\":{\"role\":\"assistant\",\"content\":\"ok\",\"extra\":{\"a\":[1,2,3]}},"
                        "\"done\":false,\"eval_count\":12}\n";
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(feed_all(&stream, noise, strlen(noise), 5) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_CONTENT);
    CHECK_STR_EQ(log.data[0], "ok");
}

static void check_malformed(void) {
    event_log_t log;

    /* Not an object. */
    CHECK(run("not json\n", 9, 1, &log) == -1);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);

    /* Unterminated object. */
    CHECK(run("{\"done\":true\n", 13, 1, &log) == -1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);

    /* Bad escape inside content. */
    CHECK(run("{\"message\":{\"content\":\"\\q\"},\"done\":false}\n", 42, 1, &log) == -1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);

    /* Blank line. */
    CHECK(run("\n", 1, 1, &log) == -1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);

    /* Trailing garbage after the object. */
    CHECK(run("{\"done\":true}xyz\n", 16, 1, &log) == -1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);
}

static void check_oversized_and_truncated(void) {
    event_log_t log;
    json_stream_t stream;

    /* A record longer than JSON_STREAM_RECORD_MAX fails on the newline and the
     * full literal payload (and its exact length) is delivered. */
    char big[3000];
    memset(big, 'x', sizeof(big));
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, big, sizeof(big)) == 0);
    CHECK(json_stream_feed(&stream, "\n", 1) == -1);
    CHECK(json_stream_failed(&stream));
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);
    CHECK_STR_EQ(log.data[0], "record exceeds maximum length");
    CHECK(log.length[0] == strlen("record exceeds maximum length"));

    /* A complete record with no trailing newline is still parsed. */
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, "{\"done\":true}", 13) == 0);
    CHECK(json_stream_finish(&stream) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_DONE);

    /* A truncated record fails at finish with the full literal payload. */
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, "{\"done\":tr", 10) == 0);
    CHECK(json_stream_finish(&stream) == -1);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);
    CHECK_STR_EQ(log.data[0], "truncated NDJSON record");
    CHECK(log.length[0] == strlen("truncated NDJSON record"));

    /* A malformed (non-JSON) record pins the malformed literal payload. */
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, "nope\n", 5) == -1);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);
    CHECK_STR_EQ(log.data[0], "malformed NDJSON record");
    CHECK(log.length[0] == strlen("malformed NDJSON record"));
}

static void check_terminal_semantics(void) {
    event_log_t log;
    json_stream_t stream;

    const char *content = "{\"message\":{\"content\":\"hi\"},\"done\":false}\n";
    const char *done = "{\"message\":{\"content\":\"\"},\"done\":true}\n";
    const char *error = "{\"error\":\"boom\"}\n";

    /* content + EOF: the content is delivered but the stream fails at finish. */
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(feed_all(&stream, content, strlen(content), 1) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_CONTENT);
    CHECK(json_stream_finish(&stream) == -1);

    /* DONE + CONTENT is invalid: exactly one terminal record must end the stream. */
    char combined[512];
    size_t done_len = strlen(done);
    size_t content_len = strlen(content);
    memcpy(combined, done, done_len);
    memcpy(combined + done_len, content, content_len);
    for (size_t chunk = 1; chunk <= done_len + content_len; chunk++) {
        memset(&log, 0, sizeof(log));
        json_stream_init(&stream, collect, &log);
        CHECK(feed_all(&stream, combined, done_len + content_len, chunk) == -1);
        CHECK(log.count == 2);
        CHECK(log.type[0] == JSON_STREAM_EVENT_DONE);
        CHECK(log.type[1] == JSON_STREAM_EVENT_ERROR);
        CHECK(json_stream_finish(&stream) == -1);
    }

    /* ERROR + CONTENT in a single feed: the later record is ignored. */
    memcpy(combined, error, strlen(error));
    memcpy(combined + strlen(error), content, content_len);
    size_t error_plus_content = strlen(error) + content_len;
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, combined, error_plus_content) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);
    CHECK_STR_EQ(log.data[0], "boom");
    CHECK(json_stream_finish(&stream) == -1);
    CHECK(json_stream_feed(&stream, done, done_len) == -1); /* latched failure */
    CHECK(log.count == 1);

    /* Duplicate terminal records fail after the first terminal event. */
    memcpy(combined, done, done_len);
    memcpy(combined + done_len, done, done_len);
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(feed_all(&stream, combined, done_len * 2u, 1) == -1);
    CHECK(log.count == 2);
    CHECK(log.type[0] == JSON_STREAM_EVENT_DONE);
    CHECK(log.type[1] == JSON_STREAM_EVENT_ERROR);
    CHECK(json_stream_finish(&stream) == -1);

    /* A later record supplied after DONE also latches failure. */
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, done, done_len) == 0);
    CHECK(json_stream_feed(&stream, content, content_len) == -1);
    CHECK(log.count == 2);
    CHECK(log.type[0] == JSON_STREAM_EVENT_DONE);
    CHECK(log.type[1] == JSON_STREAM_EVENT_ERROR);
    CHECK(json_stream_finish(&stream) == -1);
}

static void check_embedded_nul(void) {
    event_log_t log;
    json_stream_t stream;

    /* done\u0000x must not be accepted as the "done" key. */
    const char *spoof_key = "{\"done\\u0000x\":true}\n";
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, spoof_key, strlen(spoof_key)) == -1);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);

    /* The same for content and error keys. */
    const char *spoof_content_key = "{\"message\":{\"content\\u0000x\":\"y\"},\"done\":false}\n";
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, spoof_content_key, strlen(spoof_content_key)) == -1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);

    const char *spoof_error_key = "{\"error\\u0000x\":\"boom\"}\n";
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, spoof_error_key, strlen(spoof_error_key)) == -1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);

    /* An embedded NUL in content is rejected because it would alias a C string. */
    const char *nul_content = "{\"message\":{\"content\":\"a\\u0000b\"},\"done\":false}\n";
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, nul_content, strlen(nul_content)) == -1);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);

    /* A NUL in an unknown string value is rejected too. */
    const char *nul_value = "{\"model\":\"a\\u0000b\",\"done\":true}\n";
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, nul_value, strlen(nul_value)) == -1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);
}

static void check_terminal_state_is_latched_before_callback(void) {
    const char *done = "{\"done\":true}\n";
    json_stream_t stream;
    reentrant_log_t log;

    memset(&log, 0, sizeof(log));
    log.stream = &stream;
    log.nested_finish = -1;
    json_stream_init(&stream, collect_reentrant, &log);
    CHECK(json_stream_feed(&stream, done, strlen(done)) == 0);
    CHECK(log.nested_finish == 0);
    CHECK(log.events == 1u);
    CHECK(log.done_events == 1u);
    CHECK(json_stream_finish(&stream) == 0);
}

static void check_error_state_is_latched_before_callback(void) {
    const char *error = "{\"error\":\"boom\"}\n";
    json_stream_t stream;
    reentrant_log_t log;

    memset(&log, 0, sizeof(log));
    log.stream = &stream;
    log.nested_error_finish = 0;
    json_stream_init(&stream, collect_reentrant, &log);
    CHECK(json_stream_feed(&stream, error, strlen(error)) == 0);
    CHECK(log.nested_error_finish == -1);
    CHECK(log.events == 1u);
    CHECK(log.error_events == 1u);
    CHECK(json_stream_failed(&stream));
    CHECK(json_stream_finish(&stream) == -1);
}

static void check_null_preconditions(void) {
    event_log_t log;
    json_stream_t stream;
    char buffer[16];

    memset(&log, 0, sizeof(log));
    json_stream_init(NULL, collect, &log);
    json_stream_init(&stream, collect, &log);

    CHECK(json_stream_feed(NULL, "x", 1u) == -1);
    CHECK(json_stream_feed(&stream, NULL, 1u) == -1);
    CHECK(json_stream_feed(&stream, NULL, 0u) == 0);
    CHECK(json_stream_finish(NULL) == -1);
    CHECK(json_stream_failed(NULL) == false);

    size_t written = 0;
    CHECK(json_escape_string(NULL, 1u, buffer, sizeof(buffer), &written) == -1);
    CHECK(written == 0u);
    CHECK(json_unescape_string(NULL, 1u, buffer, sizeof(buffer), &written) == -1);
    CHECK(written == 0u);
}

void test_json_stream(void) {
    check_escapes();
    check_hello_at_every_split();
    check_records();
    check_malformed();
    check_oversized_and_truncated();
    check_terminal_semantics();
    check_terminal_state_is_latched_before_callback();
    check_error_state_is_latched_before_callback();
    check_embedded_nul();
    check_null_preconditions();
}
