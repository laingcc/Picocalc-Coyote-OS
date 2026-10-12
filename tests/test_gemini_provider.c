#include <string.h>

#include "ai/gemini_provider.h"
#include "test_sse.h"

static const ollama_message_t hello[] = {{"user", "hi"}};

static void sse_test_build(void) {
    CHECK(gemini_provider_build_request(&sse_test_provider, "gemini-3.8-flash", hello, 1u, 64, NULL, 80) == 0);
}

static void check_request(void) {
    sse_provider_t *p = &sse_test_provider;
    char path[GEMINI_PATH_CAPACITY];

    sse_provider_init(p, NULL, NULL);
    CHECK(gemini_provider_build_request(p, "gemini-3.8-flash", hello, 1u, 384, "Be brief.", 80) == 0);
    CHECK_STR_EQ(p->body,
                 "{\"systemInstruction\":{\"parts\":[{\"text\":\"Be brief.\"}]},"
                 "\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"hi\"}]}],"
                 "\"generationConfig\":{\"maxOutputTokens\":1408,\"thinkingConfig\":{\"thinkingLevel\":\"LOW\"}}}");
    CHECK(gemini_provider_path("gemini-3.8-flash", path, sizeof(path)) == 0);
    CHECK_STR_EQ(path, "/v1beta/models/gemini-3.8-flash:streamGenerateContent?alt=sse");
    CHECK(gemini_provider_path("bad/model", path, sizeof(path)) == -1);
    CHECK(gemini_provider_path("x", path, 2u) == -1);
    CHECK(gemini_provider_build_request(p, "", hello, 1u, 8, NULL, 80) == -1);
    CHECK(gemini_provider_build_request(p, "gemini-3.8-flash", NULL, 1u, 8, NULL, 80) == -1);
}

static void check_reply(void) {
    sse_check_accepted(
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"thought\":true,\"text\":\"secret\"},{\"text\":\"Hel\"}]}}]}\n\n"
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"lo\"}]},\"finishReason\":\"STOP\"}]}\n\n", "Hello");
    sse_check_accepted("data: {\"candidates\":[{\"finishReason\":\"MAX_TOKENS\"}]}\n", "");
    sse_check_rejected("data: {\"promptFeedback\":{\"blockReason\":\"SAFETY\"}}\n", GEMINI_ERROR_PROMPT_BLOCKED, "");
    sse_check_rejected("data: {\"candidates\":[{\"finishReason\":\"SAFETY\"}]}\n", GEMINI_ERROR_REPLY_BLOCKED, "");
    sse_check_rejected("data: {\"error\":{\"message\":\"secret\"}}\n", GEMINI_ERROR_SERVER, "");
    sse_check_rejected("data: {\"candidates\":{}}\n", SSE_ERROR_MALFORMED, "");
    sse_check_rejected("data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"partial\"}]}}]}\n", SSE_ERROR_TRUNCATED, "partial");
}

void test_gemini_provider(void);
void test_gemini_provider(void) {
    check_request();
    check_reply();
}
