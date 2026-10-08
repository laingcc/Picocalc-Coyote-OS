#include <string.h>

#include "ai/ollama_provider.h"
#include "test_util.h"

typedef struct {
    int count;
    int type[16];
    char data[16][256];
    size_t length[16];
} event_log_t;

static void collect(void *context, const provider_event_t *event) {
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

static void check_single_message_request(void) {
    ollama_provider_t provider;
    event_log_t log;
    memset(&log, 0, sizeof(log));
    ollama_provider_init(&provider, collect, &log);

    ollama_message_t messages[1] = {{"user", "hi"}};
    CHECK(ollama_provider_build_request(&provider, "llama3.2", messages, 1, 64) == 0);

    const char *expected = "{\"model\":\"llama3.2\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                           "\"stream\":true,\"options\":{\"num_predict\":64}}";
    size_t expected_length = strlen(expected);
    CHECK(ollama_provider_request_length(&provider) == expected_length);

    /* Read the request in 7-byte windows by offset. */
    char assembled[512];
    size_t offset = 0;
    for (;;) {
        size_t got = ollama_provider_read(&provider, offset, assembled + offset, 7);
        if (got == 0) {
            break;
        }
        offset += got;
        if (offset >= sizeof(assembled)) {
            break;
        }
    }
    CHECK(offset == expected_length);
    CHECK_BYTES_EQ(assembled, expected, expected_length);

    /* Offset semantics. */
    provider_request_t request = ollama_provider_request(&provider);
    CHECK(request.length(request.context) == expected_length);
    char window[8];
    CHECK(request.read(request.context, 5, window, 4) == 4);
    CHECK_BYTES_EQ(window, expected + 5, 4);
    CHECK(request.read(request.context, expected_length, window, 4) == 0);
    CHECK(request.read(request.context, expected_length + 10, window, 4) == 0);

    /* Provider metadata. */
    CHECK_STR_EQ(ollama_provider_method(), "POST");
    CHECK_STR_EQ(ollama_provider_path(), "/api/chat");
    CHECK_STR_EQ(ollama_provider_content_type(), "application/json");
}

static void check_escaping_and_messages(void) {
    ollama_provider_t provider;
    event_log_t log;
    memset(&log, 0, sizeof(log));
    ollama_provider_init(&provider, collect, &log);

    /* Content with a quote and a newline is escaped in the payload. */
    ollama_message_t single[1] = {{"user", "he\"llo\n"}};
    CHECK(ollama_provider_build_request(&provider, "m", single, 1, 8) == 0);
    const char *expected_single = "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"he\\\"llo\\n\"}],"
                                  "\"stream\":true,\"options\":{\"num_predict\":8}}";
    CHECK(ollama_provider_request_length(&provider) == strlen(expected_single));
    CHECK_BYTES_EQ(provider.body, expected_single, strlen(expected_single));

    /* Two messages, alternating roles. */
    ollama_message_t pair[2] = {{"user", "hi"}, {"assistant", "hello"}};
    CHECK(ollama_provider_build_request(&provider, "m", pair, 2, 16) == 0);
    const char *expected_pair = "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"},"
                                "{\"role\":\"assistant\",\"content\":\"hello\"}],"
                                "\"stream\":true,\"options\":{\"num_predict\":16}}";
    CHECK(ollama_provider_request_length(&provider) == strlen(expected_pair));
    CHECK_BYTES_EQ(provider.body, expected_pair, strlen(expected_pair));

    /* num_predict formatting. */
    CHECK(ollama_provider_build_request(&provider, "m", pair, 1, 0) == 0);
    CHECK_STR_EQ(provider.body,
                 "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                 "\"stream\":true,\"options\":{\"num_predict\":0}}");
    CHECK(ollama_provider_build_request(&provider, "m", pair, 1, 1000000) == 0);
    CHECK_STR_EQ(provider.body,
                 "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                 "\"stream\":true,\"options\":{\"num_predict\":1000000}}");
}

static void check_rejections(void) {
    ollama_provider_t provider;
    event_log_t log;
    memset(&log, 0, sizeof(log));
    ollama_provider_init(&provider, collect, &log);

    ollama_message_t one[1] = {{"user", "hi"}};
    ollama_message_t many[9];
    for (size_t i = 0; i < 9; i++) {
        many[i].role = "user";
        many[i].content = "x";
    }

    CHECK(ollama_provider_build_request(&provider, "", one, 1, 8) == -1);
    CHECK(ollama_provider_build_request(&provider, "m", many, 9, 8) == -1);
    CHECK(ollama_provider_build_request(&provider, "m", one, 1, -1) == -1);

    /* A message too large for the fixed request buffer is rejected. */
    char big[3000];
    memset(big, 'a', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = '\0';
    ollama_message_t huge[1] = {{"user", big}};
    CHECK(ollama_provider_build_request(&provider, "m", huge, 1, 8) == -1);
}

static void check_feed(void) {
    ollama_provider_t provider;
    event_log_t log;
    memset(&log, 0, sizeof(log));
    ollama_provider_init(&provider, collect, &log);

    /* Feeding before a request is built is rejected. */
    CHECK(ollama_provider_feed(&provider, "x", 1) == -1);

    ollama_message_t one[1] = {{"user", "hi"}};
    CHECK(ollama_provider_build_request(&provider, "m", one, 1, 8) == 0);

    const char *response = "{\"message\":{\"role\":\"assistant\",\"content\":\"Hel\"},\"done\":false}\n"
                           "{\"message\":{\"role\":\"assistant\",\"content\":\"lo\"},\"done\":false}\n"
                           "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"done\":true}\n";
    size_t length = strlen(response);

    /* Split at every byte boundary. */
    for (size_t chunk = 1; chunk <= length; chunk++) {
        memset(&log, 0, sizeof(log));
        ollama_provider_init(&provider, collect, &log);
        CHECK(ollama_provider_build_request(&provider, "m", one, 1, 8) == 0);
        size_t offset = 0;
        int rc = 0;
        while (offset < length) {
            size_t take = length - offset < chunk ? length - offset : chunk;
            if (ollama_provider_feed(&provider, response + offset, take) != 0) {
                rc = -1;
                break;
            }
            offset += take;
        }
        if (rc == 0) {
            rc = ollama_provider_finish(&provider);
        }
        CHECK(rc == 0);
        CHECK(log.count == 3);
        CHECK(log.type[0] == PROVIDER_EVENT_CONTENT);
        CHECK_STR_EQ(log.data[0], "Hel");
        CHECK(log.type[1] == PROVIDER_EVENT_CONTENT);
        CHECK_STR_EQ(log.data[1], "lo");
        CHECK(log.type[2] == PROVIDER_EVENT_DONE);
        CHECK(ollama_provider_is_done(&provider));
        CHECK(ollama_provider_failed(&provider) == false);
    }

    /* An error record flows through as an ERROR event and latches failure. */
    memset(&log, 0, sizeof(log));
    ollama_provider_init(&provider, collect, &log);
    CHECK(ollama_provider_build_request(&provider, "m", one, 1, 8) == 0);
    const char *error = "{\"error\":\"boom\"}\n";
    CHECK(ollama_provider_feed(&provider, error, strlen(error)) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == PROVIDER_EVENT_ERROR);
    CHECK_STR_EQ(log.data[0], "boom");
    CHECK(ollama_provider_failed(&provider));
    CHECK(ollama_provider_feed(&provider, error, strlen(error)) == -1);
}

static void check_terminal_semantics(void) {
    ollama_provider_t provider;
    event_log_t log;
    ollama_message_t one[1] = {{"user", "hi"}};

    const char *content = "{\"message\":{\"content\":\"hi\"},\"done\":false}\n";
    const char *done = "{\"message\":{\"content\":\"\"},\"done\":true}\n";

    /* content + EOF: the stream did not reach a done terminal. */
    memset(&log, 0, sizeof(log));
    ollama_provider_init(&provider, collect, &log);
    CHECK(ollama_provider_build_request(&provider, "m", one, 1, 8) == 0);
    CHECK(ollama_provider_feed(&provider, content, strlen(content)) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == PROVIDER_EVENT_CONTENT);
    CHECK(ollama_provider_finish(&provider) == -1);
    CHECK(ollama_provider_is_done(&provider) == false);

    /* DONE + CONTENT in one feed is a terminal protocol failure. */
    char combined[512];
    size_t done_len = strlen(done);
    size_t content_len = strlen(content);
    memcpy(combined, done, done_len);
    memcpy(combined + done_len, content, content_len);
    memset(&log, 0, sizeof(log));
    ollama_provider_init(&provider, collect, &log);
    CHECK(ollama_provider_build_request(&provider, "m", one, 1, 8) == 0);
    CHECK(ollama_provider_feed(&provider, combined, done_len + content_len) == -1);
    CHECK(log.count == 2);
    CHECK(log.type[0] == PROVIDER_EVENT_DONE);
    CHECK(log.type[1] == PROVIDER_EVENT_ERROR);
    CHECK(ollama_provider_is_done(&provider));
    CHECK(ollama_provider_failed(&provider));
    CHECK(ollama_provider_feed(&provider, content, content_len) == -1); /* latched */
    CHECK(log.count == 2);
    CHECK(ollama_provider_finish(&provider) == -1);

    /* ERROR + CONTENT in one feed: the later record is ignored and finish
     * fails. */
    const char *error = "{\"error\":\"boom\"}\n";
    memcpy(combined, error, strlen(error));
    memcpy(combined + strlen(error), content, content_len);
    memset(&log, 0, sizeof(log));
    ollama_provider_init(&provider, collect, &log);
    CHECK(ollama_provider_build_request(&provider, "m", one, 1, 8) == 0);
    CHECK(ollama_provider_feed(&provider, combined, strlen(error) + content_len) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == PROVIDER_EVENT_ERROR);
    CHECK_STR_EQ(log.data[0], "boom");
    CHECK(ollama_provider_failed(&provider));
    CHECK(ollama_provider_finish(&provider) == -1);

    /* Duplicate terminal records emit DONE, then fail the stream. */
    memcpy(combined, done, done_len);
    memcpy(combined + done_len, done, done_len);
    memset(&log, 0, sizeof(log));
    ollama_provider_init(&provider, collect, &log);
    CHECK(ollama_provider_build_request(&provider, "m", one, 1, 8) == 0);
    CHECK(ollama_provider_feed(&provider, combined, done_len * 2u) == -1);
    CHECK(log.count == 2);
    CHECK(log.type[0] == PROVIDER_EVENT_DONE);
    CHECK(log.type[1] == PROVIDER_EVENT_ERROR);
    CHECK(ollama_provider_failed(&provider));
    CHECK(ollama_provider_finish(&provider) == -1);
}

static void check_null_preconditions(void) {
    ollama_provider_t provider;
    event_log_t log;
    char buffer[8];
    ollama_message_t one[1] = {{"user", "hi"}};

    /* A NULL provider is tolerated. */
    ollama_provider_init(NULL, collect, &log);
    CHECK(ollama_provider_feed(NULL, "x", 1u) == -1);
    CHECK(ollama_provider_finish(NULL) == -1);
    CHECK(ollama_provider_is_done(NULL) == false);
    CHECK(ollama_provider_failed(NULL) == false);

    memset(&log, 0, sizeof(log));
    ollama_provider_init(&provider, collect, &log);
    CHECK(ollama_provider_build_request(&provider, "m", one, 1, 8) == 0);

    /* message_count > 0 with a NULL array is rejected. */
    CHECK(ollama_provider_build_request(&provider, "m", NULL, 1, 8) == -1);
    /* message_count == 0 with a NULL array is allowed. */
    CHECK(ollama_provider_build_request(&provider, "m", NULL, 0, 8) == 0);

    /* Reading with a non-zero capacity and a NULL destination is rejected. */
    CHECK(ollama_provider_read(&provider, 0u, NULL, 4u) == 0u);
    CHECK(ollama_provider_read(NULL, 0u, buffer, 4u) == 0u);

    /* Feeding a NULL buffer with a non-zero length is rejected. */
    CHECK(ollama_provider_feed(&provider, NULL, 1u) == -1);
    CHECK(ollama_provider_feed(&provider, NULL, 0u) == 0);
}

void test_ollama_provider(void) {
    check_single_message_request();
    check_escaping_and_messages();
    check_rejections();
    check_feed();
    check_terminal_semantics();
    check_null_preconditions();
}
