#include "ai/conv_store.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define LINE_CAPACITY (CHAT_MESSAGE_MAX + 32u)

typedef struct {
    FILE *file;
    char buf[512];
    size_t pos;
    size_t len;
    int eof;
} file_reader_t;

static char file_path[CONV_STORE_PATH_CAPACITY];
static char temp_path[CONV_STORE_PATH_CAPACITY];
static char backup_path[CONV_STORE_PATH_CAPACITY];

static char line_buf[LINE_CAPACITY];
static char msg_buf[CHAT_MESSAGE_MAX + 1u];
static char clean_title_buf[CONV_STORE_TITLE_MAX + 1u];

static int build_path(char *path, const char *dir, const char *name) {
    size_t dir_length = strlen(dir);
    size_t name_length = strlen(name);

    if (dir_length == 0u || dir_length + 1u + name_length >= CONV_STORE_PATH_CAPACITY) {
        return -1;
    }
    memcpy(path, dir, dir_length);
    path[dir_length] = '/';
    memcpy(path + dir_length + 1u, name, name_length + 1u);
    return 0;
}

static int build_paths(const char *dir) {
    if (dir == NULL || build_path(file_path, dir, CONV_STORE_FILE_NAME) != 0 ||
        build_path(temp_path, dir, CONV_STORE_TEMP_NAME) != 0 ||
        build_path(backup_path, dir, CONV_STORE_BACKUP_NAME) != 0) {
        return -1;
    }
    return 0;
}

static int replace_file(void) {
    if (rename(temp_path, file_path) == 0) {
        /* A stale backup from an earlier interrupted swap is no longer needed. */
        remove(backup_path);
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

static FILE *open_existing(void) {
    FILE *f = fopen(file_path, "rb");
    if (f == NULL) {
        f = fopen(backup_path, "rb");
    }
    return f;
}

static void reader_init(file_reader_t *r, FILE *f) {
    r->file = f;
    r->pos = 0u;
    r->len = 0u;
    r->eof = 0;
}

static int reader_getc(file_reader_t *r) {
    if (r->pos >= r->len) {
        if (r->eof) {
            return -1;
        }
        r->len = fread(r->buf, 1u, sizeof(r->buf), r->file);
        r->pos = 0u;
        if (r->len == 0u) {
            r->eof = 1;
            return -1;
        }
    }
    return (unsigned char)r->buf[r->pos++];
}

static int reader_peek(file_reader_t *r) {
    if (r->pos >= r->len) {
        if (r->eof) {
            return -1;
        }
        r->len = fread(r->buf, 1u, sizeof(r->buf), r->file);
        r->pos = 0u;
        if (r->len == 0u) {
            r->eof = 1;
            return -1;
        }
    }
    return (unsigned char)r->buf[r->pos];
}

/*
 * Reads a line up to '\n'.
 * Strips trailing '\r' and '\n'.
 * Returns:
 *   0: line read
 *   1: EOF reached with 0 characters read
 *  -1: line too long
 *  -2: embedded NUL
 */
static int read_line(file_reader_t *r, char *line, size_t capacity, size_t *out_len) {
    size_t n = 0u;
    int ch;

    while ((ch = reader_getc(r)) != -1) {
        if (ch == '\0') {
            return -2;
        }
        if (ch == '\r') {
            if (reader_peek(r) == '\n') {
                reader_getc(r);
            }
            line[n] = '\0';
            if (out_len != NULL) {
                *out_len = n;
            }
            return 0;
        }
        if (ch == '\n') {
            line[n] = '\0';
            if (out_len != NULL) {
                *out_len = n;
            }
            return 0;
        }
        if (n + 1u >= capacity) {
            return -1;
        }
        line[n++] = (char)ch;
    }
    if (n > 0u) {
        line[n] = '\0';
        if (out_len != NULL) {
            *out_len = n;
        }
        return 0;
    }
    return 1;
}

static int write_bytes(FILE *f, const void *data, size_t len) {
    if (len == 0u) {
        return 0;
    }
    return fwrite(data, 1u, len, f) == len ? 0 : -1;
}

static int write_str(FILE *f, const char *str) {
    return write_bytes(f, str, strlen(str));
}

static int write_line(FILE *f, const char *line) {
    if (write_str(f, line) != 0) {
        return -1;
    }
    return write_str(f, "\n");
}

static int write_message_text(FILE *f, const char *text, size_t length) {
    /* Emit the message body after "text=" line by line.  The first line needs no
     * escape (the "text=" prefix already disambiguates it); later lines that start
     * with '[' or '\' are prefixed with '\' so the reader can tell literal content
     * from the [message]/[conversation] markers. */
    const char *p = text;
    const char *end = text + length;
    int first = 1;
    while (p <= end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') {
            nl++;
        }
        size_t line_len = (size_t)(nl - p);
        if (!first && line_len > 0u && (p[0] == '[' || p[0] == '\\')) {
            if (write_str(f, "\\") != 0) {
                return -1;
            }
        }
        first = 0;
        if (write_bytes(f, p, line_len) != 0) {
            return -1;
        }
        if (write_str(f, "\n") != 0) {
            return -1;
        }
        if (nl == end) {
            break;
        }
        p = nl + 1;
    }
    return 0;
}

static int write_conversation(FILE *f, const chat_model_t *model, const char *title) {
    if (write_str(f, "[conversation]\ntitle=") != 0) {
        return -1;
    }
    if (write_line(f, title) != 0) {
        return -1;
    }

    size_t count = chat_model_message_count(model);
    for (size_t i = 0; i < count; i++) {
        const chat_message_t *m = chat_model_message_at(model, i);
        if (m == NULL) {
            return -1;
        }
        if (write_str(f, "[message]\n") != 0) {
            return -1;
        }
        if (m->role == CHAT_ROLE_USER) {
            if (write_str(f, "role=user\n") != 0) {
                return -1;
            }
        } else {
            if (write_str(f, "role=assistant\n") != 0) {
                return -1;
            }
        }
        if (m->partial) {
            if (write_str(f, "partial=1\n") != 0) {
                return -1;
            }
        }
        if (write_str(f, "text=") != 0) {
            return -1;
        }
        if (write_message_text(f, m->text, m->length) != 0) {
            return -1;
        }
    }
    return 0;
}

static int sanitize_title(char *dst, const char *src) {
    if (src == NULL) {
        return -1;
    }
    size_t len = strlen(src);
    if (len == 0u) {
        return -1;
    }
    /* Reject control characters: they would break the convs.txt line structure. */
    for (size_t i = 0u; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20u || c == 0x7fu) {
            return -1;
        }
    }
    size_t copy_len = len > CONV_STORE_TITLE_MAX ? CONV_STORE_TITLE_MAX : len;
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
    return 0;
}

/*
 * Parses all [message] sections in the current conversation from reader.
 * If model != NULL, appends each message into model.
 * Stops when line is [conversation] or EOF.
 * Upon return, line_buf contains the next line ([conversation] or EOF).
 */
static conv_store_status_t parse_messages(file_reader_t *r, chat_model_t *model, int *out_res) {
    size_t len = 0u;
    size_t msg_count = 0u;
    size_t total_conv_bytes = 0u;
    int res = 0;

    while (strcmp(line_buf, "[message]") == 0) {
        int has_role = 0;
        chat_role_t role = CHAT_ROLE_USER;
        bool partial = false;
        int in_text = 0;
        size_t msg_len = 0u;

        while ((res = read_line(r, line_buf, LINE_CAPACITY, &len)) == 0) {
            if (!in_text) {
                if (strcmp(line_buf, "role=user") == 0) {
                    if (has_role) return CONV_STORE_CORRUPT;
                    role = CHAT_ROLE_USER;
                    has_role = 1;
                } else if (strcmp(line_buf, "role=assistant") == 0) {
                    if (has_role) return CONV_STORE_CORRUPT;
                    role = CHAT_ROLE_ASSISTANT;
                    has_role = 1;
                } else if (strcmp(line_buf, "partial=1") == 0) {
                    partial = true;
                } else if (strncmp(line_buf, "text=", 5) == 0) {
                    if (!has_role) return CONV_STORE_CORRUPT;
                    in_text = 1;
                    size_t first_len = len - 5u;
                    if (first_len > CHAT_MESSAGE_MAX) {
                        return CONV_STORE_TOO_LARGE;
                    }
                    memcpy(msg_buf, line_buf + 5, first_len);
                    msg_len = first_len;
                    msg_buf[msg_len] = '\0';
                } else {
                    return CONV_STORE_CORRUPT;
                }
            } else {
                const char *content = line_buf;
                size_t content_len = len;
                if (content_len > 0u && content[0] == '\\') {
                    /* Escaped literal line: drop the '\' and keep the rest. */
                    content++;
                    content_len--;
                } else if (strcmp(line_buf, "[message]") == 0 || strcmp(line_buf, "[conversation]") == 0) {
                    break;
                }
                if (msg_len + 1u + content_len > CHAT_MESSAGE_MAX) {
                    return CONV_STORE_TOO_LARGE;
                }
                msg_buf[msg_len++] = '\n';
                if (content_len > 0u) {
                    memcpy(msg_buf + msg_len, content, content_len);
                    msg_len += content_len;
                }
                msg_buf[msg_len] = '\0';
            }
        }
        if (res < 0) {
            return res == -1 ? CONV_STORE_TOO_LARGE : CONV_STORE_CORRUPT;
        }
        if (!in_text) {
            return CONV_STORE_CORRUPT;
        }

        total_conv_bytes += msg_len;
        if (total_conv_bytes > (size_t)CHAT_MAX_MESSAGES * CHAT_MESSAGE_MAX) {
            return CONV_STORE_TOO_LARGE;
        }
        if (msg_count >= CHAT_MAX_MESSAGES) {
            return CONV_STORE_TOO_LARGE;
        }
        if (model != NULL) {
            if (chat_model_append_message(model, role, msg_buf, msg_len) != 0) {
                return CONV_STORE_CORRUPT;
            }
            if (partial && model->message_count > 0u) {
                model->messages[model->message_count - 1u].partial = true;
            }
        }
        msg_count++;

        if (res == 1 || strcmp(line_buf, "[conversation]") == 0) {
            break;
        }
    }

    if (msg_count == 0u) {
        return CONV_STORE_CORRUPT;
    }
    if (out_res != NULL) {
        *out_res = res;
    }
    return CONV_STORE_OK;
}

typedef struct {
    size_t count;
    bool title_found;
} scan_summary_t;

static conv_store_status_t scan_file(FILE *f, const char *match_title, scan_summary_t *summary) {
    file_reader_t reader;
    reader_init(&reader, f);
    size_t len = 0u;
    summary->count = 0u;
    summary->title_found = false;

    int res = read_line(&reader, line_buf, LINE_CAPACITY, &len);
    while (res == 0 && line_buf[0] == '\0') {
        res = read_line(&reader, line_buf, LINE_CAPACITY, &len);
    }

    while (res == 0) {
        if (strcmp(line_buf, "[conversation]") != 0) {
            return CONV_STORE_CORRUPT;
        }
        res = read_line(&reader, line_buf, LINE_CAPACITY, &len);
        if (res != 0 || strncmp(line_buf, "title=", 6) != 0) {
            return CONV_STORE_CORRUPT;
        }
        char title[CONV_STORE_TITLE_MAX + 1u];
        size_t title_len = len - 6u;
        if (title_len > CONV_STORE_TITLE_MAX) {
            title_len = CONV_STORE_TITLE_MAX;
        }
        memcpy(title, line_buf + 6, title_len);
        title[title_len] = '\0';

        if (match_title != NULL && strcmp(title, match_title) == 0) {
            summary->title_found = true;
        }
        summary->count++;

        res = read_line(&reader, line_buf, LINE_CAPACITY, &len);
        if (res != 0 || strcmp(line_buf, "[message]") != 0) {
            return CONV_STORE_CORRUPT;
        }
        conv_store_status_t st = parse_messages(&reader, NULL, &res);
        if (st != CONV_STORE_OK) {
            return st;
        }
    }
    if (res < 0) {
        return res == -1 ? CONV_STORE_TOO_LARGE : CONV_STORE_CORRUPT;
    }
    return CONV_STORE_OK;
}

conv_store_status_t conv_store_list(const char *dir,
                                    char titles[][CONV_STORE_TITLE_MAX + 1u],
                                    size_t *count,
                                    size_t capacity) {
    if (dir == NULL || titles == NULL || count == NULL) {
        return CONV_STORE_INVALID_ARGUMENT;
    }
    *count = 0u;
    if (build_paths(dir) != 0) {
        return CONV_STORE_INVALID_ARGUMENT;
    }

    FILE *f = open_existing();
    if (f == NULL) {
        return CONV_STORE_OK;
    }

    file_reader_t reader;
    reader_init(&reader, f);
    size_t len = 0u;
    size_t conv_count = 0u;

    int res = read_line(&reader, line_buf, LINE_CAPACITY, &len);
    while (res == 0 && line_buf[0] == '\0') {
        res = read_line(&reader, line_buf, LINE_CAPACITY, &len);
    }

    while (res == 0) {
        if (strcmp(line_buf, "[conversation]") != 0) {
            fclose(f);
            *count = 0u;
            return CONV_STORE_CORRUPT;
        }
        res = read_line(&reader, line_buf, LINE_CAPACITY, &len);
        if (res != 0 || strncmp(line_buf, "title=", 6) != 0) {
            fclose(f);
            *count = 0u;
            return CONV_STORE_CORRUPT;
        }
        char title[CONV_STORE_TITLE_MAX + 1u];
        size_t title_len = len - 6u;
        if (title_len > CONV_STORE_TITLE_MAX) {
            title_len = CONV_STORE_TITLE_MAX;
        }
        memcpy(title, line_buf + 6, title_len);
        title[title_len] = '\0';

        if (conv_count < capacity) {
            memcpy(titles[conv_count], title, title_len + 1u);
        }
        conv_count++;

        res = read_line(&reader, line_buf, LINE_CAPACITY, &len);
        if (res != 0 || strcmp(line_buf, "[message]") != 0) {
            fclose(f);
            *count = 0u;
            return CONV_STORE_CORRUPT;
        }
        conv_store_status_t st = parse_messages(&reader, NULL, &res);
        if (st != CONV_STORE_OK) {
            fclose(f);
            *count = 0u;
            return st;
        }
    }
    fclose(f);

    if (res < 0) {
        *count = 0u;
        return res == -1 ? CONV_STORE_TOO_LARGE : CONV_STORE_CORRUPT;
    }
    *count = conv_count;
    return CONV_STORE_OK;
}

conv_store_status_t conv_store_load(chat_model_t *model,
                                    const char *title,
                                    const char *dir) {
    if (model == NULL || title == NULL || dir == NULL) {
        return CONV_STORE_INVALID_ARGUMENT;
    }
    chat_model_reset(model);
    if (sanitize_title(clean_title_buf, title) != 0 || build_paths(dir) != 0) {
        return CONV_STORE_INVALID_ARGUMENT;
    }

    FILE *f = open_existing();
    if (f == NULL) {
        return CONV_STORE_NOT_FOUND;
    }

    file_reader_t reader;
    reader_init(&reader, f);
    size_t len = 0u;
    bool found = false;

    int res = read_line(&reader, line_buf, LINE_CAPACITY, &len);
    while (res == 0 && line_buf[0] == '\0') {
        res = read_line(&reader, line_buf, LINE_CAPACITY, &len);
    }

    while (res == 0) {
        if (strcmp(line_buf, "[conversation]") != 0) {
            fclose(f);
            chat_model_reset(model);
            return CONV_STORE_CORRUPT;
        }
        res = read_line(&reader, line_buf, LINE_CAPACITY, &len);
        if (res != 0 || strncmp(line_buf, "title=", 6) != 0) {
            fclose(f);
            chat_model_reset(model);
            return CONV_STORE_CORRUPT;
        }
        char current_title[CONV_STORE_TITLE_MAX + 1u];
        size_t title_len = len - 6u;
        if (title_len > CONV_STORE_TITLE_MAX) {
            title_len = CONV_STORE_TITLE_MAX;
        }
        memcpy(current_title, line_buf + 6, title_len);
        current_title[title_len] = '\0';

        bool is_target = (strcmp(current_title, clean_title_buf) == 0);
        res = read_line(&reader, line_buf, LINE_CAPACITY, &len);
        if (res != 0 || strcmp(line_buf, "[message]") != 0) {
            fclose(f);
            chat_model_reset(model);
            return CONV_STORE_CORRUPT;
        }

        conv_store_status_t st = parse_messages(&reader, is_target ? model : NULL, &res);
        if (st != CONV_STORE_OK) {
            fclose(f);
            chat_model_reset(model);
            return st;
        }

        if (is_target) {
            found = true;
            break;
        }
    }
    fclose(f);

    if (!found) {
        chat_model_reset(model);
        return CONV_STORE_NOT_FOUND;
    }
    return CONV_STORE_OK;
}

static conv_store_status_t rewrite_file(FILE *in, FILE *out, const chat_model_t *model, const char *clean_title) {
    file_reader_t reader;
    reader_init(&reader, in);
    char line[LINE_CAPACITY];
    size_t len = 0u;
    bool written_target = false;

    int res = read_line(&reader, line, sizeof(line), &len);
    while (res == 0 && line[0] == '\0') {
        res = read_line(&reader, line, sizeof(line), &len);
    }

    while (res == 0) {
        if (strcmp(line, "[conversation]") != 0) {
            return CONV_STORE_CORRUPT;
        }
        res = read_line(&reader, line, sizeof(line), &len);
        if (res != 0 || strncmp(line, "title=", 6) != 0) {
            return CONV_STORE_CORRUPT;
        }
        char title[CONV_STORE_TITLE_MAX + 1u];
        size_t title_len = len - 6u;
        if (title_len > CONV_STORE_TITLE_MAX) {
            title_len = CONV_STORE_TITLE_MAX;
        }
        memcpy(title, line + 6, title_len);
        title[title_len] = '\0';

        bool is_match = (strcmp(title, clean_title) == 0);
        if (is_match) {
            if (model != NULL) {
                if (write_conversation(out, model, clean_title) != 0) {
                    return CONV_STORE_IO_ERROR;
                }
                written_target = true;
            }
        } else {
            if (write_line(out, "[conversation]") != 0 ||
                write_line(out, line) != 0) {
                return CONV_STORE_IO_ERROR;
            }
        }

        while ((res = read_line(&reader, line, sizeof(line), &len)) == 0) {
            if (strcmp(line, "[conversation]") == 0) {
                break;
            }
            if (!is_match) {
                if (write_line(out, line) != 0) {
                    return CONV_STORE_IO_ERROR;
                }
            }
        }
        if (res < 0) {
            return CONV_STORE_CORRUPT;
        }
    }

    if (model != NULL && !written_target) {
        if (write_conversation(out, model, clean_title) != 0) {
            return CONV_STORE_IO_ERROR;
        }
    }
    return CONV_STORE_OK;
}

conv_store_status_t conv_store_delete(const char *title,
                                      const char *dir) {
    if (title == NULL || dir == NULL) {
        return CONV_STORE_INVALID_ARGUMENT;
    }
    if (sanitize_title(clean_title_buf, title) != 0 || build_paths(dir) != 0) {
        return CONV_STORE_INVALID_ARGUMENT;
    }

    FILE *in = open_existing();
    if (in == NULL) {
        return CONV_STORE_NOT_FOUND;
    }

    scan_summary_t summary;
    conv_store_status_t status = scan_file(in, clean_title_buf, &summary);
    fclose(in);

    if (status != CONV_STORE_OK) {
        return status;
    }
    if (!summary.title_found) {
        return CONV_STORE_NOT_FOUND;
    }

    in = open_existing();
    if (in == NULL) {
        return CONV_STORE_NOT_FOUND;
    }

    FILE *out = fopen(temp_path, "wb");
    if (out == NULL) {
        fclose(in);
        return CONV_STORE_IO_ERROR;
    }

    status = rewrite_file(in, out, NULL, clean_title_buf);
    fclose(in);

    int failed = (status != CONV_STORE_OK);
    failed |= fflush(out) != 0;
    failed |= fclose(out) != 0;

    if (failed || replace_file() != 0) {
        remove(temp_path);
        return CONV_STORE_IO_ERROR;
    }
    return CONV_STORE_OK;
}

conv_store_status_t conv_store_save(const chat_model_t *model,
                                    const char *title,
                                    const char *dir) {
    if (model == NULL || title == NULL || dir == NULL) {
        return CONV_STORE_INVALID_ARGUMENT;
    }
    if (sanitize_title(clean_title_buf, title) != 0 || build_paths(dir) != 0) {
        return CONV_STORE_INVALID_ARGUMENT;
    }

    size_t count = chat_model_message_count(model);
    if (count == 0u) {
        return CONV_STORE_INVALID_ARGUMENT;
    }
    if (count > CHAT_MAX_MESSAGES) {
        return CONV_STORE_TOO_LARGE;
    }

    size_t total_bytes = 0u;
    for (size_t i = 0; i < count; i++) {
        const chat_message_t *m = chat_model_message_at(model, i);
        if (m == NULL || m->length > CHAT_MESSAGE_MAX) {
            return CONV_STORE_TOO_LARGE;
        }
        total_bytes += m->length;
    }
    if (total_bytes > (size_t)CHAT_MAX_MESSAGES * CHAT_MESSAGE_MAX) {
        return CONV_STORE_TOO_LARGE;
    }

    bool file_exists = false;
    scan_summary_t summary = {0, false};
    FILE *in = open_existing();
    if (in != NULL) {
        file_exists = true;
        conv_store_status_t st = scan_file(in, clean_title_buf, &summary);
        fclose(in);
        if (st != CONV_STORE_OK) {
            return st;
        }
        if (!summary.title_found && summary.count >= CONV_STORE_MAX_CONVERSATIONS) {
            return CONV_STORE_FULL;
        }
    }

    FILE *out = fopen(temp_path, "wb");
    if (out == NULL) {
        return CONV_STORE_IO_ERROR;
    }

    conv_store_status_t status = CONV_STORE_OK;
    if (file_exists) {
        in = open_existing();
        if (in == NULL) {
            fclose(out);
            remove(temp_path);
            return CONV_STORE_IO_ERROR;
        }
        status = rewrite_file(in, out, model, clean_title_buf);
        fclose(in);
    } else {
        if (write_conversation(out, model, clean_title_buf) != 0) {
            status = CONV_STORE_IO_ERROR;
        }
    }

    int failed = (status != CONV_STORE_OK);
    failed |= fflush(out) != 0;
    failed |= fclose(out) != 0;

    if (failed || replace_file() != 0) {
        remove(temp_path);
        return CONV_STORE_IO_ERROR;
    }
    return CONV_STORE_OK;
}
