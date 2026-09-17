/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "opts.h"

#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

/* Codes for the options that have no short letter. Above 255 so they cannot
 * collide with a character. */
enum {
    OPT_FORMAT = 1000,
    OPT_COLOR
};

/* --- the three expansions of the option table --------------------------- */

#define TMD_OPT(code, name, arg, argname, help) { name, arg, NULL, code },
static const struct option long_options[] = {
#include "options.def"
    { NULL, 0, NULL, 0 }
};
#undef TMD_OPT

/*
 * Pointers first, then the ints.
 *
 * Declaration order is the layout: with `int code` leading, the compiler
 * inserts four bytes of padding before each pointer that follows it, and the
 * clang analyzer is right to call that out. The X-macro expansion below is
 * reordered to match, so options.def still reads in the order a person would
 * write an option — code, name, argument, argument name, help — while the
 * struct is packed the way the machine wants it.
 */
struct opt_doc {
    const char *name;
    const char *argname;
    const char *help;
    int         code;
    int         arg;
};

#define TMD_OPT(code, name, arg, argname, help) { name, argname, help, code, arg },
static const struct opt_doc opt_docs[] = {
#include "options.def"
};
#undef TMD_OPT

#define N_OPTS (sizeof(opt_docs) / sizeof(opt_docs[0]))

/*
 * The short option string, built at startup from the same table.
 *
 * A leading '+' stops GNU getopt permuting argv, so `tmd -f a.tar extra` is
 * reported as the mistake it is rather than silently working. A leading ':'
 * would change how a missing argument is reported; the default message is
 * already clear, so it is left alone.
 */
static const char *short_options(void)
{
    static char buf[3 * N_OPTS + 2];
    size_t      i;
    size_t      n = 0;

    if (buf[0] != '\0')
        return buf;

    buf[n++] = '+';
    for (i = 0; i < N_OPTS; i++) {
        if (opt_docs[i].code <= 0 || opt_docs[i].code > 255)
            continue;
        buf[n++] = (char)opt_docs[i].code;
        if (opt_docs[i].arg == required_argument)
            buf[n++] = ':';
        else if (opt_docs[i].arg == optional_argument) {
            buf[n++] = ':';
            buf[n++] = ':';
        }
    }
    buf[n] = '\0';
    return buf;
}

/* ------------------------------------------------------------------------- */
/* Help                                                                      */
/* ------------------------------------------------------------------------- */

void tmd_print_version(FILE *out)
{
    (void)fprintf(out, "tmd %s\n", TMD_VERSION);
    (void)fprintf(out, "%s\n", TMD_COPYRIGHT);
    (void)fprintf(out, "Licensed under the BSD 2-Clause License. "
                       "There is NO WARRANTY, to the extent permitted by law.\n");
}

/*
 * The help text.
 *
 * This is the master copy of the documentation: scripts/gen-man.sh parses it
 * to produce man/tmd.1, and `make man-check` fails the build when the two
 * disagree. The layout below is therefore load-bearing — the section headings
 * ending in ':' and the two-space indent on option lines are what the
 * generator keys on.
 */
void tmd_print_usage(FILE *out)
{
    size_t i;

    (void)fprintf(out, "tmd %s — dump the metadata out of a tar archive\n", TMD_VERSION);
    (void)fprintf(out, "%s\n\n", TMD_COPYRIGHT);
    (void)fprintf(out, "Usage: tmd -f FILE [OPTION]...\n");
    (void)fprintf(out, "       tmd [OPTION]... < FILE      (or: ... | tmd)\n\n");
    (void)fprintf(out,
        "Reads a tar archive and reports what is inside it without extracting\n"
        "anything: one ls -l style line per member by default, or every header\n"
        "field in long, JSON or CSV form. BSD archives (ustar and pax, as bsdtar\n"
        "and libarchive write them), GNU tar archives including long names and\n"
        "sparse files, and pre-POSIX v7 archives are all understood. The archive\n"
        "is only ever opened for reading.\n\n");

    (void)fprintf(out, "Options:\n");
    for (i = 0; i < N_OPTS; i++) {
        const struct opt_doc *o = &opt_docs[i];
        struct tmd_buf        left;

        /*
         * Built with the growable buffer rather than by advancing an offset
         * into a fixed array on snprintf's return value.
         *
         * That idiom — `n += snprintf(buf + n, sizeof(buf) - n, ...)` — is a
         * buffer overflow waiting for a long enough option name. snprintf
         * returns the length it WOULD have written, so one truncation puts `n`
         * past the end of the array: `buf + n` then points outside it, and
         * `sizeof(buf) - n` underflows to a size_t near 2^64, which tells the
         * next call it has exabytes of room. It was safe here only because the
         * first write happens to be exactly four characters, which is a
         * property of the data rather than of the code.
         */
        tmd_buf_init(&left);
        if (o->code > 0 && o->code <= 255)
            tmd_buf_addf(&left, "-%c, ", o->code);
        else
            tmd_buf_addstr(&left, "    ");

        if (o->arg == required_argument)
            tmd_buf_addf(&left, "--%s=%s", o->name, o->argname);
        else if (o->arg == optional_argument)
            tmd_buf_addf(&left, "--%s[=%s]", o->name, o->argname);
        else
            tmd_buf_addf(&left, "--%s", o->name);

        (void)fprintf(out, "  %-24s %s\n", left.data, o->help);
        tmd_buf_free(&left);
    }

    (void)fprintf(out, "\nExit status:\n");
    (void)fprintf(out, "  %-24s %s\n", "0", "the archive was read");
    (void)fprintf(out, "  %-24s %s\n", "1", "the archive could not be read");
    (void)fprintf(out, "  %-24s %s\n", "2", "the command line was wrong");
    (void)fprintf(out, "  %-24s %s\n", "3",
                  "--check found damage: a bad checksum or a truncated archive");

    (void)fprintf(out, "\nExamples:\n");
    (void)fprintf(out, "  %-38s %s\n", "tmd -f archive.tar",
                  "list every member, ls -l style");
    (void)fprintf(out, "  %-38s %s\n", "tmd -f archive.tar -l",
                  "every field of every member");
    (void)fprintf(out, "  %-38s %s\n", "tmd -f archive.tar -i",
                  "what the archive is: format, extensions, integrity");
    (void)fprintf(out, "  %-38s %s\n", "tmd -f archive.tar -s",
                  "just the summary: format, counts, sizes");
    (void)fprintf(out, "  %-38s %s\n", "tmd -f archive.tar -o report.txt",
                  "write the report to a file");
    (void)fprintf(out, "  %-38s %s\n", "tmd -f archive.tar --format=json",
                  "machine-readable output for a script");
    (void)fprintf(out, "  %-38s %s\n", "gzip -dc a.tar.gz | tmd",
                  "read a compressed archive through a pipe");

    (void)fprintf(out, "\nReport bugs at https://github.com/bceverly/tmd\n");
}

/* ------------------------------------------------------------------------- */
/* Parsing                                                                   */
/* ------------------------------------------------------------------------- */

static int bad_usage(const char *fmt, ...) TMD_PRINTF(1, 2);
static int bad_usage(const char *fmt, ...)
{
    va_list ap;

    (void)fprintf(stderr, "tmd: ");
    va_start(ap, fmt);
    /* The format is never attacker-controlled: bad_usage is declared
     * TMD_PRINTF(1, 2), so the compiler checks every call site, and
     * -Wformat-nonliteral makes a non-literal format a build failure. */
    (void)vfprintf(stderr, fmt, ap); /* Flawfinder: ignore */
    va_end(ap);
    (void)fprintf(stderr, "\nTry 'tmd --help' for the full list of options.\n");
    return TMD_EXIT_USAGE;
}

int tmd_parse_args(int argc, char **argv, struct tmd_cli *cli)
{
    const char *color_when = "auto";
    int         c;

    memset(cli, 0, sizeof(*cli));

    /*
     * A bare `tmd` AT A TERMINAL prints the help and succeeds.
     *
     * The alternative — "missing required option -f", exit 2 — is what a strict
     * reading of the option table gives, and it is the wrong answer for the one
     * case where somebody has typed the name of a tool to find out what it
     * does. Every other missing or wrong argument is still an error.
     *
     * "At a terminal" is what makes this compatible with reading a pipe. When
     * standard input is not a tty somebody has redirected something into it and
     * means for it to be read — printing the help at them instead would be
     * useless, and worse, it would exit 0 having done nothing, so a pipeline
     * would look as though it had worked.
     */
    if (argc <= 1 && isatty(STDIN_FILENO)) {
        cli->want_help = true;
        return TMD_EXIT_OK;
    }

    /* Room for one -f per argument, plus the implicit stdin below. */
    cli->files = tmd_xcalloc((size_t)argc + 1, sizeof(*cli->files));

    opterr = 1;
    /* Flawfinder objects to getopt_long on the grounds that "some older
     * implementations" did not bound their internal buffers. glibc's does, and
     * the alternative is parsing argv by hand, which is how that class of bug
     * actually gets written. */
    while ((c = getopt_long(argc, argv, short_options(), /* Flawfinder: ignore */
                            long_options, NULL)) != -1) {
        switch (c) {
        case 'f':
            cli->files[cli->nfiles++] = optarg;
            break;
        case 'o':
            cli->output_path = optarg;
            break;
        case 'l':
            cli->options.long_form = true;
            break;
        case 'R':
            cli->options.headers = true;
            cli->options.long_form = true;
            break;
        case 'i':
            cli->options.info = true;
            break;
        case 's':
            cli->options.summary_only = true;
            break;
        case 'S':
            cli->options.with_summary = true;
            break;
        case 'n':
            cli->options.numeric = true;
            break;
        case 'H':
            cli->options.human = true;
            break;
        case 'u':
            cli->options.utc = true;
            break;
        case 'T':
            cli->options.full_time = true;
            break;
        case 'c':
            cli->options.check = true;
            break;
        case 'q':
            cli->options.quiet = true;
            break;
        case OPT_FORMAT:
            if (strcmp(optarg, "text") == 0)
                cli->options.output = TMD_OUT_TEXT;
            else if (strcmp(optarg, "json") == 0)
                cli->options.output = TMD_OUT_JSON;
            else if (strcmp(optarg, "csv") == 0)
                cli->options.output = TMD_OUT_CSV;
            else
                return bad_usage("unknown output format \"%s\" — expected text, json or csv",
                                 optarg);
            break;
        case OPT_COLOR:
            color_when = optarg ? optarg : "always";
            if (strcmp(color_when, "auto") != 0 &&
                strcmp(color_when, "always") != 0 &&
                strcmp(color_when, "never") != 0)
                return bad_usage("unknown --color value \"%s\" — expected auto, always or never",
                                 color_when);
            break;
        case 'h':
            cli->want_help = true;
            return TMD_EXIT_OK;
        case 'V':
            cli->want_version = true;
            return TMD_EXIT_OK;
        default:
            /* getopt_long has already said what was wrong. */
            return TMD_EXIT_USAGE;
        }
    }

    if (optind < argc)
        return bad_usage("unexpected argument \"%s\" — the archive is named with -f "
                         "(did you mean: tmd -f %s?)",
                         argv[optind], argv[optind]);

    if (cli->nfiles == 0) {
        /*
         * No -f, so read standard input — unless it is a terminal, in which
         * case there is nothing there and waiting for a tar archive to be typed
         * in is not a helpful way to spend the afternoon.
         *
         * This is what makes `gzip -dc a.tar.gz | tmd` and `tmd < a.tar` work.
         * `-f -` still means the same thing and is worth keeping for scripts
         * that would rather be explicit.
         */
        if (isatty(STDIN_FILENO))
            return bad_usage("no archive given — use -f FILE, or pipe one in "
                             "(gzip -dc a.tar.gz | tmd)");
        cli->files[cli->nfiles++] = "-";
    }

    /*
     * Color is decided here rather than at the point of printing, because the
     * decision needs the destination: with -o the output is a file, and a file
     * full of escape sequences is a file nobody can grep.
     */
    if (strcmp(color_when, "always") == 0)
        cli->options.color = true;
    else if (strcmp(color_when, "never") == 0)
        cli->options.color = false;
    else
        cli->options.color = !cli->output_path && isatty(STDOUT_FILENO) &&
                             /* Only tested for presence: the value is never
                              * read, copied or parsed, so its length and
                              * contents cannot matter. */
                             getenv("NO_COLOR") == NULL; /* Flawfinder: ignore */

    /* JSON and CSV are for machines; an escape sequence in the middle of a
     * string is not something a parser is expected to cope with. */
    if (cli->options.output != TMD_OUT_TEXT)
        cli->options.color = false;

    return TMD_EXIT_OK;
}

void tmd_free_args(struct tmd_cli *cli)
{
    free(cli->files);
    cli->files = NULL;
    cli->nfiles = 0;
}
