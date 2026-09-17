/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * Comparing an archive against something else.
 *
 * --diff compares two archives; --verify compares one against a manifest of
 * expected paths and sizes. They are the same operation underneath — build an
 * index of what is expected, stream what is actually there past it, and report
 * the three ways they can disagree — so they share it rather than each growing
 * their own.
 */
#ifndef TMD_COMPARE_H
#define TMD_COMPARE_H

#include <stdio.h>

#include "tmd.h"

/*
 * Both return the exit status the run deserves: TMD_EXIT_OK when everything
 * matched, TMD_EXIT_DIFFER when it did not, TMD_EXIT_ERROR when something
 * could not be read. Diagnostics go to stderr, the report to `out`.
 */
int tmd_diff_archives(const char *from, const char *to, FILE *out,
                      const struct tmd_options *opt);
int tmd_verify_archive(const char *archive, const char *manifest, FILE *out,
                       const struct tmd_options *opt);

#endif /* TMD_COMPARE_H */
