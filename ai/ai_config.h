#ifndef COYOTE_AI_CONFIG_H
#define COYOTE_AI_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define AI_CONFIG_VERSION 1u

/* Capacities include the trailing NUL. */
#define AI_CONFIG_SSID_CAPACITY 33u
#define AI_CONFIG_PASSWORD_CAPACITY 65u
#define AI_CONFIG_PROVIDER_CAPACITY 9u
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
/* UTC-12:00 to UTC+14:00, the span of offsets in civil use. */
#define AI_CONFIG_UTC_OFFSET_MINUTES_MIN (-720)
#define AI_CONFIG_UTC_OFFSET_MINUTES_MAX 840

#define AI_CONFIG_PROVIDER_OLLAMA "ollama"
#define AI_CONFIG_PROVIDER_MUSE "muse"
#define AI_CONFIG_PROVIDER_DEEPSEEK "deepseek"
#define AI_CONFIG_OLLAMA_PORT 11434u

/*
 * The "deepseek" provider talks straight to DeepSeek over verified TLS.  Its
 * endpoint is fixed: the API key in bearer_token is only ever sent to this
 * host and port, so a configuration naming any other is rejected.  The model
 * must be one of the two below.
 */
#define AI_CONFIG_DEEPSEEK_HOST "api.deepseek.com"
#define AI_CONFIG_DEEPSEEK_PORT 443u
#define AI_CONFIG_DEEPSEEK_MODEL_DEFAULT "deepseek-chat"
#define AI_CONFIG_DEEPSEEK_MODEL_REASONER "deepseek-reasoner"

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
    int16_t utc_offset_minutes; /* local time is UTC plus this */
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

int ai_config_is_deepseek(const ai_config_t *config);
int ai_config_deepseek_model_known(const char *model);
/* True for a non-empty RFC 6750 b64token (letters, digits, "-._~+/" and
 * trailing "="): the only text that may follow "Authorization: Bearer ". */
int ai_config_bearer_token_valid(const char *token);

/*
 * Check the rules that span fields.  A "deepseek" configuration must name the
 * fixed DeepSeek host and port and a known model, and its bearer_token (the
 * API key) must be empty or a valid bearer token.  Returns AI_CONFIG_OK or
 * AI_CONFIG_INVALID_VALUE.  The parser and the serializer both apply it.
 */
ai_config_status_t ai_config_validate(const ai_config_t *config);

/*
 * Switch provider.  Moving to "deepseek" sets its fixed host and port and the
 * default model; moving away from it clears the host and model and restores
 * the Ollama port, so the DeepSeek endpoint is never carried over.  Setting
 * the provider already in use changes nothing.  bearer_token is left alone.
 */
ai_config_status_t ai_config_set_provider(ai_config_t *config, const char *provider);
ai_config_status_t ai_config_parser_init(ai_config_parser_t *parser, ai_config_t *config);
ai_config_status_t ai_config_parser_feed(ai_config_parser_t *parser, const void *data, size_t length);
/* A "deepseek" file may leave out host, port and model: they take the DeepSeek
 * defaults.  The cross-field rules of ai_config_validate are applied here. */
ai_config_status_t ai_config_parser_finish(ai_config_parser_t *parser);
const char *ai_config_parser_error_key(const ai_config_parser_t *parser);
ai_config_status_t ai_config_serialize(const ai_config_t *config, char *destination,
                                       size_t capacity, size_t *length);

#endif
