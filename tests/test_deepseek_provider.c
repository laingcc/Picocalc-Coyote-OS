#include <string.h>

#include "ai/deepseek_provider.h"
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
    deepseek_provider_t provider;
    event_log_t log;
    memset(&log, 0, sizeof(log));
    deepseek_provider_init(&provider, collect, &log);

    deepseek_message_t messages[1] = {{"user", "hi"}};
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", messages, 1, 64, "be concise", 80) == 0);

    const char *expected = "{\"model\":\"deepseek-chat\",\"messages\":[{\"role\":\"system\",\"content\":\"be concise\"},"
                           "{\"role\":\"user\",\"content\":\"hi\"}],"
                           "\"stream\":true,\"max_tokens\":64,\"temperature\":0.80}";
    size_t expected_length = strlen(expected);
    CHECK(deepseek_provider_request_length(&provider) == expected_length);

    char assembled[512];
    size_t offset = 0;
    for (;;) {
        size_t got = deepseek_provider_read(&provider, offset, assembled + offset, 7);
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

    provider_request_t request = deepseek_provider_request(&provider);
    CHECK(request.length(request.context) == expected_length);
    char window[8];
    CHECK(request.read(request.context, 5, window, 4) == 4);
    CHECK_BYTES_EQ(window, expected + 5, 4);
    CHECK(request.read(request.context, expected_length, window, 4) == 0);
    CHECK(request.read(request.context, expected_length + 10, window, 4) == 0);

    CHECK_STR_EQ(deepseek_provider_method(), "POST");
    CHECK_STR_EQ(deepseek_provider_path(), "/chat/completions");
    CHECK_STR_EQ(deepseek_provider_content_type(), "application/json");
}

static void check_escaping_and_messages(void) {
    deepseek_provider_t provider;
    event_log_t log;
    memset(&log, 0, sizeof(log));
    deepseek_provider_init(&provider, collect, &log);

    deepseek_message_t single[1] = {{"user", "he\"llo\n"}};
    CHECK(deepseek_provider_build_request(&provider, "m", single, 1, 8, NULL, 80) == 0);
    const char *expected_single = "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"he\\\"llo\\n\"}],"
                                  "\"stream\":true,\"max_tokens\":8,\"temperature\":0.80}";
    CHECK(deepseek_provider_request_length(&provider) == strlen(expected_single));
    CHECK_BYTES_EQ(provider.body, expected_single, strlen(expected_single));

    deepseek_message_t pair[2] = {{"user", "hi"}, {"assistant", "hello"}};
    CHECK(deepseek_provider_build_request(&provider, "m", pair, 2, 16, NULL, 80) == 0);
    const char *expected_pair = "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"},"
                                "{\"role\":\"assistant\",\"content\":\"hello\"}],"
                                "\"stream\":true,\"max_tokens\":16,\"temperature\":0.80}";
    CHECK(deepseek_provider_request_length(&provider) == strlen(expected_pair));
    CHECK_BYTES_EQ(provider.body, expected_pair, strlen(expected_pair));

    CHECK(deepseek_provider_build_request(&provider, "m", pair, 1, 0, NULL, 0) == 0);
    CHECK_STR_EQ(provider.body,
                 "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                 "\"stream\":true,\"max_tokens\":0,\"temperature\":0.00}");
    CHECK(deepseek_provider_build_request(&provider, "m", pair, 1, 1000000, NULL, 200) == 0);
    CHECK_STR_EQ(provider.body,
                 "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                 "\"stream\":true,\"max_tokens\":1000000,\"temperature\":2.00}");
}

static void check_rejections(void) {
    deepseek_provider_t provider;
    event_log_t log;
    memset(&log, 0, sizeof(log));
    deepseek_provider_init(&provider, collect, &log);

    deepseek_message_t one[1] = {{"user", "hi"}};
    deepseek_message_t many[9];
    for (size_t i = 0; i < 9; i++) {
        many[i].role = "user";
        many[i].content = "x";
    }

    CHECK(deepseek_provider_build_request(&provider, "", one, 1, 8, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "m", many, 9, 8, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "m", one, 1, -1, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "m", one, 1, 8, NULL, -1) == -1);
    CHECK(deepseek_provider_build_request(&provider, "m", one, 1, 8, NULL, 201) == -1);

    char big[3000];
    memset(big, 'a', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = '\0';
    deepseek_message_t huge[1] = {{"user", big}};
    CHECK(deepseek_provider_build_request(&provider, "m", huge, 1, 8, NULL, 80) == -1);
}

static void check_feed(void) {
    deepseek_provider_t provider;
    event_log_t log;
    memset(&log, 0, sizeof(log));
    deepseek_provider_init(&provider, collect, &log);

    CHECK(deepseek_provider_feed(&provider, "x", 1) == -1);

    deepseek_message_t one[1] = {{"user", "hi"}};
    CHECK(deepseek_provider_build_request(&provider, "m", one, 1, 8, NULL, 80) == 0);

    const char *response = "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n"
                           "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n"
                           "data: [DONE]\n";
    size_t length = strlen(response);

    for (size_t chunk = 1; chunk <= length; chunk++) {
        memset(&log, 0, sizeof(log));
        deepseek_provider_init(&provider, collect, &log);
        CHECK(deepseek_provider_build_request(&provider, "m", one, 1, 8, NULL, 80) == 0);
        size_t offset = 0;
        int rc = 0;
        while (offset < length) {
            size_t take = length - offset < chunk ? length - offset : chunk;
            if (deepseek_provider_feed(&provider, response + offset, take) != 0) {
                rc = -1;
                break;
            }
            offset += take;
        }
        if (rc == 0) {
            rc = deepseek_provider_finish(&provider);
        }
        CHECK(rc == 0);
        CHECK(log.count == 3);
        CHECK(log.type[0] == PROVIDER_EVENT_CONTENT);
        CHECK_STR_EQ(log.data[0], "Hel");
        CHECK(log.type[1] == PROVIDER_EVENT_CONTENT);
        CHECK_STR_EQ(log.data[1], "lo");
        CHECK(log.type[2] == PROVIDER_EVENT_DONE);
        CHECK(deepseek_provider_is_done(&provider));
        CHECK(deepseek_provider_failed(&provider) == false);
    }

    /* An error record flows through as an ERROR event and latches failure. */
    memset(&log, 0, sizeof(log));
    deepseek_provider_init(&provider, collect, &log);
    CHECK(deepseek_provider_build_request(&provider, "m", one, 1, 8, NULL, 80) == 0);
    const char *error = "data: {\"error\":{\"message\":\"Invalid API key\"}}\n";
    CHECK(deepseek_provider_feed(&provider, error, strlen(error)) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == PROVIDER_EVENT_ERROR);
    CHECK_STR_EQ(log.data[0], "Invalid API key");
    CHECK(deepseek_provider_failed(&provider));
    CHECK(deepseek_provider_feed(&provider, error, strlen(error)) == -1);
}

static void check_finish_reason_followed_by_sse_done(void) {
    deepseek_provider_t provider;
    event_log_t log;
    deepseek_message_t one[1] = {{"user", "hi"}};

    memset(&log, 0, sizeof(log));
    deepseek_provider_init(&provider, collect, &log);
    CHECK(deepseek_provider_build_request(&provider, "m", one, 1, 8, NULL, 80) == 0);

    /* Real DeepSeek stream sending finish_reason: "stop" followed by data: [DONE] */
    const char *chunk1 = "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"},\"finish_reason\":null}]}\n";
    const char *chunk2 = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n";
    const char *chunk3 = "data: [DONE]\n";

    CHECK(deepseek_provider_feed(&provider, chunk1, strlen(chunk1)) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == PROVIDER_EVENT_CONTENT);
    CHECK_STR_EQ(log.data[0], "Hello");

    CHECK(deepseek_provider_feed(&provider, chunk2, strlen(chunk2)) == 0);
    CHECK(log.count == 2);
    CHECK(log.type[1] == PROVIDER_EVENT_DONE);
    CHECK(deepseek_provider_is_done(&provider));

    /* Chunk 3 sending data: [DONE] after done is harmlessly consumed */
    CHECK(deepseek_provider_feed(&provider, chunk3, strlen(chunk3)) == 0);
    CHECK(log.count == 2); /* no extra event */
    CHECK(deepseek_provider_is_done(&provider));
    CHECK(deepseek_provider_failed(&provider) == false);

    CHECK(deepseek_provider_finish(&provider) == 0);
}

static void check_terminal_semantics(void) {
    deepseek_provider_t provider;
    event_log_t log;
    deepseek_message_t one[1] = {{"user", "hi"}};

    const char *content = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n";
    const char *done = "data: [DONE]\n";

    memset(&log, 0, sizeof(log));
    deepseek_provider_init(&provider, collect, &log);
    CHECK(deepseek_provider_build_request(&provider, "m", one, 1, 8, NULL, 80) == 0);
    CHECK(deepseek_provider_feed(&provider, content, strlen(content)) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == PROVIDER_EVENT_CONTENT);
    CHECK(deepseek_provider_finish(&provider) == -1);
    CHECK(deepseek_provider_is_done(&provider) == false);
    CHECK(deepseek_provider_failed(&provider));

    char combined[512];
    size_t done_len = strlen(done);
    size_t content_len = strlen(content);
    memcpy(combined, done, done_len);
    memcpy(combined + done_len, content, content_len);
    memset(&log, 0, sizeof(log));
    deepseek_provider_init(&provider, collect, &log);
    CHECK(deepseek_provider_build_request(&provider, "m", one, 1, 8, NULL, 80) == 0);
    CHECK(deepseek_provider_feed(&provider, combined, done_len + content_len) == -1);
    CHECK(log.count == 2);
    CHECK(log.type[0] == PROVIDER_EVENT_DONE);
    CHECK(log.type[1] == PROVIDER_EVENT_ERROR);
    CHECK(deepseek_provider_is_done(&provider));
    CHECK(deepseek_provider_failed(&provider));

    memset(&log, 0, sizeof(log));
    deepseek_provider_init(&provider, collect, &log);
    CHECK(deepseek_provider_build_request(&provider, "m", one, 1, 8, NULL, 80) == 0);
    CHECK(deepseek_provider_feed(&provider, done, done_len) == 0);
    CHECK(log.count == 1);
    CHECK(log.type[0] == PROVIDER_EVENT_DONE);
    CHECK(deepseek_provider_feed(&provider, NULL, 0u) == 0);
    CHECK(deepseek_provider_feed(&provider, content, content_len) == -1);
    CHECK(log.count == 2);
    CHECK(log.type[1] == PROVIDER_EVENT_ERROR);
    CHECK(deepseek_provider_is_done(&provider));
    CHECK(deepseek_provider_failed(&provider));
    CHECK(deepseek_provider_finish(&provider) == -1);
}

static void check_null_preconditions(void) {
    deepseek_provider_t provider;
    event_log_t log;
    char buffer[8];
    deepseek_message_t one[1] = {{"user", "hi"}};

    deepseek_provider_init(NULL, collect, &log);
    CHECK(deepseek_provider_feed(NULL, "x", 1u) == -1);
    CHECK(deepseek_provider_finish(NULL) == -1);
    CHECK(deepseek_provider_is_done(NULL) == false);
    CHECK(deepseek_provider_failed(NULL) == false);

    memset(&log, 0, sizeof(log));
    deepseek_provider_init(&provider, collect, &log);
    CHECK(deepseek_provider_build_request(&provider, "m", one, 1, 8, NULL, 80) == 0);

    CHECK(deepseek_provider_build_request(&provider, "m", NULL, 1, 8, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "m", NULL, 0, 8, NULL, 80) == 0);

    CHECK(deepseek_provider_read(&provider, 0u, NULL, 4u) == 0u);
    CHECK(deepseek_provider_read(NULL, 0u, buffer, 4u) == 0u);

    CHECK(deepseek_provider_feed(&provider, NULL, 1u) == -1);
    CHECK(deepseek_provider_feed(&provider, NULL, 0u) == 0);
}

void test_deepseek_provider(void) {
    check_single_message_request();
    check_escaping_and_messages();
    check_rejections();
    check_feed();
    check_finish_reason_followed_by_sse_done();
    check_terminal_semantics();
    check_null_preconditions();
}
