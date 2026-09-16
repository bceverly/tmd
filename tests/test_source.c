/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "test.h"

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
    if (f) {
        size_t i;
        for (i = 0; i < 100; i++)
            (void)fputc((int)('a' + (i % 26)), f);
        (void)fclose(f);
    }

    s = tmd_source_open(path, &err);
    CHECK(s != NULL);
    if (s) {
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

void test_source(void)
{
    test_memory_source();
    test_file_source();
}
