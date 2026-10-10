#include "storage/export.h"
#include "storage/file_store.h"

#include <stdio.h>
#include <string.h>

#define EXPORT_SUFFIX ".txt"

static int is_control(unsigned char c) {
    return c < 0x20u || c == 0x7fu;
}

static int equals_ignore_case(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) {
            return 0;
        }
    }
    return *a == *b;
}

export_status_t export_path(const char *title,
                            const char *dir,
                            const char *reserved,
                            char *path,
                            size_t capacity) {
    /* Stem, an optional '_' to dodge the reserved name, suffix and NUL. */
    char name[EXPORT_NAME_MAX + 1u + sizeof(EXPORT_SUFFIX)];
    size_t n = 0u;

    if (title == NULL || dir == NULL || path == NULL) {
        return EXPORT_INVALID_ARGUMENT;
    }

    for (const char *p = title; *p != '\0' && n < EXPORT_NAME_MAX; p++) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
            name[n++] = c;
        } else if (n > 0u && name[n - 1u] != '_') {
            name[n++] = '_';
        }
    }
    while (n > 0u && name[n - 1u] == '_') {
        n--;
    }
    if (n == 0u) {
        memcpy(name, "chat", 4u);
        n = 4u;
    }
    memcpy(name + n, EXPORT_SUFFIX, sizeof(EXPORT_SUFFIX));
    /* FAT ignores case, so "Convs" would overwrite a reserved "convs.txt". */
    if (reserved != NULL && equals_ignore_case(name, reserved)) {
        name[n++] = '_';
        memcpy(name + n, EXPORT_SUFFIX, sizeof(EXPORT_SUFFIX));
    }

    size_t dir_length = strlen(dir);
    size_t name_length = n + sizeof(EXPORT_SUFFIX) - 1u;
    if (dir_length == 0u || dir_length + 1u + name_length >= capacity) {
        return EXPORT_INVALID_ARGUMENT;
    }
    memcpy(path, dir, dir_length);
    path[dir_length] = '/';
    memcpy(path + dir_length + 1u, name, name_length + 1u);
    return EXPORT_OK;
}

static int export_bytes(FILE *f, const char *data, size_t len, size_t *total) {
    if (len != 0u && fwrite(data, 1u, len, f) != len) {
        return -1;
    }
    *total += len;
    return 0;
}

static int export_str(FILE *f, const char *str, size_t *total) {
    return export_bytes(f, str, strlen(str), total);
}

/* Writes text in runs, skipping control characters other than newline and tab. */
static int export_text(FILE *f, const char *text, size_t length, size_t *total) {
    size_t start = 0u;

    for (size_t i = 0u; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (is_control(c) && c != '\n' && c != '\t') {
            if (export_bytes(f, text + start, i - start, total) != 0) {
                return -1;
            }
            start = i + 1u;
        }
    }
    return export_bytes(f, text + start, length - start, total);
}

static int export_messages(FILE *f, const chat_model_t *model, const char *title, size_t *total) {
    if (export_str(f, "# ", total) != 0 || export_str(f, title, total) != 0 ||
        export_str(f, "\n\n", total) != 0) {
        return -1;
    }

    size_t count = chat_model_message_count(model);
    for (size_t i = 0; i < count; i++) {
        const chat_message_t *m = chat_model_message_at(model, i);
        const char *label = "assistant: ";
        if (m == NULL) {
            return -1;
        }
        if (m->role == CHAT_ROLE_USER) {
            label = "user: ";
        } else if (m->partial) {
            label = "assistant (incomplete): ";
        }
        if (export_str(f, label, total) != 0 ||
            export_text(f, m->text, m->length, total) != 0 ||
            export_str(f, "\n", total) != 0) {
            return -1;
        }
    }
    return 0;
}

export_status_t export_conversation(const chat_model_t *model,
                                    const char *title,
                                    const char *path,
                                    size_t *written) {
    size_t total = 0u;

    if (written != NULL) {
        *written = 0u;
    }
    if (model == NULL || title == NULL || path == NULL) {
        return EXPORT_INVALID_ARGUMENT;
    }
    size_t path_length = strlen(path);
    if (path_length == 0u || path_length + sizeof(FILE_STORE_TEMP_SUFFIX) > FILE_STORE_PATH_CAPACITY) {
        return EXPORT_INVALID_ARGUMENT;
    }

    size_t count = chat_model_message_count(model);
    if (count == 0u) {
        return EXPORT_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < count; i++) {
        const chat_message_t *m = chat_model_message_at(model, i);
        if (m == NULL || m->length > CHAT_MESSAGE_MAX) {
            return EXPORT_TOO_LARGE;
        }
    }

    /* Same atomic temp -> rename swap as the stores, aimed at the export file. */
    FILE *out = file_store_open_temp(path);
    if (out == NULL) {
        return EXPORT_IO_ERROR;
    }

    int failed = export_messages(out, model, title, &total) != 0;
    if (file_store_commit(path, out, failed) != 0) {
        return EXPORT_IO_ERROR;
    }
    if (written != NULL) {
        *written = total;
    }
    return EXPORT_OK;
}
