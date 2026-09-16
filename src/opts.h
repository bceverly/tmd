/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef TMD_OPTS_H
#define TMD_OPTS_H

#include <stdio.h>

#include "tmd.h"

/*
 * Exit status.
 *
 * Three distinct failures rather than one, because the difference matters to a
 * script: a mistyped flag is the caller's bug, an unreadable archive is the
 * file's problem, and a bad checksum is a finding — the tool worked perfectly
 * and the archive is damaged.
 */
#define TMD_EXIT_OK      0
#define TMD_EXIT_ERROR   1
#define TMD_EXIT_USAGE   2
#define TMD_EXIT_CHECK   3

/* What the command line asked for. `files` points into argv and is not owned. */
struct tmd_cli {
    struct tmd_options options;
    const char       **files;
    size_t             nfiles;
    const char        *output_path;   /* NULL for standard output */
    bool               want_help;
    bool               want_version;
};

/* Fills `cli`. Returns TMD_EXIT_OK to carry on, or the status to exit with.
 * Diagnostics go to stderr; the help text goes to stdout. */
int tmd_parse_args(int argc, char **argv, struct tmd_cli *cli);
void tmd_free_args(struct tmd_cli *cli);

void tmd_print_usage(FILE *out);
void tmd_print_version(FILE *out);

#endif /* TMD_OPTS_H */
