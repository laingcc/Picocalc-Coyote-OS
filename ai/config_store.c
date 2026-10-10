#include "ai/config_store.h"

#include <stdio.h>
#include <string.h>

/* Upper bound of ai_config_serialize output: every text field at capacity
 * plus, per line, the key, '=', a 32-bit decimal and the newline. */
#define CONFIG_STORE_KEY_COUNT 15u
#define CONFIG_STORE_TEXT_CAPACITY \
    (AI_CONFIG_SSID_CAPACITY + AI_CONFIG_PASSWORD_CAPACITY + AI_CONFIG_PROVIDER_CAPACITY + \
     AI_CONFIG_HOST_CAPACITY + AI_CONFIG_MODEL_CAPACITY + AI_CONFIG_BEARER_TOKEN_CAPACITY + \
     AI_CONFIG_SYSTEM_PROMPT_CAPACITY + \
     CONFIG_STORE_KEY_COUNT * (AI_CONFIG_KEY_CAPACITY + 1u + 10u + 1u) + 1u)

/* Scratch state: a file chunk while loading, the whole file while saving. */
static ai_config_parser_t parser;
static char text[CONFIG_STORE_TEXT_CAPACITY];
static char file_path[CONFIG_STORE_PATH_CAPACITY];
static char temp_path[CONFIG_STORE_PATH_CAPACITY];
static char backup_path[CONFIG_STORE_PATH_CAPACITY];

static int build_path(char *path, const char *dir, const char *name) {
    size_t dir_length = strlen(dir);
    size_t name_length = strlen(name);

    if (dir_length == 0u || dir_length + 1u + name_length >= CONFIG_STORE_PATH_CAPACITY) {
        return -1;
    }
    memcpy(path, dir, dir_length);
    path[dir_length] = '/';
    memcpy(path + dir_length + 1u, name, name_length + 1u);
    return 0;
}

static int build_paths(const char *dir) {
    if (dir == NULL || build_path(file_path, dir, CONFIG_STORE_FILE_NAME) != 0 ||
        build_path(temp_path, dir, CONFIG_STORE_TEMP_NAME) != 0 ||
        build_path(backup_path, dir, CONFIG_STORE_BACKUP_NAME) != 0) {
        return -1;
    }
    return 0;
}

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
    if (build_paths(dir) != 0) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }

    file = fopen(file_path, "rb");
    if (file == NULL) {
        /* An interrupted save can leave only the backup behind. */
        file = fopen(backup_path, "rb");
    }
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

static int write_temp(size_t length) {
    FILE *file = fopen(temp_path, "wb");
    int failed;

    if (file == NULL) {
        return -1;
    }
    failed = fwrite(text, 1u, length, file) != length;
    failed |= fflush(file) != 0;
    failed |= fclose(file) != 0;
    return failed ? -1 : 0;
}

static int replace_file(void) {
    if (rename(temp_path, file_path) == 0) {
        return 0;
    }
    /* FAT refuses to rename over an existing file: step the old one aside. */
    remove(backup_path);
    if (rename(file_path, backup_path) != 0) {
        return -1;
    }
    if (rename(temp_path, file_path) != 0) {
        rename(backup_path, file_path);
        return -1;
    }
    remove(backup_path);
    return 0;
}

ai_config_status_t config_store_save(const ai_config_t *config, const char *dir) {
    ai_config_status_t status;
    size_t length = 0u;

    if (config == NULL || build_paths(dir) != 0) {
        return AI_CONFIG_INVALID_ARGUMENT;
    }
    status = ai_config_serialize(config, text, sizeof(text), &length);
    if (status == AI_CONFIG_OK && (write_temp(length) != 0 || replace_file() != 0)) {
        remove(temp_path);
        status = AI_CONFIG_IO_ERROR;
    }
    wipe_scratch();
    return status;
}
