#include <stdio.h>
#include <string.h>

#include "ai/deepseek_provider.h"
#include "ai/ollama_provider.h"
#include "test_util.h"

/* Static: the provider is too large for a stack local. */
static deepseek_provider_t provider;
static ollama_provider_t ollama;

/* ---- Event recorder ------------------------------------------------------ */

typedef struct {
    char content[DEEPSEEK_LINE_MAX + 1u];
    size_t content_length;
    int content_events;
    int done_events;
    int error_events;
    char error[128];
} recorder_t;

static recorder_t seen;

static void record(void *context, const provider_event_t *event) {
    recorder_t *r = (recorder_t *)context;
    switch (event->type) {
        case PROVIDER_EVENT_CONTENT:
            r->content_events++;
            if (event->length >= sizeof(r->content) - r->content_length) {
                break; /* counted but not kept; the length check then fails */
            }
            memcpy(r->content + r->content_length, event->data, event->length);
            r->content_length += event->length;
            r->content[r->content_length] = '\0';
            break;
        case PROVIDER_EVENT_DONE:
            r->done_events++;
            break;
        case PROVIDER_EVENT_ERROR:
            r->error_events++;
            snprintf(r->error, sizeof(r->error), "%.*s", (int)event->length, event->data);
            break;
    }
}

static const ollama_message_t hello[] = {{"user", "hi"}};

static void begin(void) {
    memset(&seen, 0, sizeof(seen));
    deepseek_provider_init(&provider, record, &seen);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", hello, 1u, 64, NULL, 80) == 0);
}

/* Feed text in pieces of chunk bytes, then finish.  Returns finish's result,
 * or -1 as soon as a feed fails. */
static int run(const char *text, size_t length, size_t chunk) {
    size_t offset = 0u;
    begin();
    while (offset < length) {
        size_t take = length - offset < chunk ? length - offset : chunk;
        if (deepseek_provider_feed(&provider, text + offset, take) != 0) {
            return -1;
        }
        offset += take;
    }
    return deepseek_provider_finish(&provider);
}

static int feed_text(const char *text) {
    return deepseek_provider_feed(&provider, text, strlen(text));
}

static int run_text(const char *text) {
    return run(text, strlen(text), strlen(text) + 1u);
}

#define CHUNK(content_json) "data: {\"id\":\"1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":" content_json ",\"logprobs\":null,\"finish_reason\":null}]}\n\n"

/* ---- Request ------------------------------------------------------------- */

static void test_endpoint_literals(void) {
    /* Spelled out here, not taken from the code under test. */
    CHECK_STR_EQ(deepseek_provider_method(), "POST");
    CHECK_STR_EQ(deepseek_provider_path(), "/chat/completions");
    CHECK_STR_EQ(deepseek_provider_content_type(), "application/json");
    CHECK_STR_EQ(AI_CONFIG_DEEPSEEK_HOST, "api.deepseek.com");
    CHECK(AI_CONFIG_DEEPSEEK_PORT == 443u);
    CHECK_STR_EQ(AI_CONFIG_DEEPSEEK_MODEL_DEFAULT, "deepseek-chat");
    CHECK_STR_EQ(AI_CONFIG_DEEPSEEK_MODEL_REASONER, "deepseek-reasoner");
}

static void test_request_body(void) {
    static const ollama_message_t messages[] = {
        {"user", "say \"hi\"\n"}, {"assistant", "hi"}, {"user", "tab\there \\ done"}};
    char buffer[DEEPSEEK_REQUEST_MAX + 1u];
    provider_request_t view;
    size_t length, i;

    deepseek_provider_init(&provider, NULL, NULL);
    CHECK(deepseek_provider_request_length(&provider) == 0u);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", messages, 3u, 384, "Be brief.", 80) == 0);
    CHECK_STR_EQ(provider.body,
                 "{\"model\":\"deepseek-chat\",\"messages\":["
                 "{\"role\":\"system\",\"content\":\"Be brief.\"},"
                 "{\"role\":\"user\",\"content\":\"say \\\"hi\\\"\\n\"},"
                 "{\"role\":\"assistant\",\"content\":\"hi\"},"
                 "{\"role\":\"user\",\"content\":\"tab\\there \\\\ done\"}"
                 "],\"stream\":true,\"max_tokens\":384,\"temperature\":0.80}");
    length = deepseek_provider_request_length(&provider);
    CHECK(length == strlen(provider.body));

    /* The offset-readable view returns the same bytes in any window size. */
    view = deepseek_provider_request(&provider);
    CHECK(view.length(view.context) == length);
    for (i = 0u; i < length;) {
        size_t got = view.read(view.context, i, buffer + i, 7u);
        CHECK(got > 0u && got <= 7u);
        if (got == 0u) break;
        i += got;
    }
    CHECK(i == length);
    CHECK_BYTES_EQ(buffer, provider.body, length);
    CHECK(view.read(view.context, length, buffer, 7u) == 0u);
    CHECK(deepseek_provider_read(&provider, 0u, NULL, 4u) == 0u);

    /* No system prompt, the other model, and the temperature extremes. */
    CHECK(deepseek_provider_build_request(&provider, "deepseek-reasoner", hello, 1u, 1, "", 0) == 0);
    CHECK_STR_EQ(provider.body, "{\"model\":\"deepseek-reasoner\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                                "\"stream\":true,\"max_tokens\":1,\"temperature\":0.00}");
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", hello, 1u, 4096, NULL, 200) == 0);
    CHECK(strstr(provider.body, "\"max_tokens\":4096,\"temperature\":2.00}") != NULL);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", hello, 1u, 10, NULL, 105) == 0);
    CHECK(strstr(provider.body, "\"temperature\":1.05}") != NULL);
}

static void test_request_rejections(void) {
    static const ollama_message_t bad_role[] = {{"tool", "x"}};
    static const ollama_message_t injected_role[] = {{"user\",\"x\":\"", "x"}};
    static const ollama_message_t no_role[] = {{NULL, "x"}};
    static ollama_message_t many[OLLAMA_MAX_MESSAGES + 1u];
    static char huge[DEEPSEEK_REQUEST_MAX + 1u];
    ollama_message_t big;
    size_t i;

    deepseek_provider_init(&provider, NULL, NULL);
    /* Only the two DeepSeek models are sent, whatever else is asked for. */
    CHECK(deepseek_provider_build_request(&provider, "gpt-4o", hello, 1u, 64, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat ", hello, 1u, 64, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "Deepseek-Chat", hello, 1u, 64, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "llama3", hello, 1u, 64, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "", hello, 1u, 64, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, NULL, hello, 1u, 64, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(NULL, "deepseek-chat", hello, 1u, 64, NULL, 80) == -1);

    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", bad_role, 1u, 64, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", injected_role, 1u, 64, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", no_role, 1u, 64, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", NULL, 1u, 64, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", hello, 1u, 0, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", hello, 1u, -5, NULL, 80) == -1);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", hello, 1u, 64, NULL, -1) == -1);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", hello, 1u, 64, NULL, 201) == -1);

    for (i = 0u; i < OLLAMA_MAX_MESSAGES + 1u; i++) {
        many[i].role = "user";
        many[i].content = "x";
    }
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", many, OLLAMA_MAX_MESSAGES, 64, NULL, 80) == 0);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", many, OLLAMA_MAX_MESSAGES + 1u, 64, NULL, 80) == -1);

    memset(huge, 'a', DEEPSEEK_REQUEST_MAX);
    huge[DEEPSEEK_REQUEST_MAX] = '\0';
    big.role = "user";
    big.content = huge;
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", &big, 1u, 64, NULL, 80) == -1);
    /* A failed build leaves nothing to send and refuses reply bytes. */
    CHECK(deepseek_provider_request_length(&provider) == 0u);
    CHECK(deepseek_provider_read(&provider, 0u, huge, 8u) == 0u);
    CHECK(deepseek_provider_feed(&provider, "data: [DONE]\n", 13u) == -1);
    CHECK(deepseek_provider_finish(&provider) == -1);
}

/* The chat model sizes requests with the Ollama builder; that only stays a
 * safe bound while the DeepSeek body is never the longer of the two. */
static void test_request_never_longer_than_ollama(void) {
    static const ollama_message_t messages[] = {{"user", "one \"two\"\n"}, {"assistant", "three"}, {"user", "4"}};
    static const int predict[] = {1, 9, 10, 384, 4096};
    size_t count, p;

    deepseek_provider_init(&provider, NULL, NULL);
    ollama_provider_init(&ollama, NULL, NULL);
    for (count = 0u; count <= 3u; count++) {
        for (p = 0u; p < sizeof(predict) / sizeof(predict[0]); p++) {
            CHECK(deepseek_provider_build_request(&provider, "deepseek-reasoner", messages, count, predict[p], "sys", 200) == 0);
            CHECK(ollama_provider_build_request(&ollama, "deepseek-reasoner", messages, count, predict[p], "sys", 200) == 0);
            CHECK(deepseek_provider_request_length(&provider) <= ollama_provider_request_length(&ollama));
        }
    }
}

/* ---- Reply: success ------------------------------------------------------ */

static const char full_reply[] =
    ": keep-alive\n"
    "\n"
    CHUNK("{\"role\":\"assistant\",\"content\":\"\"}")
    CHUNK("{\"content\":\"Hel\"}")
    ": keep-alive\r\n"
    "\r\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"lo, \\\"w\\u00f6rld\\\"\\n\"}}]}\r\n"
    "\r\n"
    "data:{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"!\"},\"finish_reason\":null}]}\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
    "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":3,\"total_tokens\":8}}\n"
    "\n"
    "data: [DONE]\n"
    "\n";

static void test_reply_success(void) {
    static const size_t chunks[] = {1u, 2u, 3u, 7u, 64u, 4096u};
    size_t c;

    for (c = 0u; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        CHECK(run(full_reply, sizeof(full_reply) - 1u, chunks[c]) == 0);
        CHECK_STR_EQ(seen.content, "Hello, \"w\xc3\xb6rld\"\n!");
        CHECK(seen.content_events == 3);
        CHECK(seen.done_events == 1);
        CHECK(seen.error_events == 0);
        CHECK(deepseek_provider_is_done(&provider));
        CHECK(!deepseek_provider_failed(&provider));
    }

    /* [DONE] on a final line with no line ending, and with CRLF. */
    CHECK(run_text(CHUNK("{\"content\":\"a\"}") "data: [DONE]") == 0);
    CHECK(seen.done_events == 1);
    CHECK(run_text(CHUNK("{\"content\":\"a\"}") "data: [DONE]\r\n\r\n") == 0);
    /* Whitespace is all that may follow it. */
    CHECK(run_text("data: [DONE]\n\n \t\r\n\n   ") == 0);
    CHECK(seen.content_events == 0 && seen.done_events == 1 && seen.error_events == 0);
    /* A zero-length feed is harmless at any point. */
    begin();
    CHECK(deepseek_provider_feed(&provider, NULL, 0u) == 0);
    CHECK(deepseek_provider_feed(&provider, "data: [DONE]\n", 13u) == 0);
    CHECK(deepseek_provider_feed(&provider, "", 0u) == 0);
    CHECK(deepseek_provider_finish(&provider) == 0);
}

/* Chunks that carry no reply text must not produce CONTENT events. */
static void test_reply_non_content_chunks(void) {
    CHECK(run_text(CHUNK("{\"role\":\"assistant\"}")
                   CHUNK("{\"role\":\"assistant\",\"content\":null}")
                   CHUNK("{\"role\":\"assistant\",\"content\":\"\"}")
                   CHUNK("{\"content\":null,\"reasoning_content\":\"thinking hard\"}")
                   CHUNK("{}")
                   "data: {\"choices\":[],\"usage\":{\"total_tokens\":3}}\n"
                   "data: {\"choices\":null}\n"
                   "data: {\"id\":\"x\",\"role\":\"assistant\",\"content\":\"not a delta\"}\n"
                   "data: {\"choices\":[{\"index\":0,\"message\":{\"content\":\"not a delta\"},\"content\":\"nor this\"}]}\n"
                   "data: [DONE]\n") == 0);
    CHECK(seen.content_events == 0);
    CHECK(seen.content_length == 0u);
    CHECK(seen.done_events == 1);
    CHECK(seen.error_events == 0);

    /* The reasoner: thinking is dropped, the answer is kept. */
    CHECK(run_text(CHUNK("{\"reasoning_content\":\"hmm\",\"content\":null}")
                   CHUNK("{\"reasoning_content\":null,\"content\":\"42\"}")
                   "data: [DONE]\n") == 0);
    CHECK_STR_EQ(seen.content, "42");
    CHECK(seen.content_events == 1);
}

/* ---- Reply: errors ------------------------------------------------------- */

static void check_rejected(const char *text, const char *error) {
    static const size_t chunks[] = {1u, 5u, 4096u};
    size_t c;
    for (c = 0u; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        CHECK(run(text, strlen(text), chunks[c]) == -1);
        CHECK(seen.error_events == 1);
        CHECK_STR_EQ(seen.error, error);
        CHECK(deepseek_provider_failed(&provider));
        CHECK(!deepseek_provider_is_done(&provider));
        /* Latched: nothing more is accepted and nothing more is reported. */
        CHECK(deepseek_provider_feed(&provider, "data: [DONE]\n", 13u) == -1);
        CHECK(deepseek_provider_finish(&provider) == -1);
        CHECK(seen.error_events == 1);
    }
}

static void test_reply_malformed(void) {
    static const char *const malformed[] = {
        "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]\n",          /* unclosed object */
        "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}}\n",        /* extra brace */
        "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]} x\n",       /* trailing text */
        "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}{}\n",       /* two values */
        "data: {\"choices\":[{\"delta\":{\"content\":\"a}}]}\n",           /* unterminated string */
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\u0000\"}}]}\n",   /* embedded NUL */
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\ud800\"}}]}\n",   /* lone surrogate */
        "data: {\"choices\":[{\"delta\":{\"content\":7}}]}\n",             /* content not a string */
        "data: {\"choices\":[{\"delta\":{\"content\":[\"a\"]}}]}\n",
        "data: {\"choices\":[{\"delta\":\"a\"}]}\n",                       /* delta not an object */
        "data: {\"choices\":[{\"delta\":null}]}\n",
        "data: {\"choices\":[\"a\"]}\n",                                   /* choice not an object */
        "data: {\"choices\":{\"delta\":{\"content\":\"a\"}}}\n",           /* choices not an array */
        "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}},]}\n",        /* trailing comma */
        "data: [{\"choices\":[]}]\n",                                      /* not an object */
        "data: \"text\"\n",
        "data: [DONE] extra\n",
        "data: [done]\n",
        "data:\n",
        "data: \n",
        "data\n",
        "{\"choices\":[]}\n",                                              /* no field name */
        "event: message\n",                                                /* fields we never expect */
        "id: 7\n",
        "retry: 100\n",
        "Data: [DONE]\n",
        " data: [DONE]\n",
        "<html>502 Bad Gateway</html>\n",
        "data: {\"choices\":[]}\rdata: [DONE]\n",                          /* bare CR is not a line end */
    };
    size_t i;

    for (i = 0u; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        check_rejected(malformed[i], "malformed reply stream");
        /* Nothing is delivered out of a record that fails validation. */
        CHECK(seen.content_events == 0);
        CHECK(seen.done_events == 0);
    }

    /* Content delivered before the bad record stays delivered; DONE does not come. */
    CHECK(run_text(CHUNK("{\"content\":\"ok\"}") "data: {oops}\n" "data: [DONE]\n") == -1);
    CHECK_STR_EQ(seen.content, "ok");
    CHECK(seen.done_events == 0 && seen.error_events == 1);

    /* A NUL byte anywhere, even in a comment. */
    begin();
    CHECK(deepseek_provider_feed(&provider, ": a\0b\n", 6u) == -1);
    CHECK_STR_EQ(seen.error, "malformed reply stream");
    CHECK(deepseek_provider_feed(NULL, "x", 1u) == -1);
    begin();
    CHECK(deepseek_provider_feed(&provider, NULL, 3u) == -1);
}

static void test_reply_truncated(void) {
    /* End of input before [DONE], however much arrived. */
    check_rejected("", "reply ended early");
    check_rejected("\n\n", "reply ended early");
    check_rejected(": keep-alive\n\n", "reply ended early");
    check_rejected(CHUNK("{\"content\":\"partial\"}"), "reply ended early");
    check_rejected(CHUNK("{\"content\":\"a\"}") "data: {\"choices\":[],\"usage\":{}}\n", "reply ended early");
    /* A record cut off mid-line is malformed when the input ends. */
    check_rejected(CHUNK("{\"content\":\"a\"}") "data: {\"choices\":[{\"delta\":{\"cont", "malformed reply stream");
    check_rejected("data: [DON", "malformed reply stream");
}

static void test_reply_trailing_input(void) {
    /* [DONE] is terminal: nothing but whitespace may follow, on the same
     * feed or a later one, with or without a line ending. */
    check_rejected("data: [DONE]\n" CHUNK("{\"content\":\"late\"}"), "data after end of reply");
    check_rejected("data: [DONE]\ndata: [DONE]\n", "data after end of reply");
    check_rejected("data: [DONE]\n\n: keep-alive\n", "data after end of reply");
    check_rejected("data: [DONE]\n\nHTTP/1.1 200 OK\r\n", "data after end of reply");
    check_rejected("data: [DONE]\nx", "data after end of reply");
    check_rejected("data: [DONE]\n\n   x", "data after end of reply");

    /* DONE was already delivered when the trailing bytes turn up; the stream
     * still ends failed, so finish cannot report success. */
    CHECK(run_text(CHUNK("{\"content\":\"a\"}") "data: [DONE]\n" "garbage") == -1);
    CHECK_STR_EQ(seen.content, "a");
    CHECK(seen.done_events == 1);
    CHECK(seen.error_events == 1);
    CHECK(!deepseek_provider_is_done(&provider));
    CHECK(deepseek_provider_failed(&provider));
}

static void test_reply_oversized(void) {
    static char line[DEEPSEEK_LINE_MAX + 64u];
    static const char prefix[] = "data: {\"choices\":[{\"delta\":{\"content\":\"";
    static const char suffix[] = "\"}}]}";
    size_t fill;

    /* A line of exactly DEEPSEEK_LINE_MAX bytes is accepted... */
    fill = DEEPSEEK_LINE_MAX - (sizeof(prefix) - 1u) - (sizeof(suffix) - 1u);
    memcpy(line, prefix, sizeof(prefix) - 1u);
    memset(line + sizeof(prefix) - 1u, 'a', fill);
    memcpy(line + sizeof(prefix) - 1u + fill, suffix, sizeof(suffix));
    CHECK(strlen(line) == DEEPSEEK_LINE_MAX);
    begin();
    CHECK(deepseek_provider_feed(&provider, line, strlen(line)) == 0);
    CHECK(deepseek_provider_feed(&provider, "\n", 1u) == 0);
    CHECK(seen.content_length == fill);
    CHECK(seen.error_events == 0);

    /* ...and one byte more is not, before its line ending is ever seen. */
    memset(line + sizeof(prefix) - 1u, 'a', fill + 1u);
    memcpy(line + sizeof(prefix) - 1u + fill + 1u, suffix, sizeof(suffix));
    begin();
    CHECK(deepseek_provider_feed(&provider, line, strlen(line)) == -1);
    CHECK_STR_EQ(seen.error, "reply line too long");
    CHECK(seen.content_events == 0);
    CHECK(deepseek_provider_finish(&provider) == -1);
}

static void test_reply_server_error(void) {
    /* A well-formed error record is a successful feed with an ERROR event. */
    begin();
    CHECK(feed_text("data: {\"error\":{\"message\":\"Insufficient Balance\",\"type\":\"unknown_error\","
                                 "\"param\":null,\"code\":\"invalid_request_error\"}}\n") == 0);
    CHECK(seen.error_events == 1);
    CHECK_STR_EQ(seen.error, "Insufficient Balance");
    CHECK(deepseek_provider_failed(&provider));
    CHECK(deepseek_provider_feed(&provider, "data: [DONE]\n", 13u) == -1);
    CHECK(deepseek_provider_finish(&provider) == -1);
    CHECK(seen.error_events == 1 && seen.done_events == 0);

    /* The rest of the segment after the error is not parsed. */
    begin();
    CHECK(feed_text("data: {\"error\":\"overloaded\"}\ndata: [DONE]\n") == 0);
    CHECK_STR_EQ(seen.error, "overloaded");
    CHECK(seen.done_events == 0);
    CHECK(deepseek_provider_finish(&provider) == -1);

    /* An error with no usable message still reports. */
    CHECK(run_text("data: {\"error\":{\"code\":500}}\n") == -1);
    CHECK_STR_EQ(seen.error, "DeepSeek error");
    CHECK(run_text("data: {\"error\":true}\n") == -1);
    CHECK_STR_EQ(seen.error, "DeepSeek error");
    CHECK(run_text("data: {\"error\":{\"message\":null}}\n") == -1);
    CHECK_STR_EQ(seen.error, "DeepSeek error");
    /* "error": null is not an error. */
    CHECK(run_text("data: {\"error\":null,\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\ndata: [DONE]\n") == 0);
    CHECK_STR_EQ(seen.content, "x");

    /* A long message is cut to the buffer, never overrun. */
    {
        static char record[512];
        memset(record, 0, sizeof(record));
        strcpy(record, "data: {\"error\":{\"message\":\"");
        memset(record + strlen(record), 'e', 300u);
        strcat(record, "\"}}\n");
        CHECK(run_text(record) == -1);
        CHECK(seen.error_events == 1);
        CHECK(strlen(seen.error) == DEEPSEEK_ERROR_MAX - 1u);
    }
}

/* A callback that feeds the provider again must not cause a second ERROR. */
static int reentered;

static void reenter(void *context, const provider_event_t *event) {
    (void)context;
    if (event->type == PROVIDER_EVENT_ERROR) {
        reentered++;
        CHECK(deepseek_provider_feed(&provider, "x\n", 2u) == -1);
        CHECK(deepseek_provider_finish(&provider) == -1);
    }
}

static void test_reply_reentrancy(void) {
    reentered = 0;
    deepseek_provider_init(&provider, reenter, NULL);
    CHECK(deepseek_provider_build_request(&provider, "deepseek-chat", hello, 1u, 64, NULL, 80) == 0);
    CHECK(deepseek_provider_feed(&provider, "nonsense\n", 9u) == -1);
    CHECK(reentered == 1);
}

/* ---- Start policy -------------------------------------------------------- */

static void ready_config(ai_config_t *config) {
    ai_config_init(config);
    CHECK(ai_config_set_provider(config, "deepseek") == AI_CONFIG_OK);
    strcpy(config->bearer_token, "sk-test-not-a-real-key");
}

static void test_start_policy(void) {
    ai_config_t config;

    ready_config(&config);
    CHECK(deepseek_provider_start_check(&config, true) == DEEPSEEK_START_OK);
    CHECK_STR_EQ(deepseek_provider_start_text(DEEPSEEK_START_OK), "");

    /* No clock sync, no request: an otherwise complete configuration is
     * refused locally, with a message, before any network use. */
    CHECK(deepseek_provider_start_check(&config, false) == DEEPSEEK_START_CLOCK_NOT_SYNCED);
    CHECK_STR_EQ(deepseek_provider_start_text(DEEPSEEK_START_CLOCK_NOT_SYNCED), "clock not synced: wait");

    config.bearer_token[0] = '\0';
    CHECK(deepseek_provider_start_check(&config, true) == DEEPSEEK_START_NO_KEY);
    CHECK_STR_EQ(deepseek_provider_start_text(DEEPSEEK_START_NO_KEY), "set the API key (F5)");
    strcpy(config.bearer_token, "sk bad\r\nX-Injected: 1");
    CHECK(deepseek_provider_start_check(&config, true) == DEEPSEEK_START_BAD_KEY);

    ready_config(&config);
    strcpy(config.host, "api.deepseek.com.evil.example");
    CHECK(deepseek_provider_start_check(&config, true) == DEEPSEEK_START_BAD_ENDPOINT);
    ready_config(&config);
    config.port = 80u;
    CHECK(deepseek_provider_start_check(&config, true) == DEEPSEEK_START_BAD_ENDPOINT);
    ready_config(&config);
    strcpy(config.model, "llama3");
    CHECK(deepseek_provider_start_check(&config, true) == DEEPSEEK_START_BAD_MODEL);

    ai_config_init(&config);
    CHECK(deepseek_provider_start_check(&config, true) == DEEPSEEK_START_NOT_DEEPSEEK);
    CHECK(deepseek_provider_start_check(NULL, true) == DEEPSEEK_START_NOT_DEEPSEEK);

    /* Every refusal has a message that fits the 30-column status note. */
    {
        int status;
        for (status = DEEPSEEK_START_NOT_DEEPSEEK; status <= DEEPSEEK_START_CLOCK_NOT_SYNCED; status++) {
            const char *text = deepseek_provider_start_text((deepseek_start_t)status);
            CHECK(text[0] != '\0');
            CHECK(strlen(text) <= 30u);
        }
    }
}

void test_deepseek_provider(void);
void test_deepseek_provider(void) {
    test_endpoint_literals();
    test_request_body();
    test_request_rejections();
    test_request_never_longer_than_ollama();
    test_reply_success();
    test_reply_non_content_chunks();
    test_reply_malformed();
    test_reply_truncated();
    test_reply_trailing_input();
    test_reply_oversized();
    test_reply_server_error();
    test_reply_reentrancy();
    test_start_policy();
}
