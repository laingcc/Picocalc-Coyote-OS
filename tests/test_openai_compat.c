#include <string.h>

#include "ai/openai_compat.h"
#include "test_sse.h"

/*
 * The OpenAI and Mistral dialects of the shared OpenAI-style protocol.  (The
 * DeepSeek dialect has its own, older tests in test_deepseek_provider.c,
 * which run against the same code.)
 */

static const openai_compat_dialect_t *dialect;
static const char *model;

static const ollama_message_t hello[] = {{"user", "hi"}};

static void sse_test_build(void) {
    CHECK(openai_compat_build_request(&sse_test_provider, dialect, model, hello, 1u, 64, NULL, 80) == 0);
}

static void use(const openai_compat_dialect_t *which, const char *name) {
    dialect = which;
    model = name;
}

/* ---- Request ------------------------------------------------------------ */

static void check_openai_request(void) {
    static const ollama_message_t turns[] = {
        {"user", "hi"}, {"assistant", "Hello \"there\"\n"}, {"user", "caf\xc3\xa9 \\ tab\there"}};
    sse_provider_t *p = &sse_test_provider;

    /* Written out byte for byte.  A reasoning model: the system prompt is a
     * developer message, the limit is max_completion_tokens with the
     * reasoning headroom added (384 + 1024), and no temperature is sent. */
    sse_provider_init(p, NULL, NULL);
    CHECK(openai_compat_build_request(p, &openai_compat_openai, "gpt-6-astra", turns, 3u, 384, "Be brief.", 80) == 0);
    CHECK_STR_EQ(p->body,
                 "{\"model\":\"gpt-6-astra\",\"messages\":["
                 "{\"role\":\"developer\",\"content\":\"Be brief.\"},"
                 "{\"role\":\"user\",\"content\":\"hi\"},"
                 "{\"role\":\"assistant\",\"content\":\"Hello \\\"there\\\"\\n\"},"
                 "{\"role\":\"user\",\"content\":\"caf\xc3\xa9 \\\\ tab\\there\"}],"
                 "\"stream\":true,\"max_completion_tokens\":1408,\"reasoning_effort\":\"low\"}");
    CHECK(sse_provider_request_length(p) == strlen(p->body));
    CHECK(strstr(p->body, "temperature") == NULL);
    CHECK(strstr(p->body, "\"max_tokens\"") == NULL);

    /* No system prompt: no developer message. */
    CHECK(openai_compat_build_request(p, &openai_compat_openai, "gpt-6-luna", hello, 1u, 1, "", 200) == 0);
    CHECK_STR_EQ(p->body, "{\"model\":\"gpt-6-luna\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                          "\"stream\":true,\"max_completion_tokens\":1025,\"reasoning_effort\":\"low\"}");
}

static void check_mistral_request(void) {
    sse_provider_t *p = &sse_test_provider;

    sse_provider_init(p, NULL, NULL);
    CHECK(openai_compat_build_request(p, &openai_compat_mistral, "mistral-small-latest", hello, 1u, 384,
                                      "Be brief.", 80) == 0);
    CHECK_STR_EQ(p->body,
                 "{\"model\":\"mistral-small-latest\",\"messages\":["
                 "{\"role\":\"system\",\"content\":\"Be brief.\"},"
                 "{\"role\":\"user\",\"content\":\"hi\"}],"
                 "\"stream\":true,\"max_tokens\":384,\"temperature\":0.80}");
    /* Mistral refuses a temperature above 1.5, so a higher setting is capped. */
    CHECK(openai_compat_build_request(p, &openai_compat_mistral, "mistral-large-latest", hello, 1u, 4096, NULL, 150) == 0);
    CHECK(strstr(p->body, "\"max_tokens\":4096,\"temperature\":1.50}") != NULL);
    CHECK(openai_compat_build_request(p, &openai_compat_mistral, "mistral-large-latest", hello, 1u, 4096, NULL, 200) == 0);
    CHECK(strstr(p->body, "\"max_tokens\":4096,\"temperature\":1.50}") != NULL);
    CHECK(openai_compat_build_request(p, &openai_compat_mistral, "mistral-large-latest", hello, 1u, 4096, NULL, 5) == 0);
    CHECK(strstr(p->body, "\"temperature\":0.05}") != NULL);
}

static void check_request_rejections(const openai_compat_dialect_t *which, const char *name) {
    static const ollama_message_t bad_role[] = {{"tool", "x"}};
    static ollama_message_t many[OLLAMA_MAX_MESSAGES + 1u];
    static char huge[SSE_REQUEST_CAPACITY + 1u];
    ollama_message_t big[1];
    sse_provider_t *p = &sse_test_provider;
    size_t i;

    sse_provider_init(p, NULL, NULL);
    CHECK(openai_compat_build_request(NULL, which, name, hello, 1u, 8, NULL, 80) == -1);
    CHECK(openai_compat_build_request(p, NULL, name, hello, 1u, 8, NULL, 80) == -1);
    CHECK(openai_compat_build_request(p, which, NULL, hello, 1u, 8, NULL, 80) == -1);
    CHECK(openai_compat_build_request(p, which, "", hello, 1u, 8, NULL, 80) == -1);
    CHECK(openai_compat_build_request(p, which, name, NULL, 1u, 8, NULL, 80) == -1);
    CHECK(openai_compat_build_request(p, which, name, bad_role, 1u, 8, NULL, 80) == -1);
    CHECK(openai_compat_build_request(p, which, name, hello, 1u, 0, NULL, 80) == -1);
    CHECK(openai_compat_build_request(p, which, name, hello, 1u, -1, NULL, 80) == -1);
    CHECK(openai_compat_build_request(p, which, name, hello, 1u, 8, NULL, -1) == -1);
    CHECK(openai_compat_build_request(p, which, name, hello, 1u, 8, NULL, 201) == -1);
    for (i = 0u; i < OLLAMA_MAX_MESSAGES + 1u; i++) {
        many[i].role = "user";
        many[i].content = "x";
    }
    CHECK(openai_compat_build_request(p, which, name, many, OLLAMA_MAX_MESSAGES + 1u, 8, NULL, 80) == -1);
    CHECK(openai_compat_build_request(p, which, name, many, OLLAMA_MAX_MESSAGES, 8, NULL, 80) == 0);

    /* Too large for the buffer: refused, and no request is left behind. */
    memset(huge, 'a', SSE_REQUEST_CAPACITY);
    huge[SSE_REQUEST_CAPACITY] = '\0';
    big[0].role = "user";
    big[0].content = huge;
    CHECK(openai_compat_build_request(p, which, name, big, 1u, 8, NULL, 80) == -1);
    CHECK(sse_provider_request_length(p) == 0u);
    CHECK(sse_provider_feed(p, "data: [DONE]\n", 13u) == -1);
    CHECK(sse_provider_finish(p) == -1);
}

/* ---- Reply -------------------------------------------------------------- */

#define CHUNK(delta) "data: {\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":" delta ",\"finish_reason\":null}]}\n\n"

/* What both dialects must do with the reply. */
static void check_reply(const char *error_text) {
    /* An ordinary reply, as OpenAI and Mistral frame it. */
    sse_check_accepted(CHUNK("{\"role\":\"assistant\",\"content\":\"\",\"refusal\":null}")
                       CHUNK("{\"content\":\"Hel\"}")
                       CHUNK("{\"content\":\"lo \\\"you\\\"\\n\"}")
                       "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                       "data: {\"id\":\"c1\",\"choices\":[],\"usage\":{\"prompt_tokens\":4,\"completion_tokens\":2}}\n\n"
                       "data: [DONE]\n\n",
                       "Hello \"you\"\n");
    /* CRLF line endings and a comment. */
    sse_check_accepted(": ping\r\n\r\n" "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\r\n\r\ndata: [DONE]\r\n\r\n", "ok");
    /* The marker without a line ending, at the very end of input. */
    sse_check_accepted(CHUNK("{\"content\":\"a\"}") "data: [DONE]", "a");

    /* Role and metadata only: a complete reply with no content. */
    sse_check_accepted(CHUNK("{\"role\":\"assistant\"}")
                       CHUNK("{\"role\":\"assistant\",\"content\":null}")
                       CHUNK("{\"content\":\"\"}")
                       CHUNK("{\"refusal\":\"no\"}")
                       CHUNK("{\"reasoning_content\":\"hmm\"}")
                       CHUNK("{}")
                       "data: {\"choices\":[],\"usage\":{\"total_tokens\":3}}\n"
                       "data: {\"choices\":null}\n"
                       "data: {\"id\":\"x\",\"role\":\"assistant\",\"content\":\"not a delta\"}\n"
                       "data: {\"choices\":[{\"message\":{\"content\":\"not a delta\"},\"content\":\"nor this\"}]}\n"
                       "data: [DONE]\n",
                       "");
    CHECK(sse_seen.content_events == 0);

    /* Malformed events. */
    sse_check_rejected("data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]} x\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("data: not json\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("data: [\"a\"]\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("data: {\"choices\":{}}\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("data: {\"choices\":[7]}\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("data: {\"choices\":[{\"delta\":7}]}\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("data: {\"choices\":[{\"delta\":{\"content\":7}}]}\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("data: {\"choices\":[{\"delta\":{\"content\":\"bad \\x escape\"}}]}\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("data: {\"choices\":[{\"delta\":{\"content\":\"nul \\u0000\"}}]}\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("event: message\ndata: [DONE]\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("{\"choices\":[]}\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("data:[DONE] \ndata: [done]\n", SSE_ERROR_TRAILING, "");
    sse_check_rejected("data: [done]\n", SSE_ERROR_MALFORMED, "");
    /* Content before the fault was delivered; none comes from the bad record. */
    sse_check_rejected(CHUNK("{\"content\":\"good\"}") "data: {\"choices\":[{\"delta\":{\"content\":\"bad\"}}],}\n",
                       SSE_ERROR_MALFORMED, "good");
    {
        static const char nul[] = "data: {\"choices\":[]}\n\0data: [DONE]\n";
        CHECK(sse_run(nul, sizeof(nul) - 1u, 100000u) == -1);
        CHECK_STR_EQ(sse_seen.error, SSE_ERROR_MALFORMED);
    }

    /* Truncated input: the stream ended before the marker. */
    sse_check_rejected("", SSE_ERROR_TRUNCATED, "");
    sse_check_rejected(CHUNK("{\"content\":\"partial\"}"), SSE_ERROR_TRUNCATED, "partial");
    sse_check_rejected(CHUNK("{\"content\":\"partial\"}") "data: [DON", SSE_ERROR_MALFORMED, "partial");
    sse_check_rejected(CHUNK("{\"content\":\"partial\"}") "data: {\"choices\":[{\"delta\":{\"content\":\"cu", SSE_ERROR_MALFORMED,
                       "partial");
    sse_check_rejected("data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n", SSE_ERROR_TRUNCATED, "");

    /* The terminal marker ends the reply: only whitespace may follow. */
    sse_check_accepted(CHUNK("{\"content\":\"a\"}") "data: [DONE]\n\n \r\n\t\n", "a");
    sse_check_rejected(CHUNK("{\"content\":\"a\"}") "data: [DONE]\n\n" CHUNK("{\"content\":\"late\"}"), SSE_ERROR_TRAILING, "a");
    sse_check_rejected(CHUNK("{\"content\":\"a\"}") "data: [DONE]\ndata: [DONE]\n", SSE_ERROR_TRAILING, "a");
    sse_check_rejected(CHUNK("{\"content\":\"a\"}") "data: [DONE]\n: comment\n", SSE_ERROR_TRAILING, "a");
    sse_check_rejected(CHUNK("{\"content\":\"a\"}") "data: [DONE]\njunk", SSE_ERROR_TRAILING, "a");

    /* An error record: reported with a fixed message, never the server's. */
    {
        static const char *const errors[] = {
            "data: {\"error\":{\"message\":\"Your key sk-secret was leaked\",\"type\":\"server_error\",\"code\":null}}\n\n",
            "data: {\"error\":\"remote text\"}\n",
            "data: {\"error\":{}}\n",
            "data: {\"object\":\"error\",\"error\":{\"message\":\"x\"},\"choices\":[]}\n",
        };
        size_t i, c;
        for (i = 0u; i < sizeof(errors) / sizeof(errors[0]); i++) {
            for (c = 0u; c < SSE_FRAGMENT_COUNT; c++) {
                /* A well-formed record: the feed succeeds, the stream fails. */
                CHECK(sse_run(errors[i], strlen(errors[i]), sse_fragments[c]) == -1);
                CHECK(sse_seen.error_events == 1);
                CHECK_STR_EQ(sse_seen.error, error_text);
                CHECK(sse_seen.done_events == 0);
                CHECK(sse_provider_failed(&sse_test_provider));
            }
        }
        sse_begin();
        CHECK(sse_provider_feed(&sse_test_provider, errors[0], strlen(errors[0])) == 0);
        CHECK(sse_provider_feed(&sse_test_provider, "data: [DONE]\n", 13u) == -1);
        CHECK(sse_seen.error_events == 1);
    }
    /* "error": null is not an error. */
    sse_check_accepted("data: {\"error\":null,\"choices\":[{\"delta\":{\"content\":\"fine\"}}]}\ndata: [DONE]\n", "fine");
}

static void check_line_limit(void) {
    static const char prefix[] = "data: {\"choices\":[{\"delta\":{\"content\":\"";
    static const char suffix[] = "\"}}]}";
    static char line[OPENAI_COMPAT_LINE_MAX + 64u];
    size_t fill = OPENAI_COMPAT_LINE_MAX - (sizeof(prefix) - 1u) - (sizeof(suffix) - 1u);

    /* A line of exactly the limit is accepted... */
    memcpy(line, prefix, sizeof(prefix) - 1u);
    memset(line + sizeof(prefix) - 1u, 'x', fill);
    memcpy(line + sizeof(prefix) - 1u + fill, suffix, sizeof(suffix));
    CHECK(strlen(line) == OPENAI_COMPAT_LINE_MAX);
    strcat(line, "\ndata: [DONE]\n");
    CHECK(sse_run(line, strlen(line), 100000u) == 0);
    CHECK(sse_seen.content_length == fill);

    /* ...and one byte more is refused, whatever follows. */
    memcpy(line, prefix, sizeof(prefix) - 1u);
    memset(line + sizeof(prefix) - 1u, 'x', fill + 1u);
    memcpy(line + sizeof(prefix) - 1u + fill + 1u, suffix, sizeof(suffix));
    strcat(line, "\ndata: [DONE]\n");
    CHECK(sse_run(line, strlen(line), 100000u) == -1);
    CHECK_STR_EQ(sse_seen.error, SSE_ERROR_OVERSIZED);
    CHECK(sse_seen.content_events == 0);
    CHECK(sse_seen.done_events == 0);
}

/* Mistral's reasoning models send content as an array of typed chunks. */
static void check_mistral_content_chunks(void) {
    use(&openai_compat_mistral, "mistral-small-latest");
    sse_check_accepted(CHUNK("{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":[{\"type\":\"text\",\"text\":\"hmm\"}]}]}")
                       CHUNK("{\"content\":[{\"type\":\"text\",\"text\":\"Hel\"},{\"text\":\"lo\",\"type\":\"text\"}]}")
                       CHUNK("{\"content\":[{\"text\":\"secret\",\"type\":\"thinking\"},{\"type\":\"reference\",\"reference_ids\":[1]}]}")
                       CHUNK("{\"content\":[]}")
                       CHUNK("{\"content\":\"!\"}")
                       "data: [DONE]\n",
                       "Hello!");
    sse_check_rejected(CHUNK("{\"content\":[\"bare\"]}") "data: [DONE]\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected(CHUNK("{\"content\":[{\"type\":\"text\",\"text\":\"a\"}") "data: [DONE]\n", SSE_ERROR_MALFORMED, "");

    /* OpenAI never sends an array there: for it that is malformed. */
    use(&openai_compat_openai, "gpt-6-astra");
    sse_check_rejected(CHUNK("{\"content\":[{\"type\":\"text\",\"text\":\"a\"}]}") "data: [DONE]\n", SSE_ERROR_MALFORMED, "");
}

void test_openai_compat(void);
void test_openai_compat(void) {
    check_openai_request();
    check_mistral_request();
    check_request_rejections(&openai_compat_openai, "gpt-6-astra");
    check_request_rejections(&openai_compat_mistral, "mistral-small-latest");

    use(&openai_compat_openai, "gpt-6-astra");
    check_reply("OpenAI error");
    check_line_limit();
    use(&openai_compat_mistral, "mistral-small-latest");
    check_reply("Mistral error");
    check_line_limit();
    check_mistral_content_chunks();
}
