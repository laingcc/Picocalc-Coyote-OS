#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ai/conv_store.h"
#include "test_util.h"

static const char *const test_dir = ".";
static char file_text[16384];

/* ---- rename double --------------------------------------------------------
 * conv_store.c is compiled with rename mapped to this function. */

static int rename_calls;
static int rename_fail_call;
static int rename_fail_all;
static int rename_like_fat;

static int exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

int conv_store_test_rename(const char *old_path, const char *new_path);
int conv_store_test_rename(const char *old_path, const char *new_path) {
    rename_calls++;
    if (rename_fail_all || rename_calls == rename_fail_call) {
        errno = EIO;
        return -1;
    }
    if (rename_like_fat && exists(new_path)) {
        errno = EEXIST;
        return -1;
    }
    return rename(old_path, new_path);
}

static void reset_rename(void) {
    rename_calls = 0;
    rename_fail_call = 0;
    rename_fail_all = 0;
    rename_like_fat = 0;
}

/* ---- helpers --------------------------------------------------------------- */

static void clean(void) {
    remove(CONV_STORE_FILE_NAME);
    remove(CONV_STORE_TEMP_NAME);
    remove(CONV_STORE_BACKUP_NAME);
    reset_rename();
}

static void write_raw_file(const char *name, const char *text, size_t length) {
    FILE *file = fopen(name, "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        CHECK(fwrite(text, 1u, length, file) == length);
        CHECK(fclose(file) == 0);
    }
}

static const char *read_raw_file(const char *name) {
    FILE *file = fopen(name, "rb");
    size_t length = 0u;
    if (file != NULL) {
        length = fread(file_text, 1u, sizeof(file_text) - 1u, file);
        fclose(file);
    }
    file_text[length] = '\0';
    return file_text;
}

/* ---- test cases ------------------------------------------------------------ */

static void check_arguments(void) {
    static char long_dir[CONV_STORE_PATH_CAPACITY + 1u];
    chat_model_t model;
    char titles[CONV_STORE_MAX_CONVERSATIONS][CONV_STORE_TITLE_MAX + 1u];
    size_t count = 0u;

    clean();
    memset(long_dir, 'd', sizeof(long_dir) - 1u);
    chat_model_init(&model, NULL, NULL, 0u);
    CHECK(chat_model_append_message(&model, CHAT_ROLE_USER, "msg", 3u) == 0);

    /* NULL / invalid arguments for list */
    CHECK(conv_store_list(NULL, titles, &count, 8u) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_list(test_dir, NULL, &count, 8u) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_list(test_dir, titles, NULL, 8u) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_list("", titles, &count, 8u) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_list(long_dir, titles, &count, 8u) == CONV_STORE_INVALID_ARGUMENT);

    /* NULL / invalid arguments for save */
    CHECK(conv_store_save(NULL, "title", test_dir) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_save(&model, NULL, test_dir) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_save(&model, "", test_dir) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_save(&model, "title", NULL) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_save(&model, "title", "") == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_save(&model, "title", long_dir) == CONV_STORE_INVALID_ARGUMENT);

    /* NULL / invalid arguments for load */
    CHECK(conv_store_load(NULL, "title", test_dir) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_load(&model, NULL, test_dir) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_load(&model, "", test_dir) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_load(&model, "title", NULL) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_load(&model, "title", "") == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_load(&model, "title", long_dir) == CONV_STORE_INVALID_ARGUMENT);

    /* NULL / invalid arguments for delete */
    CHECK(conv_store_delete(NULL, test_dir) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_delete("", test_dir) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_delete("title", NULL) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_delete("title", "") == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_delete("title", long_dir) == CONV_STORE_INVALID_ARGUMENT);

    CHECK(!exists(CONV_STORE_FILE_NAME));
}

static void check_empty_transcript_save_rejected(void) {
    chat_model_t model;

    clean();
    chat_model_init(&model, NULL, NULL, 0u);
    CHECK(chat_model_message_count(&model) == 0u);

    /* Saving empty transcript rejected */
    CHECK(conv_store_save(&model, "Empty", test_dir) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(!exists(CONV_STORE_FILE_NAME));
    CHECK(!exists(CONV_STORE_TEMP_NAME));
}

static void check_round_trip(void) {
    chat_model_t saved;
    chat_model_t loaded;

    clean();
    chat_model_init(&saved, NULL, NULL, 0u);
    chat_model_init(&loaded, NULL, NULL, 0u);

    /* Message 0: multi-line user message */
    const char *msg0 = "Hello!\nThis is a multi-line\nmessage with newlines.";
    CHECK(chat_model_append_message(&saved, CHAT_ROLE_USER, msg0, strlen(msg0)) == 0);

    /* Message 1: assistant message with partial flag */
    const char *msg1 = "Line one\nLine two\nIncomplete reply...";
    CHECK(chat_model_append_message(&saved, CHAT_ROLE_ASSISTANT, msg1, strlen(msg1)) == 0);
    saved.messages[1].partial = true;

    /* Message 2: single-line user message */
    const char *msg2 = "Continue please.";
    CHECK(chat_model_append_message(&saved, CHAT_ROLE_USER, msg2, strlen(msg2)) == 0);

    /* Message 3: assistant message ending with newline */
    const char *msg3 = "Here is the rest.\n";
    CHECK(chat_model_append_message(&saved, CHAT_ROLE_ASSISTANT, msg3, strlen(msg3)) == 0);

    CHECK(conv_store_save(&saved, "My Conversation", test_dir) == CONV_STORE_OK);
    CHECK(exists(CONV_STORE_FILE_NAME));
    CHECK(!exists(CONV_STORE_TEMP_NAME));
    CHECK(!exists(CONV_STORE_BACKUP_NAME));

    /* Verify file content contains markers */
    const char *content = read_raw_file(CONV_STORE_FILE_NAME);
    CHECK(strstr(content, "[conversation]\ntitle=My Conversation\n") != NULL);
    CHECK(strstr(content, "partial=1\n") != NULL);

    /* Load into loaded model */
    CHECK(conv_store_load(&loaded, "My Conversation", test_dir) == CONV_STORE_OK);
    CHECK(chat_model_message_count(&loaded) == 4u);

    /* Verify all messages, roles, order, newlines, and partial flag */
    const chat_message_t *m0 = chat_model_message_at(&loaded, 0);
    CHECK(m0 != NULL);
    CHECK(m0->role == CHAT_ROLE_USER);
    CHECK_STR_EQ(m0->text, msg0);
    CHECK(m0->length == strlen(msg0));
    CHECK(m0->partial == false);

    const chat_message_t *m1 = chat_model_message_at(&loaded, 1);
    CHECK(m1 != NULL);
    CHECK(m1->role == CHAT_ROLE_ASSISTANT);
    CHECK_STR_EQ(m1->text, msg1);
    CHECK(m1->length == strlen(msg1));
    CHECK(m1->partial == true);

    const chat_message_t *m2 = chat_model_message_at(&loaded, 2);
    CHECK(m2 != NULL);
    CHECK(m2->role == CHAT_ROLE_USER);
    CHECK_STR_EQ(m2->text, msg2);
    CHECK(m2->length == strlen(msg2));
    CHECK(m2->partial == false);

    const chat_message_t *m3 = chat_model_message_at(&loaded, 3);
    CHECK(m3 != NULL);
    CHECK(m3->role == CHAT_ROLE_ASSISTANT);
    CHECK_STR_EQ(m3->text, msg3);
    CHECK(m3->length == strlen(msg3));
    CHECK(m3->partial == false);
}

static void check_list_order_and_count(void) {
    chat_model_t m;
    char titles[CONV_STORE_MAX_CONVERSATIONS][CONV_STORE_TITLE_MAX + 1u];
    size_t count = 99u;

    clean();
    /* List missing file returns OK with count = 0 */
    CHECK(conv_store_list(test_dir, titles, &count, 8u) == CONV_STORE_OK);
    CHECK(count == 0u);

    /* Save 3 conversations */
    chat_model_init(&m, NULL, NULL, 0u);
    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "one", 3u) == 0);
    CHECK(conv_store_save(&m, "First", test_dir) == CONV_STORE_OK);

    chat_model_reset(&m);
    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "two", 3u) == 0);
    CHECK(conv_store_save(&m, "Second", test_dir) == CONV_STORE_OK);

    chat_model_reset(&m);
    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "three", 5u) == 0);
    CHECK(conv_store_save(&m, "Third", test_dir) == CONV_STORE_OK);

    CHECK(conv_store_list(test_dir, titles, &count, 8u) == CONV_STORE_OK);
    CHECK(count == 3u);
    CHECK_STR_EQ(titles[0], "First");
    CHECK_STR_EQ(titles[1], "Second");
    CHECK_STR_EQ(titles[2], "Third");

    /* Truncated capacity in list */
    count = 0u;
    CHECK(conv_store_list(test_dir, titles, &count, 2u) == CONV_STORE_OK);
    CHECK(count == 3u);
    CHECK_STR_EQ(titles[0], "First");
    CHECK_STR_EQ(titles[1], "Second");
}

static void check_replace_on_same_title(void) {
    chat_model_t m;
    chat_model_t loaded;
    char titles[CONV_STORE_MAX_CONVERSATIONS][CONV_STORE_TITLE_MAX + 1u];
    size_t count = 0u;

    clean();
    chat_model_init(&m, NULL, NULL, 0u);
    chat_model_init(&loaded, NULL, NULL, 0u);

    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "old prompt", 10u) == 0);
    CHECK(conv_store_save(&m, "Chat A", test_dir) == CONV_STORE_OK);

    chat_model_reset(&m);
    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "other conv", 10u) == 0);
    CHECK(conv_store_save(&m, "Chat B", test_dir) == CONV_STORE_OK);

    CHECK(conv_store_list(test_dir, titles, &count, 8u) == CONV_STORE_OK);
    CHECK(count == 2u);

    /* Replace "Chat A" with new transcript */
    chat_model_reset(&m);
    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "new prompt", 10u) == 0);
    CHECK(chat_model_append_message(&m, CHAT_ROLE_ASSISTANT, "new reply", 9u) == 0);
    CHECK(conv_store_save(&m, "Chat A", test_dir) == CONV_STORE_OK);

    /* Count should still be 2 */
    CHECK(conv_store_list(test_dir, titles, &count, 8u) == CONV_STORE_OK);
    CHECK(count == 2u);
    CHECK_STR_EQ(titles[0], "Chat A");
    CHECK_STR_EQ(titles[1], "Chat B");

    /* Loading "Chat A" gets the replaced transcript */
    CHECK(conv_store_load(&loaded, "Chat A", test_dir) == CONV_STORE_OK);
    CHECK(chat_model_message_count(&loaded) == 2u);
    CHECK_STR_EQ(chat_model_message_at(&loaded, 0)->text, "new prompt");
    CHECK_STR_EQ(chat_model_message_at(&loaded, 1)->text, "new reply");
}

static void check_delete_operations(void) {
    chat_model_t m;
    chat_model_t loaded;
    char titles[CONV_STORE_MAX_CONVERSATIONS][CONV_STORE_TITLE_MAX + 1u];
    size_t count = 0u;

    clean();
    chat_model_init(&m, NULL, NULL, 0u);
    chat_model_init(&loaded, NULL, NULL, 0u);

    /* Delete on missing file returns NOT_FOUND */
    CHECK(conv_store_delete("Anything", test_dir) == CONV_STORE_NOT_FOUND);

    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "a", 1u) == 0);
    CHECK(conv_store_save(&m, "A", test_dir) == CONV_STORE_OK);
    chat_model_reset(&m);
    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "b", 1u) == 0);
    CHECK(conv_store_save(&m, "B", test_dir) == CONV_STORE_OK);
    chat_model_reset(&m);
    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "c", 1u) == 0);
    CHECK(conv_store_save(&m, "C", test_dir) == CONV_STORE_OK);

    /* Delete missing title returns NOT_FOUND and does not affect file */
    CHECK(conv_store_delete("NonExistent", test_dir) == CONV_STORE_NOT_FOUND);
    CHECK(conv_store_list(test_dir, titles, &count, 8u) == CONV_STORE_OK);
    CHECK(count == 3u);

    /* Delete "B" removes only "B" */
    CHECK(conv_store_delete("B", test_dir) == CONV_STORE_OK);
    CHECK(conv_store_list(test_dir, titles, &count, 8u) == CONV_STORE_OK);
    CHECK(count == 2u);
    CHECK_STR_EQ(titles[0], "A");
    CHECK_STR_EQ(titles[1], "C");

    CHECK(conv_store_load(&loaded, "A", test_dir) == CONV_STORE_OK);
    CHECK(chat_model_message_count(&loaded) == 1u);
    CHECK_STR_EQ(chat_model_message_at(&loaded, 0)->text, "a");

    CHECK(conv_store_load(&loaded, "C", test_dir) == CONV_STORE_OK);
    CHECK(chat_model_message_count(&loaded) == 1u);
    CHECK_STR_EQ(chat_model_message_at(&loaded, 0)->text, "c");

    /* "B" is now NOT_FOUND */
    CHECK(conv_store_load(&loaded, "B", test_dir) == CONV_STORE_NOT_FOUND);
    CHECK(chat_model_message_count(&loaded) == 0u);
}

static void check_load_missing_title(void) {
    chat_model_t loaded;

    clean();
    chat_model_init(&loaded, NULL, NULL, 0u);
    CHECK(chat_model_append_message(&loaded, CHAT_ROLE_USER, "prev", 4u) == 0);

    /* On missing file, resets model and returns NOT_FOUND */
    CHECK(conv_store_load(&loaded, "Missing", test_dir) == CONV_STORE_NOT_FOUND);
    CHECK(chat_model_message_count(&loaded) == 0u);

    /* Save a conversation */
    CHECK(chat_model_append_message(&loaded, CHAT_ROLE_USER, "present", 7u) == 0);
    CHECK(conv_store_save(&loaded, "Existing", test_dir) == CONV_STORE_OK);

    /* Now load non-existent title */
    CHECK(conv_store_load(&loaded, "Other", test_dir) == CONV_STORE_NOT_FOUND);
    CHECK(chat_model_message_count(&loaded) == 0u);
}

static void check_eight_conversation_cap(void) {
    chat_model_t m;
    char titles[CONV_STORE_MAX_CONVERSATIONS][CONV_STORE_TITLE_MAX + 1u];
    size_t count = 0u;

    clean();
    chat_model_init(&m, NULL, NULL, 0u);
    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "msg", 3u) == 0);

    /* Save 8 conversations (CONV_STORE_MAX_CONVERSATIONS) */
    char name[16];
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "Conv %d", i);
        CHECK(conv_store_save(&m, name, test_dir) == CONV_STORE_OK);
    }

    CHECK(conv_store_list(test_dir, titles, &count, 8u) == CONV_STORE_OK);
    CHECK(count == 8u);

    /* Saving 9th distinct conversation returns CONV_STORE_FULL */
    CHECK(conv_store_save(&m, "Conv 8", test_dir) == CONV_STORE_FULL);

    /* Existing file is intact, count is still 8 */
    CHECK(conv_store_list(test_dir, titles, &count, 8u) == CONV_STORE_OK);
    CHECK(count == 8u);

    /* But replacing one of the 8 conversations succeeds */
    CHECK(conv_store_save(&m, "Conv 0", test_dir) == CONV_STORE_OK);
    CHECK(conv_store_list(test_dir, titles, &count, 8u) == CONV_STORE_OK);
    CHECK(count == 8u);
}

static void check_size_bounds(void) {
    chat_model_t m;
    chat_model_init(&m, NULL, NULL, 0u);

    clean();
    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "valid", 5u) == 0);
    CHECK(conv_store_save(&m, "Valid", test_dir) == CONV_STORE_OK);

    /* Message exceeding CHAT_MESSAGE_MAX cannot be saved */
    chat_model_t big_msg;
    chat_model_init(&big_msg, NULL, NULL, 0u);
    big_msg.message_count = 1u;
    big_msg.messages[0].role = CHAT_ROLE_USER;
    big_msg.messages[0].length = CHAT_MESSAGE_MAX + 1u;
    CHECK(conv_store_save(&big_msg, "Oversized", test_dir) == CONV_STORE_TOO_LARGE);

    /* Existing file is intact */
    chat_model_t loaded;
    chat_model_init(&loaded, NULL, NULL, 0u);
    CHECK(conv_store_load(&loaded, "Valid", test_dir) == CONV_STORE_OK);
    CHECK_STR_EQ(chat_model_message_at(&loaded, 0)->text, "valid");

    /* Total conversation bytes exceeding bound rejected */
    chat_model_t big_conv;
    chat_model_init(&big_conv, NULL, NULL, 0u);
    big_conv.message_count = CHAT_MAX_MESSAGES;
    for (size_t i = 0; i < CHAT_MAX_MESSAGES; i++) {
        big_conv.messages[i].role = (i % 2 == 0) ? CHAT_ROLE_USER : CHAT_ROLE_ASSISTANT;
        big_conv.messages[i].length = CHAT_MESSAGE_MAX;
    }
    /* Exactly at cap is allowed */
    CHECK(conv_store_save(&big_conv, "MaxConv", test_dir) == CONV_STORE_OK);
    /* Exceeding total byte cap rejected */
    big_conv.messages[0].length = CHAT_MESSAGE_MAX + 1u;
    CHECK(conv_store_save(&big_conv, "OverConv", test_dir) == CONV_STORE_TOO_LARGE);
}

static void check_corrupt_files(void) {
    chat_model_t model;
    chat_model_init(&model, NULL, NULL, 0u);

    /* Corrupt 1: Missing title */
    static const char corrupt1[] = "[conversation]\n[message]\nrole=user\ntext=hi\n";
    clean();
    write_raw_file(CONV_STORE_FILE_NAME, corrupt1, sizeof(corrupt1) - 1u);
    CHECK(chat_model_append_message(&model, CHAT_ROLE_USER, "seeded", 6u) == 0);
    CHECK(conv_store_load(&model, "any", test_dir) == CONV_STORE_CORRUPT);
    CHECK(chat_model_message_count(&model) == 0u);

    /* Corrupt 2: Missing role */
    static const char corrupt2[] = "[conversation]\ntitle=Test\n[message]\ntext=hi\n";
    clean();
    write_raw_file(CONV_STORE_FILE_NAME, corrupt2, sizeof(corrupt2) - 1u);
    CHECK(conv_store_load(&model, "Test", test_dir) == CONV_STORE_CORRUPT);
    CHECK(chat_model_message_count(&model) == 0u);

    /* Corrupt 3: Invalid role */
    static const char corrupt3[] = "[conversation]\ntitle=Test\n[message]\nrole=robot\ntext=hi\n";
    clean();
    write_raw_file(CONV_STORE_FILE_NAME, corrupt3, sizeof(corrupt3) - 1u);
    CHECK(conv_store_load(&model, "Test", test_dir) == CONV_STORE_CORRUPT);
    CHECK(chat_model_message_count(&model) == 0u);

    /* Corrupt 4: Garbage header */
    static const char corrupt4[] = "garbage line before conversation\n[conversation]\ntitle=T\n";
    clean();
    write_raw_file(CONV_STORE_FILE_NAME, corrupt4, sizeof(corrupt4) - 1u);
    CHECK(conv_store_load(&model, "T", test_dir) == CONV_STORE_CORRUPT);
    CHECK(chat_model_message_count(&model) == 0u);

    /* Corrupt 5: Embedded NUL */
    static const char corrupt5[] = "[conversation]\ntitle=T\n[message]\nrole=user\ntext=h\0i\n";
    clean();
    write_raw_file(CONV_STORE_FILE_NAME, corrupt5, sizeof(corrupt5) - 1u);
    CHECK(conv_store_load(&model, "T", test_dir) == CONV_STORE_CORRUPT);
    CHECK(chat_model_message_count(&model) == 0u);

    /* Corrupt 6: Conversation with 0 messages */
    static const char corrupt6[] = "[conversation]\ntitle=T1\n[conversation]\ntitle=T2\n[message]\nrole=user\ntext=hi\n";
    clean();
    write_raw_file(CONV_STORE_FILE_NAME, corrupt6, sizeof(corrupt6) - 1u);
    CHECK(conv_store_load(&model, "T1", test_dir) == CONV_STORE_CORRUPT);
    CHECK(chat_model_message_count(&model) == 0u);

    clean();
}

static void check_fat_replace(void) {
    chat_model_t m;
    chat_model_t loaded;

    clean();
    chat_model_init(&m, NULL, NULL, 0u);
    chat_model_init(&loaded, NULL, NULL, 0u);

    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "initial", 7u) == 0);
    CHECK(conv_store_save(&m, "C", test_dir) == CONV_STORE_OK);

    /* FAT simulator refuses rename over existing file */
    reset_rename();
    rename_like_fat = 1;
    chat_model_reset(&m);
    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "updated", 7u) == 0);
    CHECK(conv_store_save(&m, "C", test_dir) == CONV_STORE_OK);
    CHECK(rename_calls == 3);
    CHECK(!exists(CONV_STORE_TEMP_NAME));
    CHECK(!exists(CONV_STORE_BACKUP_NAME));

    CHECK(conv_store_load(&loaded, "C", test_dir) == CONV_STORE_OK);
    CHECK_STR_EQ(chat_model_message_at(&loaded, 0)->text, "updated");
}

static void check_failed_rename_keeps_original(void) {
    chat_model_t m;
    chat_model_t loaded;

    clean();
    chat_model_init(&m, NULL, NULL, 0u);
    chat_model_init(&loaded, NULL, NULL, 0u);

    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "kept", 4u) == 0);
    CHECK(conv_store_save(&m, "C", test_dir) == CONV_STORE_OK);

    rename_fail_all = 1;
    chat_model_reset(&m);
    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "new", 3u) == 0);
    CHECK(conv_store_save(&m, "C", test_dir) == CONV_STORE_IO_ERROR);

    CHECK(!exists(CONV_STORE_TEMP_NAME));
    CHECK(!exists(CONV_STORE_BACKUP_NAME));

    reset_rename();
    CHECK(conv_store_load(&loaded, "C", test_dir) == CONV_STORE_OK);
    CHECK_STR_EQ(chat_model_message_at(&loaded, 0)->text, "kept");
}

static void check_backup_fallback(void) {
    chat_model_t loaded;
    char titles[CONV_STORE_MAX_CONVERSATIONS][CONV_STORE_TITLE_MAX + 1u];
    size_t count = 0u;

    clean();
    chat_model_init(&loaded, NULL, NULL, 0u);

    /* If convs.txt missing but convs.txt.bak exists (interrupted swap) */
    static const char bak_content[] = "[conversation]\ntitle=FromBak\n[message]\nrole=user\ntext=bak text\n";
    write_raw_file(CONV_STORE_BACKUP_NAME, bak_content, sizeof(bak_content) - 1u);

    CHECK(conv_store_list(test_dir, titles, &count, 8u) == CONV_STORE_OK);
    CHECK(count == 1u);
    CHECK_STR_EQ(titles[0], "FromBak");

    CHECK(conv_store_load(&loaded, "FromBak", test_dir) == CONV_STORE_OK);
    CHECK_STR_EQ(chat_model_message_at(&loaded, 0)->text, "bak text");
}

static void check_title_truncation(void) {
    chat_model_t m;
    chat_model_t loaded;
    char long_title[100];
    char titles[CONV_STORE_MAX_CONVERSATIONS][CONV_STORE_TITLE_MAX + 1u];
    size_t count = 0u;

    clean();
    chat_model_init(&m, NULL, NULL, 0u);
    chat_model_init(&loaded, NULL, NULL, 0u);

    memset(long_title, 't', sizeof(long_title) - 1u);
    long_title[sizeof(long_title) - 1u] = '\0';

    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "msg", 3u) == 0);
    CHECK(conv_store_save(&m, long_title, test_dir) == CONV_STORE_OK);

    CHECK(conv_store_list(test_dir, titles, &count, 8u) == CONV_STORE_OK);
    CHECK(count == 1u);
    CHECK(strlen(titles[0]) == CONV_STORE_TITLE_MAX);

    /* Loading by either truncated title or original long title matches */
    CHECK(conv_store_load(&loaded, long_title, test_dir) == CONV_STORE_OK);
    CHECK_STR_EQ(chat_model_message_at(&loaded, 0)->text, "msg");
}

static void check_marker_round_trip(void) {
    chat_model_t saved;
    chat_model_t loaded;

    clean();
    chat_model_init(&saved, NULL, NULL, 0u);
    chat_model_init(&loaded, NULL, NULL, 0u);

    /* Text whose lines collide with the file format's section markers, plus a
     * leading-backslash line, must survive a save/load round-trip verbatim. */
    const char *msg =
        "Before\n"
        "[message]\n"
        "middle\n"
        "[conversation]\n"
        "\\backslash\n"
        "[end]\n";
    CHECK(chat_model_append_message(&saved, CHAT_ROLE_USER, msg, strlen(msg)) == 0);

    CHECK(conv_store_save(&saved, "Markers", test_dir) == CONV_STORE_OK);
    CHECK(conv_store_load(&loaded, "Markers", test_dir) == CONV_STORE_OK);

    CHECK(chat_model_message_count(&loaded) == 1u);
    const chat_message_t *m = chat_model_message_at(&loaded, 0);
    CHECK(m != NULL);
    CHECK(m->role == CHAT_ROLE_USER);
    CHECK(m->length == strlen(msg));
    CHECK_STR_EQ(m->text, msg);
}

static void check_title_control_chars_rejected(void) {
    chat_model_t m;

    clean();
    chat_model_init(&m, NULL, NULL, 0u);
    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "msg", 3u) == 0);

    /* A title containing a control character must be rejected, not written
     * verbatim into the file where it would break the line structure. */
    CHECK(conv_store_save(&m, "bad\ntitle", test_dir) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(conv_store_save(&m, "bad\rtitle", test_dir) == CONV_STORE_INVALID_ARGUMENT);
    CHECK(!exists(CONV_STORE_FILE_NAME));
    CHECK(!exists(CONV_STORE_TEMP_NAME));
}

static void check_message_count_too_large(void) {
    chat_model_t loaded;
    char buf[2048];
    size_t off = 0u;

    clean();
    chat_model_init(&loaded, NULL, NULL, 0u);

    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "[conversation]\ntitle=Nine\n");
    for (int i = 0; i < 9; i++) {
        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                                "[message]\nrole=user\ntext=msg%d\n", i);
    }
    write_raw_file(CONV_STORE_FILE_NAME, buf, off);

    /* A conversation over CHAT_MAX_MESSAGES is TOO_LARGE, not CORRUPT. */
    CHECK(conv_store_load(&loaded, "Nine", test_dir) == CONV_STORE_TOO_LARGE);
    CHECK(chat_model_message_count(&loaded) == 0u);
}

static void check_stale_backup_removed(void) {
    chat_model_t m;

    clean();
    chat_model_init(&m, NULL, NULL, 0u);
    CHECK(chat_model_append_message(&m, CHAT_ROLE_USER, "fresh", 5u) == 0);

    /* A stale backup from an interrupted swap must not survive a save. */
    static const char bak[] = "[conversation]\ntitle=Old\n[message]\nrole=user\ntext=old\n";
    write_raw_file(CONV_STORE_BACKUP_NAME, bak, sizeof(bak) - 1u);

    CHECK(conv_store_save(&m, "Fresh", test_dir) == CONV_STORE_OK);
    CHECK(exists(CONV_STORE_FILE_NAME));
    CHECK(!exists(CONV_STORE_BACKUP_NAME));
}

void test_conv_store(void) {
    check_arguments();
    check_empty_transcript_save_rejected();
    check_round_trip();
    check_list_order_and_count();
    check_replace_on_same_title();
    check_delete_operations();
    check_load_missing_title();
    check_eight_conversation_cap();
    check_size_bounds();
    check_corrupt_files();
    check_fat_replace();
    check_failed_rename_keeps_original();
    check_backup_fallback();
    check_title_truncation();
    check_marker_round_trip();
    check_title_control_chars_rejected();
    check_message_count_too_large();
    check_stale_backup_removed();
    clean();
}
