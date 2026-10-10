#include "ai/config_store.h"

#include "storage/file_store.h"

#include <stdio.h>
#include <string.h>

/* Upper bound of ai_config_serialize output: every text field at capacity
 * plus, per line, the key, '=', a 32-bit decimal and the newline. */
#define CONFIG_STORE_KEY_COUNT 12u
#define CONFIG_STORE_TEXT_CAPACITY \
    (AI_CONFIG_SSID_CAPACITY + AI_CONFIG_PASSWORD_CAPACITY + AI_CONFIG_PROVIDER_CAPACITY + \
     AI_CONFIG_HOST_CAPACITY + AI_CONFIG_MODEL_CAPACITY + AI_CONFIG_BEARER_TOKEN_CAPACITY + \
     CONFIG_STORE_KEY_COUNT * (AI_CONFIG_KEY_CAPACITY + 1u + 10u + 1u) + 1u)

/* Scratch state: a file chunk while loading, the whole file while saving. */
static ai_config_parser_t parser;
static char text[CONFIG_STORE_TEXT_CAPACITY];
static char file_path[CONFIG_STORE_PATH_CAPACITY];

/* The scratch buffers have held the password and the bearer token. */
static void wipe_scratch(void) {
    memset(&parser, 0, sizeof(parser));
    memset(text, 0, sizeof(text));
}

ai_config_status_t config_store_load(ai_config_t *config, const char *dir) {
    ai_config_status_t status;
    FILE *file;

    status = ai_config_init(config);
    if (status != AI_CONFIG_OK) {
        return status;
    }
    if (file_store_path(file_path, dir, CONFIG_STORE_FILE_NAME) != 0) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }

    file = file_store_open(file_path);
    if (file == NULL) {
        return AI_CONFIG_OK;
    }

    status = ai_config_parser_init(&parser, config);
    while (status == AI_CONFIG_OK) {
        size_t count = fread(text, 1u, AI_CONFIG_LINE_MAX, file);
        if (count == 0u) {
            break;
        }
        status = ai_config_parser_feed(&parser, text, count);
    }
    if (status == AI_CONFIG_OK && ferror(file) != 0) {
        status = AI_CONFIG_IO_ERROR;
    }
    if (status == AI_CONFIG_OK) {
        status = ai_config_parser_finish(&parser);
    }
    fclose(file);
    wipe_scratch();

    if (status != AI_CONFIG_OK) {
        ai_config_init(config);
    }
    return status;
}

ai_config_status_t config_store_save(const ai_config_t *config, const char *dir) {
    ai_config_status_t status;
    size_t length = 0u;

    if (config == NULL || file_store_path(file_path, dir, CONFIG_STORE_FILE_NAME) != 0) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    status = ai_config_serialize(config, text, sizeof(text), &length);
    if (status == AI_CONFIG_OK && file_store_write_atomic(file_path, text, length) != 0) {
        status = AI_CONFIG_IO_ERROR;
    }
    wipe_scratch();
    return status;
}
