#ifndef COYOTE_STORAGE_FILE_STORE_H
#define COYOTE_STORAGE_FILE_STORE_H

#include <stddef.h>
#include <stdio.h>

/*
 * Shared file persistence for the stores and the text editor.
 *
 * A file is replaced through "<path>.tmp" in the same directory, so a failed
 * write leaves the existing file as it was and removes the temporary file.
 *
 * FAT cannot rename over an existing file.  There the old file is moved to
 * "<path>.bak" for the swap and restored if the swap fails; file_store_open
 * falls back to the backup if power was lost in between.
 *
 * Only stdio is used, so the module builds for the host tests as well as the
 * firmware.  Scratch state is static: nothing allocates, nothing is reentrant.
 */

#define FILE_STORE_TEMP_SUFFIX ".tmp"
#define FILE_STORE_BACKUP_SUFFIX ".bak"

/* Longest accepted "<dir>/<name><suffix>" path, including the trailing NUL. */
#define FILE_STORE_PATH_CAPACITY 128u

/*
 * Join dir and name into path[FILE_STORE_PATH_CAPACITY].  Returns -1 when dir
 * is NULL or empty, or when the path would leave no room for a suffix.
 */
int file_store_path(char *path, const char *dir, const char *name);

/* Open path for reading, or its backup when path is missing. */
FILE *file_store_open(const char *path);

/* Open the temporary file that file_store_commit swaps into path. */
FILE *file_store_open_temp(const char *path);

/*
 * Close temp and swap it into path.  A nonzero failed, or a failed close or
 * swap, removes the temporary file instead and returns -1.
 */
int file_store_commit(const char *path, FILE *temp, int failed);

/* Replace path with length bytes of data. */
int file_store_write_atomic(const char *path, const void *data, size_t length);

#endif
