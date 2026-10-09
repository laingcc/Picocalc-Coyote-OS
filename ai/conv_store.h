#ifndef COYOTE_AI_CONV_STORE_H
#define COYOTE_AI_CONV_STORE_H

#include <stddef.h>
#include "ai/chat_model.h"
#include "ai/file_store.h"

/*
 * Persistence of AI chat conversations as <dir>/convs.txt.
 *
 * Only stdio and chat_model are used, so the module builds for host tests
 * as well as the firmware (where dir is "/coyote" on the FAT SD card).
 * Scratch state is static: calls do not allocate or use large stack frames.
 *
 * File format:
 *   [conversation]
 *   title=<title>
 *   [message]
 *   role=user|assistant
 *   partial=1        (only when partial is true)
 *   text=<content, may span multiple lines>
 */

#define CONV_STORE_FILE_NAME "convs.txt"
#define CONV_STORE_TEMP_NAME CONV_STORE_FILE_NAME FILE_STORE_TEMP_SUFFIX
#define CONV_STORE_BACKUP_NAME CONV_STORE_FILE_NAME FILE_STORE_BACKUP_SUFFIX

#define CONV_STORE_MAX_CONVERSATIONS 8u
#define CONV_STORE_TITLE_MAX 63u
#define CONV_STORE_PATH_CAPACITY FILE_STORE_PATH_CAPACITY

/* Longest export file name stem built from a title, before ".txt". */
#define CONV_STORE_EXPORT_NAME_MAX 32u

typedef enum {
    CONV_STORE_OK = 0,
    CONV_STORE_IO_ERROR = 1,
    CONV_STORE_NOT_FOUND = 2,
    CONV_STORE_INVALID_ARGUMENT = 3,
    CONV_STORE_FULL = 4,
    CONV_STORE_TOO_LARGE = 5,
    CONV_STORE_CORRUPT = 6
} conv_store_status_t;

/*
 * List saved conversation titles in file order.
 * Fills up to capacity titles into the titles buffer and sets *count.
 * A missing file sets *count = 0 and returns CONV_STORE_OK.
 */
conv_store_status_t conv_store_list(const char *dir,
                                    char titles[][CONV_STORE_TITLE_MAX + 1u],
                                    size_t *count,
                                    size_t capacity);

/*
 * Save a conversation under title into <dir>/convs.txt.
 * Replaces an existing conversation with the same title.
 * Rejects when chat_model_message_count(model) == 0 (nothing to save),
 * when transcript bounds are exceeded, or when the 8-conversation capacity
 * is reached and this is a new title.
 */
conv_store_status_t conv_store_save(const chat_model_t *model,
                                    const char *title,
                                    const char *dir);

/*
 * Load a conversation by title into model.
 * Resets model first. If title is not found or parsing fails, resets model again
 * and returns an error status. Never leaves a half-loaded transcript.
 */
conv_store_status_t conv_store_load(chat_model_t *model,
                                    const char *title,
                                    const char *dir);

/*
 * Delete a conversation by title from <dir>/convs.txt.
 * Rewrites the file without the conversation. Missing title returns CONV_STORE_NOT_FOUND.
 */
conv_store_status_t conv_store_delete(const char *title,
                                      const char *dir);

/*
 * Build the export path <dir>/<name>.txt for a conversation title.
 * The name keeps the title's ASCII letters, digits and '-'; every other run of
 * characters becomes a single '_', and the stem is cut to
 * CONV_STORE_EXPORT_NAME_MAX. A title with nothing left exports as "chat",
 * and a name that would land on convs.txt gets a trailing '_'.
 */
conv_store_status_t conv_store_export_path(const char *title,
                                           const char *dir,
                                           char *path,
                                           size_t capacity);

/*
 * Write model's transcript to path as plain text for reading off the device:
 *   # <title>
 *   <blank line>
 *   user: <text>
 *   assistant: <text>
 * Message text is written as stored (no convs.txt escaping) minus control
 * characters other than newline and tab; a reply that was cut short is
 * labelled "assistant (incomplete)". An existing file is replaced through
 * <path>.tmp, so path must leave room for that suffix.
 * *written (optional) receives the size of the file, 0 on failure.
 * Rejects an empty transcript like conv_store_save.
 */
conv_store_status_t conv_store_export(const chat_model_t *model,
                                      const char *title,
                                      const char *path,
                                      size_t *written);

#endif
