#ifndef COYOTE_AI_CHAT_SETTINGS_H
#define COYOTE_AI_CHAT_SETTINGS_H

#include <stddef.h>

#include "ai/ai_config.h"

/*
 * Field-at-a-time editing of an ai_config_t for the chat settings menu.
 *
 * Every value is validated by the ai_config parser itself (the field is
 * parsed as a "key=value" line into a scratch configuration), so the settings
 * menu accepts exactly what a configuration file would: the same lengths and
 * the same numeric bounds.  A rejected value leaves the configuration
 * untouched.
 *
 * The Wi-Fi password can be set but never read back: chat_settings_format
 * renders it as a fixed run of asterisks that does not reveal its length.
 */

typedef enum {
    CHAT_SETTING_PROVIDER = 0,
    CHAT_SETTING_MODEL,
    CHAT_SETTING_HOST,
    CHAT_SETTING_PORT,
    CHAT_SETTING_BEARER_TOKEN,
    CHAT_SETTING_SSID,
    CHAT_SETTING_PASSWORD,
    CHAT_SETTING_CONNECT_TIMEOUT,
    CHAT_SETTING_REQUEST_TIMEOUT,
    CHAT_SETTING_IDLE_TIMEOUT,
    CHAT_SETTING_MAX_PREDICT,
    CHAT_SETTING_COUNT
} chat_setting_t;

/* Short display name, e.g. "Host"; "" for an unknown field. */
const char *chat_settings_label(chat_setting_t field);

/* True for fields whose stored value is never displayed (it is shown only
 * while being typed). */
int chat_settings_is_secret(chat_setting_t field);

/* True for fields that may be set to the empty string. */
int chat_settings_allows_empty(chat_setting_t field);

/*
 * Validate value and, only if it is acceptable, store it in config.  Beyond
 * the ai_config rules this rejects control characters, leading or trailing
 * blanks (the parser would silently trim them), any provider but "ollama",
 * and a model name longer than the Ollama provider can send.
 */
ai_config_status_t chat_settings_set(ai_config_t *config, chat_setting_t field, const char *value);

/*
 * Write the field's display value to out (always NUL-terminated when
 * capacity is non-zero, truncated to fit) and return its length.  The
 * password is rendered as "********" when set and "" when empty.
 */
size_t chat_settings_format(const ai_config_t *config, chat_setting_t field, char *out, size_t capacity);

#endif
