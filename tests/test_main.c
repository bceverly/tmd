/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "test.h"

#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

int         tmd_test_checks = 0;
int         tmd_test_failures = 0;
const char *tmd_test_case = "(none)";

static int  case_count = 0;

/*
 * Where a failure is reported, and why it is not just stderr.
 *
 * A test may legitimately silence stderr for the length of a case -- the
 * truncated-gzip case does, because gzip complains on its own stderr and that
 * is noise in a passing run. A failure inside such a window used to be written
 * into /dev/null along with it, so the count at the end said one check failed
 * and nothing anywhere said which. That is the worst possible form of a test
 * failure: it happened on a machine you do not have, and the log does not name
 * it. Found exactly that way, on a macOS runner.
 *
 * So the reporting stream is duplicated out of stderr in main(), before any
 * test can redirect the descriptor, and every failure goes there instead.
 * Redirecting fd 2 afterwards moves what the code under test writes; it cannot
 * move this.
 */
static FILE *report = NULL;

void tmd_test_begin(const char *name)
{
    tmd_test_case = name;
    case_count++;
}

void tmd_test_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    FILE   *out = report ? report : stderr;

    tmd_test_failures++;
    /* The summary at the end goes to stdout; this goes to another stream
     * entirely, and a CI log shows them interleaved by arrival rather than by
     * order written. Flushed so that what arrives is in the order it happened. */
    (void)fflush(stdout);
    (void)fprintf(out, "  \033[91mFAIL\033[0m %s\n    %s:%d: ", tmd_test_case,
                  file, line);
    va_start(ap, fmt);
    (void)vfprintf(out, fmt, ap);
    va_end(ap);
    (void)fputc('\n', out);
    (void)fflush(out);
}

int main(void)
{
    int report_fd = dup(STDERR_FILENO);

    if (report_fd >= 0)
    {
        report = fdopen(report_fd, "w");
        if (!report)
        {
            (void)close(report_fd);
        }
    }

    (void)printf("tmd unit tests\n");

    test_util();
    test_source();
    test_tar();
    test_pax();
    test_render();

    if (report)
    {
        (void)fclose(report);
        report = NULL;
    }

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
