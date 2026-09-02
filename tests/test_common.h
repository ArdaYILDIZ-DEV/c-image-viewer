#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

extern int g_tests_run;
extern int g_tests_failed;

#define TEST_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s:%d: assertion failed: %s\n", __FILE__, __LINE__, #cond); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

#define TEST_ASSERT_STR_EQ(a, b) do { \
    const char *_a = (a); \
    const char *_b = (b); \
    if (!_a || !_b || strcmp(_a, _b) != 0) { \
        fprintf(stderr, "  FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

#define TEST_ASSERT_INT_EQ(a, b) do { \
    long long _a = (long long)(a); \
    long long _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s:%d: %lld != %lld\n", __FILE__, __LINE__, _a, _b); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

#define TEST_RUN(fn) do { \
    int prev_failures = g_tests_failed; \
    g_tests_run++; \
    printf("  RUN  %s\n", #fn); \
    fn(); \
    if (g_tests_failed == prev_failures) { \
        printf("  PASS %s\n", #fn); \
    } \
} while (0)

#endif /* TEST_COMMON_H */
