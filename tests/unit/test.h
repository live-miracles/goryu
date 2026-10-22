#ifndef TEST_H
#define TEST_H

/* A minimal test harness: CHECK records a failure and carries on, so one run
 * lists every broken expectation. Each test file has its own main(). */

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                               \
        }                                                                                          \
    } while (0)

#define CHECK_STR(actual, expected)                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        const char *a_ = (actual);                                                                 \
        const char *e_ = (expected);                                                               \
        if (strcmp(a_, e_) != 0) {                                                                 \
            ++g_failures;                                                                          \
            printf("  FAIL %s:%d: %s\n    got:      [%s]\n    expected: [%s]\n", __FILE__,         \
                   __LINE__, #actual, a_, e_);                                                     \
        }                                                                                          \
    } while (0)

#define TEST_SUMMARY(name)                                                                         \
    do {                                                                                           \
        printf("%s: %d checks, %d failed\n", name, g_checks, g_failures);                          \
        return g_failures ? 1 : 0;                                                                 \
    } while (0)

#endif
