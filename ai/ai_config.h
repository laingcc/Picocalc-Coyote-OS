#ifndef COYOTE_AI_CONFIG_H
#define COYOTE_AI_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define AI_CONFIG_VERSION 1u

/* Capacities include the trailing NUL. */
#define AI_CONFIG_SSID_CAPACITY 33u
#define AI_CONFIG_PASSWORD_CAPACITY 65u
#define AI_CONFIG_PROVIDER_CAPACITY 7u
#define AI_CONFIG_HOST_CAPACITY 254u
#define AI_CONFIG_MODEL_CAPACITY 129u
#define AI_CONFIG_BEARER_TOKEN_CAPACITY 257u
#define AI_CONFIG_KEY_CAPACITY 32u
#define AI_CONFIG_LINE_MAX 512u

/* Inclusive numeric bounds accepted by the parser and serializer. */
#define AI_CONFIG_CONNECT_TIMEOUT_MS_MIN 100u
#define AI_CONFIG_CONNECT_TIMEOUT_MS_MAX 120000u
#define AI_CONFIG_REQUEST_TIMEOUT_MS_MIN 1000u
#define AI_CONFIG_REQUEST_TIMEOUT_MS_MAX 600000u
#define AI_CONFIG_IDLE_TIMEOUT_MS_MIN 100u
#define AI_CONFIG_IDLE_TIMEOUT_MS_MAX 120000u
#define AI_CONFIG_MAX_PREDICT_MIN 1u
#define AI_CONFIG_MAX_PREDICT_MAX 4096u
#define AI_CONFIG_SYSTEM_PROMPT_CAPACITY 128u
#define AI_CONFIG_TEMPERATURE_MIN 0u
#define AI_CONFIG_TEMPERATURE_MAX 200u
#define AI_CONFIG_TEMPERATURE_DEFAULT 80u
#define AI_CONFIG_DEFAULT_SYSTEM_PROMPT \
    "You are an AI on a Picocalc handheld with a tiny screen and keyboard. Be concise; avoid markdown and long code."

typedef enum {
    AI_CONFIG_OK = 0,
    AI_CONFIG_INVALID_ARGUMENT,
    AI_CONFIG_EMBEDDED_NUL,
    AI_CONFIG_LINE_TOO_LONG,
    AI_CONFIG_MALFORMED_LINE,
    AI_CONFIG_DUPLICATE_KEY,
    AI_CONFIG_VALUE_TOO_LONG,
    AI_CONFIG_INVALID_VALUE,
    AI_CONFIG_UNSUPPORTED_VERSION,
    AI_CONFIG_OUT_OF_RANGE,
    AI_CONFIG_NO_SPACE,
    AI_CONFIG_IO_ERROR
} ai_config_status_t;

typedef struct {
    uint32_t version;
    char ssid[AI_CONFIG_SSID_CAPACITY];
    char password[AI_CONFIG_PASSWORD_CAPACITY];
    char provider[AI_CONFIG_PROVIDER_CAPACITY];
    char host[AI_CONFIG_HOST_CAPACITY];
    uint16_t port;
    char model[AI_CONFIG_MODEL_CAPACITY];
    char bearer_token[AI_CONFIG_BEARER_TOKEN_CAPACITY];
    uint32_t connect_timeout_ms;
    uint32_t request_timeout_ms;
    uint32_t idle_timeout_ms;
    uint32_t max_predict;
    char system_prompt[AI_CONFIG_SYSTEM_PROMPT_CAPACITY];
    uint16_t temperature;
} ai_config_t;

typedef struct {
    ai_config_t *config;
    char line[AI_CONFIG_LINE_MAX + 1u];
    size_t line_length;
    uint16_t seen_keys;
    ai_config_status_t status;
    char error_key[AI_CONFIG_KEY_CAPACITY];
    unsigned char pending_cr;
    unsigned char finished;
} ai_config_parser_t;

ai_config_status_t ai_config_init(ai_config_t *config);
ai_config_status_t ai_config_parser_init(ai_config_parser_t *parser, ai_config_t *config);
ai_config_status_t ai_config_parser_feed(ai_config_parser_t *parser, const void *data, size_t length);
ai_config_status_t ai_config_parser_finish(ai_config_parser_t *parser);
const char *ai_config_parser_error_key(const ai_config_parser_t *parser);
ai_config_status_t ai_config_serialize(const ai_config_t *config, char *destination,
                                       size_t capacity, size_t *length);

#endif
