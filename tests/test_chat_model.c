#include <string.h>

#include "ai/chat_model.h"
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

void test_chat_model(void) {
    check_basic();
    check_composer_limits();
    check_oversized_response();
    check_partial_preservation();
    check_capacity_eviction();
    check_bound_eviction();
    check_reject_oversized_submission();
}
