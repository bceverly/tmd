/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "source.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

/*
 * What a file turned out to be when it is not a plain tar archive.
 *
 * `argv` is the command that gets a tar stream out of it, or NULL where no
 * command helps — a zip or an RPM is not a tar archive in a wrapper, it is a
 * different format, and telling somebody to pipe it through something would be
 * a wrong answer rather than an unhelpful one.
 *
 * Decompression runs the system's own tool rather than linking a library, for
 * the same three reasons GNU tar does it (tar -z forks gzip): the binary stays
 * linked against nothing but libc, one mechanism covers every format in this
 * table at once, and the code that parses hostile compressed bytes runs in a
 * different process from the code that parses hostile tar headers.
 */
struct wrapper {
    const char *magic;
    size_t      magic_len;
    const char *name;
    const char *argv[4];
};

static const struct wrapper wrappers[] = {
    { "\x1f\x8b",             2, "gzip",        { "gzip", "-dc", NULL } },
    { "BZh",                   3, "bzip2",       { "bzip2", "-dc", NULL } },
    { "\xfd" "7zXZ\x00",       6, "xz",          { "xz", "-dc", NULL } },
    { "\x28\xb5\x2f\xfd",      4, "zstd",        { "zstd", "-dc", NULL } },
    { "\x04\x22\x4d\x18",      4, "lz4",         { "lz4", "-dc", NULL } },
    { "LZIP",                  4, "lzip",        { "lzip", "-dc", NULL } },
    { "\x1f\x9d",             2, "compress",    { "gzip", "-dc", NULL } },
    /* Not compression, but the same mistake: containers routinely confused
     * with tar. There is no command that helps, so the message for these says
     * only what the file is. */
    { "PK\x03\x04",            4, "zip",         { NULL } },
    { "070701",                6, "cpio (newc)", { NULL } },
    { "070707",                6, "cpio (odc)",  { NULL } },
    { "!<arch>\n",             8, "ar",          { NULL } },
    { "\xed\xab\xee\xdb",      4, "RPM",         { NULL } },
    { "SQLite format 3",      14, "SQLite",      { NULL } },
    { "\x7f" "ELF",             4, "ELF",         { NULL } }
};

/* Enough for the longest magic above, rounded up. */
#define PEEK_BYTES 32

const struct wrapper *tmd_wrapper_lookup(const void *bytes, size_t len)
{
    const char *b = bytes;
    size_t      i;

    for (i = 0; i < sizeof(wrappers) / sizeof(wrappers[0]); i++)
    {
        if (len >= wrappers[i].magic_len &&
            memcmp(b, wrappers[i].magic, wrappers[i].magic_len) == 0)
        {
            return &wrappers[i];
        }
    }
    return NULL;
}

const char *tmd_wrapper_name(const struct wrapper *w)
{
    return w ? w->name : NULL;
}

const char *tmd_wrapper_command(const struct wrapper *w)
{
    return (w && w->argv[0]) ? w->argv[0] : NULL;
}

struct tmd_source {
    FILE       *file;        /* NULL for a memory source            */
    bool        owns_file;   /* stdin is borrowed, a fopen'd one is not */
    const char *mem;
    size_t      mem_len;
    size_t      mem_pos;
    uint64_t    offset;
    uint64_t    size;
    char       *name;
    bool        eof;
    bool        error;

    /*
     * Bytes read to identify the file, before anything else consumed them.
     *
     * Read with read(2) rather than fread so that no stdio buffering happens
     * on the descriptor: for a compressed file the descriptor is handed to a
     * feeder process, which must continue from exactly where this stopped.
     */
    char        peek[PEEK_BYTES];
    size_t      peek_len;
    size_t      peek_pos;

    /* The decompressor and the process feeding it, when the file was
     * compressed. -1 when it was not. */
    pid_t       codec_pid;
    pid_t       feeder_pid;
    /* The decompressor's exit status, once the stream has ended. A non-zero
     * one means the bytes tmd read are only as much as could be decompressed
     * before it gave up -- a partial archive, not a whole one. */
    int         codec_status;
    bool        codec_reaped;
    const char *codec;       /* "gzip", "xz", ... for the report */
};

/*
 * Run the decompressor, with a second process feeding it.
 *
 * Three processes, and the middle one is the reason: tmd has already read the
 * first bytes to find out what the file is, and those bytes have to reach the
 * decompressor too. A seekable file could be rewound instead, but standard
 * input cannot, and one path that always works beats two that each work
 * sometimes. The feeder writes the bytes already read, then copies the rest.
 *
 *   feeder --> [decompressor] --> tmd
 *
 * Deadlock is not possible: the feeder only writes and tmd only reads, so
 * neither waits on the other. If tmd stops reading early the decompressor gets
 * SIGPIPE and dies, and the feeder follows it for the same reason, which is
 * what should happen.
 */
static bool spawn_codec(struct tmd_source *s, const struct wrapper *w,
                        char **err)
{
    int   to_codec[2];
    int   from_codec[2];
    int   source_fd = fileno(s->file);
    pid_t codec;
    pid_t feeder;

    if (pipe(to_codec) != 0)
    {
        return false;
    }
    if (pipe(from_codec) != 0)
    {
        (void)close(to_codec[0]);
        (void)close(to_codec[1]);
        return false;
    }

    codec = fork();
    if (codec < 0)
    {
        (void)close(to_codec[0]);
        (void)close(to_codec[1]);
        (void)close(from_codec[0]);
        (void)close(from_codec[1]);
        return false;
    }
    if (codec == 0)
    {
        (void)dup2(to_codec[0], STDIN_FILENO);
        (void)dup2(from_codec[1], STDOUT_FILENO);
        (void)close(to_codec[0]);
        (void)close(to_codec[1]);
        (void)close(from_codec[0]);
        (void)close(from_codec[1]);
        /*
         * execvp, so the tool is found on PATH the way every other program
         * finds it. Not a shell: there is no command line to quote, and no
         * archive name reaches this, so there is nothing for a crafted
         * filename to do.
         *
         * The union is not cleverness for its own sake. execvp takes
         * `char *const argv[]` while this table is `const char *[]`, which is a
         * historical wart in POSIX -- execvp does not modify argv -- and the
         * plain cast to fix it is exactly what -Wcast-qual is on to catch.
         * Copying the strings would mean allocating between fork and exec,
         * which is worse. This reinterprets a pointer the callee will not
         * write through, and says so.
         */
        {
            union {
                const char *const *readonly;
                char *const       *writable;
            } args;

            args.readonly = w->argv;
            /*
             * flawfinder flags every exec, and it is right to.
             *
             * What makes this one safe is what reaches it: argv is the
             * compile-time table at the top of this file and nothing else.
             * No filename, no archive content, no environment variable and no
             * user string is passed, so there is nothing for a crafted input
             * to influence. It is execvp rather
             * than system(), so no shell parses anything and metacharacters
             * have no meaning.
             *
             * PATH decides which "gzip" runs, which is the same trust every
             * program that runs another program places in its environment --
             * and an attacker who can set PATH for this process can already run
             * whatever they like as this user. Hardcoding /bin/gzip would trade
             * that for breaking on every system that puts it somewhere else.
             */
            (void)execvp(w->argv[0], args.writable); /* Flawfinder: ignore */
        }
        /* 127 is what a shell reports for "command not found", and reading it
         * back is how the caller tells a missing tool from a corrupt file. */
        _exit(127);
    }

    feeder = fork();
    if (feeder < 0)
    {
        (void)close(to_codec[0]);
        (void)close(to_codec[1]);
        (void)close(from_codec[0]);
        (void)close(from_codec[1]);
        (void)waitpid(codec, NULL, 0);
        return false;
    }
    if (feeder == 0)
    {
        (void)close(to_codec[0]);
        (void)close(from_codec[0]);
        (void)close(from_codec[1]);
        /* The bytes already read to identify the file, then the rest of it. */
        _exit(tmd_copy_fd(source_fd, to_codec[1], s->peek, s->peek_len) ? 0 : 1);
    }

    (void)close(to_codec[0]);
    (void)close(to_codec[1]);
    (void)close(from_codec[1]);

    /* The original file belongs to the feeder now. */
    if (s->owns_file)
    {
        (void)fclose(s->file);
    }
    s->file = fdopen(from_codec[0], "rb");
    if (!s->file)
    {
        (void)close(from_codec[0]);
        (void)waitpid(codec, NULL, 0);
        (void)waitpid(feeder, NULL, 0);
        return false;
    }
    s->owns_file = true;
    s->codec_pid = codec;
    s->feeder_pid = feeder;
    s->codec = w->name;
    /* The length of the compressed file says nothing about the length of what
     * comes out of it, and a wrong answer is worse than none. */
    s->size = 0;
    s->peek_len = 0;
    s->peek_pos = 0;

    /*
     * Read the first bytes of the decompressed stream now, so that a missing
     * tool is reported as a missing tool.
     *
     * Without this, execvp failing looks exactly like an empty file: the reader
     * would say "empty file, not a tar archive" about an archive that is
     * perfectly good and a decompressor that is not installed.
     */
    s->peek_len = fread(s->peek, 1, sizeof(s->peek), s->file);
    if (s->peek_len == 0)
    {
        int status = 0;

        /*
         * Nothing came out, so the decompressor is already finished and its
         * status is available now rather than at end of stream.
         *
         * Recorded, not merely inspected. This used to read the status into a
         * local, test it for 127, and drop it -- which meant a decompressor
         * that failed before producing a byte left the source looking like a
         * clean empty stream, and tmd reported a corrupt archive as "not a tar
         * archive" with nothing to say why. Everything the ordinary path
         * learns at end of stream has to be learned here too, because for this
         * stream this IS the end.
         *
         * Which of the two paths a given input takes belongs to the
         * decompressor: GNU gzip flushes what it managed before failing, so a
         * half-written stream goes the ordinary way, while Apple's buffers and
         * emits nothing, so the same bytes come here instead.
         */
        (void)waitpid(codec, &status, 0);
        s->codec_pid = -1;
        s->codec_status = status;
        s->codec_reaped = true;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        {
            if (err)
            {
                *err = tmd_xasprintf("%s: %s-compressed, and %s is not "
                                     "installed", s->name, w->name, w->argv[0]);
            }
            return false;
        }
    }
    return true;
}

struct tmd_source *tmd_source_open(const char *path, char **err)
{
    struct tmd_source *s;
    FILE              *file;
    bool               owns = true;
    uint64_t           size = 0;

    if (strcmp(path, "-") == 0)
    {
        file = stdin;
        owns = false;
    } else
    {
        file = fopen(path, "rb");
        if (!file)
        {
            if (err)
            {
                *err = tmd_xasprintf("%s: %s", path, strerror(errno));
            }
            return NULL;
        }
    }

    if (owns)
    {
        /* The length is worth having for the summary line and for deciding
         * whether trailing bytes after the end-of-archive marker are padding
         * or something else. stat, not fseek/ftell: it costs one syscall and
         * works the same on a file opened for reading on any platform. */
        struct stat st;
        if (fstat(fileno(file), &st) == 0 && S_ISREG(st.st_mode))
        {
            size = (uint64_t)st.st_size;
        }
    }

    s = tmd_xcalloc(1, sizeof(*s));
    s->file = file;
    s->owns_file = owns;
    s->size = size;
    s->name = tmd_xstrdup(owns ? path : "(stdin)");
    s->codec_pid = -1;
    s->feeder_pid = -1;

    /*
     * Look at the first bytes before anything else does.
     *
     * read(2) rather than fread: for a compressed file the descriptor is handed
     * to a feeder process, and stdio buffering would have swallowed bytes that
     * process needs. Nothing has touched the descriptor yet, so this is the
     * only place the two can be mixed safely.
     */
    {
        ssize_t n = read(fileno(file), s->peek, sizeof(s->peek));

        if (n > 0)
        {
            s->peek_len = (size_t)n;
        }
    }

    {
        const struct wrapper *w = tmd_wrapper_lookup(s->peek, s->peek_len);

        if (w && w->argv[0])
        {
            char *why = NULL;

            if (!spawn_codec(s, w, &why))
            {
                if (err)
                {
                    *err = why ? why
                               : tmd_xasprintf("%s: could not run %s", path,
                                               w->argv[0]);
                } else
                {
                    free(why);
                }
                tmd_source_close(s);
                return NULL;
            }
        }
    }
    return s;
}

struct tmd_source *tmd_source_open_memory(const void *data, size_t len,
                                          const char *name)
{
    struct tmd_source *s = tmd_xcalloc(1, sizeof(*s));

    s->mem = (const char *)data;
    s->mem_len = len;
    s->size = len;
    s->name = tmd_xstrdup(name ? name : "(memory)");
    return s;
}

void tmd_source_close(struct tmd_source *s)
{
    if (!s)
    {
        return;
    }
    if (s->file && s->owns_file)
    {
        (void)fclose(s->file);
    }
    /*
     * Reap both helpers.
     *
     * The file is closed first, on purpose: that is what ends the decompressor
     * if tmd stopped reading early. Closing our end gives it SIGPIPE, it exits,
     * the feeder writing into it gets SIGPIPE in turn, and both waits below
     * return immediately. Waiting first would hang on exactly the case that
     * matters -- an archive tmd gave up on part way through.
     */
    if (s->codec_pid > 0)
    {
        (void)waitpid(s->codec_pid, NULL, 0);
    }
    if (s->feeder_pid > 0)
    {
        (void)waitpid(s->feeder_pid, NULL, 0);
    }
    free(s->name);
    free(s);
}

const char *tmd_source_codec(const struct tmd_source *s)
{
    return s->codec;
}

bool tmd_source_codec_failed(const struct tmd_source *s)
{
    /*
     * Copied out of the struct before being asked about, because W* are macros
     * and Darwin's spell themselves `(*(int *)&(w))` -- a cast through a
     * pointer, to convert the historical `union wait`. Handed a const lvalue
     * that casts away const, which -Wcast-qual rejects and rightly so: the
     * warning cannot tell a header's own cast from ours. glibc's version does
     * not do this, so it only ever appeared on macOS. A local int is an lvalue
     * the macro can take the address of and is not const, which costs one
     * register and settles it everywhere.
     */
    int status = s->codec_status;

    if (!s->codec_reaped)
    {
        return false; /* the stream never ended; the reader reports its own */
    }
    return !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}

size_t tmd_source_read(struct tmd_source *s, void *buf, size_t n)
{
    size_t got;
    size_t served = 0;

    if (n == 0)
    {
        return 0;
    }

    /*
     * Whatever was read to identify the file comes first; the descriptor is
     * positioned just past it.
     *
     * Served by adjusting the request and falling through rather than by
     * calling this function again: the recursion was only ever one level deep,
     * but "this function calls itself" is a thing a reader has to check the
     * depth of, and clang-tidy flags it for the same reason.
     */
    if (s->peek_pos < s->peek_len)
    {
        size_t have = s->peek_len - s->peek_pos;

        served = have < n ? have : n;
        memcpy(buf, s->peek + s->peek_pos, served);
        s->peek_pos += served;
        s->offset += served;
        if (served == n)
        {
            return served;
        }
        buf = (char *)buf + served;
        n -= served;
    }

    if (s->file)
    {
        got = fread(buf, 1, n, s->file);
        /*
         * A short read is the end of the stream, which for a decompressed
         * source is the moment its exit status becomes knowable -- and the
         * moment it matters. gzip exits non-zero on truncated input, and
         * without this tmd printed the members it had managed to decompress
         * and exited 0: a partial answer presented as a complete one.
         */
        if (got < n && s->codec_pid > 0 && !s->codec_reaped)
        {
            (void)waitpid(s->codec_pid, &s->codec_status, 0);
            s->codec_pid = -1;
            s->codec_reaped = true;
        }
        if (got < n)
        {
            if (ferror(s->file))
            {
                s->error = true;
            }
            s->eof = true;
        }
    } else
    {
        /* Spelled as an if rather than `got = n < left ? n : left` followed by
         * a separate `got < n` test. The two together say the same thing less
         * directly: a reader has to re-derive that a short read is exactly the
         * case where the buffer ran out, and a static analyzer following the
         * ternary concludes the second test can never fire. */
        size_t left = s->mem_len - s->mem_pos;

        if (n > left)
        {
            got = left;
            s->eof = true;
        } else
        {
            got = n;
        }
        memcpy(buf, s->mem + s->mem_pos, got);
        s->mem_pos += got;
    }

    s->offset += got;
    return served + got;
}

bool tmd_source_skip(struct tmd_source *s, uint64_t n)
{
    char   scratch[8192];
    bool   complete = true;

    if (n == 0)
    {
        return true;
    }

    /*
     * The peeked bytes first.
     *
     * They are already in memory and the descriptor is positioned past them,
     * so seeking on the descriptor would skip from the wrong place: a read of
     * 3 followed by a skip of 20 landed at 52 instead of 23, which is exactly
     * what the source test caught when the peek was added.
     */
    if (s->peek_pos < s->peek_len)
    {
        uint64_t have = (uint64_t)(s->peek_len - s->peek_pos);
        uint64_t take = have < n ? have : n;

        s->peek_pos += (size_t)take;
        s->offset += take;
        n -= take;
        if (n == 0)
        {
            return true;
        }
    }

    if (s->file)
    {
        /*
         * Seek when we can, read when we cannot.
         *
         * fseeko on a regular file is O(1) and is what makes `tmd` on a 40 GB
         * archive as fast as reading its headers. On a pipe it fails, and the
         * only way past a member is through it. The seek is deliberately NOT
         * trusted to have landed inside the file: seeking past the end of a
         * regular file succeeds, so the end has to be detected by the read
         * that follows rather than here.
         */
        if (s->size > 0)
        {
            if (s->offset + n > s->size)
            {
                /* Consume what is actually there, then report the shortfall,
                 * rather than seeking into nothing and reporting success. */
                uint64_t available = s->size - s->offset;
                if (fseeko(s->file, (off_t)s->size, SEEK_SET) == 0)
                {
                    s->offset = s->size;
                    s->eof = true;
                    return false;
                }
                n = available;
                complete = false;
            } else if (fseeko(s->file, (off_t)n, SEEK_CUR) == 0)
            {
                s->offset += n;
                return true;
            }
        }
    }

    while (n > 0)
    {
        size_t want = n < sizeof(scratch) ? (size_t)n : sizeof(scratch);
        size_t got = tmd_source_read(s, scratch, want);
        if (got == 0)
        {
            return false;
        }
        n -= got;
    }
    return complete;
}

uint64_t tmd_source_offset(const struct tmd_source *s) { return s->offset; }
uint64_t tmd_source_size(const struct tmd_source *s) { return s->size; }
const char *tmd_source_name(const struct tmd_source *s) { return s->name; }
bool tmd_source_at_eof(const struct tmd_source *s) { return s->eof; }
bool tmd_source_error(const struct tmd_source *s) { return s->error; }
