/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * tmd — dump the metadata out of a tar archive.
 *
 * Reads headers, never data, and never writes to the archive. The whole
 * program is: parse the command line, open each archive, pump entries from the
 * reader into the renderer, and report what went wrong on the way.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opts.h"
#include "render.h"
#include "source.h"
#include "tar.h"
#include "tmd.h"

/*
 * Warnings go to standard error, always.
 *
 * That is what makes `tmd -f x.tar -o report.txt` produce a clean report and
 * still tell the operator that three headers were damaged, and what makes
 * `tmd -f x.tar | grep foo` work on a damaged archive instead of matching a
 * warning line. The archive's name is on every one of them because -f can be
 * given more than once.
 */
static void report_warnings(const struct tmd_archive *a, const struct tmd_entry *e,
                            const struct tmd_options *opt)
{
    size_t i;

    if (opt->quiet)
        return;
    for (i = 0; i < e->nwarnings; i++)
        (void)fprintf(stderr, "tmd: %s: %s: %s\n", a->name,
                      e->path && e->path[0] ? e->path : "(unnamed member)",
                      e->warnings[i]);
}

static void report_archive_warnings(const struct tmd_archive *a,
                                    const struct tmd_options *opt, size_t from)
{
    size_t i;

    if (opt->quiet)
        return;
    for (i = from; i < a->nwarnings; i++)
        (void)fprintf(stderr, "tmd: %s: %s\n", a->name, a->warnings[i]);
}

/* Reads one archive into the renderer. Returns the exit status this archive
 * deserves; the caller keeps the worst one. */
static int dump_archive(const char *path, struct tmd_render *rd,
                        const struct tmd_options *opt)
{
    struct tmd_source        *src;
    struct tmd_reader        *reader;
    const struct tmd_entry   *entry;
    const struct tmd_archive *archive;
    char                     *err = NULL;
    size_t                    warnings_reported = 0;
    int                       status = TMD_EXIT_OK;
    int                       rc;

    src = tmd_source_open(path, &err);
    if (!src) {
        (void)fprintf(stderr, "tmd: %s\n", err ? err : "cannot open the archive");
        free(err);
        return TMD_EXIT_ERROR;
    }

    reader = tmd_reader_new(src);
    archive = tmd_reader_archive(reader);
    tmd_render_archive_begin(rd, archive);

    while ((rc = tmd_reader_next(reader, &entry)) == 1) {
        tmd_render_entry(rd, entry);
        report_warnings(archive, entry, opt);
        /* Archive-level complaints can be raised while reading a member — a
         * malformed pax record, for one — so they are drained as we go rather
         * than only at the end, keeping them next to the member they concern. */
        report_archive_warnings(archive, opt, warnings_reported);
        warnings_reported = archive->nwarnings;
    }

    if (rc < 0) {
        (void)fprintf(stderr, "tmd: %s\n", tmd_reader_error(reader));
        status = TMD_EXIT_ERROR;
    } else {
        tmd_render_archive_end(rd, archive);
        report_archive_warnings(archive, opt, warnings_reported);
        /* A missing end-of-archive marker means the file was cut short, which
         * is a finding whether or not every header that survived is intact. */
        if (opt->check && (archive->bad_checksums > 0 || !archive->eof_marker ||
                           archive->trailing_garbage))
            status = TMD_EXIT_CHECK;
    }

    tmd_reader_free(reader);
    tmd_source_close(src);
    return status;
}

int main(int argc, char **argv)
{
    struct tmd_cli     cli;
    struct tmd_render *rd;
    FILE              *out = stdout;
    int                status;
    size_t             i;

    status = tmd_parse_args(argc, argv, &cli);
    if (status != TMD_EXIT_OK) {
        tmd_free_args(&cli);
        return status;
    }
    if (cli.want_help) {
        tmd_print_usage(stdout);
        tmd_free_args(&cli);
        return TMD_EXIT_OK;
    }
    if (cli.want_version) {
        tmd_print_version(stdout);
        tmd_free_args(&cli);
        return TMD_EXIT_OK;
    }

    if (cli.output_path) {
        out = fopen(cli.output_path, "w");
        if (!out) {
            (void)fprintf(stderr, "tmd: %s: %s\n", cli.output_path, strerror(errno));
            tmd_free_args(&cli);
            return TMD_EXIT_ERROR;
        }
    }

    rd = tmd_render_new(out, &cli.options, (int)cli.nfiles);
    for (i = 0; i < cli.nfiles; i++) {
        int one = dump_archive(cli.files[i], rd, &cli.options);
        /* The worst outcome wins, and one unreadable archive does not stop the
         * others: `tmd -f a.tar -f b.tar` should report on b even when a is
         * missing. */
        if (one > status)
            status = one;
    }
    tmd_render_finish(rd);
    tmd_render_free(rd);

    /*
     * fclose, checked.
     *
     * A full disk or a broken pipe shows up here and nowhere else: every
     * fprintf before it succeeded into the buffer. Exiting 0 after failing to
     * write the report is the kind of lie that makes a backup script report
     * success.
     */
    if (fflush(out) != 0 || (cli.output_path && fclose(out) != 0)) {
        (void)fprintf(stderr, "tmd: %s: %s\n",
                      cli.output_path ? cli.output_path : "(standard output)",
                      strerror(errno));
        status = TMD_EXIT_ERROR;
    }

    tmd_free_args(&cli);
    return status;
}
