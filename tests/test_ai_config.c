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
    CHECK_STR_EQ(config.host, "wang.local");
    CHECK(config.port == 11434u);
    CHECK_STR_EQ(config.model, "");
    CHECK_STR_EQ(config.bearer_token, "");
    CHECK(config.connect_timeout_ms == 15000u);
    CHECK(config.request_timeout_ms == 120000u);
    CHECK(config.idle_timeout_ms == 15000u);
    CHECK(config.max_predict == 384u);
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
        "host=wang.local\n"
        "port=11434\n"
        "model=\n"
        "bearer_token=\n"
        "connect_timeout_ms=15000\n"
        "request_timeout_ms=120000\n"
        "idle_timeout_ms=15000\n"
        "max_predict=384\n";
    ai_config_t config;
    char output[sizeof(expected)];
    size_t length = 0u;

    CHECK(ai_config_init(&config) == AI_CONFIG_OK);
    CHECK(ai_config_serialize(&config, output, sizeof(output), &length) == AI_CONFIG_OK);
    CHECK(length == sizeof(expected) - 1u);
    CHECK(strcmp(output, expected) == 0);
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
    static char input[AI_CONFIG_LINE_MAX + 2u];
    static char value[AI_CONFIG_SSID_CAPACITY + 8u];
    ai_config_t config;
    ai_config_parser_t parser;

    input[0] = 'x';
    input[1] = '=';
    memset(input + 2u, 'x', AI_CONFIG_LINE_MAX - 2u);
    input[AI_CONFIG_LINE_MAX] = '\0';
    CHECK(parse_text(&config, &parser, input, 17u) == AI_CONFIG_OK);
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
                     "request_timeout_ms=1000\nidle_timeout_ms=100\nmax_predict=1\n", 7u) == AI_CONFIG_OK);
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

void test_ai_config(void) {
    check_defaults();
    check_incremental_parse();
    check_serialize_defaults();
    check_line_forms_and_unknown_keys();
    check_parse_errors();
    check_numeric_validation();
    check_bounded_input();
    check_serializer_bounds_and_round_trip();
    check_null_safety();
}
