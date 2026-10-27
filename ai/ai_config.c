#include "ai/ai_config.h"

#include <limits.h>
#include <string.h>

enum {
    KEY_VERSION = 0,
    KEY_SSID,
    KEY_PASSWORD,
    KEY_PROVIDER,
    KEY_HOST,
    KEY_PORT,
    KEY_MODEL,
    KEY_BEARER_TOKEN,
    KEY_CONNECT_TIMEOUT,
    KEY_REQUEST_TIMEOUT,
    KEY_IDLE_TIMEOUT,
    KEY_MAX_PREDICT,
    KEY_SYSTEM_PROMPT,
    KEY_TEMPERATURE,
    KEY_UTC_OFFSET,
    /* One remembered key per provider; the order matches token_providers. */
    KEY_BEARER_OLLAMA,
    KEY_BEARER_MUSE,
    KEY_BEARER_DEEPSEEK,
    KEY_BEARER_OPENAI,
    KEY_BEARER_MISTRAL,
    KEY_BEARER_GEMINI,
    KEY_BEARER_CLAUDE,
    KEY_UNKNOWN
};

static const char *const key_names[] = {
    "version", "ssid", "password", "provider", "host", "port", "model",
    "bearer_token", "connect_timeout_ms", "request_timeout_ms", "idle_timeout_ms",
    "max_predict", "system_prompt", "temperature", "utc_offset_minutes",
    "bearer_token_ollama", "bearer_token_muse", "bearer_token_deepseek",
    "bearer_token_openai", "bearer_token_mistral", "bearer_token_gemini",
    "bearer_token_claude"
};

_Static_assert(sizeof(key_names) / sizeof(key_names[0]) == (size_t)KEY_UNKNOWN,
               "key_names is out of step with the key ids");
_Static_assert((size_t)KEY_UNKNOWN <= 32u, "seen_keys cannot hold every key");

/* The providers whose keys are remembered, in slot order.  This is also the
 * order of the bearer_token_<provider> key ids, so the id minus
 * KEY_BEARER_OLLAMA is the slot. */
enum { TOKEN_SLOT_OLLAMA = 0, TOKEN_SLOT_MUSE = 1, TOKEN_SLOT_FIRST_CLOUD = 2 };

static const char *const token_providers[] = {
    AI_CONFIG_PROVIDER_OLLAMA, AI_CONFIG_PROVIDER_MUSE,  AI_CONFIG_PROVIDER_DEEPSEEK,
    AI_CONFIG_PROVIDER_OPENAI, AI_CONFIG_PROVIDER_MISTRAL, AI_CONFIG_PROVIDER_GEMINI,
    AI_CONFIG_PROVIDER_CLAUDE
};

_Static_assert(sizeof(token_providers) / sizeof(token_providers[0]) == AI_CONFIG_TOKEN_SLOTS,
               "token_providers is out of step with AI_CONFIG_TOKEN_SLOTS");

ai_config_status_t ai_config_init(ai_config_t *config) {
    if (config == NULL) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }

    memset(config, 0, sizeof(*config));
    config->version = AI_CONFIG_VERSION;
    memcpy(config->provider, AI_CONFIG_PROVIDER_OLLAMA, sizeof(AI_CONFIG_PROVIDER_OLLAMA));
    /* No default host: a fresh device is unconfigured until one is entered. */
    config->port = AI_CONFIG_OLLAMA_PORT;
    config->connect_timeout_ms = 15000u;
    config->request_timeout_ms = 120000u;
    config->idle_timeout_ms = 15000u;
    config->max_predict = 384u;
    config->temperature = 80u;
    memcpy(config->system_prompt, AI_CONFIG_DEFAULT_SYSTEM_PROMPT,
           sizeof(AI_CONFIG_DEFAULT_SYSTEM_PROMPT));
    return AI_CONFIG_OK;
}

/*
 * The cloud providers.  Model names are the ones each provider documents for
 * its chat API; "-latest" names are that provider's own aliases.
 */
static const ai_config_cloud_t clouds[] = {
    {AI_CONFIG_PROVIDER_DEEPSEEK, AI_CONFIG_DEEPSEEK_HOST,
     {AI_CONFIG_DEEPSEEK_MODEL_DEFAULT, AI_CONFIG_DEEPSEEK_MODEL_REASONER, NULL}},
    {AI_CONFIG_PROVIDER_OPENAI, AI_CONFIG_OPENAI_HOST, {"gpt-6-astra", "gpt-6.1-sol", "gpt-6-luna"}},
    {AI_CONFIG_PROVIDER_MISTRAL, AI_CONFIG_MISTRAL_HOST,
     {"mistral-small-latest", "mistral-medium-latest", "mistral-large-latest"}},
    {AI_CONFIG_PROVIDER_GEMINI, AI_CONFIG_GEMINI_HOST,
     {"gemini-3.8-flash", "gemini-3.6-flash", "gemini-3.5-flash-lite"}},
    {AI_CONFIG_PROVIDER_CLAUDE, AI_CONFIG_CLAUDE_HOST,
     {"claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5"}},
};

_Static_assert(sizeof(clouds) / sizeof(clouds[0]) == AI_CONFIG_CLOUD_MAX, "AI_CONFIG_CLOUD_MAX is out of step");

size_t ai_config_cloud_count(void) {
    return sizeof(clouds) / sizeof(clouds[0]);
}

const ai_config_cloud_t *ai_config_cloud_at(size_t index) {
    return index < ai_config_cloud_count() ? &clouds[index] : NULL;
}

const ai_config_cloud_t *ai_config_cloud_find(const char *provider) {
    size_t i;

    if (provider == NULL) {
        return NULL;
    }
    for (i = 0u; i < ai_config_cloud_count(); i++) {
        if (strcmp(provider, clouds[i].name) == 0) {
            return &clouds[i];
        }
    }
    return NULL;
}

const ai_config_cloud_t *ai_config_cloud(const ai_config_t *config) {
    return config != NULL ? ai_config_cloud_find(config->provider) : NULL;
}

size_t ai_config_cloud_model_count(const ai_config_cloud_t *cloud) {
    size_t count = 0u;

    while (cloud != NULL && count < AI_CONFIG_CLOUD_MODELS_MAX && cloud->models[count] != NULL) {
        count++;
    }
    return count;
}

int ai_config_cloud_model_known(const ai_config_cloud_t *cloud, const char *model) {
    size_t count = ai_config_cloud_model_count(cloud);
    size_t i;

    if (model == NULL) {
        return 0;
    }
    for (i = 0u; i < count; i++) {
        if (strcmp(model, cloud->models[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int known_provider(const char *provider) {
    return strcmp(provider, AI_CONFIG_PROVIDER_OLLAMA) == 0 || strcmp(provider, AI_CONFIG_PROVIDER_MUSE) == 0 ||
           ai_config_cloud_find(provider) != NULL;
}

int ai_config_is_deepseek(const ai_config_t *config) {
    return config != NULL && strcmp(config->provider, AI_CONFIG_PROVIDER_DEEPSEEK) == 0;
}

int ai_config_deepseek_model_known(const char *model) {
    return ai_config_cloud_model_known(ai_config_cloud_find(AI_CONFIG_PROVIDER_DEEPSEEK), model);
}

int ai_config_bearer_token_valid(const char *token) {
    size_t i = 0u;

    if (token == NULL) {
        return 0;
    }
    for (; token[i] != '\0' && token[i] != '='; i++) {
        char c = token[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
              c == '.' || c == '_' || c == '~' || c == '+' || c == '/')) {
            return 0;
        }
    }
    if (i == 0u) {
        return 0;
    }
    for (; token[i] != '\0'; i++) {
        if (token[i] != '=') {
            return 0;
        }
    }
    return i < AI_CONFIG_BEARER_TOKEN_CAPACITY;
}

size_t ai_config_token_slot(const char *provider) {
    size_t i;

    if (provider == NULL) {
        return AI_CONFIG_TOKEN_SLOTS;
    }
    for (i = 0u; i < AI_CONFIG_TOKEN_SLOTS; i++) {
        if (strcmp(token_providers[i], provider) == 0) {
            return i;
        }
    }
    return AI_CONFIG_TOKEN_SLOTS;
}

const char *ai_config_provider_token(const ai_config_t *config, const char *provider) {
    size_t slot = ai_config_token_slot(provider);

    if (config == NULL || slot == AI_CONFIG_TOKEN_SLOTS) {
        return "";
    }
    return config->tokens[slot];
}

ai_config_status_t ai_config_set_provider_token(ai_config_t *config, const char *provider,
                                                const char *token) {
    size_t slot = ai_config_token_slot(provider);
    size_t length;

    if (config == NULL || token == NULL) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    if (slot == AI_CONFIG_TOKEN_SLOTS) {
        return AI_CONFIG_INVALID_VALUE;
    }
    length = strlen(token);
    if (length >= AI_CONFIG_BEARER_TOKEN_CAPACITY) {
        return AI_CONFIG_VALUE_TOO_LONG;
    }
    /* Wipe first: a shorter key must not leave the tail of the old one. */
    memset(config->tokens[slot], 0, sizeof(config->tokens[slot]));
    memcpy(config->tokens[slot], token, length + 1u);
    if (strcmp(config->provider, provider) == 0) {
        memset(config->bearer_token, 0, sizeof(config->bearer_token));
        memcpy(config->bearer_token, token, length + 1u);
    }
    return AI_CONFIG_OK;
}

/* Make the current provider's own key the live one. */
static void load_active_token(ai_config_t *config) {
    size_t slot = ai_config_token_slot(config->provider);

    memset(config->bearer_token, 0, sizeof(config->bearer_token));
    if (slot != AI_CONFIG_TOKEN_SLOTS) {
        memcpy(config->bearer_token, config->tokens[slot], sizeof(config->bearer_token));
    }
}

/* The field breaking a cross-field rule, or KEY_UNKNOWN if none does. */
static int invalid_provider_field(const ai_config_t *config) {
    const ai_config_cloud_t *cloud = ai_config_cloud(config);

    if (cloud == NULL) {
        return KEY_UNKNOWN;
    }
    if (strcmp(config->host, cloud->host) != 0) {
        return KEY_HOST;
    }
    if (config->port != AI_CONFIG_CLOUD_PORT) {
        return KEY_PORT;
    }
    if (!ai_config_cloud_model_known(cloud, config->model)) {
        return KEY_MODEL;
    }
    if (config->bearer_token[0] != '\0' && !ai_config_bearer_token_valid(config->bearer_token)) {
        return KEY_BEARER_TOKEN;
    }
    return KEY_UNKNOWN;
}

/* Point config at a cloud provider's own endpoint and default model. */
static void use_cloud_endpoint(ai_config_t *config, const ai_config_cloud_t *cloud) {
    memset(config->host, 0, sizeof(config->host));
    memcpy(config->host, cloud->host, strlen(cloud->host) + 1u);
    config->port = AI_CONFIG_CLOUD_PORT;
    memset(config->model, 0, sizeof(config->model));
    memcpy(config->model, cloud->models[0], strlen(cloud->models[0]) + 1u);
}

ai_config_status_t ai_config_validate(const ai_config_t *config) {
    if (config == NULL) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    return invalid_provider_field(config) == KEY_UNKNOWN ? AI_CONFIG_OK : AI_CONFIG_INVALID_VALUE;
}

ai_config_status_t ai_config_set_provider(ai_config_t *config, const char *provider) {
    if (config == NULL || provider == NULL) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    if (!known_provider(provider)) {
        return AI_CONFIG_INVALID_VALUE;
    }
    if (strcmp(config->provider, provider) == 0) {
        return AI_CONFIG_OK;
    }
    if (ai_config_cloud(config) != NULL) {
        /* Nothing of the endpoint being left goes to the next one. */
        memset(config->host, 0, sizeof(config->host));
        config->port = AI_CONFIG_OLLAMA_PORT;
        memset(config->model, 0, sizeof(config->model));
    }
    /* The key stays with the provider it was entered for, and the provider
     * being entered brings its own back: no key ever reaches a host it was not
     * typed for, and none has to be retyped. */
    ai_config_set_provider_token(config, config->provider, config->bearer_token);
    if (ai_config_cloud_find(provider) != NULL) {
        use_cloud_endpoint(config, ai_config_cloud_find(provider));
    }
    memset(config->provider, 0, sizeof(config->provider));
    memcpy(config->provider, provider, strlen(provider) + 1u);
    load_active_token(config);
    return AI_CONFIG_OK;
}

static int key_id(const char *key) {
    size_t i;
    for (i = 0u; i < sizeof(key_names) / sizeof(key_names[0]); i++) {
        if (strcmp(key, key_names[i]) == 0) {
            return (int)i;
        }
    }
    return KEY_UNKNOWN;
}

static ai_config_status_t fail(ai_config_parser_t *parser, ai_config_status_t status,
                               int id) {
    parser->status = status;
    parser->error_key[0] = '\0';
    if (id != KEY_UNKNOWN) {
        memcpy(parser->error_key, key_names[id], strlen(key_names[id]) + 1u);
    }
    return status;
}

static ai_config_status_t copy_value(ai_config_parser_t *parser, int id, char *destination,
                                     size_t capacity, const char *value) {
    size_t length = strlen(value);
    if (length >= capacity) {
        return fail(parser, AI_CONFIG_VALUE_TOO_LONG, id);
    }
    memcpy(destination, value, length + 1u);
    return AI_CONFIG_OK;
}

static ai_config_status_t parse_u32(ai_config_parser_t *parser, int id, const char *value,
                                    uint32_t minimum, uint32_t maximum, uint32_t *result) {
    uint32_t number = 0u;
    const unsigned char *cursor = (const unsigned char *)value;

    if (*cursor == '\0') {
        return fail(parser, AI_CONFIG_INVALID_VALUE, id);
    }
    while (*cursor != '\0') {
        uint32_t digit;
        if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9') {
            return fail(parser, AI_CONFIG_INVALID_VALUE, id);
        }
        digit = (uint32_t)(*cursor - (unsigned char)'0');
        if (number > (UINT32_MAX - digit) / 10u) {
            return fail(parser, AI_CONFIG_INVALID_VALUE, id);
        }
        number = number * 10u + digit;
        cursor++;
    }
    if (number < minimum || number > maximum) {
        return fail(parser, AI_CONFIG_OUT_OF_RANGE, id);
    }
    *result = number;
    return AI_CONFIG_OK;
}

/* A decimal with an optional sign.  The magnitude is parsed as an unsigned
 * number no larger than the wider bound, so it always fits the result. */
static ai_config_status_t parse_i32(ai_config_parser_t *parser, int id, const char *value,
                                    int32_t minimum, int32_t maximum, int32_t *result) {
    uint32_t limit = (uint32_t)(-minimum > maximum ? -minimum : maximum);
    uint32_t magnitude;
    int negative = *value == '-';
    int32_t number;
    ai_config_status_t status;

    if (*value == '-' || *value == '+') {
        value++;
    }
    status = parse_u32(parser, id, value, 0u, limit, &magnitude);
    if (status != AI_CONFIG_OK) {
        return status;
    }
    number = negative ? -(int32_t)magnitude : (int32_t)magnitude;
    if (number < minimum || number > maximum) {
        return fail(parser, AI_CONFIG_OUT_OF_RANGE, id);
    }
    *result = number;
    return AI_CONFIG_OK;
}

static ai_config_status_t parse_known(ai_config_parser_t *parser, int id, const char *value) {
    ai_config_t *config = parser->config;
    uint32_t number;
    int32_t offset;
    ai_config_status_t status = AI_CONFIG_OK;

    if ((parser->seen_keys & (1u << (unsigned)id)) != 0u) {
        return fail(parser, AI_CONFIG_DUPLICATE_KEY, id);
    }
    parser->seen_keys |= (1u << (unsigned)id);

    switch (id) {
        case KEY_VERSION:
            status = parse_u32(parser, id, value, 0u, UINT32_MAX, &number);
            if (status == AI_CONFIG_OK && number != AI_CONFIG_VERSION) {
                return fail(parser, AI_CONFIG_UNSUPPORTED_VERSION, id);
            }
            if (status == AI_CONFIG_OK) {
                config->version = number;
            }
            return status;
        case KEY_SSID:
            return copy_value(parser, id, config->ssid, sizeof(config->ssid), value);
        case KEY_PASSWORD:
            return copy_value(parser, id, config->password, sizeof(config->password), value);
        case KEY_PROVIDER:
            if (!known_provider(value)) {
                return fail(parser, AI_CONFIG_INVALID_VALUE, id);
            }
            return copy_value(parser, id, config->provider, sizeof(config->provider), value);
        case KEY_HOST:
            if (*value == '\0') {
                return fail(parser, AI_CONFIG_INVALID_VALUE, id);
            }
            return copy_value(parser, id, config->host, sizeof(config->host), value);
        case KEY_PORT:
            status = parse_u32(parser, id, value, 1u, 65535u, &number);
            if (status == AI_CONFIG_OK) {
                config->port = (uint16_t)number;
            }
            return status;
        case KEY_MODEL:
            return copy_value(parser, id, config->model, sizeof(config->model), value);
        case KEY_BEARER_TOKEN:
            /* The older single-key form.  The provider it belongs to may be
             * named before or after this line, so hold it until the end. */
            parser->pending_token_set = 1u;
            return copy_value(parser, id, parser->pending_token,
                              sizeof(parser->pending_token), value);
        case KEY_BEARER_OLLAMA:
        case KEY_BEARER_MUSE:
        case KEY_BEARER_DEEPSEEK:
        case KEY_BEARER_OPENAI:
        case KEY_BEARER_MISTRAL:
        case KEY_BEARER_GEMINI:
        case KEY_BEARER_CLAUDE:
            return copy_value(parser, id,
                              config->tokens[(size_t)id - (size_t)KEY_BEARER_OLLAMA],
                              AI_CONFIG_BEARER_TOKEN_CAPACITY, value);
        case KEY_CONNECT_TIMEOUT:
            return parse_u32(parser, id, value, AI_CONFIG_CONNECT_TIMEOUT_MS_MIN,
                             AI_CONFIG_CONNECT_TIMEOUT_MS_MAX, &config->connect_timeout_ms);
        case KEY_REQUEST_TIMEOUT:
            return parse_u32(parser, id, value, AI_CONFIG_REQUEST_TIMEOUT_MS_MIN,
                             AI_CONFIG_REQUEST_TIMEOUT_MS_MAX, &config->request_timeout_ms);
        case KEY_IDLE_TIMEOUT:
            return parse_u32(parser, id, value, AI_CONFIG_IDLE_TIMEOUT_MS_MIN,
                             AI_CONFIG_IDLE_TIMEOUT_MS_MAX, &config->idle_timeout_ms);
        case KEY_MAX_PREDICT:
            return parse_u32(parser, id, value, AI_CONFIG_MAX_PREDICT_MIN,
                             AI_CONFIG_MAX_PREDICT_MAX, &config->max_predict);
        case KEY_SYSTEM_PROMPT: {
            const unsigned char *p;
            for (p = (const unsigned char *)value; *p != '\0'; p++) {
                if (*p < 0x20u || *p == 0x7fu) {
                    return fail(parser, AI_CONFIG_INVALID_VALUE, id);
                }
            }
            return copy_value(parser, id, config->system_prompt, sizeof(config->system_prompt), value);
        }
        case KEY_TEMPERATURE:
            status = parse_u32(parser, id, value, AI_CONFIG_TEMPERATURE_MIN,
                               AI_CONFIG_TEMPERATURE_MAX, &number);
            if (status == AI_CONFIG_OK) {
                config->temperature = (uint16_t)number;
            }
            return status;
        case KEY_UTC_OFFSET:
            status = parse_i32(parser, id, value, AI_CONFIG_UTC_OFFSET_MINUTES_MIN,
                               AI_CONFIG_UTC_OFFSET_MINUTES_MAX, &offset);
            if (status == AI_CONFIG_OK) {
                config->utc_offset_minutes = (int16_t)offset;
            }
            return status;
        default:
            return AI_CONFIG_OK;
    }
}

static ai_config_status_t parse_line(ai_config_parser_t *parser) {
    char *begin = parser->line;
    char *end = begin + parser->line_length;
    char *separator;
    int id;

    if (end > begin && end[-1] == '\r') {
        end--;
    }
    *end = '\0';
    while (*begin == ' ' || *begin == '\t') {
        begin++;
    }
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    if (*begin == '\0' || *begin == '#' || *begin == ';') {
        return AI_CONFIG_OK;
    }

    separator = strchr(begin, '=');
    if (separator == NULL) {
        return fail(parser, AI_CONFIG_MALFORMED_LINE, KEY_UNKNOWN);
    }
    *separator = '\0';
    end = separator;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    if (*begin == '\0') {
        return fail(parser, AI_CONFIG_MALFORMED_LINE, KEY_UNKNOWN);
    }

    id = key_id(begin);
    if (id == KEY_UNKNOWN) {
        return AI_CONFIG_OK;
    }

    begin = separator + 1;
    while (*begin == ' ' || *begin == '\t') {
        begin++;
    }
    end = parser->line + parser->line_length;
    if (end > parser->line && end[-1] == '\r') {
        end--;
    }
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';
    return parse_known(parser, id, begin);
}

ai_config_status_t ai_config_parser_init(ai_config_parser_t *parser, ai_config_t *config) {
    if (parser == NULL || config == NULL) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    memset(parser, 0, sizeof(*parser));
    parser->config = config;
    return ai_config_init(config);
}

ai_config_status_t ai_config_parser_feed(ai_config_parser_t *parser, const void *data,
                                         size_t length) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t i;

    if (parser == NULL || (data == NULL && length != 0u) || parser->config == NULL ||
        parser->finished != 0u) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    if (parser->status != AI_CONFIG_OK) {
        return parser->status;
    }
    for (i = 0u; i < length; i++) {
        ai_config_status_t status;
        if (bytes[i] == '\0') {
            return fail(parser, AI_CONFIG_EMBEDDED_NUL, KEY_UNKNOWN);
        }
        if (parser->pending_cr != 0u) {
            if (bytes[i] != '\n') {
                return fail(parser, AI_CONFIG_MALFORMED_LINE, KEY_UNKNOWN);
            }
            parser->pending_cr = 0u;
            status = parse_line(parser);
            parser->line_length = 0u;
            if (status != AI_CONFIG_OK) {
                return status;
            }
        } else if (bytes[i] == '\r') {
            parser->pending_cr = 1u;
        } else if (bytes[i] == '\n') {
            status = parse_line(parser);
            parser->line_length = 0u;
            if (status != AI_CONFIG_OK) {
                return status;
            }
        } else {
            if (parser->line_length == AI_CONFIG_LINE_MAX) {
                return fail(parser, AI_CONFIG_LINE_TOO_LONG, KEY_UNKNOWN);
            }
            parser->line[parser->line_length++] = (char)bytes[i];
        }
    }
    return AI_CONFIG_OK;
}

static int seen(const ai_config_parser_t *parser, int id) {
    return (parser->seen_keys & (1u << (unsigned)id)) != 0u;
}

/* Fill in what a cloud provider's file left out, then apply the cross-field
 * rules. */
static ai_config_status_t finish_provider(ai_config_parser_t *parser) {
    ai_config_t *config = parser->config;
    const ai_config_cloud_t *cloud = ai_config_cloud(config);
    size_t slot;
    int id;

    if (cloud != NULL) {
        if (!seen(parser, KEY_HOST)) {
            memcpy(config->host, cloud->host, strlen(cloud->host) + 1u);
        }
        if (!seen(parser, KEY_PORT)) {
            config->port = AI_CONFIG_CLOUD_PORT;
        }
        if (config->model[0] == '\0') {
            memcpy(config->model, cloud->models[0], strlen(cloud->models[0]) + 1u);
        }
    }
    /* A legacy key belongs to the provider this file names; an explicit key for
     * that provider's slot wins over it. */
    slot = ai_config_token_slot(config->provider);
    if (slot != AI_CONFIG_TOKEN_SLOTS && parser->pending_token_set != 0u &&
        config->tokens[slot][0] == '\0') {
        memcpy(config->tokens[slot], parser->pending_token, sizeof(config->tokens[slot]));
    }
    load_active_token(config);
    /* The selected provider's key is reported by its own field name, so an
     * older file that used "bearer_token" still points at that line. */
    id = invalid_provider_field(config);
    if (id != KEY_UNKNOWN) {
        return fail(parser, AI_CONFIG_INVALID_VALUE, id);
    }
    /* Every remembered cloud key must be usable as a header value, not only the
     * one selected right now: switching to it must not discover a bad key. */
    for (slot = TOKEN_SLOT_FIRST_CLOUD; slot < AI_CONFIG_TOKEN_SLOTS; slot++) {
        if (slot == ai_config_token_slot(config->provider)) {
            continue;
        }
        if (config->tokens[slot][0] != '\0' && !ai_config_bearer_token_valid(config->tokens[slot])) {
            return fail(parser, AI_CONFIG_INVALID_VALUE, (int)KEY_BEARER_OLLAMA + (int)slot);
        }
    }
    return AI_CONFIG_OK;
}

ai_config_status_t ai_config_parser_finish(ai_config_parser_t *parser) {
    ai_config_status_t status;
    if (parser == NULL || parser->config == NULL || parser->finished != 0u) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    parser->finished = 1u;
    if (parser->status != AI_CONFIG_OK) {
        return parser->status;
    }
    if (parser->pending_cr != 0u) {
        return fail(parser, AI_CONFIG_MALFORMED_LINE, KEY_UNKNOWN);
    }
    if (parser->line_length != 0u) {
        status = parse_line(parser);
        parser->line_length = 0u;
        if (status != AI_CONFIG_OK) {
            return status;
        }
    }
    return finish_provider(parser);
}

const char *ai_config_parser_error_key(const ai_config_parser_t *parser) {
    if (parser == NULL || parser->error_key[0] == '\0') {
        return NULL;
    }
    return parser->error_key;
}

static size_t bounded_length(const char *text, size_t capacity) {
    size_t length;
    for (length = 0u; length < capacity && text[length] != '\0'; length++) {
    }
    return length;
}

static int serializable_text(const char *text, size_t capacity, int allow_empty) {
    size_t length = bounded_length(text, capacity);
    size_t i;
    if (length == capacity || (!allow_empty && length == 0u)) {
        return 0;
    }
    if (length > 0u && (text[0] == ' ' || text[0] == '\t' ||
                        text[length - 1u] == ' ' || text[length - 1u] == '\t')) {
        return 0;
    }
    for (i = 0u; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20u || c == 0x7fu) {
            return 0;
        }
    }
    return 1;
}

static size_t decimal_length(uint32_t number) {
    size_t length = 1u;
    while (number >= 10u) {
        number /= 10u;
        length++;
    }
    return length;
}

static void append_text(char *destination, size_t *offset, const char *text) {
    size_t length = strlen(text);
    memcpy(destination + *offset, text, length);
    *offset += length;
}

static void append_number(char *destination, size_t *offset, uint32_t number) {
    char digits[10];
    size_t count = 0u;
    do {
        digits[count++] = (char)('0' + number % 10u);
        number /= 10u;
    } while (number != 0u);
    while (count > 0u) {
        destination[(*offset)++] = digits[--count];
    }
}

static uint32_t magnitude_of(int32_t number) {
    return number < 0 ? (uint32_t)-number : (uint32_t)number;
}

/* One "bearer_token_<provider>=<key>\n" per provider that has a key. */
static size_t token_lines_length(const ai_config_t *config) {
    size_t total = 0u;
    size_t slot;

    for (slot = 0u; slot < AI_CONFIG_TOKEN_SLOTS; slot++) {
        if (config->tokens[slot][0] == '\0') {
            continue;
        }
        total += sizeof("bearer_token_") - 1u + strlen(token_providers[slot]) + 1u +
                 strlen(config->tokens[slot]) + 1u;
    }
    return total;
}

static void append_token_lines(char *destination, size_t *offset, const ai_config_t *config) {
    size_t slot;

    for (slot = 0u; slot < AI_CONFIG_TOKEN_SLOTS; slot++) {
        if (config->tokens[slot][0] == '\0') {
            continue;
        }
        append_text(destination, offset, "bearer_token_");
        append_text(destination, offset, token_providers[slot]);
        destination[(*offset)++] = '=';
        append_text(destination, offset, config->tokens[slot]);
        destination[(*offset)++] = '\n';
    }
}

ai_config_status_t ai_config_serialize(const ai_config_t *config, char *destination,
                                       size_t capacity, size_t *length) {
    size_t required;
    size_t offset = 0u;
    size_t slot;

    if (config == NULL || length == NULL || (destination == NULL && capacity != 0u)) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    for (slot = 0u; slot < AI_CONFIG_TOKEN_SLOTS; slot++) {
        if (!serializable_text(config->tokens[slot], AI_CONFIG_BEARER_TOKEN_CAPACITY, 1)) {
            *length = 0u;
            return AI_CONFIG_INVALID_VALUE;
        }
    }
    if (config->version != AI_CONFIG_VERSION ||
        !serializable_text(config->ssid, sizeof(config->ssid), 1) ||
        !serializable_text(config->password, sizeof(config->password), 1) ||
        !serializable_text(config->provider, sizeof(config->provider), 0) ||
        !known_provider(config->provider) || invalid_provider_field(config) != KEY_UNKNOWN ||
        !serializable_text(config->host, sizeof(config->host), 1) ||
        !serializable_text(config->model, sizeof(config->model), 1) ||
        !serializable_text(config->bearer_token, sizeof(config->bearer_token), 1) ||
        !serializable_text(config->system_prompt, sizeof(config->system_prompt), 1) ||
        config->port == 0u ||
        config->connect_timeout_ms < AI_CONFIG_CONNECT_TIMEOUT_MS_MIN ||
        config->connect_timeout_ms > AI_CONFIG_CONNECT_TIMEOUT_MS_MAX ||
        config->request_timeout_ms < AI_CONFIG_REQUEST_TIMEOUT_MS_MIN ||
        config->request_timeout_ms > AI_CONFIG_REQUEST_TIMEOUT_MS_MAX ||
        config->idle_timeout_ms < AI_CONFIG_IDLE_TIMEOUT_MS_MIN ||
        config->idle_timeout_ms > AI_CONFIG_IDLE_TIMEOUT_MS_MAX ||
        config->max_predict < AI_CONFIG_MAX_PREDICT_MIN ||
        config->max_predict > AI_CONFIG_MAX_PREDICT_MAX ||
        config->temperature > AI_CONFIG_TEMPERATURE_MAX ||
        config->utc_offset_minutes < AI_CONFIG_UTC_OFFSET_MINUTES_MIN ||
        config->utc_offset_minutes > AI_CONFIG_UTC_OFFSET_MINUTES_MAX) {
        *length = 0u;
        return AI_CONFIG_INVALID_VALUE;
    }

    required = sizeof("version=\n") - 1u + decimal_length(config->version) +
               sizeof("ssid=\n") - 1u + strlen(config->ssid) +
               sizeof("password=\n") - 1u + strlen(config->password) +
               sizeof("provider=\n") - 1u + strlen(config->provider) +
               (config->host[0] != '\0' ? sizeof("host=\n") - 1u + strlen(config->host) : 0u) +
               sizeof("port=\n") - 1u + decimal_length(config->port) +
               sizeof("model=\n") - 1u + strlen(config->model) +
               sizeof("bearer_token=\n") - 1u + strlen(config->bearer_token) +
               sizeof("connect_timeout_ms=\n") - 1u + decimal_length(config->connect_timeout_ms) +
               sizeof("request_timeout_ms=\n") - 1u + decimal_length(config->request_timeout_ms) +
               sizeof("idle_timeout_ms=\n") - 1u + decimal_length(config->idle_timeout_ms) +
               sizeof("max_predict=\n") - 1u + decimal_length(config->max_predict) +
               sizeof("system_prompt=\n") - 1u + strlen(config->system_prompt) +
               sizeof("temperature=\n") - 1u + decimal_length(config->temperature) +
               sizeof("utc_offset_minutes=\n") - 1u + (config->utc_offset_minutes < 0 ? 1u : 0u) +
               decimal_length(magnitude_of(config->utc_offset_minutes)) + token_lines_length(config);
    *length = required;
    if (destination == NULL || capacity <= required) {
        return AI_CONFIG_NO_SPACE;
    }

#define APPEND_FIELD(name_, value_) \
    do {                              \
        append_text(destination, &offset, name_ "="); \
        append_text(destination, &offset, value_);      \
        destination[offset++] = '\n';                   \
    } while (0)
#define APPEND_NUMBER_FIELD(name_, value_) \
    do {                                     \
        append_text(destination, &offset, name_ "=");  \
        append_number(destination, &offset, value_);    \
        destination[offset++] = '\n';                  \
    } while (0)

    APPEND_NUMBER_FIELD("version", config->version);
    APPEND_FIELD("ssid", config->ssid);
    APPEND_FIELD("password", config->password);
    APPEND_FIELD("provider", config->provider);
    /* An unset host is left out: the parser refuses an empty "host=". */
    if (config->host[0] != '\0') {
        APPEND_FIELD("host", config->host);
    }
    APPEND_NUMBER_FIELD("port", config->port);
    APPEND_FIELD("model", config->model);
    APPEND_FIELD("bearer_token", config->bearer_token);
    /* Each provider's remembered key, so switching back never needs it retyped.
     * The single line above stays for firmware that predates these slots. */
    append_token_lines(destination, &offset, config);
    APPEND_NUMBER_FIELD("connect_timeout_ms", config->connect_timeout_ms);
    APPEND_NUMBER_FIELD("request_timeout_ms", config->request_timeout_ms);
    APPEND_NUMBER_FIELD("idle_timeout_ms", config->idle_timeout_ms);
    APPEND_NUMBER_FIELD("max_predict", config->max_predict);
    APPEND_FIELD("system_prompt", config->system_prompt);
    APPEND_NUMBER_FIELD("temperature", config->temperature);
    append_text(destination, &offset,
                config->utc_offset_minutes < 0 ? "utc_offset_minutes=-" : "utc_offset_minutes=");
    append_number(destination, &offset, magnitude_of(config->utc_offset_minutes));
    destination[offset++] = '\n';

#undef APPEND_NUMBER_FIELD
#undef APPEND_FIELD
    destination[offset] = '\0';
    return AI_CONFIG_OK;
}
