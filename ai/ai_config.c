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
    KEY_UNKNOWN
};

static const char *const key_names[] = {
    "version", "ssid", "password", "provider", "host", "port", "model",
    "bearer_token", "connect_timeout_ms", "request_timeout_ms", "idle_timeout_ms",
    "max_predict"
};

ai_config_status_t ai_config_init(ai_config_t *config) {
    if (config == NULL) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }

    memset(config, 0, sizeof(*config));
    config->version = AI_CONFIG_VERSION;
    memcpy(config->provider, "ollama", sizeof("ollama"));
    memcpy(config->host, "wang.local", sizeof("wang.local"));
    config->port = 11434u;
    config->connect_timeout_ms = 15000u;
    config->request_timeout_ms = 120000u;
    config->idle_timeout_ms = 15000u;
    config->max_predict = 384u;
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

static ai_config_status_t parse_known(ai_config_parser_t *parser, int id, const char *value) {
    ai_config_t *config = parser->config;
    uint32_t number;
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
            if (strcmp(value, "ollama") != 0 && strcmp(value, "muse") != 0) {
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
        if (bytes[i] == '\n') {
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

ai_config_status_t ai_config_parser_finish(ai_config_parser_t *parser) {
    ai_config_status_t status;
    if (parser == NULL || parser->config == NULL || parser->finished != 0u) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    parser->finished = 1u;
    if (parser->status != AI_CONFIG_OK) {
        return parser->status;
    }
    if (parser->line_length == 0u) {
        return AI_CONFIG_OK;
    }
    status = parse_line(parser);
    parser->line_length = 0u;
    return status;
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
        if (text[i] == '\r' || text[i] == '\n') {
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

ai_config_status_t ai_config_serialize(const ai_config_t *config, char *destination,
                                       size_t capacity, size_t *length) {
    size_t required;
    size_t offset = 0u;

    if (config == NULL || length == NULL || (destination == NULL && capacity != 0u)) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    if (config->version != AI_CONFIG_VERSION ||
        !serializable_text(config->ssid, sizeof(config->ssid), 1) ||
        !serializable_text(config->password, sizeof(config->password), 1) ||
        !serializable_text(config->provider, sizeof(config->provider), 0) ||
        (strcmp(config->provider, "ollama") != 0 && strcmp(config->provider, "muse") != 0) ||
        !serializable_text(config->host, sizeof(config->host), 0) ||
        !serializable_text(config->model, sizeof(config->model), 1) ||
        !serializable_text(config->bearer_token, sizeof(config->bearer_token), 1) ||
        config->port == 0u ||
        config->connect_timeout_ms < AI_CONFIG_CONNECT_TIMEOUT_MS_MIN ||
        config->connect_timeout_ms > AI_CONFIG_CONNECT_TIMEOUT_MS_MAX ||
        config->request_timeout_ms < AI_CONFIG_REQUEST_TIMEOUT_MS_MIN ||
        config->request_timeout_ms > AI_CONFIG_REQUEST_TIMEOUT_MS_MAX ||
        config->idle_timeout_ms < AI_CONFIG_IDLE_TIMEOUT_MS_MIN ||
        config->idle_timeout_ms > AI_CONFIG_IDLE_TIMEOUT_MS_MAX ||
        config->max_predict < AI_CONFIG_MAX_PREDICT_MIN ||
        config->max_predict > AI_CONFIG_MAX_PREDICT_MAX) {
        *length = 0u;
        return AI_CONFIG_INVALID_VALUE;
    }

    required = sizeof("version=\n") - 1u + decimal_length(config->version) +
               sizeof("ssid=\n") - 1u + strlen(config->ssid) +
               sizeof("password=\n") - 1u + strlen(config->password) +
               sizeof("provider=\n") - 1u + strlen(config->provider) +
               sizeof("host=\n") - 1u + strlen(config->host) +
               sizeof("port=\n") - 1u + decimal_length(config->port) +
               sizeof("model=\n") - 1u + strlen(config->model) +
               sizeof("bearer_token=\n") - 1u + strlen(config->bearer_token) +
               sizeof("connect_timeout_ms=\n") - 1u + decimal_length(config->connect_timeout_ms) +
               sizeof("request_timeout_ms=\n") - 1u + decimal_length(config->request_timeout_ms) +
               sizeof("idle_timeout_ms=\n") - 1u + decimal_length(config->idle_timeout_ms) +
               sizeof("max_predict=\n") - 1u + decimal_length(config->max_predict);
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
    APPEND_FIELD("host", config->host);
    APPEND_NUMBER_FIELD("port", config->port);
    APPEND_FIELD("model", config->model);
    APPEND_FIELD("bearer_token", config->bearer_token);
    APPEND_NUMBER_FIELD("connect_timeout_ms", config->connect_timeout_ms);
    APPEND_NUMBER_FIELD("request_timeout_ms", config->request_timeout_ms);
    APPEND_NUMBER_FIELD("idle_timeout_ms", config->idle_timeout_ms);
    APPEND_NUMBER_FIELD("max_predict", config->max_predict);

#undef APPEND_NUMBER_FIELD
#undef APPEND_FIELD
    destination[offset] = '\0';
    return AI_CONFIG_OK;
}
