/*
 * PwnaUI Test Framework
 * A lightweight unit testing framework for C
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ANSI color codes for test output */
#define ANSI_RED     "\x1b[31m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_BLUE    "\x1b[34m"
#define ANSI_RESET   "\x1b[0m"

/* Test result counters */
static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;
static int g_assertions = 0;
static int g_assertions_passed = 0;
static int g_assertions_failed = 0;

/* Current test name for error reporting */
static const char *g_current_test = "unknown";
static const char *g_current_suite = "unknown";

/*
 * Test macros
 */
#define TEST_SUITE(name) \
    static void suite_##name(void); \
    static const char *suite_name_##name = #name;

#define RUN_SUITE(name) \
    do { \
        g_current_suite = suite_name_##name; \
        printf(ANSI_BLUE "\n=== Test Suite: %s ===" ANSI_RESET "\n", suite_name_##name); \
        suite_##name(); \
    } while(0)

#define TEST(name) \
    static void test_##name(void)

#define RUN_TEST(name) \
    do { \
        g_current_test = #name; \
        g_tests_run++; \
        int failed_before = g_assertions_failed; \
        test_##name(); \
        if (g_assertions_failed == failed_before) { \
            g_tests_passed++; \
            printf(ANSI_GREEN "  ✓ %s" ANSI_RESET "\n", #name); \
        } else { \
            g_tests_failed++; \
            printf(ANSI_RED "  ✗ %s" ANSI_RESET "\n", #name); \
        } \
    } while(0)

/*
 * Assertion macros
 */
#define ASSERT_TRUE(condition) \
    do { \
        g_assertions++; \
        if (condition) { \
            g_assertions_passed++; \
        } else { \
            g_assertions_failed++; \
            printf(ANSI_RED "    ASSERT_TRUE failed: %s (line %d)" ANSI_RESET "\n", \
                   #condition, __LINE__); \
        } \
    } while(0)

#define ASSERT_FALSE(condition) \
    do { \
        g_assertions++; \
        if (!(condition)) { \
            g_assertions_passed++; \
        } else { \
            g_assertions_failed++; \
            printf(ANSI_RED "    ASSERT_FALSE failed: %s (line %d)" ANSI_RESET "\n", \
                   #condition, __LINE__); \
        } \
    } while(0)

#define ASSERT_EQUAL(expected, actual) \
    do { \
        g_assertions++; \
        if ((expected) == (actual)) { \
            g_assertions_passed++; \
        } else { \
            g_assertions_failed++; \
            printf(ANSI_RED "    ASSERT_EQUAL failed: expected %ld, got %ld (line %d)" ANSI_RESET "\n", \
                   (long)(expected), (long)(actual), __LINE__); \
        } \
    } while(0)

#define ASSERT_NOT_EQUAL(expected, actual) \
    do { \
        g_assertions++; \
        if ((expected) != (actual)) { \
            g_assertions_passed++; \
        } else { \
            g_assertions_failed++; \
            printf(ANSI_RED "    ASSERT_NOT_EQUAL failed: both are %ld (line %d)" ANSI_RESET "\n", \
                   (long)(expected), __LINE__); \
        } \
    } while(0)

#define ASSERT_NULL(ptr) \
    do { \
        g_assertions++; \
        if ((ptr) == NULL) { \
            g_assertions_passed++; \
        } else { \
            g_assertions_failed++; \
            printf(ANSI_RED "    ASSERT_NULL failed: pointer is not NULL (line %d)" ANSI_RESET "\n", \
                   __LINE__); \
        } \
    } while(0)

#define ASSERT_NOT_NULL(ptr) \
    do { \
        g_assertions++; \
        if ((ptr) != NULL) { \
            g_assertions_passed++; \
        } else { \
            g_assertions_failed++; \
            printf(ANSI_RED "    ASSERT_NOT_NULL failed: pointer is NULL (line %d)" ANSI_RESET "\n", \
                   __LINE__); \
        } \
    } while(0)

#define ASSERT_STR_EQUAL(expected, actual) \
    do { \
        g_assertions++; \
        if ((expected) && (actual) && strcmp((expected), (actual)) == 0) { \
            g_assertions_passed++; \
        } else { \
            g_assertions_failed++; \
            printf(ANSI_RED "    ASSERT_STR_EQUAL failed: expected \"%s\", got \"%s\" (line %d)" ANSI_RESET "\n", \
                   (expected) ? (expected) : "(null)", \
                   (actual) ? (actual) : "(null)", __LINE__); \
        } \
    } while(0)

#define ASSERT_STR_NOT_EQUAL(expected, actual) \
    do { \
        g_assertions++; \
        if (strcmp((expected), (actual)) != 0) { \
            g_assertions_passed++; \
        } else { \
            g_assertions_failed++; \
            printf(ANSI_RED "    ASSERT_STR_NOT_EQUAL failed: both are \"%s\" (line %d)" ANSI_RESET "\n", \
                   (expected), __LINE__); \
        } \
    } while(0)

#define ASSERT_MEM_EQUAL(expected, actual, size) \
    do { \
        g_assertions++; \
        if (memcmp((expected), (actual), (size)) == 0) { \
            g_assertions_passed++; \
        } else { \
            g_assertions_failed++; \
            printf(ANSI_RED "    ASSERT_MEM_EQUAL failed: memory differs (line %d)" ANSI_RESET "\n", \
                   __LINE__); \
        } \
    } while(0)

#define ASSERT_RANGE(value, min, max) \
    do { \
        g_assertions++; \
        long v = (long)(value); \
        if (v >= (long)(min) && v <= (long)(max)) { \
            g_assertions_passed++; \
        } else { \
            g_assertions_failed++; \
            printf(ANSI_RED "    ASSERT_RANGE failed: %ld not in [%ld, %ld] (line %d)" ANSI_RESET "\n", \
                   v, (long)(min), (long)(max), __LINE__); \
        } \
    } while(0)

/*
 * Test setup and teardown
 */
#define SETUP() static void setup(void)
#define TEARDOWN() static void teardown(void)
#define CALL_SETUP() setup()
#define CALL_TEARDOWN() teardown()

/*
 * Print test summary
 */
static inline void test_print_summary(void) {
    printf("\n" ANSI_BLUE "═══════════════════════════════════════" ANSI_RESET "\n");
    printf(ANSI_BLUE "           TEST SUMMARY" ANSI_RESET "\n");
    printf(ANSI_BLUE "═══════════════════════════════════════" ANSI_RESET "\n");
    printf("  Tests:      %d run, ", g_tests_run);
    if (g_tests_passed > 0) printf(ANSI_GREEN "%d passed" ANSI_RESET ", ", g_tests_passed);
    if (g_tests_failed > 0) printf(ANSI_RED "%d failed" ANSI_RESET, g_tests_failed);
    printf("\n");
    printf("  Assertions: %d run, ", g_assertions);
    if (g_assertions_passed > 0) printf(ANSI_GREEN "%d passed" ANSI_RESET ", ", g_assertions_passed);
    if (g_assertions_failed > 0) printf(ANSI_RED "%d failed" ANSI_RESET, g_assertions_failed);
    printf("\n");
    printf(ANSI_BLUE "═══════════════════════════════════════" ANSI_RESET "\n\n");
    
    if (g_tests_failed == 0) {
        printf(ANSI_GREEN "All tests passed! ✓" ANSI_RESET "\n\n");
    } else {
        printf(ANSI_RED "Some tests failed! ✗" ANSI_RESET "\n\n");
    }
}

/*
 * Return exit code based on test results
 */
static inline int test_exit_code(void) {
    return g_tests_failed > 0 ? 1 : 0;
}

/*
 * Reset test counters (for running multiple test files)
 */
static inline void test_reset_counters(void) {
    g_tests_run = 0;
    g_tests_passed = 0;
    g_tests_failed = 0;
    g_assertions = 0;
    g_assertions_passed = 0;
    g_assertions_failed = 0;
}

#endif /* TEST_FRAMEWORK_H */
