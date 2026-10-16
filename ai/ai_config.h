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
/* Providers that can keep a key of their own - ollama, muse, and the clouds.
 * A key is remembered per provider, so switching providers does not lose it. */
#define AI_CONFIG_KEY_SLOT_COUNT 7u
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
#define AI_CONFIG_PROVIDER_OPENAI "openai"
#define AI_CONFIG_PROVIDER_MISTRAL "mistral"
#define AI_CONFIG_PROVIDER_GEMINI "gemini"
#define AI_CONFIG_PROVIDER_CLAUDE "claude"
#define AI_CONFIG_OLLAMA_PORT 11434u
/* The Muse bridge answers Ollama's protocol on the port it is given, with one
 * model, which is what picking Muse fills in. */
#define AI_CONFIG_MUSE_PORT 8765u
#define AI_CONFIG_MUSE_MODEL "muse"

/*
 * The cloud providers ("deepseek", "openai", "mistral", "gemini", "claude")
 * are reached directly over verified TLS.  Each one's endpoint is fixed: the
 * API key in bearer_token is only ever sent to that provider's host on port
 * 443, so a configuration naming any other host or port is rejected, and the
 * model must be one of the provider's listed models.  There is no way to
 * name a host, path or model of one's own for a cloud provider.
 */
#define AI_CONFIG_CLOUD_PORT 443u
#define AI_CONFIG_CLOUD_MAX 5u /* how many cloud providers there are */
#define AI_CONFIG_CLOUD_MODELS_MAX 3u

#define AI_CONFIG_DEEPSEEK_HOST "api.deepseek.com"
#define AI_CONFIG_DEEPSEEK_PORT AI_CONFIG_CLOUD_PORT
#define AI_CONFIG_DEEPSEEK_MODEL_DEFAULT "deepseek-chat"
#define AI_CONFIG_DEEPSEEK_MODEL_REASONER "deepseek-reasoner"

#define AI_CONFIG_OPENAI_HOST "api.openai.com"
#define AI_CONFIG_MISTRAL_HOST "api.mistral.ai"
#define AI_CONFIG_GEMINI_HOST "generativelanguage.googleapis.com"
#define AI_CONFIG_CLAUDE_HOST "api.anthropic.com"

typedef struct {
    const char *name; /* the "provider" value */
    const char *host;
    /* The models that may be chosen, the default first; unused entries NULL. */
    const char *models[AI_CONFIG_CLOUD_MODELS_MAX];
} ai_config_cloud_t;

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
    /* The key each provider was last given, so switching providers does not
     * mean typing it again.  The key of the provider in force is
     * bearer_token, and its slot mirrors that; a remembered key is only ever
     * offered to the provider it was typed for. */
    char provider_keys[AI_CONFIG_KEY_SLOT_COUNT][AI_CONFIG_BEARER_TOKEN_CAPACITY];
} ai_config_t;

typedef struct {
    ai_config_t *config;
    char line[AI_CONFIG_LINE_MAX + 1u];
    size_t line_length;
    uint16_t seen_keys;
    unsigned char seen_slots; /* one bit per ai_config_key_slot */
    ai_config_status_t status;
    char error_key[AI_CONFIG_KEY_CAPACITY];
    unsigned char pending_cr;
    unsigned char finished;
} ai_config_parser_t;

ai_config_status_t ai_config_init(ai_config_t *config);

int ai_config_is_deepseek(const ai_config_t *config);
int ai_config_deepseek_model_known(const char *model);

/* The cloud providers, in the order menus list them. */
size_t ai_config_cloud_count(void);
const ai_config_cloud_t *ai_config_cloud_at(size_t index);
/* The entry for a provider name, or NULL if it is not a cloud provider. */
const ai_config_cloud_t *ai_config_cloud_find(const char *provider);
/* The entry for config's provider, or NULL ("ollama", "muse", or NULL). */
const ai_config_cloud_t *ai_config_cloud(const ai_config_t *config);
size_t ai_config_cloud_model_count(const ai_config_cloud_t *cloud);
int ai_config_cloud_model_known(const ai_config_cloud_t *cloud, const char *model);

/* True for a non-empty RFC 6750 b64token (letters, digits, "-._~+/" and
 * trailing "="): the only text that may follow "Authorization: Bearer ".  The
 * same rule is applied to a key sent in a header of its own. */
int ai_config_bearer_token_valid(const char *token);

/* The slot that remembers a provider's key: 0 for ollama, 1 for muse, then the
 * cloud providers in table order - or -1 for a name that is not a provider. */
int ai_config_key_slot(const char *provider);

/* The provider a slot belongs to, or NULL when slot is out of range. */
const char *ai_config_key_slot_name(size_t slot);

/*
 * Check the rules that span fields.  A cloud provider's configuration must
 * name that provider's fixed host and port and one of its models, and its
 * bearer_token (the API key) must be empty or a valid bearer token.  Returns
 * AI_CONFIG_OK or AI_CONFIG_INVALID_VALUE.  The parser and the serializer
 * both apply it.
 */
ai_config_status_t ai_config_validate(const ai_config_t *config);

/*
 * Switch provider.  Moving to a cloud provider sets its fixed host and port
 * and its default model.  Moving away from one clears the host and model,
 * restores the Ollama port when the new provider is not a cloud one, and
 * clears bearer_token: that key belongs to the provider being left and must
 * never be offered to another host.  Otherwise bearer_token is left alone.
 * Setting the provider already in use changes nothing.
 */
ai_config_status_t ai_config_set_provider(ai_config_t *config, const char *provider);
ai_config_status_t ai_config_parser_init(ai_config_parser_t *parser, ai_config_t *config);
ai_config_status_t ai_config_parser_feed(ai_config_parser_t *parser, const void *data, size_t length);
/* A cloud provider's file may leave out host, port and model: they take that
 * provider's own.  The cross-field rules of ai_config_validate are applied
 * here. */
ai_config_status_t ai_config_parser_finish(ai_config_parser_t *parser);
const char *ai_config_parser_error_key(const ai_config_parser_t *parser);
ai_config_status_t ai_config_serialize(const ai_config_t *config, char *destination,
                                       size_t capacity, size_t *length);

#endif
