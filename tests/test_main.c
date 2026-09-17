/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "test.h"

#include <stdarg.h>
#include <stdlib.h>

int         tmd_test_checks = 0;
int         tmd_test_failures = 0;
const char *tmd_test_case = "(none)";

static int  case_count = 0;

void tmd_test_begin(const char *name)
{
    tmd_test_case = name;
    case_count++;
}

void tmd_test_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    tmd_test_failures++;
    (void)fprintf(stderr, "  \033[91mFAIL\033[0m %s\n    %s:%d: ", tmd_test_case,
                  file, line);
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);
}

int main(void)
{
    (void)printf("tmd unit tests\n");

    test_util();
    test_source();
    test_tar();
    test_pax();
    test_render();

    if (tmd_test_failures == 0)
    {
        (void)printf("  \033[92mok\033[0m  %d checks across %d cases\n",
                     tmd_test_checks, case_count);
        return 0;
    }
    (void)printf("  \033[91m%d of %d checks failed\033[0m across %d cases\n",
                 tmd_test_failures, tmd_test_checks, case_count);
    return 1;
}
