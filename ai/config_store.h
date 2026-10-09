#ifndef COYOTE_AI_CONFIG_STORE_H
#define COYOTE_AI_CONFIG_STORE_H

#include "ai/ai_config.h"
#include "ai/file_store.h"

/*
 * Persistence of the AI configuration as <dir>/ai.ini.
 *
 * Only stdio and ai_config are used, so the module builds for the host tests
 * as well as the firmware (where dir is "/coyote" on the FAT SD card).  All
 * scratch state is static: neither call allocates or needs a large stack, and
 * neither is reentrant.
 *
 * The file holds the Wi-Fi password and the bearer token.  Nothing here logs,
 * and the scratch buffers are wiped before each call returns.
 */

#define CONFIG_STORE_FILE_NAME "ai.ini"
#define CONFIG_STORE_TEMP_NAME CONFIG_STORE_FILE_NAME FILE_STORE_TEMP_SUFFIX
#define CONFIG_STORE_BACKUP_NAME CONFIG_STORE_FILE_NAME FILE_STORE_BACKUP_SUFFIX

/* Longest accepted "<dir>/<name>" path, including the trailing NUL. */
#define CONFIG_STORE_PATH_CAPACITY FILE_STORE_PATH_CAPACITY

/*
 * Reset config to its defaults, then apply <dir>/ai.ini if there is one.
 *
 * A missing file is not an error: the result is AI_CONFIG_OK with the
 * defaults.  A file that cannot be read or parsed returns AI_CONFIG_IO_ERROR
 * or the parser's status, and config is left holding the defaults, never a
 * half-applied file.
 */
ai_config_status_t config_store_load(ai_config_t *config, const char *dir);

/*
 * Write config to <dir>/ai.ini through a temporary file in the same
 * directory, so a failed save leaves the existing file as it was and removes
 * the temporary file.
 *
 * FAT cannot rename over an existing file.  There the old file is moved to
 * ai.ini.bak for the swap and restored if the swap fails; config_store_load
 * falls back to the backup if power was lost in between.
 */
ai_config_status_t config_store_save(const ai_config_t *config, const char *dir);

#endif
