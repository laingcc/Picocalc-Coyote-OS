#include <string.h>

#include "ai/chat_model.h"
#include "ai/ollama_provider.h"
#include "test_util.h"

/* Deterministic stand-in for a provider request measurement. */
static size_t measure(void *context, const chat_message_t *messages, size_t count, const char *pending,
                      size_t pending_length) {
    (void)context;
    (void)pending;
    size_t total = 10u;
    for (size_t i = 0; i < count; i++) {
        total += messages[i].length;
    }
    total += pending_length;
    return total;
}

static void run_turn(chat_model_t *model, const char *user, const char *assistant) {
    CHECK(chat_model_composer_append(model, user, strlen(user)) == 0);
    CHECK(chat_model_submit(model) == 0);
    CHECK(chat_model_append_response(model, assistant, strlen(assistant)) == 0);
    chat_model_complete_response(model);
}

static void check_basic(void) {
    chat_model_t model;
    chat_model_init(&model, measure, NULL, 1000u);
    CHECK(chat_model_message_count(&model) == 0);
    CHECK(chat_model_is_streaming(&model) == false);

    CHECK(chat_model_submit(&model) == -1); /* empty submission */

    CHECK(chat_model_composer_append(&model, "hello", 5) == 0);
    CHECK(chat_model_composer_length(&model) == 5);
    CHECK_STR_EQ(chat_model_composer_text(&model), "hello");
    CHECK(chat_model_submit(&model) == 0);
    CHECK(chat_model_message_count(&model) == 2);
    CHECK(chat_model_is_streaming(&model));

    CHECK(chat_model_message_at(&model, 0)->role == CHAT_ROLE_USER);
    CHECK_STR_EQ(chat_model_message_at(&model, 0)->text, "hello");
    CHECK(chat_model_message_at(&model, 0)->length == 5);
    CHECK(chat_model_message_at(&model, 0)->partial == false);
    CHECK(chat_model_message_at(&model, 1)->role == CHAT_ROLE_ASSISTANT);
    CHECK(chat_model_message_at(&model, 1)->length == 0);
    CHECK(chat_model_composer_length(&model) == 0);
    CHECK(chat_model_message_at(&model, 5) == NULL);

    /* Concurrent submission is rejected and preserves the composer. */
    CHECK(chat_model_composer_append(&model, "next", 4) == 0);
    CHECK(chat_model_submit(&model) == -1);
    CHECK(chat_model_message_count(&model) == 2);
    CHECK_STR_EQ(chat_model_composer_text(&model), "next");

    /* Streaming response appends. */
    CHECK(chat_model_append_response(&model, "wor", 3) == 0);
    CHECK(chat_model_append_response(&model, "ld", 2) == 0);
    CHECK_STR_EQ(chat_model_message_at(&model, 1)->text, "world");
    CHECK(chat_model_message_at(&model, 1)->length == 5);
    chat_model_complete_response(&model);
    CHECK(chat_model_is_streaming(&model) == false);
    CHECK(chat_model_message_at(&model, 1)->partial == false);

    /* Response appends outside streaming are rejected. */
    CHECK(chat_model_append_response(&model, "x", 1) == -1);

    chat_model_reset(&model);
    CHECK(chat_model_message_count(&model) == 0);
    CHECK(chat_model_composer_length(&model) == 0);
    CHECK_STR_EQ(chat_model_composer_text(&model), "");
}

static void check_composer_limits(void) {
    chat_model_t model;
    chat_model_init(&model, NULL, NULL, 0u);

    char big[CHAT_COMPOSER_MAX + 2u];
    memset(big, 'a', sizeof(big));
    CHECK(chat_model_composer_append(&model, big, CHAT_COMPOSER_MAX) == 0);
    CHECK(chat_model_composer_length(&model) == CHAT_COMPOSER_MAX);
    CHECK(chat_model_composer_append(&model, "x", 1) == -1);
    CHECK(chat_model_composer_length(&model) == CHAT_COMPOSER_MAX);
    CHECK(chat_model_composer_backspace(&model) == 0);
    CHECK(chat_model_composer_length(&model) == CHAT_COMPOSER_MAX - 1u);
    chat_model_composer_clear(&model);
    CHECK(chat_model_composer_length(&model) == 0);
    CHECK(chat_model_composer_backspace(&model) == -1);
}

static void check_oversized_response(void) {
    chat_model_t model;
    chat_model_init(&model, NULL, NULL, 0u);
    CHECK(chat_model_composer_append(&model, "q", 1) == 0);
    CHECK(chat_model_submit(&model) == 0);

    char chunk[CHAT_MESSAGE_MAX + 1u];
    memset(chunk, 'b', sizeof(chunk));
    CHECK(chat_model_append_response(&model, chunk, CHAT_MESSAGE_MAX) == 0);
    CHECK(chat_model_message_at(&model, 1)->length == CHAT_MESSAGE_MAX);
    CHECK(chat_model_append_response(&model, "c", 1) == -1);
    CHECK(chat_model_message_at(&model, 1)->length == CHAT_MESSAGE_MAX);
}

static void check_partial_preservation(void) {
    chat_model_t model;
    chat_model_init(&model, NULL, NULL, 0u);

    CHECK(chat_model_composer_append(&model, "q", 1) == 0);
    CHECK(chat_model_submit(&model) == 0);
    CHECK(chat_model_append_response(&model, "half", 4) == 0);
    chat_model_fail_response(&model);
    CHECK(chat_model_is_streaming(&model) == false);
    CHECK(chat_model_message_at(&model, 1)->partial);
    CHECK_STR_EQ(chat_model_message_at(&model, 1)->text, "half");

    chat_model_reset(&model);
    CHECK(chat_model_composer_append(&model, "q", 1) == 0);
    CHECK(chat_model_submit(&model) == 0);
    CHECK(chat_model_append_response(&model, "part", 4) == 0);
    chat_model_cancel_response(&model);
    CHECK(chat_model_message_at(&model, 1)->partial);
    CHECK_STR_EQ(chat_model_message_at(&model, 1)->text, "part");

    /* fail/cancel while idle is a no-op. */
    chat_model_fail_response(&model);
    chat_model_cancel_response(&model);
    CHECK(chat_model_is_streaming(&model) == false);
}

static void check_capacity_eviction(void) {
    chat_model_t model;
    chat_model_init(&model, measure, NULL, 100000u);

    run_turn(&model, "u1", "a1");
    run_turn(&model, "u2", "a2");
    run_turn(&model, "u3", "a3");
    run_turn(&model, "u4", "a4");
    CHECK(chat_model_message_count(&model) == 8);
    CHECK_STR_EQ(chat_model_message_at(&model, 0)->text, "u1");

    /* A fifth turn evicts the oldest complete turn. */
    run_turn(&model, "u5", "a5");
    CHECK(chat_model_message_count(&model) == 8);
    CHECK(chat_model_message_at(&model, 0)->role == CHAT_ROLE_USER);
    CHECK_STR_EQ(chat_model_message_at(&model, 0)->text, "u2");
    CHECK_STR_EQ(chat_model_message_at(&model, 2)->text, "u3");
    CHECK_STR_EQ(chat_model_message_at(&model, 4)->text, "u4");
    CHECK_STR_EQ(chat_model_message_at(&model, 6)->text, "u5");
    CHECK_STR_EQ(chat_model_message_at(&model, 7)->text, "a5");
}

static void check_bound_eviction(void) {
    chat_model_t model;
    /* measure = 10 + sum(lengths) + pending, bound 30. */
    chat_model_init(&model, measure, NULL, 30u);

    run_turn(&model, "aaa", "AAA"); /* projected 10 + 0 + 3 = 13 */
    run_turn(&model, "bbb", "BBB"); /* projected 10 + 6 + 3 = 19 */
    run_turn(&model, "ccc", "CCC"); /* projected 10 + 12 + 3 = 25 */
    CHECK(chat_model_message_count(&model) == 6);

    /* The fourth turn projects 10 + 18 + 3 = 31 and evicts the oldest turn. */
    run_turn(&model, "ddd", "DDD");
    CHECK(chat_model_message_count(&model) == 6);
    CHECK_STR_EQ(chat_model_message_at(&model, 0)->text, "bbb");
    CHECK_STR_EQ(chat_model_message_at(&model, 2)->text, "ccc");
    CHECK_STR_EQ(chat_model_message_at(&model, 4)->text, "ddd");
    CHECK_STR_EQ(chat_model_message_at(&model, 5)->text, "DDD");
}

static void check_reject_oversized_submission(void) {
    chat_model_t model;
    chat_model_init(&model, measure, NULL, 2u);
    CHECK(chat_model_composer_append(&model, "x", 1) == 0);
    CHECK(chat_model_submit(&model) == -1);
    CHECK(chat_model_message_count(&model) == 0);
    CHECK(chat_model_is_streaming(&model) == false);
    CHECK_STR_EQ(chat_model_composer_text(&model), "x");
}

static void check_null_preconditions(void) {
    chat_model_t model;
    chat_model_init(&model, NULL, NULL, 0u);

    /* A NULL model is tolerated everywhere. */
    chat_model_init(NULL, NULL, NULL, 0u);
    chat_model_reset(NULL);
    chat_model_composer_clear(NULL);
    chat_model_complete_response(NULL);
    chat_model_fail_response(NULL);
    chat_model_cancel_response(NULL);
    CHECK(chat_model_composer_text(NULL) == NULL);
    CHECK(chat_model_composer_length(NULL) == 0u);
    CHECK(chat_model_message_count(NULL) == 0u);
    CHECK(chat_model_message_at(NULL, 0u) == NULL);
    CHECK(chat_model_is_streaming(NULL) == false);

    CHECK(chat_model_composer_append(NULL, "x", 1u) == -1);
    CHECK(chat_model_composer_append(&model, NULL, 1u) == -1);
    CHECK(chat_model_composer_append(&model, NULL, 0u) == 0);
    CHECK(chat_model_composer_backspace(NULL) == -1);
    CHECK(chat_model_submit(NULL) == -1);
    CHECK(chat_model_append_response(NULL, "x", 1u) == -1);
    CHECK(chat_model_append_response(&model, NULL, 1u) == -1);
}

/*
 * Real provider measurement: every candidate transcript is serialised through
 * ollama_provider_build_request so escaping expansion (each '"' becomes \" ) is
 * accounted for exactly as in firmware.  Both the provider and the converted
 * message array are static: they are large and must never be stack locals.
 */
static size_t provider_measure(void *context,
                               const chat_message_t *messages,
                               size_t count,
                               const char *pending,
                               size_t pending_length) {
    (void)context;
    static ollama_provider_t provider;
    static ollama_message_t converted[CHAT_MAX_MESSAGES + 1u];

    size_t n = 0u;
    for (size_t i = 0; i < count; i++) {
        converted[n].role = messages[i].role == CHAT_ROLE_USER ? "user" : "assistant";
        converted[n].content = messages[i].text;
        n++;
    }
    if (pending != NULL && pending_length > 0u) {
        converted[n].role = "user";
        converted[n].content = pending;
        n++;
    }
    if (n > OLLAMA_MAX_MESSAGES) {
        return (size_t)-1; /* cannot be serialised at all */
    }
    if (ollama_provider_build_request(&provider, "m", converted, n, 8, "", 80) != 0) {
        return (size_t)-1; /* request does not fit the fixed buffer */
    }
    return ollama_provider_request_length(&provider);
}

static void check_provider_integration_eviction(void) {
    chat_model_t model;
    chat_model_init(&model, provider_measure, NULL, OLLAMA_REQUEST_MAX);
    CHECK(chat_model_message_count(&model) == 0u);

    /* Each turn is 199 quotes plus a distinct leading letter; escaping doubles
     * the quotes, so only a couple of turns fit inside OLLAMA_REQUEST_MAX. */
    for (int turn = 0; turn < 5; turn++) {
        char text[200];
        memset(text, '"', sizeof(text));
        text[0] = (char)('a' + turn);
        CHECK(chat_model_composer_append(&model, text, sizeof(text)) == 0);
        CHECK(chat_model_submit(&model) == 0);
        CHECK(chat_model_append_response(&model, text, sizeof(text)) == 0);
        chat_model_complete_response(&model);

        CHECK(chat_model_message_count(&model) <= CHAT_MAX_MESSAGES);
        /* The retained transcript must still serialise inside the bound. */
        CHECK(provider_measure(NULL, model.messages, model.message_count, NULL, 0u) <= OLLAMA_REQUEST_MAX);

        /* The third turn already forces size-based eviction down to two turns
         * (four messages); the eight-message capacity cap has not yet applied. */
        if (turn >= 2) {
            CHECK(chat_model_message_count(&model) == 4u);
        }
    }

    /* Five turns cannot fit; the oldest turns were evicted, leaving the two
     * most recent (turns 3 and 4, whose leading letters are 'd' and 'e'). */
    CHECK(chat_model_message_count(&model) == 4u);
    CHECK(chat_model_message_at(&model, 0)->role == CHAT_ROLE_USER);
    CHECK(chat_model_message_at(&model, 0)->text[0] == 'd');
    CHECK(chat_model_message_at(&model, 2)->text[0] == 'e');
}

static void check_append_message(void) {
    chat_model_t model;
    chat_model_init(&model, NULL, NULL, 0u);

    /* NULL model rejected */
    CHECK(chat_model_append_message(NULL, CHAT_ROLE_USER, "hi", 2u) == -1);

    /* NULL text with non-zero length rejected */
    CHECK(chat_model_append_message(&model, CHAT_ROLE_USER, NULL, 5u) == -1);

    /* NULL text with zero length accepted */
    CHECK(chat_model_append_message(&model, CHAT_ROLE_USER, NULL, 0u) == 0);
    CHECK(chat_model_message_count(&model) == 1u);
    CHECK_STR_EQ(chat_model_message_at(&model, 0)->text, "");
    CHECK(chat_model_message_at(&model, 0)->length == 0u);
    CHECK(chat_model_message_at(&model, 0)->role == CHAT_ROLE_USER);
    CHECK(chat_model_message_at(&model, 0)->partial == false);

    /* Invalid role rejected */
    CHECK(chat_model_append_message(&model, (chat_role_t)99, "x", 1u) == -1);

    /* Normal append works and preserves role and content */
    CHECK(chat_model_append_message(&model, CHAT_ROLE_ASSISTANT, "hello world", 11u) == 0);
    CHECK(chat_model_message_count(&model) == 2u);
    CHECK(chat_model_message_at(&model, 1)->role == CHAT_ROLE_ASSISTANT);
    CHECK_STR_EQ(chat_model_message_at(&model, 1)->text, "hello world");
    CHECK(chat_model_message_at(&model, 1)->length == 11u);
    CHECK(chat_model_message_at(&model, 1)->partial == false);

    /* Composer is untouched */
    CHECK_STR_EQ(chat_model_composer_text(&model), "");
    CHECK(chat_model_composer_append(&model, "comp", 4u) == 0);
    CHECK(chat_model_append_message(&model, CHAT_ROLE_USER, "user2", 5u) == 0);
    CHECK_STR_EQ(chat_model_composer_text(&model), "comp");

    /* Oversized message rejected */
    static char overlong[CHAT_MESSAGE_MAX + 2u];
    memset(overlong, 'a', sizeof(overlong));
    CHECK(chat_model_append_message(&model, CHAT_ROLE_USER, overlong, CHAT_MESSAGE_MAX + 1u) == -1);
    CHECK(chat_model_message_count(&model) == 3u);

    /* Fill to capacity (8 messages total, 3 currently) */
    CHECK(chat_model_append_message(&model, CHAT_ROLE_ASSISTANT, "m3", 2u) == 0);
    CHECK(chat_model_append_message(&model, CHAT_ROLE_USER, "m4", 2u) == 0);
    CHECK(chat_model_append_message(&model, CHAT_ROLE_ASSISTANT, "m5", 2u) == 0);
    CHECK(chat_model_append_message(&model, CHAT_ROLE_USER, "m6", 2u) == 0);
    CHECK(chat_model_append_message(&model, CHAT_ROLE_ASSISTANT, "m7", 2u) == 0);
    CHECK(chat_model_message_count(&model) == CHAT_MAX_MESSAGES);

    /* Exceeding capacity rejected */
    CHECK(chat_model_append_message(&model, CHAT_ROLE_ASSISTANT, "overflow", 8u) == -1);
    CHECK(chat_model_message_count(&model) == CHAT_MAX_MESSAGES);

    /* Reject while streaming */
    chat_model_reset(&model);
    CHECK(chat_model_composer_append(&model, "q", 1u) == 0);
    CHECK(chat_model_submit(&model) == 0);
    CHECK(chat_model_is_streaming(&model));
    CHECK(chat_model_append_message(&model, CHAT_ROLE_USER, "blocked", 7u) == -1);
    chat_model_cancel_response(&model);
}

void test_chat_model(void) {
    check_basic();
    check_composer_limits();
    check_oversized_response();
    check_partial_preservation();
    check_capacity_eviction();
    check_bound_eviction();
    check_reject_oversized_submission();
    check_null_preconditions();
    check_provider_integration_eviction();
    check_append_message();
}
