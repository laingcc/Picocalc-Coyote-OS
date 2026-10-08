#ifndef COYOTE_TEST_UTIL_H
#define COYOTE_TEST_UTIL_H

#include <stdio.h>
#include <string.h>

extern int g_tests_run;
extern int g_tests_failed;

#define CHECK(condition)                                                       \
    do {                                                                       \
        g_tests_run++;                                                         \
        if (!(condition)) {                                                    \
            g_tests_failed++;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
        }                                                                      \
    } while (0)

#define CHECK_STR_EQ(actual, expected)                                         \
    do {                                                                       \
        g_tests_run++;                                                         \
        const char *actual_ = (actual);                                        \
        const char *expected_ = (expected);                                    \
        if (actual_ == NULL || strcmp(actual_, expected_) != 0) {              \
            g_tests_failed++;                                                  \
            printf("FAIL %s:%d: expected \"%s\" got \"%s\"\n", __FILE__, __LINE__, \
                   expected_, actual_ == NULL ? "(null)" : actual_);           \
        }                                                                      \
    } while (0)

#define CHECK_BYTES_EQ(actual, expected, length)                               \
    do {                                                                       \
        g_tests_run++;                                                         \
        if (memcmp((actual), (expected), (length)) != 0) {                     \
            g_tests_failed++;                                                  \
            printf("FAIL %s:%d: byte comparison failed\n", __FILE__, __LINE__);\
        }                                                                      \
    } while (0)

#endif
