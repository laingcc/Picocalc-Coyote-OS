#include "storage/file_store.h"

#include <string.h>

static char temp_path[FILE_STORE_PATH_CAPACITY];
static char backup_path[FILE_STORE_PATH_CAPACITY];

int file_store_path(char *path, const char *dir, const char *name) {
    size_t dir_length;
    size_t name_length;

    if (dir == NULL) {
        return -1;
    }
    dir_length = strlen(dir);
    name_length = strlen(name);
    if (dir_length == 0u ||
        dir_length + 1u + name_length + sizeof(FILE_STORE_TEMP_SUFFIX) > FILE_STORE_PATH_CAPACITY) {
        return -1;
    }
    memcpy(path, dir, dir_length);
    path[dir_length] = '/';
    memcpy(path + dir_length + 1u, name, name_length + 1u);
    return 0;
}

static void side_paths(const char *path) {
    snprintf(temp_path, sizeof(temp_path), "%s" FILE_STORE_TEMP_SUFFIX, path);
    snprintf(backup_path, sizeof(backup_path), "%s" FILE_STORE_BACKUP_SUFFIX, path);
}

FILE *file_store_open(const char *path) {
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        /* An interrupted swap can leave only the backup behind. */
        side_paths(path);
        file = fopen(backup_path, "rb");
    }
    return file;
}

FILE *file_store_open_temp(const char *path) {
    side_paths(path);
    return fopen(temp_path, "wb");
}

static int replace_file(const char *path) {
    if (rename(temp_path, path) == 0) {
        /* A stale backup from an earlier interrupted swap is no longer needed. */
        remove(backup_path);
        return 0;
    }
    /* FAT refuses to rename over an existing file: step the old one aside. */
    remove(backup_path);
    if (rename(path, backup_path) != 0) {
        return -1;
    }
    if (rename(temp_path, path) != 0) {
        rename(backup_path, path);
        return -1;
    }
    remove(backup_path);
    return 0;
}

int file_store_commit(const char *path, FILE *temp, int failed) {
    failed |= fflush(temp) != 0;
    failed |= fclose(temp) != 0;

    side_paths(path);
    if (failed || replace_file(path) != 0) {
        remove(temp_path);
        return -1;
    }
    return 0;
}

int file_store_write_atomic(const char *path, const void *data, size_t length) {
    FILE *file = file_store_open_temp(path);

    if (file == NULL) {
        return -1;
    }
    return file_store_commit(path, file, fwrite(data, 1u, length, file) != length);
}
