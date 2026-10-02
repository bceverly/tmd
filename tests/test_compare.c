/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * --diff and --verify, driven directly.
 *
 * The end-to-end suite covers the common shapes through the real binary; this
 * covers what it cannot reach cheaply -- the index growing past its first
 * table, every kind of field difference in both report formats, duplicate
 * paths, and the manifest and damage paths -- by calling the two entry points
 * on archives built in memory and written to temporary files.
 */
#include "test.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compare.h"
#include "opts.h"
#include "tarbuild.h"
#include "util.h"

/* Write `len` bytes to a fresh temporary file; returns its malloc'd name. */
static char *temp_file(const void *data, size_t len)
{
    char  name[] = "/tmp/tmd-test-compareXXXXXX";
    int   fd = mkstemp(name);
    FILE *f;

    if (fd < 0)
    {
        return tmd_xstrdup("/nonexistent");
    }
    f = fdopen(fd, "wb");
    if (!f)
    {
        (void)close(fd);
        return tmd_xstrdup(name);
    }
    if (len > 0)
    {
        (void)fwrite(data, 1, len, f);
    }
    (void)fclose(f);
    return tmd_xstrdup(name);
}

static char *temp_archive(const struct tarbuild *tb)
{
    return temp_file(tb->buf.data, tb->buf.len);
}

static char *temp_text(const char *s)
{
    return temp_file(s, strlen(s));
}

static void forget(char *name)
{
    (void)remove(name);
    free(name);
}

/*
 * Run one comparison and capture its report. `verify` picks the entry point:
 * false compares archive `a` with archive `b`, true checks archive `a` against
 * manifest `b`.
 *
 * Standard error is pointed at /dev/null for the duration, because several of
 * these cases are ABOUT the diagnostics -- an unreadable archive, a corrupt
 * gzip stream -- and their messages (and gzip's own, from the child it runs)
 * would otherwise land in the middle of the test report. The runner keeps its
 * own copy of the real stderr for failures, so nothing a test needs is lost.
 */
static int  muted_stderr = -1;

static void mute_stderr(void)
{
    int null_fd = open("/dev/null", O_WRONLY);

    (void)fflush(stderr);
    muted_stderr = dup(STDERR_FILENO);
    if (null_fd >= 0)
    {
        (void)dup2(null_fd, STDERR_FILENO);
        (void)close(null_fd);
    }
}

static void unmute_stderr(void)
{
    (void)fflush(stderr);
    if (muted_stderr >= 0)
    {
        (void)dup2(muted_stderr, STDERR_FILENO);
        (void)close(muted_stderr);
        muted_stderr = -1;
    }
}

static char *compare_to_string(bool verify, const char *a, const char *b,
                               const struct tmd_options *opt, int *status)
{
    char           name[] = "/tmp/tmd-test-reportXXXXXX";
    int            fd = mkstemp(name);
    FILE          *out;
    struct tmd_buf text;
    char           chunk[4096];
    size_t         got;

    *status = -1;
    if (fd < 0)
    {
        return tmd_xstrdup("");
    }
    out = fdopen(fd, "w+b");
    if (!out)
    {
        (void)close(fd);
        (void)remove(name);
        return tmd_xstrdup("");
    }
    mute_stderr();
    *status = verify ? tmd_verify_archive(a, b, out, opt)
                     : tmd_diff_archives(a, b, out, opt);
    unmute_stderr();
    rewind(out);
    tmd_buf_init(&text);
    while ((got = fread(chunk, 1, sizeof(chunk), out)) > 0)
    {
        tmd_buf_add(&text, chunk, got);
    }
    (void)fclose(out);
    (void)remove(name);
    return tmd_buf_detach(&text);
}

/* One member, every field the comparison looks at under the caller's control. */
static void member(struct tarbuild *tb, const char *name, char typeflag,
                   unsigned mode, long long mtime, const char *content,
                   const char *linkname)
{
    struct tb_hdr h;
    size_t        len = content ? strlen(content) : 0;

    memset(&h, 0, sizeof(h));
    h.name = name;
    h.magic = TB_USTAR;
    h.typeflag = typeflag;
    h.mode = mode;
    h.mtime = mtime;
    h.size = len;
    h.linkname = linkname;
    tb_header(tb, &h);
    if (len > 0)
    {
        tb_data(tb, content, len);
    }
}

static void test_field_differences(void)
{
    struct tarbuild    a;
    struct tarbuild    b;
    struct tmd_options opt;
    char              *fa;
    char              *fb;
    char              *out;
    int                status;

    /* Every field the comparison reads, changed once each. */
    tb_init(&a);
    member(&a, "size", '0', 0644, 1000, "small", NULL);
    member(&a, "mode", '0', 0644, 1000, "x", NULL);
    member(&a, "mtime", '0', 0644, 1000, "x", NULL);
    member(&a, "kind", '0', 0644, 1000, "x", NULL);
    member(&a, "link", '2', 0777, 1000, NULL, "old-target");
    member(&a, "same", '0', 0644, 1000, "x", NULL);
    tb_end(&a);
    tb_init(&b);
    member(&b, "size", '0', 0644, 1000, "much larger", NULL);
    member(&b, "mode", '0', 0755, 1000, "x", NULL);
    member(&b, "mtime", '0', 0644, 2000, "x", NULL);
    member(&b, "kind", '5', 0755, 1000, NULL, NULL);
    member(&b, "link", '2', 0777, 1000, NULL, "new-target");
    member(&b, "same", '0', 0644, 1000, "x", NULL);
    tb_end(&b);
    fa = temp_archive(&a);
    fb = temp_archive(&b);

    TEST_CASE("--diff names every kind of field difference in text");
    memset(&opt, 0, sizeof(opt));
    out = compare_to_string(false, fa, fb, &opt, &status);
    CHECK_INT(status, TMD_EXIT_DIFFER);
    CHECK_CONTAINS(out, "~ size   size 5 -> 11");
    CHECK_CONTAINS(out, "~ mode   mode 0644 -> 0755");
    CHECK_CONTAINS(out, "~ mtime   mtime 1970-01-01 00:16:40Z -> 1970-01-01 00:33:20Z");
    CHECK_CONTAINS(out, "~ kind   kind file -> directory");
    CHECK_CONTAINS(out, "~ link   target old-target -> new-target");
    CHECK_CONTAINS(out, "1 identical, 5 changed, 0 added, 0 removed");
    free(out);

    TEST_CASE("--diff names every kind of field difference in JSON");
    opt.output = TMD_OUT_JSON;
    out = compare_to_string(false, fa, fb, &opt, &status);
    CHECK_INT(status, TMD_EXIT_DIFFER);
    CHECK_CONTAINS(out, "\"mtime\": {\"from\": 1000, \"to\": 2000}");
    CHECK_CONTAINS(out, "\"kind\": {\"from\": \"file\", \"to\": \"directory\"}");
    CHECK_CONTAINS(out, "\"linkpath\": {\"from\": \"old-target\", \"to\": \"new-target\"}");
    CHECK_CONTAINS(out, "\"matches\": false");
    free(out);

    TEST_CASE("a difference in content alone is found only with --hash");
    memset(&opt, 0, sizeof(opt));
    tb_free(&b);
    tb_init(&b);
    member(&b, "size", '0', 0644, 1000, "SMALL", NULL); /* same length */
    member(&b, "mode", '0', 0644, 1000, "x", NULL);
    member(&b, "mtime", '0', 0644, 1000, "x", NULL);
    member(&b, "kind", '0', 0644, 1000, "x", NULL);
    member(&b, "link", '2', 0777, 1000, NULL, "old-target");
    member(&b, "same", '0', 0644, 1000, "x", NULL);
    tb_end(&b);
    forget(fb);
    fb = temp_archive(&b);
    out = compare_to_string(false, fa, fb, &opt, &status);
    CHECK_INT(status, TMD_EXIT_OK);
    free(out);
    opt.hash = TMD_HASH_MD5;
    opt.output = TMD_OUT_JSON;
    out = compare_to_string(false, fa, fb, &opt, &status);
    CHECK_INT(status, TMD_EXIT_DIFFER);
    CHECK_CONTAINS(out, "\"content_hash\": {\"from\": \"");
    free(out);

    forget(fa);
    forget(fb);
    tb_free(&a);
    tb_free(&b);
}

static void test_duplicates_and_size(void)
{
    struct tarbuild    a;
    struct tarbuild    b;
    struct tmd_options opt;
    char              *fa;
    char              *fb;
    char              *out;
    char               name[32];
    int                status;
    int                i;

    TEST_CASE("a path that appears twice is compared by its last occurrence, and counted");
    tb_init(&a);
    member(&a, "dup", '0', 0644, 1000, "first", NULL);
    member(&a, "dup", '0', 0644, 1000, "second!", NULL);
    tb_end(&a);
    tb_init(&b);
    member(&b, "dup", '0', 0644, 1000, "second!", NULL);
    tb_end(&b);
    fa = temp_archive(&a);
    fb = temp_archive(&b);
    memset(&opt, 0, sizeof(opt));
    out = compare_to_string(false, fa, fb, &opt, &status);
    CHECK_INT(status, TMD_EXIT_OK); /* the last "dup" matches */
    CHECK_CONTAINS(out, "1 path appear more than once");
    free(out);
    opt.output = TMD_OUT_JSON;
    out = compare_to_string(false, fa, fb, &opt, &status);
    CHECK_CONTAINS(out, "\"duplicated_paths\": 1");
    free(out);
    forget(fa);
    forget(fb);
    tb_free(&a);
    tb_free(&b);

    /*
     * The index starts at 1024 slots and grows at half full, so 700 members
     * make it grow once. Every member has to survive the move: one lost in
     * the rehash would show up as "removed" and "added" at once.
     */
    TEST_CASE("the index grows past its first table and loses nothing");
    tb_init(&a);
    tb_init(&b);
    for (i = 0; i < 700; i++)
    {
        (void)snprintf(name, sizeof(name), "dir/member-%04d", i);
        member(&a, name, '0', 0644, 1000, "x", NULL);
        member(&b, name, '0', 0644, 1000, i == 613 ? "changed" : "x", NULL);
    }
    tb_end(&a);
    tb_end(&b);
    fa = temp_archive(&a);
    fb = temp_archive(&b);
    memset(&opt, 0, sizeof(opt));
    out = compare_to_string(false, fa, fb, &opt, &status);
    CHECK_INT(status, TMD_EXIT_DIFFER);
    CHECK_CONTAINS(out, "699 identical, 1 changed, 0 added, 0 removed");
    CHECK_CONTAINS(out, "~ dir/member-0613   size 1 -> 7");
    free(out);
    forget(fa);
    forget(fb);
    tb_free(&a);
    tb_free(&b);
}

static void test_manifests(void)
{
    struct tarbuild    a;
    struct tmd_options opt;
    char              *fa;
    char              *fm;
    char              *out;
    int                status;
    const char        *patterns[1];

    tb_init(&a);
    member(&a, "keep/a.txt", '0', 0644, 1000, "abc", NULL);
    member(&a, "skip/b.txt", '0', 0644, 1000, "x", NULL);
    tb_end(&a);
    fa = temp_archive(&a);
    memset(&opt, 0, sizeof(opt));

    TEST_CASE("a digest is read in upper case as well as lower");
    fm = temp_text("3 md5:900150983CD24FB0D6963F7D28E17F72 keep/a.txt\n"
                   "1 skip/b.txt\n");
    out = compare_to_string(true, fa, fm, &opt, &status);
    CHECK_INT(status, TMD_EXIT_OK);
    free(out);
    forget(fm);

    TEST_CASE("a token that is not a well-formed digest is part of the path");
    fm = temp_text("3 md5:zz0150983cd24fb0d6963f7d28e17f72 keep/a.txt\n");
    out = compare_to_string(true, fa, fm, &opt, &status);
    CHECK_INT(status, TMD_EXIT_DIFFER);
    CHECK_CONTAINS(out, "- md5:zz0150983cd24fb0d6963f7d28e17f72 keep/a.txt");
    free(out);
    forget(fm);

    TEST_CASE("a manifest that mixes digest algorithms is refused");
    fm = temp_text("3 md5:900150983cd24fb0d6963f7d28e17f72 keep/a.txt\n"
                   "1 sha256:2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881 skip/b.txt\n");
    out = compare_to_string(true, fa, fm, &opt, &status);
    CHECK_INT(status, TMD_EXIT_ERROR);
    free(out);
    forget(fm);

    TEST_CASE("a digest mismatch is reported in JSON, with the found digest");
    fm = temp_text("3 md5:00000000000000000000000000000000 keep/a.txt\n"
                   "1 skip/b.txt\n");
    opt.output = TMD_OUT_JSON;
    out = compare_to_string(true, fa, fm, &opt, &status);
    CHECK_INT(status, TMD_EXIT_DIFFER);
    CHECK_CONTAINS(out, "\"content_hash\": {\"expected\": \"00000000000000000000000000000000\", "
                        "\"found\": \"900150983cd24fb0d6963f7d28e17f72\"}");
    free(out);
    forget(fm);

    TEST_CASE("a digest the archive cannot back is a difference, reported as null");
    fm = temp_text("3 md5:900150983cd24fb0d6963f7d28e17f72 keep/a.txt\n"
                   "1 md5:9dd4e461268c8034f5c8564e155c67a6 skip/b.txt\n");
    tb_free(&a);
    tb_init(&a);
    member(&a, "keep/a.txt", '0', 0644, 1000, "abc", NULL);
    member(&a, "skip/b.txt", '5', 0755, 1000, NULL, NULL); /* a directory now */
    tb_end(&a);
    forget(fa);
    fa = temp_archive(&a);
    out = compare_to_string(true, fa, fm, &opt, &status);
    CHECK_INT(status, TMD_EXIT_DIFFER);
    CHECK_CONTAINS(out, "\"found\": null}");
    free(out);
    opt.output = TMD_OUT_TEXT;
    out = compare_to_string(true, fa, fm, &opt, &status);
    CHECK_CONTAINS(out, "(content not read) found");
    free(out);
    forget(fm);

    TEST_CASE("-m narrows the manifest as well as the archive");
    fm = temp_text("3 keep/a.txt\n1 skip/b.txt\n");
    patterns[0] = "keep/";
    opt.match = patterns;
    opt.nmatch = 1;
    out = compare_to_string(true, fa, fm, &opt, &status);
    CHECK_INT(status, TMD_EXIT_OK);
    CHECK_CONTAINS(out, "1 identical, 0 changed, 0 unexpected, 0 missing");
    free(out);
    forget(fm);

    forget(fa);
    tb_free(&a);
}

static void test_unreadable_and_damaged(void)
{
    struct tarbuild    a;
    struct tmd_options opt;
    struct tb_hdr      h;
    char              *fa;
    char              *fb;
    char              *out;
    int                status;

    memset(&opt, 0, sizeof(opt));

    TEST_CASE("an archive that cannot be opened is an error, not a difference");
    tb_init(&a);
    member(&a, "x", '0', 0644, 1000, "x", NULL);
    tb_end(&a);
    fa = temp_archive(&a);
    out = compare_to_string(false, fa, "/nonexistent/tmd.tar", &opt, &status);
    CHECK_INT(status, TMD_EXIT_ERROR);
    free(out);

    /* Two blocks of text: long enough that the reader judges it by the header
     * checksum, not by being too short to hold a header at all. */
    TEST_CASE("a file that is not a tar archive is an error");
    {
        char text[1024];

        memset(text, 'x', sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
        fb = temp_text(text);
        out = compare_to_string(false, fa, fb, &opt, &status);
        CHECK_INT(status, TMD_EXIT_ERROR);
        free(out);
        forget(fb);
    }

    /* gzip's magic and then nothing it can decompress: the stream fails. */
    TEST_CASE("a corrupt compressed archive is not compared at all");
    {
        static const unsigned char bad_gz[] = {
            0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
            0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef
        };

        fb = temp_file(bad_gz, sizeof(bad_gz));
        out = compare_to_string(false, fa, fb, &opt, &status);
        CHECK_INT(status, TMD_EXIT_ERROR);
        free(out);
        forget(fb);
    }

    TEST_CASE("-c reports a bad checksum and trailing data, and outranks the difference");
    tb_free(&a);
    tb_init(&a);
    memset(&h, 0, sizeof(h));
    h.name = "x";
    h.magic = TB_USTAR;
    h.typeflag = '0';
    h.mode = 0644;
    h.mtime = 1000;
    h.size = 1;
    tb_header(&a, &h);
    tb_data(&a, "x", 1);
    h.name = "damaged";
    h.bad_checksum = true;
    tb_header(&a, &h);
    tb_data(&a, "y", 1);
    tb_end(&a);
    tb_raw(&a, "trailing garbage", 16);
    fb = temp_archive(&a);
    opt.check = true;
    out = compare_to_string(false, fa, fb, &opt, &status);
    CHECK_INT(status, TMD_EXIT_CHECK);
    free(out);
    opt.check = false;
    out = compare_to_string(false, fa, fb, &opt, &status);
    CHECK_INT(status, TMD_EXIT_DIFFER);
    free(out);
    forget(fb);

    forget(fa);
    tb_free(&a);
}

void test_compare(void)
{
    test_field_differences();
    test_duplicates_and_size();
    test_manifests();
    test_unreadable_and_damaged();
}
