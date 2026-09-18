/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "test.h"
#include <fcntl.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "source.h"
#include "util.h"

static void test_memory_source(void)
{
    struct tmd_source *s;
    char               buf[16];

    TEST_CASE("a memory source reads, skips and tracks its offset");
    s = tmd_source_open_memory("0123456789abcdef", 16, "(test)");
    CHECK_STR(tmd_source_name(s), "(test)");
    CHECK_INT(tmd_source_size(s), 16);
    CHECK_INT(tmd_source_offset(s), 0);

    CHECK_INT(tmd_source_read(s, buf, 4), 4);
    CHECK(memcmp(buf, "0123", 4) == 0);
    CHECK_INT(tmd_source_offset(s), 4);

    CHECK(tmd_source_skip(s, 6));
    CHECK_INT(tmd_source_offset(s), 10);

    CHECK_INT(tmd_source_read(s, buf, 6), 6);
    CHECK(memcmp(buf, "abcdef", 6) == 0);
    CHECK(tmd_source_at_eof(s) == false);

    TEST_CASE("reading past the end is a short count, not an error");
    CHECK_INT(tmd_source_read(s, buf, 4), 0);
    CHECK(tmd_source_at_eof(s));
    CHECK(!tmd_source_error(s));
    tmd_source_close(s);

    TEST_CASE("skipping past the end reports that it could not");
    s = tmd_source_open_memory("short", 5, NULL);
    CHECK(!tmd_source_skip(s, 100));
    CHECK_STR(tmd_source_name(s), "(memory)");
    tmd_source_close(s);

    TEST_CASE("a zero-length read and skip are no-ops");
    s = tmd_source_open_memory("x", 1, NULL);
    CHECK_INT(tmd_source_read(s, buf, 0), 0);
    CHECK(tmd_source_skip(s, 0));
    CHECK_INT(tmd_source_offset(s), 0);
    tmd_source_close(s);

    TEST_CASE("closing a NULL source is allowed");
    tmd_source_close(NULL);
}

static void test_file_source(void)
{
    struct tmd_source *s;
    char              *err = NULL;
    char               path[] = "/tmp/tmd-test-sourceXXXXXX";
    int                fd;
    FILE              *f;

    TEST_CASE("a file that does not exist gives a message naming it");
    s = tmd_source_open("/nonexistent/tmd/archive.tar", &err);
    CHECK(s == NULL);
    CHECK_CONTAINS(err, "/nonexistent/tmd/archive.tar");
    free(err);
    err = NULL;

    /* An unreadable path with no error pointer must still not crash. */
    CHECK(tmd_source_open("/nonexistent/tmd/archive.tar", NULL) == NULL);

    TEST_CASE("a real file reads, seeks past data and reports its size");
    fd = mkstemp(path);
    CHECK(fd >= 0);
    f = fdopen(fd, "wb");
    CHECK(f != NULL);
    if (f)
    {
        size_t i;
        for (i = 0; i < 100; i++)
        {
            (void)fputc((int)('a' + (i % 26)), f);
        }
        (void)fclose(f);
    }

    s = tmd_source_open(path, &err);
    CHECK(s != NULL);
    if (s)
    {
        char buf[32];

        CHECK_INT(tmd_source_size(s), 100);
        CHECK_INT(tmd_source_read(s, buf, 3), 3);
        CHECK(memcmp(buf, "abc", 3) == 0);
        /* The seek path, which is what makes a large archive fast. */
        CHECK(tmd_source_skip(s, 20));
        CHECK_INT(tmd_source_offset(s), 23);
        CHECK_INT(tmd_source_read(s, buf, 1), 1);
        CHECK_INT(buf[0], 'a' + (23 % 26));
        /* ...and the path where the skip runs off the end. */
        CHECK(!tmd_source_skip(s, 1000));
        tmd_source_close(s);
    }
    (void)remove(path);
}

/*
 * A gzip stream of "hello, decompressed world\n", 46 bytes.
 *
 * Embedded rather than produced by running gzip, so that creating the fixture
 * depends on nothing. Reading it still needs the gzip program, which is what
 * tmd runs -- the tests below skip when it is absent rather than fail.
 */
static const unsigned char GZ_HELLO[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xcb, 0x48,
    0xcd, 0xc9, 0xc9, 0xd7, 0x51, 0x48, 0x49, 0x4d, 0xce, 0xcf, 0x2d, 0x28,
    0x4a, 0x2d, 0x2e, 0x4e, 0x4d, 0x51, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0xe1,
    0x02, 0x00, 0xd2, 0xdc, 0xeb, 0x17, 0x1a, 0x00, 0x00, 0x00,
};
#define GZ_PLAIN "hello, decompressed world\n"

static bool have_gzip(void)
{
    /* Asked the same way tmd asks: by whether execvp would find it. */
    return system("command -v gzip > /dev/null 2>&1") == 0;
}

static char *write_temp(const void *data, size_t len)
{
    char  path[] = "/tmp/tmd-src-XXXXXX";
    int   fd = mkstemp(path);
    FILE *f;

    if (fd < 0)
    {
        return NULL;
    }
    f = fdopen(fd, "wb");
    if (!f)
    {
        (void)close(fd);
        return NULL;
    }
    (void)fwrite(data, 1, len, f);
    (void)fclose(f);
    return tmd_xstrdup(path);
}

static void test_compressed_source(void)
{
    char *path;
    char *err = NULL;

    if (!have_gzip())
    {
        return; /* nothing to decompress with */
    }

    path = write_temp(GZ_HELLO, sizeof(GZ_HELLO));
    CHECK(path != NULL);
    if (!path)
    {
        return;
    }

    TEST_CASE("a gzip file is decompressed on the way in");
    {
        struct tmd_source *s = tmd_source_open(path, &err);

        CHECK(s != NULL);
        if (s)
        {
            char buf[64];
            size_t n = tmd_source_read(s, buf, sizeof(buf));

            CHECK_INT(n, (int)strlen(GZ_PLAIN));
            CHECK(memcmp(buf, GZ_PLAIN, strlen(GZ_PLAIN)) == 0);
            CHECK_STR(tmd_source_codec(s), "gzip");
            /* A stream that ended normally did not fail. */
            CHECK(!tmd_source_codec_failed(s));
            tmd_source_close(s);
        }
    }

    /*
     * Closing before the stream ends is the case the close path is written for:
     * the decompressor is still running, and closing our end is what stops it.
     * Waiting on it first would hang here.
     */
    TEST_CASE("closing early does not hang");
    {
        struct tmd_source *s = tmd_source_open(path, &err);

        CHECK(s != NULL);
        if (s)
        {
            char buf[4];

            (void)tmd_source_read(s, buf, sizeof(buf));
            tmd_source_close(s);
        }
    }

    /*
     * The decompressor missing has to be reported as the decompressor missing.
     * Without the check for it, execvp failing looks exactly like an empty
     * file, and a perfectly good archive is reported as "not a tar archive".
     *
     * Produced by emptying PATH, which has one consequence worth naming: under
     * valgrind the child is itself run by valgrind, and valgrind needs PATH to
     * start it, so the open does NOT fail the way it does normally. The case is
     * therefore asserted only when the environment actually produced it, and
     * whatever comes back is closed either way -- the first version of this
     * asserted unconditionally and leaked the source it did not expect, which
     * is how the difference was found.
     */
    TEST_CASE("a decompressor that cannot be found is named");
    {
        char              *saved = getenv("PATH")
                                       ? tmd_xstrdup(getenv("PATH")) : NULL;
        struct tmd_source *s;

        (void)setenv("PATH", "/nonexistent-for-this-test", 1);
        err = NULL;
        s = tmd_source_open(path, &err);
        if (s)
        {
            /* The environment would not produce the failure; nothing to assert
             * beyond leaving it as we found it. */
            tmd_source_close(s);
        }
        else
        {
            CHECK(err != NULL);
            if (err)
            {
                CHECK_CONTAINS(err, "gzip");
                CHECK_CONTAINS(err, "not installed");
            }

            /*
             * The same failure, with a caller that does not want the message.
             * The message is still built internally, so this is where it would
             * be leaked -- which the leak checkers only notice if something
             * asks.
             */
            s = tmd_source_open(path, NULL);
            CHECK(s == NULL);
            if (s)
            {
                tmd_source_close(s);
            }
        }
        free(err);
        err = NULL;

        if (saved)
        {
            (void)setenv("PATH", saved, 1);
            free(saved);
        }
        else
        {
            (void)unsetenv("PATH");
        }
    }

    (void)remove(path);
    free(path);
}

/*
 * Does this machine's gzip actually treat a truncated stream as a failure?
 *
 * Asked rather than assumed, because the answer is not the same everywhere and
 * tmd's claim depends on it. tmd does not judge a compressed stream itself --
 * it runs the system's decompressor and reports what that decompressor did. So
 * "a truncated stream is reported as a failed decompression" is only a property
 * of tmd where the decompressor calls it a failure, and asserting it on a
 * platform whose gzip exits 0 would be testing the platform, not the program.
 *
 * GNU gzip exits 2 on "unexpected end of file". Apple's does not agree.
 */
static bool gzip_rejects_truncation(const char *path)
{
    char cmd[128];

    (void)snprintf(cmd, sizeof(cmd), "gzip -dc '%s' > /dev/null 2>&1", path);
    return system(cmd) != 0;
}

/*
 * Truncated compressed data: the decompressor gives up part way, and what came
 * out is a prefix. Saying so is the difference between a report and a guess.
 */
static void test_truncated_compressed_source(void)
{
    char *path;
    char *err = NULL;

    if (!have_gzip())
    {
        return;
    }
    /* Half a gzip stream: enough of a header to start, not enough to finish. */
    path = write_temp(GZ_HELLO, sizeof(GZ_HELLO) / 2);
    CHECK(path != NULL);
    if (!path)
    {
        return;
    }

    TEST_CASE("a truncated stream is reported as a failed decompression");
    {
        struct tmd_source *s;
        bool               rejects = gzip_rejects_truncation(path);
        /*
         * gzip says "unexpected end of file" on its own stderr, which is
         * exactly right for a user and only noise in a test log. Silenced for
         * the length of this case and put back afterwards.
         */
        int saved_err = dup(STDERR_FILENO);
        int devnull = open("/dev/null", O_WRONLY);

        if (devnull >= 0)
        {
            (void)dup2(devnull, STDERR_FILENO);
        }

        s = tmd_source_open(path, &err);

        if (s)
        {
            char buf[64];

            /* Read to the end, which is where the exit status becomes known. */
            while (tmd_source_read(s, buf, sizeof(buf)) > 0)
            {
                /* draining; the point is to reach the end */
            }
            /*
             * Only where the decompressor itself called this a failure. Where
             * it did not, the path has still been exercised -- opened, spawned,
             * drained and reaped -- and the one thing not asserted is the one
             * thing this machine does not provide.
             */
            if (rejects)
            {
                CHECK(tmd_source_codec_failed(s));
            }
            tmd_source_close(s);
        }
        else
        {
            /* gzip can also fail before producing a byte, which open reports. */
            CHECK(err != NULL);
            free(err);
        }

        if (saved_err >= 0)
        {
            (void)dup2(saved_err, STDERR_FILENO);
            (void)close(saved_err);
        }
        if (devnull >= 0)
        {
            (void)close(devnull);
        }
    }
    (void)remove(path);
    free(path);
}

/*
 * A decompressor that fails before producing a single byte.
 *
 * The same failure as the truncated stream above, reached by the other path
 * through the code -- and that path used to lose it. tmd reads the first bytes
 * of the decompressed stream while opening, so that a decompressor which is
 * not installed can be named rather than looking like an empty file; when that
 * read came back empty, the exit status was examined for exactly one value
 * (127, the shell's "no such command") and then discarded. Anything else the
 * decompressor might have been saying -- corrupt header, truncated stream,
 * wrong format -- was thrown away with it, and tmd reported a perfectly good
 * archive as "not a tar archive" with nothing to say why.
 *
 * Which branch a given input takes is a property of the decompressor, not of
 * tmd: GNU gzip flushes the 12 bytes it managed before failing, so a half
 * stream takes the ordinary path, while Apple's buffers and emits nothing, so
 * the same input takes this one. That is how this was found -- a macOS runner
 * failing a test that had passed on Linux since it was written.
 *
 * A gzip header with nothing after it fails this way on every gzip.
 */
static const unsigned char GZ_HEADER_ONLY[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff,
};

static void test_empty_failed_decompression(void)
{
    char *path;
    char *err = NULL;

    if (!have_gzip())
    {
        return;
    }
    path = write_temp(GZ_HEADER_ONLY, sizeof(GZ_HEADER_ONLY));
    CHECK(path != NULL);
    if (!path)
    {
        return;
    }

    TEST_CASE("a decompressor that fails before its first byte is still a "
              "failure");
    {
        struct tmd_source *s;
        int                saved_err = dup(STDERR_FILENO);
        int                devnull = open("/dev/null", O_WRONLY);

        if (devnull >= 0)
        {
            (void)dup2(devnull, STDERR_FILENO);
        }

        s = tmd_source_open(path, &err);

        if (saved_err >= 0)
        {
            (void)dup2(saved_err, STDERR_FILENO);
            (void)close(saved_err);
        }
        if (devnull >= 0)
        {
            (void)close(devnull);
        }

        /* It opens: there is nothing wrong with the file as a file, and the
         * reader is entitled to see the empty stream. What it must not do is
         * call that stream trustworthy. */
        CHECK(s != NULL);
        if (s)
        {
            char buf[16];

            CHECK_INT(tmd_source_read(s, buf, sizeof(buf)), 0);
            CHECK(tmd_source_codec_failed(s));
            tmd_source_close(s);
        }
        free(err);
        err = NULL;
    }
    (void)remove(path);
    free(path);
}

void test_source(void)
{
    test_memory_source();
    test_compressed_source();
    test_truncated_compressed_source();
    test_empty_failed_decompression();
    test_file_source();
}
