#include <string.h>

#include "ai/ai_config.h"
#include "test_util.h"

static ai_config_status_t parse_text(ai_config_t *config, ai_config_parser_t *parser,
                                     const char *text, size_t chunk_size) {
    ai_config_status_t status = ai_config_parser_init(parser, config);
    size_t offset = 0u;
    while (status == AI_CONFIG_OK && offset < strlen(text)) {
        size_t count = strlen(text) - offset;
        if (count > chunk_size) {
            count = chunk_size;
        }
        status = ai_config_parser_feed(parser, text + offset, count);
        offset += count;
    }
    if (status == AI_CONFIG_OK) {
        status = ai_config_parser_finish(parser);
    }
    return status;
}

static void check_defaults(void) {
    ai_config_t config;

    CHECK(ai_config_init(&config) == AI_CONFIG_OK);
    CHECK(config.version == 1u);
    CHECK_STR_EQ(config.ssid, "");
    CHECK_STR_EQ(config.password, "");
    CHECK_STR_EQ(config.provider, "ollama");
    CHECK_STR_EQ(config.host, "");
    CHECK(config.port == 11434u);
    CHECK_STR_EQ(config.model, "");
    CHECK_STR_EQ(config.bearer_token, "");
    CHECK(config.connect_timeout_ms == 15000u);
    CHECK(config.request_timeout_ms == 120000u);
    CHECK(config.idle_timeout_ms == 15000u);
    CHECK(config.max_predict == 384u);
    CHECK(config.utc_offset_minutes == 0);
}

static void check_incremental_parse(void) {
    static const char input[] =
        "version=1\n"
        "ssid=Lab WiFi\n"
        "password=s3cret\n"
        "provider=muse\n"
        "host=192.0.2.7\n"
        "port=65535\n"
        "model=small:model\n"
        "bearer_token=private-token\n"
        "connect_timeout_ms=100\n"
        "request_timeout_ms=600000\n"
        "idle_timeout_ms=120000\n"
        "max_predict=4096";
    ai_config_t config;
    ai_config_parser_t parser;

    CHECK(parse_text(&config, &parser, input, 3u) == AI_CONFIG_OK);
    CHECK(config.version == 1u);
    CHECK_STR_EQ(config.ssid, "Lab WiFi");
    CHECK_STR_EQ(config.password, "s3cret");
    CHECK_STR_EQ(config.provider, "muse");
    CHECK_STR_EQ(config.host, "192.0.2.7");
    CHECK(config.port == 65535u);
    CHECK_STR_EQ(config.model, "small:model");
    CHECK_STR_EQ(config.bearer_token, "private-token");
    CHECK(config.connect_timeout_ms == 100u);
    CHECK(config.request_timeout_ms == 600000u);
    CHECK(config.idle_timeout_ms == 120000u);
    CHECK(config.max_predict == 4096u);
}

static void check_serialize_defaults(void) {
    static const char expected[] =
        "version=1\n"
        "ssid=\n"
        "password=\n"
        "provider=ollama\n"
        "port=11434\n"
        "model=\n"
        "bearer_token=\n"
        "connect_timeout_ms=15000\n"
        "request_timeout_ms=120000\n"
        "idle_timeout_ms=15000\n"
        "max_predict=384\n"
        "system_prompt=" AI_CONFIG_DEFAULT_SYSTEM_PROMPT "\n"
        "temperature=80\n"
        "utc_offset_minutes=0\n";
    static const char expected_with_host[] =
        "version=1\n"
        "ssid=\n"
        "password=\n"
        "provider=ollama\n"
        "host=example.local\n"
        "port=11434\n"
        "model=\n"
        "bearer_token=\n"
        "connect_timeout_ms=15000\n"
        "request_timeout_ms=120000\n"
        "idle_timeout_ms=15000\n"
        "max_predict=384\n"
        "system_prompt=" AI_CONFIG_DEFAULT_SYSTEM_PROMPT "\n"
        "temperature=80\n"
        "utc_offset_minutes=0\n";
    ai_config_t config;
    ai_config_t reloaded;
    ai_config_parser_t parser;
    char output[sizeof(expected_with_host)];
    size_t length = 0u;

    /* The default host is unset and is left out of the file, so the defaults
     * survive a save and reload. */
    CHECK(ai_config_init(&config) == AI_CONFIG_OK);
    CHECK(ai_config_serialize(&config, output, sizeof(output), &length) == AI_CONFIG_OK);
    CHECK(length == sizeof(expected) - 1u);
    CHECK(strcmp(output, expected) == 0);
    CHECK(parse_text(&reloaded, &parser, output, 7u) == AI_CONFIG_OK);
    CHECK_STR_EQ(reloaded.host, "");
    CHECK(reloaded.port == 11434u);

    strcpy(config.host, "example.local");
    CHECK(ai_config_serialize(&config, output, sizeof(output), &length) == AI_CONFIG_OK);
    CHECK(length == sizeof(expected_with_host) - 1u);
    CHECK(strcmp(output, expected_with_host) == 0);
    CHECK(parse_text(&reloaded, &parser, output, 7u) == AI_CONFIG_OK);
    CHECK_STR_EQ(reloaded.host, "example.local");
}

static void check_line_forms_and_unknown_keys(void) {
    static const char input[] =
        "\r\n# comment\r\n; another comment\r\nfuture_option=enabled\r\n"
        "  host = example.local  \r\nprovider=ollama\r\n";
    ai_config_t config;
    ai_config_parser_t parser;

    CHECK(parse_text(&config, &parser, input, 1u) == AI_CONFIG_OK);
    CHECK_STR_EQ(config.host, "example.local");
    CHECK_STR_EQ(config.provider, "ollama");
}

static void check_parse_errors(void) {
    ai_config_t config;
    ai_config_parser_t parser;
    static const char embedded_nul[] = {'h', 'o', 's', 't', '=', 'x', '\0', 'y'};

    CHECK(parse_text(&config, &parser, "host=x\nhost=y\n", 99u) == AI_CONFIG_DUPLICATE_KEY);
    CHECK_STR_EQ(ai_config_parser_error_key(&parser), "host");
    CHECK(parse_text(&config, &parser, "host\n", 99u) == AI_CONFIG_MALFORMED_LINE);
    CHECK(ai_config_parser_error_key(&parser) == NULL);
    CHECK(parse_text(&config, &parser, "host=\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK_STR_EQ(ai_config_parser_error_key(&parser), "host");
    CHECK(parse_text(&config, &parser, "provider=\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "provider=cloud\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "host=good\rbad\n", 99u) ==
          AI_CONFIG_MALFORMED_LINE);
    CHECK(parse_text(&config, &parser, "host=good\r", 1u) ==
          AI_CONFIG_MALFORMED_LINE);
    CHECK(parse_text(&config, &parser, "version=2\n", 99u) == AI_CONFIG_UNSUPPORTED_VERSION);
    CHECK(ai_config_parser_init(&parser, &config) == AI_CONFIG_OK);
    CHECK(ai_config_parser_feed(&parser, embedded_nul, sizeof(embedded_nul)) ==
          AI_CONFIG_EMBEDDED_NUL);
    CHECK(ai_config_parser_finish(&parser) == AI_CONFIG_EMBEDDED_NUL);
}

static void check_numeric_validation(void) {
    ai_config_t config;
    ai_config_parser_t parser;

    CHECK(parse_text(&config, &parser, "port=0\n", 99u) == AI_CONFIG_OUT_OF_RANGE);
    CHECK(parse_text(&config, &parser, "port=65536\n", 99u) == AI_CONFIG_OUT_OF_RANGE);
    CHECK(parse_text(&config, &parser, "port=-1\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "port=12x\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "port=42949672960\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "connect_timeout_ms=99\n", 99u) == AI_CONFIG_OUT_OF_RANGE);
    CHECK(parse_text(&config, &parser, "connect_timeout_ms=120001\n", 99u) == AI_CONFIG_OUT_OF_RANGE);
    CHECK(parse_text(&config, &parser, "request_timeout_ms=999\n", 99u) == AI_CONFIG_OUT_OF_RANGE);
    CHECK(parse_text(&config, &parser, "request_timeout_ms=600001\n", 99u) == AI_CONFIG_OUT_OF_RANGE);
    CHECK(parse_text(&config, &parser, "idle_timeout_ms=99\n", 99u) == AI_CONFIG_OUT_OF_RANGE);
    CHECK(parse_text(&config, &parser, "idle_timeout_ms=120001\n", 99u) == AI_CONFIG_OUT_OF_RANGE);
    CHECK(parse_text(&config, &parser, "max_predict=0\n", 99u) == AI_CONFIG_OUT_OF_RANGE);
    CHECK(parse_text(&config, &parser, "max_predict=4097\n", 99u) == AI_CONFIG_OUT_OF_RANGE);
    CHECK(parse_text(&config, &parser, "temperature=201\n", 99u) == AI_CONFIG_OUT_OF_RANGE);

    CHECK(parse_text(&config, &parser,
                     "port=1\nconnect_timeout_ms=100\nrequest_timeout_ms=1000\n"
                     "idle_timeout_ms=100\nmax_predict=1\n", 2u) == AI_CONFIG_OK);
    CHECK(config.port == 1u);
    CHECK(config.connect_timeout_ms == AI_CONFIG_CONNECT_TIMEOUT_MS_MIN);
    CHECK(config.request_timeout_ms == AI_CONFIG_REQUEST_TIMEOUT_MS_MIN);
    CHECK(config.idle_timeout_ms == AI_CONFIG_IDLE_TIMEOUT_MS_MIN);
    CHECK(config.max_predict == AI_CONFIG_MAX_PREDICT_MIN);
}

static void check_bounded_input(void) {
    static char input[AI_CONFIG_LINE_MAX + 3u];
    static char value[AI_CONFIG_SSID_CAPACITY + 8u];
    ai_config_t config;
    ai_config_parser_t parser;

    input[0] = 'x';
    input[1] = '=';
    memset(input + 2u, 'x', AI_CONFIG_LINE_MAX - 2u);
    input[AI_CONFIG_LINE_MAX] = '\0';
    CHECK(parse_text(&config, &parser, input, 17u) == AI_CONFIG_OK);

    /* CRLF does not reduce the documented content limit by one byte. */
    input[AI_CONFIG_LINE_MAX] = '\r';
    input[AI_CONFIG_LINE_MAX + 1u] = '\n';
    input[AI_CONFIG_LINE_MAX + 2u] = '\0';
    CHECK(parse_text(&config, &parser, input, 17u) == AI_CONFIG_OK);

    memset(input + 2u, 'x', AI_CONFIG_LINE_MAX - 1u);
    input[AI_CONFIG_LINE_MAX] = 'x';
    input[AI_CONFIG_LINE_MAX + 1u] = '\0';
    CHECK(parse_text(&config, &parser, input, 17u) == AI_CONFIG_LINE_TOO_LONG);

    memcpy(value, "ssid=", 5u);
    memset(value + 5u, 's', AI_CONFIG_SSID_CAPACITY);
    value[5u + AI_CONFIG_SSID_CAPACITY] = '\0';
    CHECK(parse_text(&config, &parser, value, 99u) == AI_CONFIG_VALUE_TOO_LONG);
    CHECK_STR_EQ(ai_config_parser_error_key(&parser), "ssid");
}

static void check_serializer_bounds_and_round_trip(void) {
    ai_config_t config;
    ai_config_t parsed;
    ai_config_parser_t parser;
    unsigned char guarded[1024];
    size_t required = 0u;
    size_t used = 0u;

    CHECK(parse_text(&config, &parser,
                     "ssid=wifi\npassword=hidden\nprovider=muse\nhost=muse.local\nport=443\n"
                     "model=chat\nbearer_token=opaque\nconnect_timeout_ms=500\n"
                     "request_timeout_ms=1000\nidle_timeout_ms=100\nmax_predict=1\n"
                     "utc_offset_minutes=0\n", 7u) == AI_CONFIG_OK);
    CHECK(ai_config_serialize(&config, NULL, 0u, &required) == AI_CONFIG_NO_SPACE);
    CHECK(required + 1u < sizeof(guarded));
    memset(guarded, 0xa5, sizeof(guarded));
    CHECK(ai_config_serialize(&config, (char *)guarded, required, &used) == AI_CONFIG_NO_SPACE);
    CHECK(used == required);
    CHECK(guarded[0] == 0xa5u);
    CHECK(ai_config_serialize(&config, (char *)guarded, required + 1u, &used) == AI_CONFIG_OK);
    CHECK(used == required);
    CHECK(guarded[required] == '\0');
    CHECK(guarded[required + 1u] == 0xa5u);
    CHECK(parse_text(&parsed, &parser, (const char *)guarded, 1u) == AI_CONFIG_OK);
    CHECK(memcmp(&parsed, &config, sizeof(config)) == 0);
}

static void check_null_safety(void) {
    ai_config_t config;
    ai_config_parser_t parser;
    size_t length = 123u;

    CHECK(ai_config_init(NULL) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(ai_config_parser_init(NULL, &config) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(ai_config_parser_init(&parser, NULL) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(ai_config_parser_feed(NULL, "x", 1u) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(ai_config_parser_init(&parser, &config) == AI_CONFIG_OK);
    CHECK(ai_config_parser_feed(&parser, NULL, 0u) == AI_CONFIG_OK);
    CHECK(ai_config_parser_feed(&parser, NULL, 1u) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(ai_config_parser_finish(NULL) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(ai_config_parser_error_key(NULL) == NULL);
    CHECK(ai_config_serialize(NULL, NULL, 0u, &length) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(ai_config_serialize(&config, NULL, 1u, &length) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(ai_config_serialize(&config, NULL, 0u, NULL) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(ai_config_parser_finish(&parser) == AI_CONFIG_OK);
    CHECK(ai_config_parser_finish(&parser) == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(ai_config_parser_feed(&parser, "x", 1u) == AI_CONFIG_INVALID_ARGUMENT);
}

static void check_system_prompt_and_temperature(void) {
    ai_config_t config;
    ai_config_t reloaded;
    ai_config_parser_t parser;
    char output[512];
    size_t length = 0u;

    CHECK(ai_config_init(&config) == AI_CONFIG_OK);
    CHECK(config.temperature == 80u);
    CHECK_STR_EQ(config.system_prompt, AI_CONFIG_DEFAULT_SYSTEM_PROMPT);

    memcpy(config.system_prompt, "be brief", sizeof("be brief"));
    config.temperature = 150u;
    CHECK(ai_config_serialize(&config, output, sizeof(output), &length) == AI_CONFIG_OK);
    CHECK(parse_text(&reloaded, &parser, output, 99u) == AI_CONFIG_OK);
    CHECK_STR_EQ(reloaded.system_prompt, "be brief");
    CHECK(reloaded.temperature == 150u);
}

static void check_utc_offset(void) {
    ai_config_t config;
    ai_config_t reloaded;
    ai_config_parser_t parser;
    char output[512];
    size_t length = 0u;

    /* Signed, with an optional '+', inclusive of both bounds. */
    CHECK(parse_text(&config, &parser, "utc_offset_minutes=-300\n", 5u) == AI_CONFIG_OK);
    CHECK(config.utc_offset_minutes == -300);
    CHECK(parse_text(&config, &parser, "utc_offset_minutes=+330\n", 5u) == AI_CONFIG_OK);
    CHECK(config.utc_offset_minutes == 330);
    CHECK(parse_text(&config, &parser, "utc_offset_minutes=-720\n", 99u) == AI_CONFIG_OK);
    CHECK(config.utc_offset_minutes == AI_CONFIG_UTC_OFFSET_MINUTES_MIN);
    CHECK(parse_text(&config, &parser, "utc_offset_minutes=840\n", 99u) == AI_CONFIG_OK);
    CHECK(config.utc_offset_minutes == AI_CONFIG_UTC_OFFSET_MINUTES_MAX);

    CHECK(parse_text(&config, &parser, "utc_offset_minutes=-721\n", 99u) == AI_CONFIG_OUT_OF_RANGE);
    CHECK_STR_EQ(ai_config_parser_error_key(&parser), "utc_offset_minutes");
    CHECK(parse_text(&config, &parser, "utc_offset_minutes=841\n", 99u) == AI_CONFIG_OUT_OF_RANGE);
    /* Within the magnitude of one bound but past the other. */
    CHECK(parse_text(&config, &parser, "utc_offset_minutes=-840\n", 99u) == AI_CONFIG_OUT_OF_RANGE);
    CHECK(parse_text(&config, &parser, "utc_offset_minutes=99999999999\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "utc_offset_minutes=\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "utc_offset_minutes=-\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "utc_offset_minutes=--5\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "utc_offset_minutes=5.5\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "utc_offset_minutes=60\nutc_offset_minutes=60\n", 99u) ==
          AI_CONFIG_DUPLICATE_KEY);

    /* Both signs survive a save and reload. */
    ai_config_init(&config);
    config.utc_offset_minutes = -300;
    CHECK(ai_config_serialize(&config, output, sizeof(output), &length) == AI_CONFIG_OK);
    CHECK(length == strlen(output));
    CHECK(strstr(output, "\nutc_offset_minutes=-300\n") != NULL);
    CHECK(parse_text(&reloaded, &parser, output, 7u) == AI_CONFIG_OK);
    CHECK(reloaded.utc_offset_minutes == -300);

    config.utc_offset_minutes = 330;
    CHECK(ai_config_serialize(&config, output, sizeof(output), &length) == AI_CONFIG_OK);
    CHECK(length == strlen(output));
    CHECK(strstr(output, "\nutc_offset_minutes=330\n") != NULL);
    CHECK(parse_text(&reloaded, &parser, output, 7u) == AI_CONFIG_OK);
    CHECK(reloaded.utc_offset_minutes == 330);

    /* The exact size is still demanded with a sign in the output. */
    config.utc_offset_minutes = -720;
    CHECK(ai_config_serialize(&config, NULL, 0u, &length) == AI_CONFIG_NO_SPACE);
    CHECK(ai_config_serialize(&config, output, length, &length) == AI_CONFIG_NO_SPACE);
    CHECK(ai_config_serialize(&config, output, length + 1u, &length) == AI_CONFIG_OK);
    CHECK(length == strlen(output));

    /* An offset outside the range is never written. */
    config.utc_offset_minutes = -721;
    CHECK(ai_config_serialize(&config, output, sizeof(output), &length) == AI_CONFIG_INVALID_VALUE);
    config.utc_offset_minutes = 841;
    CHECK(ai_config_serialize(&config, output, sizeof(output), &length) == AI_CONFIG_INVALID_VALUE);
}

static void check_deepseek_defaults(void) {
    ai_config_t config;
    ai_config_parser_t parser;
    char text[1024];
    size_t length = 0u;

    /* A file that only names the provider gets DeepSeek's endpoint and the
     * default model.  The literals are spelled out, not taken from the code. */
    CHECK(parse_text(&config, &parser, "provider=deepseek\nbearer_token=sk-test-not-a-real-key\n", 5u) == AI_CONFIG_OK);
    CHECK_STR_EQ(config.provider, "deepseek");
    CHECK_STR_EQ(config.host, "api.deepseek.com");
    CHECK(config.port == 443u);
    CHECK_STR_EQ(config.model, "deepseek-chat");
    CHECK_STR_EQ(config.bearer_token, "sk-test-not-a-real-key");
    CHECK(ai_config_is_deepseek(&config));
    CHECK(ai_config_validate(&config) == AI_CONFIG_OK);

    /* Key order does not matter, and an empty model takes the default. */
    CHECK(parse_text(&config, &parser, "model=\nport=443\nhost=api.deepseek.com\nprovider=deepseek\n", 3u) == AI_CONFIG_OK);
    CHECK_STR_EQ(config.model, "deepseek-chat");
    CHECK_STR_EQ(config.bearer_token, "");
    CHECK(parse_text(&config, &parser, "model=deepseek-reasoner\nprovider=deepseek\n", 99u) == AI_CONFIG_OK);
    CHECK_STR_EQ(config.model, "deepseek-reasoner");

    /* It round-trips through the serializer. */
    CHECK(ai_config_serialize(&config, text, sizeof(text), &length) == AI_CONFIG_OK);
    CHECK(strstr(text, "provider=deepseek\n") != NULL);
    CHECK(strstr(text, "host=api.deepseek.com\n") != NULL);
    CHECK(strstr(text, "port=443\n") != NULL);
    CHECK(strstr(text, "model=deepseek-reasoner\n") != NULL);
    {
        ai_config_t again;
        CHECK(parse_text(&again, &parser, text, 1u) == AI_CONFIG_OK);
        CHECK(memcmp(&again, &config, sizeof(config)) == 0);
    }

    /* The other providers are untouched by any of this. */
    CHECK(parse_text(&config, &parser, "provider=ollama\nhost=wang.local\nmodel=llama3.2:1b\nbearer_token=any thing\n", 99u) == AI_CONFIG_OK);
    CHECK(!ai_config_is_deepseek(&config));
    CHECK(config.port == 11434u);
    CHECK(parse_text(&config, &parser, "provider=muse\nhost=muse.local\nport=8443\nmodel=deepseek-chat\n", 99u) == AI_CONFIG_OK);
    CHECK_STR_EQ(config.host, "muse.local");
    CHECK(!ai_config_is_deepseek(NULL));
}

/* A key is remembered per provider, so switching away and back does not mean
 * typing it again - and it is still only ever offered to its own host. */
static void check_provider_keys_are_remembered(void) {
    ai_config_t config;
    ai_config_t again;
    ai_config_parser_t parser;
    int deepseek = ai_config_key_slot("deepseek");
    int muse = ai_config_key_slot("muse");
    char text[4096];
    size_t length = 0u;

    CHECK(ai_config_key_slot("ollama") == 0);
    CHECK(muse == 1);
    CHECK(deepseek == 2);
    CHECK(ai_config_key_slot("claude") == (int)AI_CONFIG_KEY_SLOT_COUNT - 1);
    CHECK(ai_config_key_slot("bedrock") == -1);
    CHECK(ai_config_key_slot(NULL) == -1);
    CHECK_STR_EQ(ai_config_key_slot_name(0u), "ollama");
    CHECK_STR_EQ(ai_config_key_slot_name(1u), "muse");
    CHECK_STR_EQ(ai_config_key_slot_name(2u), "deepseek");
    CHECK(ai_config_key_slot_name(AI_CONFIG_KEY_SLOT_COUNT) == NULL);

    ai_config_init(&config);
    CHECK(ai_config_set_provider(&config, "muse") == AI_CONFIG_OK);
    strcpy(config.host, "192.0.2.9");
    strcpy(config.bearer_token, "0123456789abcdef");

    /* Switching away puts the bridge's token under muse... */
    CHECK(ai_config_set_provider(&config, "deepseek") == AI_CONFIG_OK);
    CHECK_STR_EQ(config.bearer_token, "");
    strcpy(config.bearer_token, "fedcba9876543210");

    /* ...and switching back brings that token, not the cloud's. */
    CHECK(ai_config_set_provider(&config, "muse") == AI_CONFIG_OK);
    CHECK_STR_EQ(config.bearer_token, "0123456789abcdef");

    /* Both are written: the one in force as bearer_token, the other under the
     * provider it belongs to. */
    CHECK(ai_config_serialize(&config, text, sizeof(text), &length) == AI_CONFIG_OK);
    CHECK(strstr(text, "bearer_token=0123456789abcdef\n") != NULL);
    CHECK(strstr(text, "key_deepseek=fedcba9876543210\n") != NULL);

    ai_config_init(&again);
    CHECK(parse_text(&again, &parser, text, 1u) == AI_CONFIG_OK);
    CHECK_BYTES_EQ(&again, &config, sizeof(config));

    /* A key is only ever written under its own provider's name. */
    CHECK(parse_text(&again, &parser, text, 1u) == AI_CONFIG_OK);
    CHECK_STR_EQ(again.provider_keys[ai_config_key_slot("deepseek")], "fedcba9876543210");
    CHECK_STR_EQ(again.provider_keys[ai_config_key_slot("muse")], "0123456789abcdef");

    /* A file that names only the remembered key still puts it in force. */
    CHECK(parse_text(&again, &parser,
                     "provider=muse\nhost=192.0.2.9\nport=8765\nmodel=muse\n"
                     "key_muse=abcdef0123456789\n",
                     1u) == AI_CONFIG_OK);
    CHECK_STR_EQ(again.bearer_token, "abcdef0123456789");
    CHECK(ai_config_validate(&again) == AI_CONFIG_OK);

    /* A remembered key is held to the same rule as the one in force: it is only
     * ever sent as a header value, wherever it is kept. */
    CHECK(parse_text(&again, &parser,
                     "provider=muse\nhost=192.0.2.9\nport=8765\nmodel=muse\n"
                     "key_muse=not a token\n",
                     1u) == AI_CONFIG_OK);
    CHECK(ai_config_validate(&again) == AI_CONFIG_INVALID_VALUE);
    CHECK(ai_config_serialize(&again, text, sizeof(text), &length) == AI_CONFIG_INVALID_VALUE);

    /* Twice for the same provider is a duplicate key, as for any other line. */
    CHECK(parse_text(&again, &parser, "provider=muse\nkey_muse=aaaaaaaa\nkey_muse=bbbbbbbb\n",
                     1u) == AI_CONFIG_DUPLICATE_KEY);
    CHECK_STR_EQ(ai_config_parser_error_key(&parser), "key_muse");

    /* A name that is not a provider is ignored, like any other unknown line. */
    CHECK(parse_text(&again, &parser, "provider=muse\nkey_bedrock=zzz\nkey_muse=a\n", 1u) ==
          AI_CONFIG_OK);
    CHECK_STR_EQ(again.bearer_token, "a");
    CHECK(parse_text(&again, &parser, "provider=muse\nkey_muse=a\nkey_muse=\n", 1u) ==
          AI_CONFIG_DUPLICATE_KEY);
}

static void check_deepseek_validation(void) {
    ai_config_t config;
    ai_config_parser_t parser;
    char text[1024];
    size_t length = 99u;

    /* The API key is only ever sent to DeepSeek: no other host or port. */
    CHECK(parse_text(&config, &parser, "provider=deepseek\nhost=evil.example\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK_STR_EQ(ai_config_parser_error_key(&parser), "host");
    CHECK(parse_text(&config, &parser, "host=api.deepseek.com.evil.example\nprovider=deepseek\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "provider=deepseek\nhost=API.DEEPSEEK.COM\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "provider=deepseek\nport=80\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK_STR_EQ(ai_config_parser_error_key(&parser), "port");
    CHECK(parse_text(&config, &parser, "port=11434\nprovider=deepseek\n", 4u) == AI_CONFIG_INVALID_VALUE);

    /* Only the known models. */
    CHECK(parse_text(&config, &parser, "provider=deepseek\nmodel=gpt-4o\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK_STR_EQ(ai_config_parser_error_key(&parser), "model");
    CHECK(parse_text(&config, &parser, "model=deepseek-coder\nprovider=deepseek\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "provider=deepseek\nmodel=DeepSeek-Chat\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(ai_config_deepseek_model_known("deepseek-chat"));
    CHECK(ai_config_deepseek_model_known("deepseek-reasoner"));
    CHECK(!ai_config_deepseek_model_known("deepseek-chat-v2"));
    CHECK(!ai_config_deepseek_model_known("deepseek"));
    CHECK(!ai_config_deepseek_model_known(""));
    CHECK(!ai_config_deepseek_model_known(NULL));

    /* The key must be usable as a bearer token as it stands. */
    CHECK(parse_text(&config, &parser, "provider=deepseek\nbearer_token=sk has space\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK_STR_EQ(ai_config_parser_error_key(&parser), "bearer_token");
    CHECK(parse_text(&config, &parser, "bearer_token=sk-\"q\"\nprovider=deepseek\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(parse_text(&config, &parser, "provider=deepseek\nbearer_token=sk:colon\n", 99u) == AI_CONFIG_INVALID_VALUE);
    CHECK(ai_config_bearer_token_valid("sk-0123456789abcdefABCDEF._~+/=="));
    CHECK(!ai_config_bearer_token_valid(""));
    CHECK(!ai_config_bearer_token_valid("a=b"));
    CHECK(!ai_config_bearer_token_valid("caf\xc3\xa9"));

    /* The serializer refuses to write what the parser would refuse to read. */
    ai_config_init(&config);
    CHECK(ai_config_set_provider(&config, "deepseek") == AI_CONFIG_OK);
    CHECK(ai_config_serialize(&config, text, sizeof(text), &length) == AI_CONFIG_OK);
    strcpy(config.host, "evil.example");
    CHECK(ai_config_validate(&config) == AI_CONFIG_INVALID_VALUE);
    CHECK(ai_config_serialize(&config, text, sizeof(text), &length) == AI_CONFIG_INVALID_VALUE);
    CHECK(length == 0u);
    strcpy(config.host, "api.deepseek.com");
    config.port = 8443u;
    CHECK(ai_config_serialize(&config, text, sizeof(text), &length) == AI_CONFIG_INVALID_VALUE);
    config.port = 443u;
    strcpy(config.model, "other");
    CHECK(ai_config_serialize(&config, text, sizeof(text), &length) == AI_CONFIG_INVALID_VALUE);
    strcpy(config.model, "deepseek-chat");
    strcpy(config.bearer_token, "bad token");
    CHECK(ai_config_serialize(&config, text, sizeof(text), &length) == AI_CONFIG_INVALID_VALUE);
    strcpy(config.bearer_token, "good-token");
    CHECK(ai_config_serialize(&config, text, sizeof(text), &length) == AI_CONFIG_OK);
    CHECK(ai_config_validate(NULL) == AI_CONFIG_INVALID_ARGUMENT);
}

static void check_set_provider(void) {
    ai_config_t config;

    ai_config_init(&config);
    strcpy(config.host, "wang.local");
    strcpy(config.model, "llama3.2:1b");
    strcpy(config.bearer_token, "9f8e7d6c5b4a39281706f5e4d3c2b1a0");
    config.port = 8080u;

    /* The same provider again changes nothing, so its key stays. */
    CHECK(ai_config_set_provider(&config, "ollama") == AI_CONFIG_OK);
    CHECK_STR_EQ(config.host, "wang.local");
    CHECK(config.port == 8080u);
    CHECK_STR_EQ(config.bearer_token, "9f8e7d6c5b4a39281706f5e4d3c2b1a0");

    /* Changing provider always drops the key: it belonged to the old one, and
     * no key is ever offered to a host it was not meant for. */
    CHECK(ai_config_set_provider(&config, "deepseek") == AI_CONFIG_OK);
    CHECK_STR_EQ(config.provider, "deepseek");
    CHECK_STR_EQ(config.host, "api.deepseek.com");
    CHECK(config.port == 443u);
    CHECK_STR_EQ(config.model, "deepseek-chat");
    CHECK_STR_EQ(config.bearer_token, "");
    CHECK(ai_config_validate(&config) == AI_CONFIG_OK);

    /* Leaving DeepSeek does not carry its endpoint to the next provider. */
    CHECK(ai_config_set_provider(&config, "ollama") == AI_CONFIG_OK);
    CHECK_STR_EQ(config.provider, "ollama");
    CHECK_STR_EQ(config.host, "");
    CHECK(config.port == 11434u);
    CHECK_STR_EQ(config.model, "");

    /* Muse brings the bridge's port and its one model; the host is the user's
     * own machine, so it is left for them to fill in. */
    strcpy(config.host, "192.0.2.9");
    strcpy(config.model, "llama3.2:1b");
    strcpy(config.bearer_token, "9f8e7d6c5b4a39281706f5e4d3c2b1a0");
    CHECK(ai_config_set_provider(&config, "muse") == AI_CONFIG_OK);
    CHECK_STR_EQ(config.provider, "muse");
    CHECK_STR_EQ(config.host, "192.0.2.9");
    CHECK(config.port == 8765u);
    CHECK_STR_EQ(config.model, "muse");
    CHECK_STR_EQ(config.bearer_token, "");
    CHECK(ai_config_validate(&config) == AI_CONFIG_OK);

    CHECK(ai_config_set_provider(&config, "bedrock") == AI_CONFIG_INVALID_VALUE);
    CHECK(ai_config_set_provider(&config, "") == AI_CONFIG_INVALID_VALUE);
    CHECK_STR_EQ(config.provider, "muse");
    CHECK(ai_config_set_provider(NULL, "ollama") == AI_CONFIG_INVALID_ARGUMENT);
    CHECK(ai_config_set_provider(&config, NULL) == AI_CONFIG_INVALID_ARGUMENT);
}

void test_ai_config(void) {
    check_defaults();
    check_incremental_parse();
    check_serialize_defaults();
    check_line_forms_and_unknown_keys();
    check_parse_errors();
    check_numeric_validation();
    check_bounded_input();
    check_serializer_bounds_and_round_trip();
    check_utc_offset();
    check_null_safety();
    check_system_prompt_and_temperature();
    check_deepseek_defaults();
    check_deepseek_validation();
    check_set_provider();
    check_provider_keys_are_remembered();
}
