#include <string.h>

#include "ai/chat_settings.h"
#include "ai/ollama_provider.h"
#include "test_util.h"

static void check_labels(void) {
    for (int i = 0; i < CHAT_SETTING_COUNT; i++) {
        CHECK(chat_settings_label((chat_setting_t)i)[0] != '\0');
    }
    CHECK_STR_EQ(chat_settings_label(CHAT_SETTING_HOST), "Host");
    CHECK_STR_EQ(chat_settings_label(CHAT_SETTING_COUNT), "");
    CHECK(chat_settings_is_secret(CHAT_SETTING_PASSWORD));
    CHECK(!chat_settings_is_secret(CHAT_SETTING_SSID));
    CHECK(chat_settings_allows_empty(CHAT_SETTING_PASSWORD));
    CHECK(chat_settings_allows_empty(CHAT_SETTING_SSID));
    CHECK(chat_settings_allows_empty(CHAT_SETTING_MODEL));
    CHECK(!chat_settings_allows_empty(CHAT_SETTING_HOST));
    CHECK(!chat_settings_allows_empty(CHAT_SETTING_PORT));
}

static void check_text_fields(void) {
    ai_config_t config;
    char out[160];
    ai_config_init(&config);

    CHECK(chat_settings_set(&config, CHAT_SETTING_HOST, "192.168.1.20") == AI_CONFIG_OK);
    CHECK_STR_EQ(config.host, "192.168.1.20");
    CHECK(chat_settings_set(&config, CHAT_SETTING_MODEL, "llama3.2:1b") == AI_CONFIG_OK);
    CHECK_STR_EQ(config.model, "llama3.2:1b");
    CHECK(chat_settings_set(&config, CHAT_SETTING_SSID, "Home Net") == AI_CONFIG_OK);
    CHECK_STR_EQ(config.ssid, "Home Net");
    CHECK(chat_settings_set(&config, CHAT_SETTING_PROVIDER, "ollama") == AI_CONFIG_OK);
    CHECK_STR_EQ(config.provider, "ollama");

    CHECK(chat_settings_format(&config, CHAT_SETTING_HOST, out, sizeof(out)) == strlen("192.168.1.20"));
    CHECK_STR_EQ(out, "192.168.1.20");
    chat_settings_format(&config, CHAT_SETTING_MODEL, out, sizeof(out));
    CHECK_STR_EQ(out, "llama3.2:1b");
    chat_settings_format(&config, CHAT_SETTING_SSID, out, sizeof(out));
    CHECK_STR_EQ(out, "Home Net");
    chat_settings_format(&config, CHAT_SETTING_PROVIDER, out, sizeof(out));
    CHECK_STR_EQ(out, "ollama");

    /* Only the edited field changes. */
    CHECK(config.port == 11434u);
    CHECK(config.max_predict == 384u);
    CHECK(config.version == AI_CONFIG_VERSION);

    /* Rejections leave the old value in place. */
    CHECK(chat_settings_set(&config, CHAT_SETTING_HOST, "") == AI_CONFIG_INVALID_VALUE);
    CHECK_STR_EQ(config.host, "192.168.1.20");
    CHECK(chat_settings_set(&config, CHAT_SETTING_PROVIDER, "muse") == AI_CONFIG_INVALID_VALUE);
    CHECK(chat_settings_set(&config, CHAT_SETTING_PROVIDER, "openai") == AI_CONFIG_INVALID_VALUE);
    CHECK_STR_EQ(config.provider, "ollama");
    CHECK(chat_settings_set(&config, CHAT_SETTING_SSID, " padded") == AI_CONFIG_INVALID_VALUE);
    CHECK(chat_settings_set(&config, CHAT_SETTING_SSID, "padded ") == AI_CONFIG_INVALID_VALUE);
    CHECK(chat_settings_set(&config, CHAT_SETTING_SSID, "two\nlines") == AI_CONFIG_INVALID_VALUE);
    CHECK(chat_settings_set(&config, CHAT_SETTING_SSID, "tab\t") == AI_CONFIG_INVALID_VALUE);
    CHECK_STR_EQ(config.ssid, "Home Net");
    /* A value cannot smuggle in a second key. */
    CHECK(chat_settings_set(&config, CHAT_SETTING_MODEL, "m\rport=1") == AI_CONFIG_INVALID_VALUE);
    CHECK(config.port == 11434u);

    /* Capacity limits. */
    char longest[300];
    memset(longest, 'a', sizeof(longest));
    longest[AI_CONFIG_SSID_CAPACITY - 1u] = '\0';
    CHECK(chat_settings_set(&config, CHAT_SETTING_SSID, longest) == AI_CONFIG_OK);
    memset(longest, 'a', sizeof(longest));
    longest[AI_CONFIG_SSID_CAPACITY] = '\0';
    CHECK(chat_settings_set(&config, CHAT_SETTING_SSID, longest) == AI_CONFIG_VALUE_TOO_LONG);
    memset(longest, 'a', sizeof(longest));
    longest[OLLAMA_MODEL_MAX] = '\0';
    CHECK(chat_settings_set(&config, CHAT_SETTING_MODEL, longest) == AI_CONFIG_OK);
    memset(longest, 'a', sizeof(longest));
    longest[OLLAMA_MODEL_MAX + 1u] = '\0';
    CHECK(chat_settings_set(&config, CHAT_SETTING_MODEL, longest) == AI_CONFIG_VALUE_TOO_LONG);
    CHECK(strlen(config.model) == OLLAMA_MODEL_MAX);
    memset(longest, 'a', sizeof(longest));
    longest[AI_CONFIG_HOST_CAPACITY] = '\0';
    CHECK(chat_settings_set(&config, CHAT_SETTING_HOST, longest) == AI_CONFIG_VALUE_TOO_LONG);
    CHECK_STR_EQ(config.host, "192.168.1.20");

    /* Emptying is allowed where the config allows it. */
    CHECK(chat_settings_set(&config, CHAT_SETTING_MODEL, "") == AI_CONFIG_OK);
    CHECK_STR_EQ(config.model, "");
    CHECK(chat_settings_format(&config, CHAT_SETTING_MODEL, out, sizeof(out)) == 0u);
    CHECK_STR_EQ(out, "");
}

typedef struct {
    chat_setting_t field;
    const char *minimum;
    const char *maximum;
    const char *below;
    const char *above;
    unsigned long low;
    unsigned long high;
} bound_case_t;

static unsigned long number_of(const ai_config_t *config, chat_setting_t field) {
    switch (field) {
        case CHAT_SETTING_PORT: return config->port;
        case CHAT_SETTING_CONNECT_TIMEOUT: return config->connect_timeout_ms;
        case CHAT_SETTING_REQUEST_TIMEOUT: return config->request_timeout_ms;
        case CHAT_SETTING_IDLE_TIMEOUT: return config->idle_timeout_ms;
        default: return config->max_predict;
    }
}

static void check_numeric_bounds(void) {
    static const bound_case_t cases[] = {
        {CHAT_SETTING_PORT, "1", "65535", "0", "65536", 1ul, 65535ul},
        {CHAT_SETTING_CONNECT_TIMEOUT, "100", "120000", "99", "120001", 100ul, 120000ul},
        {CHAT_SETTING_REQUEST_TIMEOUT, "1000", "600000", "999", "600001", 1000ul, 600000ul},
        {CHAT_SETTING_IDLE_TIMEOUT, "100", "120000", "99", "120001", 100ul, 120000ul},
        {CHAT_SETTING_MAX_PREDICT, "1", "4096", "0", "4097", 1ul, 4096ul},
    };
    static const char *const garbage[] = {"", "abc", "12x", "-5", "+5", "1.5", "0x10", "99999999999"};
    char out[16];

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const bound_case_t *c = &cases[i];
        ai_config_t config;
        ai_config_init(&config);

        CHECK(chat_settings_set(&config, c->field, c->minimum) == AI_CONFIG_OK);
        CHECK(number_of(&config, c->field) == c->low);
        chat_settings_format(&config, c->field, out, sizeof(out));
        CHECK_STR_EQ(out, c->minimum);

        CHECK(chat_settings_set(&config, c->field, c->maximum) == AI_CONFIG_OK);
        CHECK(number_of(&config, c->field) == c->high);
        chat_settings_format(&config, c->field, out, sizeof(out));
        CHECK_STR_EQ(out, c->maximum);

        CHECK(chat_settings_set(&config, c->field, c->below) == AI_CONFIG_OUT_OF_RANGE);
        CHECK(number_of(&config, c->field) == c->high);
        CHECK(chat_settings_set(&config, c->field, c->above) == AI_CONFIG_OUT_OF_RANGE);
        CHECK(number_of(&config, c->field) == c->high);

        for (size_t g = 0u; g < sizeof(garbage) / sizeof(garbage[0]); g++) {
            CHECK(chat_settings_set(&config, c->field, garbage[g]) == AI_CONFIG_INVALID_VALUE);
            CHECK(number_of(&config, c->field) == c->high);
        }
    }
}

static void check_password_is_never_shown(void) {
    ai_config_t config;
    char out[80];
    ai_config_init(&config);

    CHECK(chat_settings_format(&config, CHAT_SETTING_PASSWORD, out, sizeof(out)) == 0u);
    CHECK_STR_EQ(out, "");

    /* Characters the file-name prompt refuses are fine here. */
    CHECK(chat_settings_set(&config, CHAT_SETTING_PASSWORD, "p@ss:w/rd=*#1") == AI_CONFIG_OK);
    CHECK_STR_EQ(config.password, "p@ss:w/rd=*#1");
    CHECK(chat_settings_format(&config, CHAT_SETTING_PASSWORD, out, sizeof(out)) == 8u);
    CHECK_STR_EQ(out, "********");

    /* The mask does not track the length. */
    CHECK(chat_settings_set(&config, CHAT_SETTING_PASSWORD, "abc") == AI_CONFIG_OK);
    chat_settings_format(&config, CHAT_SETTING_PASSWORD, out, sizeof(out));
    CHECK_STR_EQ(out, "********");

    /* No other field's display leaks it. */
    CHECK(chat_settings_set(&config, CHAT_SETTING_PASSWORD, "hunter2secret") == AI_CONFIG_OK);
    for (int i = 0; i < CHAT_SETTING_COUNT; i++) {
        chat_settings_format(&config, (chat_setting_t)i, out, sizeof(out));
        CHECK(strstr(out, "hunter2") == NULL);
    }

    char longest[80];
    memset(longest, 'k', sizeof(longest));
    longest[AI_CONFIG_PASSWORD_CAPACITY - 1u] = '\0';
    CHECK(chat_settings_set(&config, CHAT_SETTING_PASSWORD, longest) == AI_CONFIG_OK);
    memset(longest, 'k', sizeof(longest));
    longest[AI_CONFIG_PASSWORD_CAPACITY] = '\0';
    CHECK(chat_settings_set(&config, CHAT_SETTING_PASSWORD, longest) == AI_CONFIG_VALUE_TOO_LONG);
    CHECK(strlen(config.password) == AI_CONFIG_PASSWORD_CAPACITY - 1u);

    CHECK(chat_settings_set(&config, CHAT_SETTING_PASSWORD, "") == AI_CONFIG_OK);
    CHECK_STR_EQ(config.password, "");
}

static void check_arguments_and_truncation(void) {
    ai_config_t config;
    char out[8];
    ai_config_init(&config);
    CHECK_STR_EQ(config.host, "");
    CHECK(chat_settings_set(&config, CHAT_SETTING_HOST, "wang.local") == AI_CONFIG_OK);

    CHECK(chat_settings_set(NULL, CHAT_SETTING_HOST, "h") == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(chat_settings_set(&config, CHAT_SETTING_HOST, NULL) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(chat_settings_set(&config, CHAT_SETTING_COUNT, "h") == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(chat_settings_set(&config, (chat_setting_t)-1, "h") == AI_CONFIG_INVALID_ARGUMENT);
    CHECK_STR_EQ(config.host, "wang.local");

    CHECK(chat_settings_format(&config, CHAT_SETTING_HOST, out, sizeof(out)) == 7u);
    CHECK_STR_EQ(out, "wang.lo");
    CHECK(chat_settings_format(&config, CHAT_SETTING_HOST, out, 0u) == 0u);
    CHECK(chat_settings_format(&config, CHAT_SETTING_HOST, NULL, 8u) == 0u);
    out[0] = 'x';
    CHECK(chat_settings_format(NULL, CHAT_SETTING_HOST, out, sizeof(out)) == 0u);
    CHECK_STR_EQ(out, "");
    out[0] = 'x';
    CHECK(chat_settings_format(&config, CHAT_SETTING_COUNT, out, sizeof(out)) == 0u);
    CHECK_STR_EQ(out, "");
}

void test_chat_settings(void) {
    check_labels();
    check_text_fields();
    check_numeric_bounds();
    check_password_is_never_shown();
    check_arguments_and_truncation();
}
