/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * A test harness in one header.
 *
 * No external framework: the whole point of this project is a small C program
 * with no dependencies, and a test runner that needs a package installed
 * before `make test` works would undo that. What is here is what a unit test
 * actually needs — a name, a set of assertions that keep going after a failure
 * so one run reports every problem, and a count at the end.
 */
#ifndef TMD_TEST_H
#define TMD_TEST_H

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "tmd.h"

extern int         tmd_test_checks;
extern int         tmd_test_failures;
extern const char *tmd_test_case;

void tmd_test_begin(const char *name);
/* TMD_PRINTF for the same reason every other formatting function here has it:
 * it checks the arguments at each call site. It also settles a warning, since
 * clang's -Wformat-nonliteral objects to the vfprintf inside a variadic
 * forwarder that has not said it is one. */
void tmd_test_fail(const char *file, int line, const char *fmt, ...)
    TMD_PRINTF(3, 4);

#define TEST_CASE(name) tmd_test_begin(name)

#define CHECK(cond)                                                            \
    do {                                                                       \
        tmd_test_checks++;                                                     \
        if (!(cond))                                                           \
            tmd_test_fail(__FILE__, __LINE__, "%s", #cond);                    \
    } while (0)

#define CHECK_STR(actual, expected)                                            \
    do {                                                                       \
        const char *a_ = (actual);                                             \
        const char *e_ = (expected);                                           \
        tmd_test_checks++;                                                     \
        if (!a_ || !e_ || strcmp(a_, e_) != 0)                                 \
            tmd_test_fail(__FILE__, __LINE__, "expected \"%s\", got \"%s\"",   \
                          e_ ? e_ : "(null)", a_ ? a_ : "(null)");             \
    } while (0)

#define CHECK_INT(actual, expected)                                            \
    do {                                                                       \
        long long a_ = (long long)(actual);                                    \
        long long e_ = (long long)(expected);                                  \
        tmd_test_checks++;                                                     \
        if (a_ != e_)                                                          \
            tmd_test_fail(__FILE__, __LINE__, "%s: expected %lld, got %lld",   \
                          #actual, e_, a_);                                    \
    } while (0)

/* Substring rather than equality, for messages whose exact wording is allowed
 * to change but whose content is not. */
#define CHECK_CONTAINS(haystack, needle)                                       \
    do {                                                                       \
        const char *h_ = (haystack);                                           \
        tmd_test_checks++;                                                     \
        if (!h_ || !strstr(h_, (needle)))                                      \
            tmd_test_fail(__FILE__, __LINE__, "\"%s\" does not contain \"%s\"",\
                          h_ ? h_ : "(null)", (needle));                       \
    } while (0)

/* Every suite, declared here and called from test_main.c. */
void test_util(void);
void test_source(void);
void test_tar(void);
void test_pax(void);
void test_render(void);

#endif /* TMD_TEST_H */
