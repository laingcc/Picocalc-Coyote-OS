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
    KEY_UNKNOWN
};

static const char *const key_names[] = {
    "version", "ssid", "password", "provider", "host", "port", "model",
    "bearer_token", "connect_timeout_ms", "request_timeout_ms", "idle_timeout_ms",
    "max_predict", "system_prompt", "temperature", "utc_offset_minutes"
};

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

/* The providers whose keys are remembered, in slot order: the two that answer
 * on the LAN first, then the clouds as the table lists them. */
static const char *const local_key_providers[] = {AI_CONFIG_PROVIDER_OLLAMA, AI_CONFIG_PROVIDER_MUSE};

int ai_config_key_slot(const char *provider) {
    size_t i;

    if (provider == NULL) {
        return -1;
    }
    for (i = 0u; i < sizeof(local_key_providers) / sizeof(local_key_providers[0]); i++) {
        if (strcmp(provider, local_key_providers[i]) == 0) {
            return (int)i;
        }
    }
    for (i = 0u; i < ai_config_cloud_count(); i++) {
        if (strcmp(provider, ai_config_cloud_at(i)->name) == 0) {
            return (int)(sizeof(local_key_providers) / sizeof(local_key_providers[0]) + i);
        }
    }
    return -1;
}

const char *ai_config_key_slot_name(size_t slot) {
    size_t local = sizeof(local_key_providers) / sizeof(local_key_providers[0]);

    if (slot < local) {
        return local_key_providers[slot];
    }
    if (slot - local < ai_config_cloud_count()) {
        return ai_config_cloud_at(slot - local)->name;
    }
    return NULL;
}

/* Remember the key in force under the provider it belongs to. */
static void remember_key(ai_config_t *config) {
    int slot = ai_config_key_slot(config->provider);

    if (slot >= 0) {
        memcpy(config->provider_keys[slot], config->bearer_token,
               sizeof(config->provider_keys[slot]));
    }
}

/* Put a remembered key back in force, if the provider has one. */
static void recall_key(ai_config_t *config) {
    int slot = ai_config_key_slot(config->provider);

    memset(config->bearer_token, 0, sizeof(config->bearer_token));
    if (slot >= 0) {
        memcpy(config->bearer_token, config->provider_keys[slot], sizeof(config->bearer_token));
    }
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

/* A remembered key is held to the rule the key in force is held to: whatever
 * the provider, it is only ever sent as a header value. */
static int invalid_remembered_key(const ai_config_t *config) {
    size_t slot;

    for (slot = 0u; slot < AI_CONFIG_KEY_SLOT_COUNT; slot++) {
        if (config->provider_keys[slot][0] != '\0' &&
            !ai_config_bearer_token_valid(config->provider_keys[slot])) {
            return KEY_BEARER_TOKEN;
        }
    }
    return KEY_UNKNOWN;
}

ai_config_status_t ai_config_validate(const ai_config_t *config) {
    if (config == NULL) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    if (invalid_provider_field(config) != KEY_UNKNOWN ||
        invalid_remembered_key(config) != KEY_UNKNOWN) {
        return AI_CONFIG_INVALID_VALUE;
    }
    return AI_CONFIG_OK;
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
    /* The key belongs to the provider it was typed for: it is put away under
     * that provider, and the provider being chosen recalls its own key, if it
     * has one.  No key is ever offered to a host it was not meant for. */
    remember_key(config);
    if (ai_config_cloud(config) != NULL) {
        /* Nothing of the provider being left goes to the next one. */
        memset(config->host, 0, sizeof(config->host));
        config->port = AI_CONFIG_OLLAMA_PORT;
        memset(config->model, 0, sizeof(config->model));
    }
    if (ai_config_cloud_find(provider) != NULL) {
        use_cloud_endpoint(config, ai_config_cloud_find(provider));
    } else if (strcmp(provider, AI_CONFIG_PROVIDER_MUSE) == 0) {
        /* The bridge is the user's own machine, so only the port and the one
         * model it serves are filled in; the host is theirs to type. */
        config->port = AI_CONFIG_MUSE_PORT;
        memset(config->model, 0, sizeof(config->model));
        memcpy(config->model, AI_CONFIG_MUSE_MODEL, strlen(AI_CONFIG_MUSE_MODEL) + 1u);
    }
    memset(config->provider, 0, sizeof(config->provider));
    memcpy(config->provider, provider, strlen(provider) + 1u);
    recall_key(config);
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

/* The same, naming a line that is not in the fixed table. */
static ai_config_status_t fail_name(ai_config_parser_t *parser, ai_config_status_t status,
                                    const char *name) {
    size_t length = strlen(name);

    parser->status = status;
    parser->error_key[0] = '\0';
    if (length < sizeof(parser->error_key)) {
        memcpy(parser->error_key, name, length + 1u);
    }
    return status;
}

/* The same as copy_value, for a line whose name is not in the fixed table. */
static ai_config_status_t copy_value_named(ai_config_parser_t *parser, char *destination,
                                           size_t capacity, const char *value, const char *name) {
    size_t length = strlen(value);
    if (length >= capacity) {
        return fail_name(parser, AI_CONFIG_VALUE_TOO_LONG, name);
    }
    memset(destination, 0, capacity);
    memcpy(destination, value, length + 1u);
    return AI_CONFIG_OK;
}

/* A "key_<provider>=" line remembers the key of a provider, which need not be
 * the one in force, so a key survives switching away and back.  A name that
 * does not name a provider is ignored, like any other unknown name. */
static ai_config_status_t parse_provider_key(ai_config_parser_t *parser, const char *name,
                                             const char *value) {
    ai_config_t *config = parser->config;
    unsigned char bit;
    int slot;

    if (strncmp(name, "key_", 4u) != 0) {
        return AI_CONFIG_OK;
    }
    slot = ai_config_key_slot(name + 4u);
    if (slot < 0) {
        return AI_CONFIG_OK;
    }
    bit = (unsigned char)(1u << (unsigned)slot);
    if ((parser->seen_slots & bit) != 0u) {
        return fail_name(parser, AI_CONFIG_DUPLICATE_KEY, name);
    }
    parser->seen_slots |= bit;
    return copy_value_named(parser, config->provider_keys[slot],
                            sizeof(config->provider_keys[slot]), value, name);
}

static ai_config_status_t copy_value(ai_config_parser_t *parser, int id, char *destination,
                                     size_t capacity, const char *value) {
    size_t length = strlen(value);
    if (length >= capacity) {
        return fail(parser, AI_CONFIG_VALUE_TOO_LONG, id);
    }
    /* A value shorter than the one it replaces leaves nothing of it behind:
     * the default provider, for one, is longer than some names that replace
     * it. */
    memset(destination, 0, capacity);
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

    if ((parser->seen_keys & (uint16_t)(1u << (unsigned)id)) != 0u) {
        return fail(parser, AI_CONFIG_DUPLICATE_KEY, id);
    }
    parser->seen_keys |= (uint16_t)(1u << (unsigned)id);

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
            return copy_value(parser, id, config->bearer_token,
                              sizeof(config->bearer_token), value);
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
    char *name;
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
    name = begin;

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
    if (id == KEY_UNKNOWN) {
        /* Either a per-provider key line or a name from another version of the
         * firmware; the latter is ignored, as it always has been. */
        return parse_provider_key(parser, name, begin);
    }
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
    return (parser->seen_keys & (uint16_t)(1u << (unsigned)id)) != 0u;
}

/* Fill in what a cloud provider's file left out, then apply the cross-field
 * rules. */
static ai_config_status_t finish_provider(ai_config_parser_t *parser) {
    ai_config_t *config = parser->config;
    const ai_config_cloud_t *cloud = ai_config_cloud(config);
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
    /* The key in force and the one remembered for its provider are the same
     * key, and a file may name either: make them agree. */
    if (config->bearer_token[0] == '\0') {
        recall_key(config);
    } else {
        remember_key(config);
    }
    id = invalid_provider_field(config);
    return id == KEY_UNKNOWN ? AI_CONFIG_OK : fail(parser, AI_CONFIG_INVALID_VALUE, id);
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

/* Bytes the "key_<provider>=" lines take: one for every remembered key that is
 * not the one in force, which is written as bearer_token instead. */
static size_t remembered_key_length(const ai_config_t *config) {
    size_t total = 0u;
    size_t slot;
    int current = ai_config_key_slot(config->provider);

    for (slot = 0u; slot < AI_CONFIG_KEY_SLOT_COUNT; slot++) {
        const char *name = ai_config_key_slot_name(slot);
        const char *key = config->provider_keys[slot];
        if (name == NULL || key[0] == '\0' || (int)slot == current) {
            continue;
        }
        /* "key_" name "=" key "\n" */
        total += 6u + strlen(name) + strlen(key);
    }
    return total;
}

ai_config_status_t ai_config_serialize(const ai_config_t *config, char *destination,
                                       size_t capacity, size_t *length) {
    size_t required;
    size_t offset = 0u;
    size_t slot;

    if (config == NULL || length == NULL || (destination == NULL && capacity != 0u)) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    if (config->version != AI_CONFIG_VERSION ||
        !serializable_text(config->ssid, sizeof(config->ssid), 1) ||
        !serializable_text(config->password, sizeof(config->password), 1) ||
        !serializable_text(config->provider, sizeof(config->provider), 0) ||
        !known_provider(config->provider) || invalid_provider_field(config) != KEY_UNKNOWN ||
        invalid_remembered_key(config) != KEY_UNKNOWN ||
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
               decimal_length(magnitude_of(config->utc_offset_minutes)) +
               remembered_key_length(config);
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

    /* The key in force is the bearer_token line above.  Every other remembered
     * key is written under the provider it belongs to, so it is still there
     * after a switch. */
    for (slot = 0u; slot < AI_CONFIG_KEY_SLOT_COUNT; slot++) {
        const char *name = ai_config_key_slot_name(slot);
        if (name == NULL || config->provider_keys[slot][0] == '\0' ||
            (int)slot == ai_config_key_slot(config->provider)) {
            continue;
        }
        append_text(destination, &offset, "key_");
        append_text(destination, &offset, name);
        destination[offset++] = '=';
        append_text(destination, &offset, config->provider_keys[slot]);
        destination[offset++] = '\n';
    }

#undef APPEND_NUMBER_FIELD
#undef APPEND_FIELD
    destination[offset] = '\0';
    return AI_CONFIG_OK;
}
