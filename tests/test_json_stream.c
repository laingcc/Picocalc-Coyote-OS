#include <string.h>

#include "ai/json_stream.h"
#include "test_util.h"

typedef struct {
    int count;
    int type[16];
    char data[16][256];
    size_t length[16];
} event_log_t;

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

/* Feed the whole input in fixed-size chunks, then finish. */
static int run(const char *input, size_t length, size_t chunk, event_log_t *log) {
    memset(log, 0, sizeof(*log));
    json_stream_t stream;
    json_stream_init(&stream, collect, log);
    size_t offset = 0;
    while (offset < length) {
        size_t take = length - offset < chunk ? length - offset : chunk;
        if (json_stream_feed(&stream, input + offset, take) != 0) {
            return -1;
        }
        offset += take;
    }
    return json_stream_finish(&stream);
}

static void check_hello_at_every_split(void) {
    const char *input = "{\"message\":{\"role\":\"assistant\",\"content\":\"Hello\"},\"done\":false}\n";
    size_t length = strlen(input);
    for (size_t chunk = 1; chunk <= length; chunk++) {
        event_log_t log;
        CHECK(run(input, length, chunk, &log) == 0);
        CHECK(log.count == 1);
        CHECK(log.type[0] == JSON_STREAM_EVENT_CONTENT);
        CHECK(log.length[0] == 5);
        CHECK_STR_EQ(log.data[0], "Hello");
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

    const char *error_record = "{\"error\":\"model not found\"}\n";
    size_t error_length = strlen(error_record);
    for (size_t chunk = 1; chunk <= error_length; chunk++) {
        CHECK(run(error_record, error_length, chunk, &log) == 0);
        CHECK(log.count == 1);
        CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);
        CHECK_STR_EQ(log.data[0], "model not found");
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
    CHECK(run(escaped, strlen(escaped), 1, &log) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_CONTENT);
    CHECK_STR_EQ(log.data[0], "a\nb\"c");

    const char *unicode = "{\"message\":{\"content\":\"\\uD83D\\uDE00\"},\"done\":false}\n";
    CHECK(run(unicode, strlen(unicode), 1, &log) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_CONTENT);
    CHECK(log.length[0] == 4);
    CHECK_BYTES_EQ(log.data[0], "\xf0\x9f\x98\x80", 4);

    /* Unknown members are skipped, including nested objects and numbers. */
    const char *noise = "{\"model\":\"m\",\"created_at\":\"2026-01-01T00:00:00Z\","
                        "\"message\":{\"role\":\"assistant\",\"content\":\"ok\",\"extra\":{\"a\":[1,2,3]}},"
                        "\"done\":false,\"eval_count\":12}\n";
    CHECK(run(noise, strlen(noise), 5, &log) == 0);
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

    /* A record longer than JSON_STREAM_RECORD_MAX fails on the newline. */
    char big[3000];
    memset(big, 'x', sizeof(big));
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, big, sizeof(big)) == 0);
    CHECK(json_stream_feed(&stream, "\n", 1) == -1);
    CHECK(json_stream_failed(&stream));
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);

    /* A complete record with no trailing newline is still parsed. */
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, "{\"done\":true}", 13) == 0);
    CHECK(json_stream_finish(&stream) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_DONE);

    /* A truncated record fails at finish. */
    memset(&log, 0, sizeof(log));
    json_stream_init(&stream, collect, &log);
    CHECK(json_stream_feed(&stream, "{\"done\":tr", 10) == 0);
    CHECK(json_stream_finish(&stream) == -1);
    CHECK(log.count == 1);
    CHECK(log.type[0] == JSON_STREAM_EVENT_ERROR);
}

void test_json_stream(void) {
    check_escapes();
    check_hello_at_every_split();
    check_records();
    check_malformed();
    check_oversized_and_truncated();
}
