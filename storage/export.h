#ifndef COYOTE_STORAGE_EXPORT_H
#define COYOTE_STORAGE_EXPORT_H

#include <stddef.h>
#include "ai/chat_model.h"

/*
 * Plain-text export of a chat transcript, for reading off the device.
 *
 * The file is replaced through storage/file_store, so a failed export leaves
 * an earlier one as it was.  Only stdio and chat_model are used: the module
 * builds for the host tests as well as the firmware.  Nothing allocates.
 */

/* Longest export file name stem built from a title, before ".txt". */
#define EXPORT_NAME_MAX 32u

typedef enum {
    EXPORT_OK = 0,
    EXPORT_IO_ERROR = 1,
    EXPORT_INVALID_ARGUMENT = 2,
    EXPORT_TOO_LARGE = 3
} export_status_t;

/*
 * Build the export path <dir>/<name>.txt for a title.
 * The name keeps the title's ASCII letters, digits and '-'; every other run of
 * characters becomes a single '_', and the stem is cut to EXPORT_NAME_MAX.
 * A title with nothing left exports as "chat", and a name that would land on
 * reserved (optional, compared ignoring case) gets a trailing '_'.
 */
export_status_t export_path(const char *title,
                            const char *dir,
                            const char *reserved,
                            char *path,
                            size_t capacity);

/*
 * Write model's transcript to path:
 *   # <title>
 *   <blank line>
 *   user: <text>
 *   assistant: <text>
 * Message text is written minus control characters other than newline and
 * tab; a reply that was cut short is labelled "assistant (incomplete)".
 * path must leave room for the ".tmp" suffix used during the swap.
 * *written (optional) receives the size of the file, 0 on failure.
 * Rejects an empty transcript.
 */
export_status_t export_conversation(const chat_model_t *model,
                                    const char *title,
                                    const char *path,
                                    size_t *written);

#endif
