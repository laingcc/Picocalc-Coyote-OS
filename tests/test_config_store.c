#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ai/config_store.h"
#include "test_util.h"

#define SECRET_PASSWORD "hunter2-wifi-secret"
#define SECRET_TOKEN "bearer-secret-token"

static const char *const test_dir = ".";
static char file_text[4096];

/* ---- rename double --------------------------------------------------------
 * config_store.c is compiled with rename mapped to this function. */

static int rename_calls;
static int rename_fail_call; /* 1-based call to fail; 0 fails none */
static int rename_fail_all;
static int rename_like_fat;  /* refuse to replace an existing file */

static int exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

int config_store_test_rename(const char *old_path, const char *new_path);
int config_store_test_rename(const char *old_path, const char *new_path) {
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
    remove(CONFIG_STORE_FILE_NAME);
    remove(CONFIG_STORE_TEMP_NAME);
    remove(CONFIG_STORE_BACKUP_NAME);
    reset_rename();
}

static void write_file(const char *name, const char *text, size_t length) {
    FILE *file = fopen(name, "wb");
    CHECK(file != NULL);
    if (file != NULL) {
        CHECK(fwrite(text, 1u, length, file) == length);
        CHECK(fclose(file) == 0);
    }
}

static const char *read_file(const char *name) {
    FILE *file = fopen(name, "rb");
    size_t length = 0u;
    if (file != NULL) {
        length = fread(file_text, 1u, sizeof(file_text) - 1u, file);
        fclose(file);
    }
    file_text[length] = '\0';
    return file_text;
}

static void fill(ai_config_t *config) {
    ai_config_init(config);
    strcpy(config->ssid, "Lab WiFi");
    strcpy(config->password, SECRET_PASSWORD);
    strcpy(config->provider, "muse");
    strcpy(config->host, "192.0.2.7");
    config->port = 8080u;
    strcpy(config->model, "small:model");
    strcpy(config->bearer_token, SECRET_TOKEN);
    config->connect_timeout_ms = 2500u;
    config->request_timeout_ms = 90000u;
    config->idle_timeout_ms = 7000u;
    config->max_predict = 1024u;
    config->utc_offset_minutes = -210;
}

static void check_filled(const ai_config_t *config) {
    CHECK(config->version == AI_CONFIG_VERSION);
    CHECK_STR_EQ(config->ssid, "Lab WiFi");
    CHECK_STR_EQ(config->password, SECRET_PASSWORD);
    CHECK_STR_EQ(config->provider, "muse");
    CHECK_STR_EQ(config->host, "192.0.2.7");
    CHECK(config->port == 8080u);
    CHECK_STR_EQ(config->model, "small:model");
    CHECK_STR_EQ(config->bearer_token, SECRET_TOKEN);
    CHECK(config->connect_timeout_ms == 2500u);
    CHECK(config->request_timeout_ms == 90000u);
    CHECK(config->idle_timeout_ms == 7000u);
    CHECK(config->max_predict == 1024u);
    CHECK(config->utc_offset_minutes == -210);
}

static void check_is_default(const ai_config_t *config) {
    ai_config_t defaults;
    ai_config_init(&defaults);
    CHECK_BYTES_EQ(config, &defaults, sizeof(defaults));
}

/* ---- cases ----------------------------------------------------------------- */

static void check_load_without_file(void) {
    ai_config_t config;

    clean();
    memset(&config, 0x5a, sizeof(config));
    CHECK(config_store_load(&config, test_dir) == AI_CONFIG_OK);
    check_is_default(&config);
    CHECK(!exists(CONFIG_STORE_FILE_NAME));
}

static void check_arguments(void) {
    static char long_dir[CONFIG_STORE_PATH_CAPACITY + 1u];
    ai_config_t config;

    clean();
    memset(long_dir, 'd', sizeof(long_dir) - 1u);
    fill(&config);
    CHECK(config_store_load(NULL, test_dir) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(config_store_save(NULL, test_dir) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(config_store_save(&config, NULL) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(config_store_save(&config, "") == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(config_store_save(&config, long_dir) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(config_store_load(&config, NULL) == AI_CONFIG_INVALID_ARGUMENT);
    check_is_default(&config);
    fill(&config);
    CHECK(config_store_load(&config, long_dir) == AI_CONFIG_INVALID_ARGUMENT);
    check_is_default(&config);
    CHECK(!exists(CONFIG_STORE_FILE_NAME));
}

static void check_round_trip(void) {
    ai_config_t saved;
    ai_config_t loaded;

    clean();
    fill(&saved);
    CHECK(config_store_save(&saved, test_dir) == AI_CONFIG_OK);
    CHECK(exists(CONFIG_STORE_FILE_NAME));
    CHECK(!exists(CONFIG_STORE_TEMP_NAME));
    CHECK(!exists(CONFIG_STORE_BACKUP_NAME));
    /* The file is the one place the secrets are written. */
    CHECK(strstr(read_file(CONFIG_STORE_FILE_NAME), "password=" SECRET_PASSWORD "\n") != NULL);
    CHECK(strstr(file_text, "bearer_token=" SECRET_TOKEN "\n") != NULL);

    memset(&loaded, 0x5a, sizeof(loaded));
    CHECK(config_store_load(&loaded, test_dir) == AI_CONFIG_OK);
    check_filled(&loaded);
    CHECK_BYTES_EQ(&loaded, &saved, sizeof(saved));

    /* A second save replaces the first. */
    strcpy(saved.model, "other:model");
    CHECK(config_store_save(&saved, test_dir) == AI_CONFIG_OK);
    CHECK(config_store_load(&loaded, test_dir) == AI_CONFIG_OK);
    CHECK_STR_EQ(loaded.model, "other:model");
    CHECK_STR_EQ(loaded.password, SECRET_PASSWORD);
    CHECK(!exists(CONFIG_STORE_TEMP_NAME));
}

static void check_maximum_size_round_trip(void) {
    ai_config_t saved;
    ai_config_t loaded;

    clean();
    ai_config_init(&saved);
    memset(saved.ssid, 's', sizeof(saved.ssid) - 1u);
    memset(saved.password, 'p', sizeof(saved.password) - 1u);
    memset(saved.host, 'h', sizeof(saved.host) - 1u);
    memset(saved.model, 'm', sizeof(saved.model) - 1u);
    memset(saved.bearer_token, 't', sizeof(saved.bearer_token) - 1u);
    saved.port = 65535u;
    saved.connect_timeout_ms = AI_CONFIG_CONNECT_TIMEOUT_MS_MAX;
    saved.request_timeout_ms = AI_CONFIG_REQUEST_TIMEOUT_MS_MAX;
    saved.idle_timeout_ms = AI_CONFIG_IDLE_TIMEOUT_MS_MAX;
    saved.max_predict = AI_CONFIG_MAX_PREDICT_MAX;
    CHECK(config_store_save(&saved, test_dir) == AI_CONFIG_OK);
    CHECK(config_store_load(&loaded, test_dir) == AI_CONFIG_OK);
    CHECK_BYTES_EQ(&loaded, &saved, sizeof(saved));
}

static void check_corrupt_file(const char *text, size_t length, ai_config_status_t expected) {
    ai_config_t config;

    clean();
    write_file(CONFIG_STORE_FILE_NAME, text, length);
    fill(&config);
    CHECK(config_store_load(&config, test_dir) == expected);
    check_is_default(&config);
}

static void check_corrupt_files(void) {
    static const char truncated[] = "version=1\nssid=Lab WiFi\npassword=" SECRET_PASSWORD "\nport=80\nmodel";
    static const char bad_value[] = "ssid=Lab WiFi\npassword=" SECRET_PASSWORD "\nport=99999\n";
    static const char bad_version[] = "ssid=Lab WiFi\nversion=2\n";
    static const char binary[] = "ssid=Lab\0WiFi\n";
    static char long_line[AI_CONFIG_LINE_MAX * 3u];

    check_corrupt_file(truncated, sizeof(truncated) - 1u, AI_CONFIG_MALFORMED_LINE);
    check_corrupt_file(bad_value, sizeof(bad_value) - 1u, AI_CONFIG_OUT_OF_RANGE);
    check_corrupt_file(bad_version, sizeof(bad_version) - 1u, AI_CONFIG_UNSUPPORTED_VERSION);
    check_corrupt_file(binary, sizeof(binary) - 1u, AI_CONFIG_EMBEDDED_NUL);
    memset(long_line, 'x', sizeof(long_line));
    check_corrupt_file(long_line, sizeof(long_line), AI_CONFIG_LINE_TOO_LONG);
    clean();
}

static void check_unsaveable_config(void) {
    ai_config_t config;

    clean();
    write_file(CONFIG_STORE_FILE_NAME, "model=kept\n", 11u);
    fill(&config);
    config.port = 0u;
    CHECK(config_store_save(&config, test_dir) == AI_CONFIG_INVALID_VALUE);
    CHECK_STR_EQ(read_file(CONFIG_STORE_FILE_NAME), "model=kept\n");
    CHECK(!exists(CONFIG_STORE_TEMP_NAME));
    CHECK(rename_calls == 0);
}

static void check_failed_rename_keeps_original(void) {
    ai_config_t config;
    ai_config_status_t status;

    clean();
    write_file(CONFIG_STORE_FILE_NAME, "model=kept\n", 11u);
    fill(&config);
    rename_fail_all = 1;
    status = config_store_save(&config, test_dir);
    CHECK(status == AI_CONFIG_IO_ERROR);
    CHECK(rename_calls > 0);
    CHECK(!exists(CONFIG_STORE_TEMP_NAME));
    CHECK(!exists(CONFIG_STORE_BACKUP_NAME));
    CHECK_STR_EQ(read_file(CONFIG_STORE_FILE_NAME), "model=kept\n");
    /* The failure leaves the caller's config, secrets included, as it was. */
    check_filled(&config);

    reset_rename();
    CHECK(config_store_load(&config, test_dir) == AI_CONFIG_OK);
    CHECK_STR_EQ(config.model, "kept");
    CHECK_STR_EQ(config.password, "");
}

static void check_failed_first_save(void) {
    ai_config_t config;

    clean();
    fill(&config);
    rename_fail_all = 1;
    CHECK(config_store_save(&config, test_dir) == AI_CONFIG_IO_ERROR);
    CHECK(!exists(CONFIG_STORE_FILE_NAME));
    CHECK(!exists(CONFIG_STORE_TEMP_NAME));
    CHECK(!exists(CONFIG_STORE_BACKUP_NAME));
}

/* FAT: the direct rename is refused, so the old file steps aside first. */
static void check_fat_replace(void) {
    ai_config_t config;
    ai_config_t loaded;

    clean();
    write_file(CONFIG_STORE_FILE_NAME, "model=kept\n", 11u);
    write_file(CONFIG_STORE_BACKUP_NAME, "model=stale\n", 12u);
    fill(&config);
    rename_like_fat = 1;
    CHECK(config_store_save(&config, test_dir) == AI_CONFIG_OK);
    CHECK(rename_calls == 3);
    CHECK(!exists(CONFIG_STORE_TEMP_NAME));
    CHECK(!exists(CONFIG_STORE_BACKUP_NAME));
    CHECK(config_store_load(&loaded, test_dir) == AI_CONFIG_OK);
    check_filled(&loaded);
}

static void check_fat_replace_failures(void) {
    ai_config_t config;
    int fail_call;

    /* Call 2 moves the old file aside; call 3 moves the new one in. */
    for (fail_call = 2; fail_call <= 3; fail_call++) {
        clean();
        write_file(CONFIG_STORE_FILE_NAME, "model=kept\n", 11u);
        fill(&config);
        rename_like_fat = 1;
        rename_fail_call = fail_call;
        CHECK(config_store_save(&config, test_dir) == AI_CONFIG_IO_ERROR);
        CHECK(!exists(CONFIG_STORE_TEMP_NAME));
        CHECK(!exists(CONFIG_STORE_BACKUP_NAME));
        CHECK_STR_EQ(read_file(CONFIG_STORE_FILE_NAME), "model=kept\n");
    }
}

/* Power lost mid-swap on FAT: only the backup is left. */
static void check_backup_fallback(void) {
    ai_config_t config;

    clean();
    write_file(CONFIG_STORE_BACKUP_NAME, "model=from-backup\n", 18u);
    CHECK(config_store_load(&config, test_dir) == AI_CONFIG_OK);
    CHECK_STR_EQ(config.model, "from-backup");

    write_file(CONFIG_STORE_FILE_NAME, "model=current\n", 14u);
    CHECK(config_store_load(&config, test_dir) == AI_CONFIG_OK);
    CHECK_STR_EQ(config.model, "current");
}

static void check_unwritable_directory(void) {
    ai_config_t config;

    clean();
    fill(&config);
    CHECK(config_store_save(&config, "no-such-directory") == AI_CONFIG_IO_ERROR);
    CHECK(rename_calls == 0);
    CHECK(config_store_load(&config, "no-such-directory") == AI_CONFIG_OK);
    check_is_default(&config);
}

void test_config_store(void) {
    check_load_without_file();
    check_arguments();
    check_round_trip();
    check_maximum_size_round_trip();
    check_corrupt_files();
    check_unsaveable_config();
    check_failed_rename_keeps_original();
    check_failed_first_save();
    check_fat_replace();
    check_fat_replace_failures();
    check_backup_fallback();
    check_unwritable_directory();
    clean();
}
