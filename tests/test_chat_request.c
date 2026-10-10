#include <string.h>

#include "ai/chat_request.h"
#include "test_util.h"

/* Both are far too large for the stack. */
static chat_model_t model;
static chat_request_measure_t measure;
static ollama_provider_t reference;

static void set_message(chat_message_t *message, chat_role_t role, const char *text) {
    memset(message, 0, sizeof(*message));
    message->role = role;
    message->length = strlen(text);
    memcpy(message->text, text, message->length + 1u);
}

static void check_roles(void) {
    CHECK_STR_EQ(chat_request_role_name(CHAT_ROLE_USER), "user");
    CHECK_STR_EQ(chat_request_role_name(CHAT_ROLE_ASSISTANT), "assistant");
}

static void check_convert_points_into_messages(void) {
    static chat_message_t messages[3];
    ollama_message_t out[4];

    set_message(&messages[0], CHAT_ROLE_USER, "hi");
    set_message(&messages[1], CHAT_ROLE_ASSISTANT, "hello");
    set_message(&messages[2], CHAT_ROLE_USER, "again");

    CHECK(chat_request_convert(messages, 3u, NULL, out, 4u) == 3u);
    CHECK_STR_EQ(out[0].role, "user");
    CHECK_STR_EQ(out[1].role, "assistant");
    CHECK_STR_EQ(out[2].role, "user");
    /* No copies: the content is the stored text itself. */
    CHECK(out[0].content == messages[0].text);
    CHECK(out[1].content == messages[1].text);
    CHECK(out[2].content == messages[2].text);

    const char *pending = "next";
    CHECK(chat_request_convert(messages, 3u, pending, out, 4u) == 4u);
    CHECK_STR_EQ(out[3].role, "user");
    CHECK(out[3].content == pending);

    CHECK(chat_request_convert(NULL, 0u, pending, out, 4u) == 1u);
    CHECK(out[0].content == pending);
    CHECK(chat_request_convert(NULL, 0u, NULL, out, 4u) == 0u);
}

static void check_convert_skips_empty_replies(void) {
    static chat_message_t messages[4];
    ollama_message_t out[4];

    set_message(&messages[0], CHAT_ROLE_USER, "one");
    set_message(&messages[1], CHAT_ROLE_ASSISTANT, ""); /* failed before any content */
    messages[1].partial = true;
    set_message(&messages[2], CHAT_ROLE_USER, "two");
    set_message(&messages[3], CHAT_ROLE_ASSISTANT, ""); /* in flight */

    CHECK(chat_request_convert(messages, 4u, NULL, out, 4u) == 2u);
    CHECK(out[0].content == messages[0].text);
    CHECK(out[1].content == messages[2].text);
    CHECK_STR_EQ(out[1].role, "user");
}

static void check_convert_rejects(void) {
    static chat_message_t messages[3];
    ollama_message_t out[3];

    set_message(&messages[0], CHAT_ROLE_USER, "a");
    set_message(&messages[1], CHAT_ROLE_ASSISTANT, "b");
    set_message(&messages[2], CHAT_ROLE_USER, "c");

    CHECK(chat_request_convert(messages, 3u, NULL, out, 2u) == CHAT_REQUEST_UNMEASURABLE);
    CHECK(chat_request_convert(messages, 3u, "d", out, 3u) == CHAT_REQUEST_UNMEASURABLE);
    CHECK(chat_request_convert(messages, 3u, NULL, out, 3u) == 3u);
    CHECK(chat_request_convert(NULL, 1u, NULL, out, 3u) == CHAT_REQUEST_UNMEASURABLE);
    CHECK(chat_request_convert(messages, 1u, NULL, NULL, 3u) == CHAT_REQUEST_UNMEASURABLE);
}

static void check_measure_matches_real_request(void) {
    static chat_message_t messages[2];
    ai_config_t config;
    ai_config_init(&config);
    memcpy(config.model, "llama3.2", sizeof("llama3.2"));
    config.max_predict = 64u;
    chat_request_measure_init(&measure, &config);

    const char *expected =
        "{\"model\":\"llama3.2\",\"messages\":[{\"role\":\"system\",\"content\":\"" AI_CONFIG_DEFAULT_SYSTEM_PROMPT "\"},"
        "{\"role\":\"user\",\"content\":\"hi\"}],"
        "\"stream\":true,\"options\":{\"num_predict\":64,\"temperature\":0.80}}";
    CHECK(chat_request_measure(&measure, NULL, 0u, "hi", 2u) == strlen(expected));

    /* Escaping counts: the measure is the serialised size, not the text size. */
    set_message(&messages[0], CHAT_ROLE_USER, "say \"x\"\n");
    set_message(&messages[1], CHAT_ROLE_ASSISTANT, "x");
    ollama_message_t direct[3] = {{"user", messages[0].text}, {"assistant", messages[1].text}, {"user", "ok"}};
    ollama_provider_init(&reference, NULL, NULL);
    CHECK(ollama_provider_build_request(&reference, "llama3.2", direct, 3u, 64, config.system_prompt, 80) == 0);
    CHECK(chat_request_measure(&measure, messages, 2u, "ok", 2u) ==
          ollama_provider_request_length(&reference));

    /* The model name and max_predict are read at measure time. */
    size_t before = chat_request_measure(&measure, NULL, 0u, "hi", 2u);
    memcpy(config.model, "llama3.2:1b", sizeof("llama3.2:1b"));
    CHECK(chat_request_measure(&measure, NULL, 0u, "hi", 2u) == before + 3u);
    config.max_predict = 4096u;
    CHECK(chat_request_measure(&measure, NULL, 0u, "hi", 2u) == before + 3u + 2u);
}

static void check_measure_unmeasurable(void) {
    static chat_message_t messages[OLLAMA_MAX_MESSAGES];
    static char big[OLLAMA_REQUEST_MAX + 1u];
    ai_config_t config;
    ai_config_init(&config);
    chat_request_measure_init(&measure, &config);

    /* No model configured. */
    CHECK(chat_request_measure(&measure, NULL, 0u, "hi", 2u) == CHAT_REQUEST_UNMEASURABLE);
    memcpy(config.model, "m", 2u);
    CHECK(chat_request_measure(&measure, NULL, 0u, "hi", 2u) != CHAT_REQUEST_UNMEASURABLE);

    /* Pending text that is not terminated where the length says. */
    CHECK(chat_request_measure(&measure, NULL, 0u, "hi", 1u) == CHAT_REQUEST_UNMEASURABLE);

    /* Larger than the request buffer. */
    memset(big, 'a', OLLAMA_REQUEST_MAX);
    big[OLLAMA_REQUEST_MAX] = '\0';
    CHECK(chat_request_measure(&measure, NULL, 0u, big, OLLAMA_REQUEST_MAX) == CHAT_REQUEST_UNMEASURABLE);

    /* More messages than one request can carry. */
    for (size_t i = 0u; i < OLLAMA_MAX_MESSAGES; i++) {
        set_message(&messages[i], (i % 2u) == 0u ? CHAT_ROLE_USER : CHAT_ROLE_ASSISTANT, "x");
    }
    CHECK(chat_request_measure(&measure, messages, OLLAMA_MAX_MESSAGES, NULL, 0u) != CHAT_REQUEST_UNMEASURABLE);
    CHECK(chat_request_measure(&measure, messages, OLLAMA_MAX_MESSAGES, "y", 1u) == CHAT_REQUEST_UNMEASURABLE);

    CHECK(chat_request_measure(NULL, NULL, 0u, "hi", 2u) == CHAT_REQUEST_UNMEASURABLE);
    chat_request_measure_init(&measure, NULL);
    CHECK(chat_request_measure(&measure, NULL, 0u, "hi", 2u) == CHAT_REQUEST_UNMEASURABLE);
}

/* Drive the real chat model with the real measure: every request it lets
 * through must build, and old turns must go once the bound is reached. */
static void check_model_evicts_until_request_fits(void) {
    static char text[400];
    ollama_message_t converted[OLLAMA_MAX_MESSAGES];
    ai_config_t config;
    ai_config_init(&config);
    memcpy(config.model, "llama3.2", sizeof("llama3.2"));
    chat_request_measure_init(&measure, &config);
    chat_model_init(&model, chat_request_measure, &measure, OLLAMA_REQUEST_MAX);

    memset(text, 'q', sizeof(text) - 1u);
    text[sizeof(text) - 1u] = '\0';

    int evicted = 0;
    for (int turn = 0; turn < 8; turn++) {
        size_t before = chat_model_message_count(&model);
        CHECK(chat_model_composer_append(&model, text, strlen(text)) == 0);
        CHECK(chat_model_submit(&model) == 0);
        if (chat_model_message_count(&model) < before + 2u) {
            evicted = 1;
        }

        size_t count = chat_request_convert(chat_model_message_at(&model, 0u), chat_model_message_count(&model),
                                            NULL, converted, OLLAMA_MAX_MESSAGES);
        CHECK(count != CHAT_REQUEST_UNMEASURABLE);
        CHECK(count == chat_model_message_count(&model) - 1u); /* the empty reply is not sent */
        CHECK_STR_EQ(converted[count - 1u].role, "user");
        ollama_provider_init(&reference, NULL, NULL);
        CHECK(ollama_provider_build_request(&reference, config.model, converted, count,
                                            (int)config.max_predict, "", 80) == 0);
        CHECK(ollama_provider_request_length(&reference) <= OLLAMA_REQUEST_MAX);

        CHECK(chat_model_append_response(&model, text, strlen(text)) == 0);
        chat_model_complete_response(&model);
    }
    CHECK(evicted == 1);

    /* A turn that cannot fit even alone is refused and the composer kept.
     * The composer is smaller than the request, so only text that grows when
     * escaped can get there. */
    static char control[CHAT_COMPOSER_MAX + 1u];
    memset(control, 0x01, CHAT_COMPOSER_MAX);
    chat_model_reset(&model);
    CHECK(chat_model_composer_append(&model, control, CHAT_COMPOSER_MAX) == 0);
    size_t held = chat_model_composer_length(&model);
    CHECK(held == CHAT_COMPOSER_MAX);
    CHECK(chat_model_submit(&model) == -1);
    CHECK(chat_model_composer_length(&model) == held);
    CHECK(chat_model_message_count(&model) == 0u);

    /* Without a model nothing can be sent. */
    config.model[0] = '\0';
    chat_model_composer_clear(&model);
    chat_model_composer_append(&model, "hi", 2u);
    CHECK(chat_model_submit(&model) == -1);
}

void test_chat_request(void) {
    check_roles();
    check_convert_points_into_messages();
    check_convert_skips_empty_replies();
    check_convert_rejects();
    check_measure_matches_real_request();
    check_measure_unmeasurable();
    check_model_evicts_until_request_fits();
}
