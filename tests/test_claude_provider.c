#include <string.h>

#include "ai/claude_provider.h"
#include "test_sse.h"

static const ollama_message_t hello[] = {{"user", "hi"}};

static void sse_test_build(void) {
    CHECK(claude_provider_build_request(&sse_test_provider, "claude-opus-5-5", hello, 1u, 64, NULL, 80) == 0);
}

/* ---- Request ------------------------------------------------------------ */

static void check_request(void) {
    static const ollama_message_t turns[] = {
        {"user", "hi"}, {"assistant", "Hello \"there\"\n"}, {"user", "caf\xc3\xa9 \\ tab\there"}};
    static const ollama_message_t system_turn[] = {{"system", "x"}};
    static const ollama_message_t bad_role[] = {{"model", "x"}};
    static ollama_message_t many[OLLAMA_MAX_MESSAGES + 1u];
    static char huge[SSE_REQUEST_CAPACITY + 1u];
    ollama_message_t big[1];
    sse_provider_t *p = &sse_test_provider;
    size_t i;

    CHECK_STR_EQ(CLAUDE_PATH, "/v1/messages");
    CHECK_STR_EQ(CLAUDE_API_KEY_HEADER, "x-api-key");
    CHECK_STR_EQ(CLAUDE_API_VERSION_HEADER, "anthropic-version: 2023-06-01");

    /* Written out byte for byte: the native Messages format.  The system
     * prompt is its own member, max_tokens carries the thinking headroom
     * (384 + 1024), and no temperature is sent whatever the setting. */
    sse_provider_init(p, NULL, NULL);
    CHECK(claude_provider_build_request(p, "claude-opus-5-5", turns, 3u, 384, "Be brief.", 80) == 0);
    CHECK_STR_EQ(p->body,
                 "{\"model\":\"claude-opus-5-5\",\"max_tokens\":1408,\"stream\":true,"
                 "\"output_config\":{\"effort\":\"low\"},"
                 "\"system\":\"Be brief.\","
                 "\"messages\":["
                 "{\"role\":\"user\",\"content\":\"hi\"},"
                 "{\"role\":\"assistant\",\"content\":\"Hello \\\"there\\\"\\n\"},"
                 "{\"role\":\"user\",\"content\":\"caf\xc3\xa9 \\\\ tab\\there\"}]}");
    CHECK(sse_provider_request_length(p) == strlen(p->body));
    CHECK(strstr(p->body, "temperature") == NULL);
    CHECK(strstr(p->body, "thinking") == NULL);

    CHECK(claude_provider_build_request(p, "claude-haiku-5-5", hello, 1u, 1, "", 200) == 0);
    CHECK_STR_EQ(p->body, "{\"model\":\"claude-haiku-5-5\",\"max_tokens\":1025,\"stream\":true,"
                          "\"output_config\":{\"effort\":\"low\"},"
                          "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");
    {
        char buffer[64];
        CHECK(sse_provider_read(p, 0u, buffer, 9u) == 9u);
        CHECK_BYTES_EQ(buffer, "{\"model\":", 9u);
        CHECK(sse_provider_read(p, strlen(p->body), buffer, sizeof(buffer)) == 0u);
    }

    CHECK(claude_provider_build_request(NULL, "claude-opus-5-5", hello, 1u, 8, NULL, 80) == -1);
    CHECK(claude_provider_build_request(p, NULL, hello, 1u, 8, NULL, 80) == -1);
    CHECK(claude_provider_build_request(p, "", hello, 1u, 8, NULL, 80) == -1);
    CHECK(claude_provider_build_request(p, "claude-opus-5-5", NULL, 1u, 8, NULL, 80) == -1);
    CHECK(claude_provider_build_request(p, "claude-opus-5-5", hello, 1u, 0, NULL, 80) == -1);
    /* The system prompt has its own member; a message may not claim the role. */
    CHECK(claude_provider_build_request(p, "claude-opus-5-5", system_turn, 1u, 8, NULL, 80) == -1);
    CHECK(claude_provider_build_request(p, "claude-opus-5-5", bad_role, 1u, 8, NULL, 80) == -1);
    for (i = 0u; i < OLLAMA_MAX_MESSAGES + 1u; i++) {
        many[i].role = "user";
        many[i].content = "x";
    }
    CHECK(claude_provider_build_request(p, "claude-opus-5-5", many, OLLAMA_MAX_MESSAGES + 1u, 8, NULL, 80) == -1);
    CHECK(claude_provider_build_request(p, "claude-opus-5-5", many, OLLAMA_MAX_MESSAGES, 8, NULL, 80) == 0);
    memset(huge, 'a', SSE_REQUEST_CAPACITY);
    huge[SSE_REQUEST_CAPACITY] = '\0';
    big[0].role = "user";
    big[0].content = huge;
    CHECK(claude_provider_build_request(p, "claude-opus-5-5", big, 1u, 8, NULL, 80) == -1);
    CHECK(sse_provider_request_length(p) == 0u);
    CHECK(sse_provider_feed(p, "event: ping\n", 12u) == -1);
}

/* ---- Reply -------------------------------------------------------------- */

#define EV(name, data) "event: " name "\ndata: " data "\n\n"
#define START EV("message_start", "{\"type\": \"message_start\", \"message\": {\"id\": \"msg_1\", \"type\": \"message\", \"role\": \"assistant\", \"content\": [], \"model\": \"claude-opus-5-5\", \"stop_reason\": null, \"stop_sequence\": null, \"usage\": {\"input_tokens\": 25, \"output_tokens\": 1}}}")
#define BLOCK_START EV("content_block_start", "{\"type\": \"content_block_start\", \"index\": 0, \"content_block\": {\"type\": \"text\", \"text\": \"\"}}")
#define TEXT(text) EV("content_block_delta", "{\"type\": \"content_block_delta\", \"index\": 0, \"delta\": {\"type\": \"text_delta\", \"text\": \"" text "\"}}")
#define BLOCK_STOP EV("content_block_stop", "{\"type\": \"content_block_stop\", \"index\": 0}")
#define PING EV("ping", "{\"type\": \"ping\"}")
#define END_TURN EV("message_delta", "{\"type\": \"message_delta\", \"delta\": {\"stop_reason\": \"end_turn\", \"stop_sequence\":null}, \"usage\": {\"output_tokens\": 15}}")
#define STOP EV("message_stop", "{\"type\": \"message_stop\"}")

static void check_ordinary_reply(void) {
    /* The documented basic stream, event for event. */
    sse_check_accepted(START BLOCK_START PING TEXT("Hello") TEXT("!") BLOCK_STOP END_TURN STOP, "Hello!");
    CHECK(sse_seen.content_events == 2);

    /* With a thinking block first: only the text block's text is delivered. */
    sse_check_accepted(
        START
        EV("content_block_start", "{\"type\": \"content_block_start\", \"index\": 0, \"content_block\": {\"type\": \"thinking\", \"thinking\": \"\", \"signature\": \"\"}}")
        EV("content_block_delta", "{\"type\": \"content_block_delta\", \"index\": 0, \"delta\": {\"type\": \"thinking_delta\", \"thinking\": \"I need the GCD.\\n\\n1071 = 2 x 462 + 147\"}}")
        EV("content_block_delta", "{\"type\": \"content_block_delta\", \"index\": 0, \"delta\": {\"type\": \"thinking_delta\", \"thinking\": \"\"}}")
        EV("content_block_delta", "{\"type\": \"content_block_delta\", \"index\": 0, \"delta\": {\"type\": \"signature_delta\", \"signature\": \"EqQBCgIYAhIM1gbcDa9GJwZA2b3hGgxBdjrkzLoky3dl1pkiMOYds\"}}")
        BLOCK_STOP
        EV("content_block_start", "{\"type\": \"content_block_start\", \"index\": 1, \"content_block\": {\"type\": \"text\", \"text\": \"\"}}")
        EV("content_block_delta", "{\"type\": \"content_block_delta\", \"index\": 1, \"delta\": {\"type\": \"text_delta\", \"text\": \"The GCD is **21**.\"}}")
        EV("content_block_stop", "{\"type\": \"content_block_stop\", \"index\": 1}")
        EV("message_delta", "{\"type\": \"message_delta\", \"delta\": {\"stop_reason\": \"end_turn\", \"stop_sequence\": null}}")
        STOP,
        "The GCD is **21**.");

    /* CRLF, compact JSON, members in another order, escapes, a comment, no
     * blank line after the last event, and max_tokens as the stop reason. */
    sse_check_accepted(": hello\r\n"
                       "event:message_start\r\ndata:{\"message\":{},\"type\":\"message_start\"}\r\n\r\n"
                       "event: content_block_delta\r\n"
                       "data: {\"delta\":{\"text\":\"caf\\u00e9 \\\"q\\\"\\n\",\"type\":\"text_delta\"},\"index\":0,\"type\":\"content_block_delta\"}\r\n\r\n"
                       "event: message_delta\r\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"max_tokens\"}}\r\n\r\n"
                       "event: message_stop\r\ndata: {\"type\":\"message_stop\"}",
                       "caf\xc3\xa9 \"q\"\n");

    /* Events this code does not know are checked for form and ignored. */
    sse_check_accepted(START EV("some_future_event", "{\"type\": \"some_future_event\", \"text\": \"not reply text\"}")
                       TEXT("ok") EV("an_event_name_longer_than_the_buffer_that_holds_names", "{\"type\":\"an_event_name_longer_than_the_buffer_that_holds_names\"}")
                       END_TURN STOP,
                       "ok");
}

/* Role and metadata with no text: a complete reply with no content. */
static void check_no_content(void) {
    sse_check_accepted(START BLOCK_START PING BLOCK_STOP END_TURN STOP, "");
    CHECK(sse_seen.content_events == 0);
    sse_check_accepted(START BLOCK_START TEXT("") BLOCK_STOP END_TURN STOP, "");
    CHECK(sse_seen.content_events == 0);
    sse_check_accepted(START STOP, "");

    /* Text anywhere but a text_delta of a content_block_delta is not reply text. */
    sse_check_accepted(
        START
        EV("content_block_start", "{\"type\": \"content_block_start\", \"index\": 0, \"content_block\": {\"type\": \"text\", \"text\": \"not a delta\"}}")
        EV("content_block_delta", "{\"type\": \"content_block_delta\", \"index\": 0, \"delta\": {\"type\": \"thinking_delta\", \"thinking\": \"secret\", \"text\": \"nor this\"}}")
        EV("content_block_delta", "{\"type\": \"content_block_delta\", \"index\": 0, \"delta\": {\"type\": \"input_json_delta\", \"partial_json\": \"{\\\"text\\\": \\\"no\\\"\"}}")
        EV("content_block_delta", "{\"type\": \"content_block_delta\", \"index\": 0, \"text\": \"outside the delta\", \"delta\": {\"type\": \"citations_delta\"}}")
        EV("message_delta", "{\"type\": \"message_delta\", \"delta\": {\"type\": \"text_delta\", \"text\": \"wrong event\", \"stop_reason\": \"end_turn\"}}")
        EV("ping", "{\"type\": \"ping\", \"delta\": {\"type\": \"text_delta\", \"text\": \"wrong event\"}}")
        STOP,
        "");
    CHECK(sse_seen.content_events == 0);
}

static void check_malformed(void) {
    /* Not JSON, not an object, or cut short. */
    sse_check_rejected(START "event: content_block_delta\ndata: not json\n\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event: content_block_delta\ndata: [1]\n\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event: content_block_delta\ndata: {\"type\": \"content_block_delta\", \"delta\": {\"type\": \"text_delta\", \"text\": \"a\"}\n\n",
                       SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event: content_block_delta\ndata: {\"type\": \"content_block_delta\", \"delta\": {\"type\": \"text_delta\", \"text\": \"a\"}} x\n\n",
                       SSE_ERROR_MALFORMED, "");
    /* The data's type must be the event's name. */
    sse_check_rejected(START "event: content_block_delta\ndata: {\"type\": \"ping\", \"delta\": {\"type\": \"text_delta\", \"text\": \"a\"}}\n\n",
                       SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event: ping\ndata: {\"type\": \"content_block_delta\", \"delta\": {\"type\": \"text_delta\", \"text\": \"a\"}}\n\n",
                       SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event: content_block_delta\ndata: {\"delta\": {\"type\": \"text_delta\", \"text\": \"a\"}}\n\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event: content_block_delta\ndata: {\"type\": 7, \"delta\": {\"type\": \"text_delta\", \"text\": \"a\"}}\n\n",
                       SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event: content_block_delta\ndata: {\"type\": \"content_block_delta\", \"type\": \"ping\", \"delta\": {\"type\": \"text_delta\", \"text\": \"a\"}}\n\n",
                       SSE_ERROR_MALFORMED, "");
    sse_check_rejected("event: message_stop\ndata: {\"type\": \"message_start\"}\n\n", SSE_ERROR_MALFORMED, "");
    /* A content_block_delta needs its delta, and a text_delta its text. */
    sse_check_rejected(START "event: content_block_delta\ndata: {\"type\": \"content_block_delta\", \"index\": 0}\n\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event: content_block_delta\ndata: {\"type\": \"content_block_delta\", \"delta\": {\"type\": \"text_delta\"}}\n\n",
                       SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event: content_block_delta\ndata: {\"type\": \"content_block_delta\", \"delta\": {\"type\": \"text_delta\", \"text\": 7}}\n\n",
                       SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event: content_block_delta\ndata: {\"type\": \"content_block_delta\", \"delta\": {\"type\": \"text_delta\", \"text\": \"nul \\u0000\"}}\n\n",
                       SSE_ERROR_MALFORMED, "");
    /* Framing: data needs an event, an event needs data, and each only once. */
    sse_check_rejected("data: {\"type\": \"message_start\", \"message\": {}}\n\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "data: {\"type\": \"content_block_delta\", \"delta\": {\"type\": \"text_delta\", \"text\": \"a\"}}\n\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event: ping\n\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event: ping\nevent: ping\ndata: {\"type\": \"ping\"}\n\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event: ping\ndata: {\"type\": \"ping\"}\ndata: {\"type\": \"ping\"}\n\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "event:\ndata: {\"type\": \"\"}\n\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "id: 7\n\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START "{\"type\": \"ping\"}\n\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("data: [DONE]\n\n", SSE_ERROR_MALFORMED, "");
    /* Order: nothing before message_start, and only one of it. */
    sse_check_rejected(TEXT("early") END_TURN STOP, SSE_ERROR_MALFORMED, "");
    sse_check_rejected(BLOCK_START, SSE_ERROR_MALFORMED, "");
    sse_check_rejected(STOP, SSE_ERROR_MALFORMED, "");
    sse_check_rejected(START START, SSE_ERROR_MALFORMED, "");
    /* Content before the fault was delivered; none comes from the bad record. */
    sse_check_rejected(START TEXT("good") "event: content_block_delta\ndata: {\"type\": \"content_block_delta\", \"delta\": {\"type\": \"text_delta\", \"text\": \"bad\"},}\n\n",
                       SSE_ERROR_MALFORMED, "good");
    {
        static const char nul[] = START "event: ping\n\0data: {\"type\": \"ping\"}\n\n";
        CHECK(sse_run(nul, sizeof(nul) - 1u, 100000u) == -1);
        CHECK_STR_EQ(sse_seen.error, SSE_ERROR_MALFORMED);
    }
}

static void check_truncated(void) {
    sse_check_rejected("", SSE_ERROR_TRUNCATED, "");
    sse_check_rejected(START, SSE_ERROR_TRUNCATED, "");
    sse_check_rejected(START BLOCK_START TEXT("partial"), SSE_ERROR_TRUNCATED, "partial");
    /* message_delta is not the end; message_stop is. */
    sse_check_rejected(START BLOCK_START TEXT("partial") BLOCK_STOP END_TURN, SSE_ERROR_TRUNCATED, "partial");
    sse_check_rejected(START TEXT("partial") "event: message_stop\n", SSE_ERROR_TRUNCATED, "partial");
    sse_check_rejected(START TEXT("partial") "event: message_stop\ndata: {\"type\": \"message_st", SSE_ERROR_MALFORMED, "partial");
    sse_check_rejected(START TEXT("partial") "event: content_block_delta\ndata: {\"type\": \"content_block_delta\", \"delta\": {\"type\": \"text_delta\", \"text\": \"cu",
                       SSE_ERROR_MALFORMED, "partial");
}

static void check_terminal(void) {
    /* Only whitespace may follow message_stop. */
    sse_check_accepted(START TEXT("a") END_TURN STOP "\n \r\n\t\n", "a");
    sse_check_rejected(START TEXT("a") END_TURN STOP TEXT("late"), SSE_ERROR_TRAILING, "a");
    sse_check_rejected(START TEXT("a") END_TURN STOP STOP, SSE_ERROR_TRAILING, "a");
    sse_check_rejected(START TEXT("a") END_TURN STOP PING, SSE_ERROR_TRAILING, "a");
    sse_check_rejected(START TEXT("a") END_TURN STOP ": comment\n", SSE_ERROR_TRAILING, "a");
    sse_check_rejected(START TEXT("a") END_TURN STOP "junk", SSE_ERROR_TRAILING, "a");
    sse_check_rejected(START TEXT("a") END_TURN STOP "data: [DONE]\n", SSE_ERROR_TRAILING, "a");
}

/* Errors and refusals map to fixed messages; the server's text is not shown. */
static void check_error_mapping(void) {
    static const struct {
        const char *stream;
        const char *error;
    } cases[] = {
        {EV("error", "{\"type\": \"error\", \"error\": {\"type\": \"overloaded_error\", \"message\": \"Overloaded\"}}"), CLAUDE_ERROR_OVERLOADED},
        {START TEXT("a") EV("error", "{\"type\": \"error\", \"error\": {\"type\": \"overloaded_error\", \"message\": \"Overloaded\"}}"),
         CLAUDE_ERROR_OVERLOADED},
        {START EV("error", "{\"type\":\"error\",\"error\":{\"message\":\"slow down\",\"type\":\"rate_limit_error\"},\"request_id\":\"req_1\"}"),
         CLAUDE_ERROR_RATE_LIMITED},
        {START EV("error", "{\"type\": \"error\", \"error\": {\"type\": \"api_error\", \"message\": \"Your key sk-ant-secret leaked\"}}"),
         CLAUDE_ERROR_SERVER},
        {START EV("error", "{\"type\": \"error\"}"), CLAUDE_ERROR_SERVER},
        {START EV("error", "{\"type\": \"error\", \"error\": \"text\"}"), CLAUDE_ERROR_SERVER},
        /* A refusal ends the message in the ordinary way, but is not a reply. */
        {START TEXT("I ") EV("message_delta", "{\"type\": \"message_delta\", \"delta\": {\"stop_reason\": \"refusal\", \"stop_sequence\": null}}") STOP,
         CLAUDE_ERROR_REFUSED},
    };
    size_t i, c;

    CHECK_STR_EQ(CLAUDE_ERROR_OVERLOADED, "Claude busy: try later");
    CHECK_STR_EQ(CLAUDE_ERROR_RATE_LIMITED, "rate limited: try later");
    CHECK_STR_EQ(CLAUDE_ERROR_SERVER, "Claude error");
    CHECK_STR_EQ(CLAUDE_ERROR_REFUSED, "Claude declined the request");
    for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (c = 0u; c < SSE_FRAGMENT_COUNT; c++) {
            CHECK(sse_run(cases[i].stream, strlen(cases[i].stream), sse_fragments[c]) == -1);
            CHECK(sse_seen.error_events == 1);
            CHECK_STR_EQ(sse_seen.error, cases[i].error);
            CHECK(sse_seen.done_events == 0);
            CHECK(strstr(sse_seen.error, "sk-ant") == NULL);
            CHECK(sse_provider_failed(&sse_test_provider));
        }
    }
    /* The error record itself is well formed: that feed succeeds, and the
     * stream then takes nothing more. */
    sse_begin();
    CHECK(sse_provider_feed(&sse_test_provider, cases[0].stream, strlen(cases[0].stream)) == 0);
    CHECK(sse_seen.error_events == 1);
    CHECK(sse_provider_feed(&sse_test_provider, STOP, strlen(STOP)) == -1);
    CHECK(sse_provider_finish(&sse_test_provider) == -1);
    CHECK(sse_seen.error_events == 1);
}

/* Build "event: NAME\ndata: PREFIX<fill bytes>SUFFIX\n\n" with a data line of
 * exactly line_length bytes. */
static char long_stream[3u * SSE_LINE_CAPACITY];

static void long_event(char *out, const char *name, const char *prefix, const char *suffix, size_t line_length) {
    size_t fill = line_length - strlen("data: ") - strlen(prefix) - strlen(suffix);
    char *p = out + strlen(out);

    p += sprintf(p, "event: %s\ndata: %s", name, prefix);
    memset(p, 'x', fill);
    p += fill;
    sprintf(p, "%s\n\n", suffix);
}

static void check_line_limit(void) {
    static const char text_prefix[] = "{\"type\": \"content_block_delta\", \"index\": 0, \"delta\": {\"type\": \"text_delta\", \"text\": \"";
    static const char signature_prefix[] = "{\"type\": \"content_block_delta\", \"index\": 0, \"delta\": {\"type\": \"signature_delta\", \"signature\": \"";
    static const char reordered_prefix[] = "{\"type\": \"content_block_delta\", \"index\": 0, \"delta\": {\"signature\": \"";
    size_t c;

    /* A text delta of exactly the limit is accepted, and one byte more refused. */
    strcpy(long_stream, START);
    long_event(long_stream, "content_block_delta", text_prefix, "\"}}", CLAUDE_LINE_MAX);
    strcat(long_stream, END_TURN STOP);
    CHECK(sse_run(long_stream, strlen(long_stream), 100000u) == 0);
    CHECK(sse_seen.content_length == CLAUDE_LINE_MAX - strlen("data: ") - strlen(text_prefix) - 3u);

    strcpy(long_stream, START TEXT("kept"));
    long_event(long_stream, "content_block_delta", text_prefix, "\"}}", CLAUDE_LINE_MAX + 1u);
    strcat(long_stream, END_TURN STOP);
    for (c = 0u; c < SSE_FRAGMENT_COUNT; c++) {
        CHECK(sse_run(long_stream, strlen(long_stream), sse_fragments[c]) == -1);
        CHECK_STR_EQ(sse_seen.error, SSE_ERROR_OVERSIZED);
        CHECK_STR_EQ(sse_seen.content, "kept");
        CHECK(sse_seen.done_events == 0);
    }

    /* A thinking block's signature can be far longer than a line may be.  It
     * carries no text, so it is passed over and the reply goes on. */
    strcpy(long_stream, START);
    long_event(long_stream, "content_block_delta", signature_prefix, "\"}}", 2u * SSE_LINE_CAPACITY);
    strcat(long_stream, TEXT("after") END_TURN STOP);
    for (c = 0u; c < SSE_FRAGMENT_COUNT; c++) {
        CHECK(sse_run(long_stream, strlen(long_stream), sse_fragments[c]) == 0);
        CHECK_STR_EQ(sse_seen.content, "after");
        CHECK(sse_seen.content_events == 1);
        CHECK(sse_seen.done_events == 1);
    }
    /* So is an over-long event of a kind that never carries text. */
    strcpy(long_stream, START);
    long_event(long_stream, "content_block_start", "{\"type\": \"content_block_start\", \"content_block\": {\"type\": \"redacted_thinking\", \"data\": \"", "\"}}",
               CLAUDE_LINE_MAX + 500u);
    long_event(long_stream, "some_future_event", "{\"type\": \"some_future_event\", \"blob\": \"", "\"}", CLAUDE_LINE_MAX + 1u);
    strcat(long_stream, TEXT("after") END_TURN STOP);
    CHECK(sse_run(long_stream, strlen(long_stream), 5u) == 0);
    CHECK_STR_EQ(sse_seen.content, "after");

    /* It is passed over only when what was kept proves it is not text: a
     * delta whose type lies beyond the kept part is refused... */
    strcpy(long_stream, START);
    long_event(long_stream, "content_block_delta", reordered_prefix, "\", \"type\": \"signature_delta\"}}", CLAUDE_LINE_MAX + 100u);
    strcat(long_stream, END_TURN STOP);
    CHECK(sse_run(long_stream, strlen(long_stream), 100000u) == -1);
    CHECK_STR_EQ(sse_seen.error, SSE_ERROR_OVERSIZED);
    /* ...as are the events that decide how the reply ends, and anything
     * that is not a well-placed data line. */
    strcpy(long_stream, START);
    long_event(long_stream, "message_delta", "{\"type\": \"message_delta\", \"blob\": \"", "\", \"delta\": {\"stop_reason\": \"refusal\"}}",
               CLAUDE_LINE_MAX + 100u);
    strcat(long_stream, STOP);
    CHECK(sse_run(long_stream, strlen(long_stream), 100000u) == -1);
    CHECK_STR_EQ(sse_seen.error, SSE_ERROR_OVERSIZED);
    strcpy(long_stream, "");
    long_event(long_stream, "message_start", "{\"type\": \"message_start\", \"blob\": \"", "\"}", CLAUDE_LINE_MAX + 100u);
    CHECK(sse_run(long_stream, strlen(long_stream), 100000u) == -1);
    CHECK_STR_EQ(sse_seen.error, SSE_ERROR_OVERSIZED);
    strcpy(long_stream, "");
    long_event(long_stream, "content_block_delta", signature_prefix, "\"}}", CLAUDE_LINE_MAX + 100u);
    CHECK(sse_run(long_stream, strlen(long_stream), 100000u) == -1); /* before message_start */
    CHECK_STR_EQ(sse_seen.error, SSE_ERROR_OVERSIZED);
    strcpy(long_stream, START "event: content_block_delta\n");
    long_event(long_stream, "content_block_delta", signature_prefix, "\"}}", CLAUDE_LINE_MAX + 100u);
    CHECK(sse_run(long_stream, strlen(long_stream), 100000u) == -1); /* a second event line */
    CHECK_STR_EQ(sse_seen.error, SSE_ERROR_MALFORMED);
    strcpy(long_stream, START "event: ");
    memset(long_stream + strlen(long_stream), 'e', CLAUDE_LINE_MAX + 10u);
    strcat(long_stream, "\n");
    CHECK(sse_run(long_stream, strlen(long_stream), 100000u) == -1);
    CHECK_STR_EQ(sse_seen.error, SSE_ERROR_OVERSIZED);

    /* A reply that ends inside a passed-over line ended early. */
    strcpy(long_stream, START TEXT("partial"));
    long_event(long_stream, "content_block_delta", signature_prefix, "\"}}", CLAUDE_LINE_MAX + 100u);
    long_stream[strlen(long_stream) - 10u] = '\0';
    CHECK(sse_run(long_stream, strlen(long_stream), 100000u) == -1);
    CHECK_STR_EQ(sse_seen.error, SSE_ERROR_TRUNCATED);
    CHECK_STR_EQ(sse_seen.content, "partial");
}

void test_claude_provider(void);
void test_claude_provider(void) {
    memset(long_stream, 0, sizeof(long_stream));
    check_request();
    check_ordinary_reply();
    check_no_content();
    check_malformed();
    check_truncated();
    check_terminal();
    check_error_mapping();
    check_line_limit();
}
