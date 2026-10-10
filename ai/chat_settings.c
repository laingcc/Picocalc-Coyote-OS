#include "ai/chat_settings.h"

#include <stdio.h>
#include <string.h>

#include "ai/ollama_provider.h"

typedef struct {
    const char *label;
    const char *key; /* ai_config key the value is validated as */
} setting_info_t;

static const setting_info_t settings[CHAT_SETTING_COUNT] = {
    {"Provider", "provider"},
    {"Model", "model"},
    {"Host", "host"},
    {"Port", "port"},
    {"SSID", "ssid"},
    {"Password", "password"},
    {"Connect ms", "connect_timeout_ms"},
    {"Request ms", "request_timeout_ms"},
    {"Idle ms", "idle_timeout_ms"},
    {"Max predict", "max_predict"},
    {"System prompt", "system_prompt"},
    {"Temperature", "temperature"},
    {"UTC offset min", "utc_offset_minutes"},
};

/* Scratch for validation; static because the parser is too big for a stack
 * that also carries the UI.  Wiped after every use: it can hold a password. */
static ai_config_parser_t scratch_parser;
static ai_config_t scratch_config;

static int known(chat_setting_t field) {
    return (int)field >= 0 && field < CHAT_SETTING_COUNT;
}

const char *chat_settings_label(chat_setting_t field) {
    return known(field) ? settings[field].label : "";
}

int chat_settings_is_secret(chat_setting_t field) {
    return field == CHAT_SETTING_PASSWORD;
}

int chat_settings_allows_empty(chat_setting_t field) {
    return field == CHAT_SETTING_MODEL || field == CHAT_SETTING_SSID ||
           field == CHAT_SETTING_PASSWORD || field == CHAT_SETTING_SYSTEM_PROMPT;
}

static ai_config_status_t precheck(chat_setting_t field, const char *value) {
    size_t length = strlen(value);
    size_t i;

    for (i = 0u; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20u || c == 0x7fu) {
            return AI_CONFIG_INVALID_VALUE;
        }
    }
    if (length > 0u && (value[0] == ' ' || value[length - 1u] == ' ')) {
        return AI_CONFIG_INVALID_VALUE;
    }
    if (field == CHAT_SETTING_PROVIDER && strcmp(value, "ollama") != 0) {
        return AI_CONFIG_INVALID_VALUE;
    }
    if (field == CHAT_SETTING_MODEL && length > OLLAMA_MODEL_MAX) {
        return AI_CONFIG_VALUE_TOO_LONG;
    }
    if (field == CHAT_SETTING_SYSTEM_PROMPT && length > AI_CONFIG_SYSTEM_PROMPT_CAPACITY - 1u) {
        return AI_CONFIG_VALUE_TOO_LONG;
    }
    return AI_CONFIG_OK;
}

static void store(ai_config_t *config, chat_setting_t field, const ai_config_t *from) {
    switch (field) {
        case CHAT_SETTING_PROVIDER:
            memcpy(config->provider, from->provider, sizeof(config->provider));
            break;
        case CHAT_SETTING_MODEL:
            memcpy(config->model, from->model, sizeof(config->model));
            break;
        case CHAT_SETTING_HOST:
            memcpy(config->host, from->host, sizeof(config->host));
            break;
        case CHAT_SETTING_PORT:
            config->port = from->port;
            break;
        case CHAT_SETTING_SSID:
            memcpy(config->ssid, from->ssid, sizeof(config->ssid));
            break;
        case CHAT_SETTING_PASSWORD:
            memcpy(config->password, from->password, sizeof(config->password));
            break;
        case CHAT_SETTING_CONNECT_TIMEOUT:
            config->connect_timeout_ms = from->connect_timeout_ms;
            break;
        case CHAT_SETTING_REQUEST_TIMEOUT:
            config->request_timeout_ms = from->request_timeout_ms;
            break;
        case CHAT_SETTING_IDLE_TIMEOUT:
            config->idle_timeout_ms = from->idle_timeout_ms;
            break;
        case CHAT_SETTING_MAX_PREDICT:
            config->max_predict = from->max_predict;
            break;
        case CHAT_SETTING_SYSTEM_PROMPT:
            memcpy(config->system_prompt, from->system_prompt, sizeof(config->system_prompt));
            break;
        case CHAT_SETTING_TEMPERATURE:
            config->temperature = from->temperature;
            break;
        case CHAT_SETTING_UTC_OFFSET:
            config->utc_offset_minutes = from->utc_offset_minutes;
            break;
        default:
            break;
    }
}

/* Parse a decimal temperature like "0.8", "1.50" or "2" into hundredths. */
static bool parse_temperature_hundredths(const char *value, uint16_t *out) {
    uint32_t whole = 0u;
    uint32_t frac = 0u;
    unsigned frac_digits = 0u;
    const char *p = value;

    if (*p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10u + (uint32_t)(*p - '0');
        if (whole > 200u) {
            return false;
        }
        p++;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (frac_digits >= 2u) {
                return false;
            }
            frac = frac * 10u + (uint32_t)(*p - '0');
            frac_digits++;
            p++;
        }
    }
    if (*p != '\0') {
        return false;
    }
    while (frac_digits < 2u) {
        frac *= 10u;
        frac_digits++;
    }
    uint32_t hundredths = whole * 100u + frac;
    if (hundredths > 200u) {
        return false;
    }
    *out = (uint16_t)hundredths;
    return true;
}

ai_config_status_t chat_settings_set(ai_config_t *config, chat_setting_t field, const char *value) {
    ai_config_status_t status;

    if (config == NULL || value == NULL || !known(field)) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    status = precheck(field, value);
    if (status != AI_CONFIG_OK) {
        return status;
    }

    if (field == CHAT_SETTING_TEMPERATURE) {
        uint16_t hundredths;
        if (!parse_temperature_hundredths(value, &hundredths)) {
            return AI_CONFIG_INVALID_VALUE;
        }
        config->temperature = hundredths;
        return AI_CONFIG_OK;
    }

    status = ai_config_parser_init(&scratch_parser, &scratch_config);
    if (status == AI_CONFIG_OK) {
        status = ai_config_parser_feed(&scratch_parser, settings[field].key, strlen(settings[field].key));
    }
    if (status == AI_CONFIG_OK) {
        status = ai_config_parser_feed(&scratch_parser, "=", 1u);
    }
    if (status == AI_CONFIG_OK) {
        status = ai_config_parser_feed(&scratch_parser, value, strlen(value));
    }
    if (status == AI_CONFIG_OK) {
        status = ai_config_parser_finish(&scratch_parser);
    }
    if (status == AI_CONFIG_OK) {
        store(config, field, &scratch_config);
    } else if (field == CHAT_SETTING_TEMPERATURE && status == AI_CONFIG_OUT_OF_RANGE) {
        status = AI_CONFIG_INVALID_VALUE;
    }
    memset(&scratch_parser, 0, sizeof(scratch_parser));
    memset(&scratch_config, 0, sizeof(scratch_config));
    return status;
}

size_t chat_settings_format(const ai_config_t *config, chat_setting_t field, char *out, size_t capacity) {
    int written = 0;

    if (out == NULL || capacity == 0u) {
        return 0u;
    }
    out[0] = '\0';
    if (config == NULL) {
        return 0u;
    }
    switch (field) {
        case CHAT_SETTING_PROVIDER:
            written = snprintf(out, capacity, "%s", config->provider);
            break;
        case CHAT_SETTING_MODEL:
            written = snprintf(out, capacity, "%s", config->model);
            break;
        case CHAT_SETTING_HOST:
            written = snprintf(out, capacity, "%s", config->host);
            break;
        case CHAT_SETTING_PORT:
            written = snprintf(out, capacity, "%u", (unsigned)config->port);
            break;
        case CHAT_SETTING_SSID:
            written = snprintf(out, capacity, "%s", config->ssid);
            break;
        case CHAT_SETTING_PASSWORD:
            written = snprintf(out, capacity, "%s", config->password[0] != '\0' ? "********" : "");
            break;
        case CHAT_SETTING_CONNECT_TIMEOUT:
            written = snprintf(out, capacity, "%lu", (unsigned long)config->connect_timeout_ms);
            break;
        case CHAT_SETTING_REQUEST_TIMEOUT:
            written = snprintf(out, capacity, "%lu", (unsigned long)config->request_timeout_ms);
            break;
        case CHAT_SETTING_IDLE_TIMEOUT:
            written = snprintf(out, capacity, "%lu", (unsigned long)config->idle_timeout_ms);
            break;
        case CHAT_SETTING_MAX_PREDICT:
            written = snprintf(out, capacity, "%lu", (unsigned long)config->max_predict);
            break;
        case CHAT_SETTING_SYSTEM_PROMPT:
            written = snprintf(out, capacity, "%s", config->system_prompt);
            break;
        case CHAT_SETTING_TEMPERATURE:
            written = snprintf(out, capacity, "%u.%02u",
                               (unsigned)(config->temperature / 100u),
                               (unsigned)(config->temperature % 100u));
            break;
        case CHAT_SETTING_UTC_OFFSET:
            written = snprintf(out, capacity, "%d", (int)config->utc_offset_minutes);
            break;
        default:
            break;
    }
    if (written < 0) {
        out[0] = '\0';
        return 0u;
    }
    return (size_t)written < capacity ? (size_t)written : capacity - 1u;
}
