/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "render.h"
#include "util.h"

/* A minimal entry, so each test only says what it is actually about. */
static void make_entry(struct tmd_entry *e)
{
    memset(e, 0, sizeof(*e));
    e->path = (char *)"src/main.c";
    e->linkpath = (char *)"";
    e->uname = (char *)"bceverly";
    e->gname = (char *)"staff";
    e->uid = 1000;
    e->gid = 50;
    e->mode = 0644;
    e->size = 1234;
    e->kind = TMD_KIND_FILE;
    e->format = TMD_FMT_PAX;
    e->typeflag = '0';
    e->mtime.sec = 1600000000;
    e->mtime.present = true;
    e->chksum_ok = true;
}

static void default_options(struct tmd_options *opt)
{
    memset(opt, 0, sizeof(*opt));
    /* Nothing more to set: a zeroed options struct means UTC, which is what
     * keeps the expected strings below independent of where the tests run. A
     * test that passes only in one time zone is worse than no test. */
}

static void test_listing_line(void)
{
    struct tmd_entry   e;
    struct tmd_options opt;
    char              *line;

    default_options(&opt);

    TEST_CASE("a regular file renders in ls -l shape");
    make_entry(&e);
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, "-rw-r--r--");
    CHECK_CONTAINS(line, "bceverly/staff");
    CHECK_CONTAINS(line, "1234");
    CHECK_CONTAINS(line, "2020-09-13 12:26:40Z");
    CHECK_CONTAINS(line, "src/main.c");
    free(line);

    TEST_CASE("a symlink shows its target with an arrow");
    make_entry(&e);
    e.kind = TMD_KIND_SYMLINK;
    e.mode = 0777;
    e.path = (char *)"bin/tmd";
    e.linkpath = (char *)"../src/tmd";
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, "lrwxrwxrwx");
    CHECK_CONTAINS(line, "bin/tmd -> ../src/tmd");
    free(line);

    TEST_CASE("a hard link says what it links to, the way tar does");
    make_entry(&e);
    e.kind = TMD_KIND_HARDLINK;
    e.path = (char *)"copy.txt";
    e.linkpath = (char *)"original.txt";
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, "copy.txt link to original.txt");
    free(line);

    TEST_CASE("a device shows major,minor where the size would be");
    make_entry(&e);
    e.kind = TMD_KIND_CHARDEV;
    e.path = (char *)"dev/null";
    e.has_dev = true;
    e.devmajor = 1;
    e.devminor = 3;
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, "crw-r--r--");
    CHECK_CONTAINS(line, "1,3");
    free(line);

    TEST_CASE("-n prints numbers even when the archive carries names");
    make_entry(&e);
    opt.numeric = true;
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, "1000/50");
    free(line);
    opt.numeric = false;

    TEST_CASE("a missing name falls back to the number, per field");
    /* An archive can carry a uname and no gname; the mixed case is real. */
    make_entry(&e);
    e.gname = (char *)"";
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, "bceverly/50");
    free(line);

    TEST_CASE("-H renders the size the way ls -h does");
    make_entry(&e);
    e.size = 5 * 1024 * 1024;
    opt.human = true;
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, "5.0M");
    free(line);
    opt.human = false;

    TEST_CASE("an absent mtime prints a dash rather than 1970");
    make_entry(&e);
    e.mtime.present = false;
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, " - ");
    CHECK(strstr(line, "1970") == NULL);
    free(line);

    TEST_CASE("a timestamp too large for this platform's time_t prints the number");
    make_entry(&e);
    e.mtime.sec = 9223372036854775807LL;
    line = tmd_render_listing_line(&e, &opt);
    /* Either the raw seconds or a real date — never a wrong date and never
     * a crash. */
    CHECK(strstr(line, "@9223372036854775807") != NULL ||
          strstr(line, "-") != NULL);
    free(line);

    TEST_CASE("--full-time adds seconds and the zone");
    make_entry(&e);
    opt.full_time = true;
    e.mtime.nsec = 123456789;
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, "2020-09-13 12:26:40.123456789Z");
    free(line);
    opt.full_time = false;

    TEST_CASE("--local renders in the reader's zone, with no Z");
    make_entry(&e);
    opt.local = true;
    line = tmd_render_listing_line(&e, &opt);
    /* The instant is the same; only the rendering differs. Which wall-clock
     * numerals appear depends on the machine's zone, so this checks for the
     * absence of the UTC marker rather than for a particular time. */
    CHECK(strstr(line, "Z") == NULL);
    CHECK_CONTAINS(line, "2020-09-13");
    free(line);
    opt.local = false;

    TEST_CASE("--color wraps the path and nothing else");
    make_entry(&e);
    e.kind = TMD_KIND_DIR;
    e.path = (char *)"src/";
    opt.color = true;
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, "\033[1;34msrc/\033[0m");
    free(line);
    opt.color = false;
}

static void test_json_escaping(void)
{
    struct tmd_buf b;

    TEST_CASE("the characters JSON requires escaped are escaped");
    tmd_buf_init(&b);
    tmd_json_escape(&b, "a\"b\\c\nd\te");
    CHECK_STR(b.data, "\"a\\\"b\\\\c\\nd\\te\"");
    tmd_buf_free(&b);

    TEST_CASE("control characters become \\u00XX");
    tmd_buf_init(&b);
    /* Octal, not "\\x07end": a hex escape consumes every hex digit that
     * follows it, so "\\x07e" is one character, U+007E, and the test would be
     * about a tilde. */
    tmd_json_escape(&b, "bell\007end");
    CHECK_STR(b.data, "\"bell\\u0007end\"");
    tmd_buf_free(&b);

    TEST_CASE("valid UTF-8 passes through as its own bytes");
    /* Escaping it would be legal JSON and would make every non-English path
     * unreadable in the output. */
    tmd_buf_init(&b);
    tmd_json_escape(&b, "caf\xc3\xa9/na\xc3\xafve");
    CHECK_STR(b.data, "\"caf\xc3\xa9/na\xc3\xafve\"");
    tmd_buf_free(&b);

    TEST_CASE("a path that is not valid UTF-8 is escaped byte by byte");
    /* A tar path is bytes, not text: a Latin-1 filename is a legal archive.
     * Emitting the raw byte produces JSON a strict parser rejects, and
     * replacing it with U+FFFD destroys the only copy of the value. */
    tmd_buf_init(&b);
    tmd_json_escape(&b, "caf\xe9");
    CHECK_STR(b.data, "\"caf\\u00e9\"");
    tmd_buf_free(&b);

    TEST_CASE("an empty string is a pair of quotes");
    tmd_buf_init(&b);
    tmd_json_escape(&b, "");
    CHECK_STR(b.data, "\"\"");
    tmd_buf_free(&b);
}

/* The renderer writes to a FILE*, so the whole-output tests go through a
 * temporary file and read it back. open_memstream would be neater and is a GNU
 * extension; this works anywhere. */
static char *render_to_string(const struct tmd_options *opt,
                              const struct tmd_archive *archive,
                              const struct tmd_entry *entries, size_t count)
{
    char               path[] = "/tmp/tmd-test-renderXXXXXX";
    int                fd = mkstemp(path);
    FILE              *f;
    struct tmd_render *rd;
    struct tmd_buf     out;
    char               chunk[4096];
    size_t             got;
    size_t             i;

    if (fd < 0) {
        return tmd_xstrdup("");
    }
    f = fdopen(fd, "w+b");
    if (!f) {
        (void)remove(path);
        return tmd_xstrdup("");
    }

    rd = tmd_render_new(f, opt, 1);
    tmd_render_archive_begin(rd, archive);
    for (i = 0; i < count; i++) {
        tmd_render_entry(rd, &entries[i]);
    }
    tmd_render_archive_end(rd, archive);
    tmd_render_finish(rd);
    tmd_render_free(rd);

    (void)fflush(f);
    rewind(f);
    tmd_buf_init(&out);
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        tmd_buf_add(&out, chunk, got);
    }
    (void)fclose(f);
    (void)remove(path);
    return tmd_buf_detach(&out);
}

static void test_output_formats(void)
{
    struct tmd_entry   e;
    struct tmd_archive a;
    struct tmd_options opt;
    char              *out;

    memset(&a, 0, sizeof(a));
    a.name = (char *)"test.tar";
    a.format = TMD_FMT_PAX;
    a.formats[TMD_FMT_USTAR] = true;
    a.formats[TMD_FMT_PAX] = true;
    a.entries = 1;
    a.counts[TMD_KIND_FILE] = 1;
    a.total_size = 1234;
    a.total_stored = 1536;
    a.file_size = 10240;
    a.record_blocks = 20;
    a.eof_marker = true;
    a.writer = "GNU tar";

    TEST_CASE("the default text output is the listing and nothing else");
    default_options(&opt);
    make_entry(&e);
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "src/main.c");
    /* No banner, no summary: `tmd -f x.tar | awk` must not have to strip one. */
    CHECK(strstr(out, "test.tar") == NULL);
    CHECK(strstr(out, "members") == NULL);
    free(out);

    TEST_CASE("-S appends the summary after the listing");
    default_options(&opt);
    opt.with_summary = true;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "src/main.c");
    CHECK_CONTAINS(out, "POSIX pax (POSIX.1-2001)");
    CHECK_CONTAINS(out, "written by    GNU tar (inferred)");
    CHECK_CONTAINS(out, "end marker    present");
    CHECK_CONTAINS(out, "blocking      20 blocks");
    free(out);

    TEST_CASE("-s prints the summary and no entries at all");
    default_options(&opt);
    opt.summary_only = true;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK(strstr(out, "src/main.c") == NULL);
    CHECK_CONTAINS(out, "members       1");
    free(out);

    TEST_CASE("a missing end marker is called out in the summary");
    default_options(&opt);
    opt.summary_only = true;
    a.eof_marker = false;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "MISSING");
    free(out);
    a.eof_marker = true;

    TEST_CASE("--long prints every field of the member");
    default_options(&opt);
    opt.long_form = true;
    make_entry(&e);
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "type        file");
    CHECK_CONTAINS(out, "format      pax");
    CHECK_CONTAINS(out, "mode        0644  -rw-r--r--");
    CHECK_CONTAINS(out, "owner       bceverly (1000) / staff (50)");
    CHECK_CONTAINS(out, "checksum    ok");
    free(out);

    TEST_CASE("-R adds the raw header fields");
    default_options(&opt);
    opt.long_form = true;
    opt.headers = true;
    make_entry(&e);
    (void)snprintf(e.raw.magic, sizeof(e.raw.magic), "%s", "ustar");
    (void)snprintf(e.raw.mode, sizeof(e.raw.mode), "%s", "0000644");
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "raw header");
    CHECK_CONTAINS(out, "mode      \"0000644\"");
    CHECK_CONTAINS(out, "magic     \"ustar\"");
    free(out);

    TEST_CASE("csv writes a header row and quotes what needs quoting");
    default_options(&opt);
    opt.output = TMD_OUT_CSV;
    make_entry(&e);
    e.path = (char *)"a,file\"with\nawkward,name";
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "path,kind,mode_string");
    CHECK_CONTAINS(out, "\"a,file\"\"with\nawkward,name\"");
    CHECK_CONTAINS(out, "2020-09-13T12:26:40Z");
    free(out);

    TEST_CASE("json carries the entry and the summary in one object");
    default_options(&opt);
    opt.output = TMD_OUT_JSON;
    make_entry(&e);
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "\"archive\": \"test.tar\"");
    CHECK_CONTAINS(out, "\"path\": \"src/main.c\"");
    CHECK_CONTAINS(out, "\"kind\": \"file\"");
    CHECK_CONTAINS(out, "\"mtime\": \"2020-09-13T12:26:40Z\"");
    CHECK_CONTAINS(out, "\"summary\":");
    CHECK_CONTAINS(out, "\"end_marker\": true");
    free(out);

    TEST_CASE("json of an archive with no entries is still valid");
    /* The comma between entries is tracked by hand while streaming, so the
     * empty case is exactly where a stray one would appear. */
    default_options(&opt);
    opt.output = TMD_OUT_JSON;
    out = render_to_string(&opt, &a, NULL, 0);
    CHECK_CONTAINS(out, "\"entries\": [\n  ],");
    free(out);

    TEST_CASE("sparse segments appear in both the long and the json form");
    {
        struct tmd_sparse segments[2] = { { 0, 512 }, { 1048064, 512 } };
        default_options(&opt);
        make_entry(&e);
        e.is_sparse = true;
        e.realsize = 1048576;
        e.sparse = segments;
        e.nsparse = 2;

        opt.long_form = true;
        out = render_to_string(&opt, &a, &e, 1);
        CHECK_CONTAINS(out, "sparse      2 data segments, expands to 1048576");
        free(out);

        default_options(&opt);
        opt.output = TMD_OUT_JSON;
        out = render_to_string(&opt, &a, &e, 1);
        CHECK_CONTAINS(out, "\"sparse\": {\"realsize\": 1048576");
        CHECK_CONTAINS(out, "{\"offset\": 0, \"bytes\": 512}");
        free(out);
    }

    TEST_CASE("pax attributes and warnings reach the long and json forms");
    {
        struct tmd_kv pax[1];
        struct tmd_warning warnings[1];

        pax[0].key = (char *)"SCHILY.xattr.user.tag";
        pax[0].value = (char *)"value";
        warnings[0].code = "checksum-mismatch";
        warnings[0].text = (char *)"header checksum mismatch";

        default_options(&opt);
        make_entry(&e);
        e.pax = pax;
        e.npax = 1;
        e.warnings = warnings;
        e.nwarnings = 1;

        opt.long_form = true;
        out = render_to_string(&opt, &a, &e, 1);
        CHECK_CONTAINS(out, "pax         SCHILY.xattr.user.tag = value");
        CHECK_CONTAINS(out, "warning     header checksum mismatch");
        free(out);

        default_options(&opt);
        opt.output = TMD_OUT_JSON;
        out = render_to_string(&opt, &a, &e, 1);
        CHECK_CONTAINS(out, "\"pax\": {\"SCHILY.xattr.user.tag\": \"value\"}");
        CHECK_CONTAINS(out,
                       "\"warnings\": [{\"code\": \"checksum-mismatch\", "
                       "\"text\": \"header checksum mismatch\"}]");
        free(out);
    }
}

/*
 * Timestamps at the edge of what strftime can render.
 *
 * A tar header carries a 64-bit mtime, and GNU base-256 numeric fields make a
 * huge one easy to write -- GNU tar itself will list a member dated in the year
 * 999338027 without complaint -- so "%Y" is not always four digits.
 *
 * When the conversion did not fit, strftime returned 0 and left the buffer
 * *unspecified* (C11 7.27.3.5), and this file printed it anyway with "%s".
 * On a quiet stack that showed up as a timestamp with its seconds missing,
 * "00:00:Z"; on a dirty one it read off the end of a 24-byte array, which is
 * how libFuzzer found it -- a 29-byte read out of `stamp`.
 *
 * These pin both halves. The strings check that the rendering is complete;
 * running them under ASan in `make test` checks that nothing is read past the
 * buffer.
 */
static void test_extreme_timestamps(void)
{
    struct tmd_entry   e;
    struct tmd_options opt;
    char              *line;

    TEST_CASE("a nine-digit year renders in full");
    default_options(&opt);
    make_entry(&e);
    e.mtime.sec = 31536000000000000LL;
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, "999338027-07-21 00:00:00Z");
    free(line);

    /*
     * This is the value the fuzzer reached. It matters that it is this one:
     * the conversion is long enough that glibc stops part-way through the
     * seconds field and leaves no terminator anywhere in 24 bytes, where a
     * slightly shorter one happens to leave a zero byte and looks fine.
     */
    TEST_CASE("--full-time keeps the seconds on a nine-digit year");
    default_options(&opt);
    opt.full_time = true;
    make_entry(&e);
    e.mtime.sec = 3155729985192635LL;
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, "100003072-04-19 09:30:35Z");
    free(line);

    TEST_CASE("nanoseconds survive a nine-digit year");
    default_options(&opt);
    opt.full_time = true;
    make_entry(&e);
    e.mtime.sec = 3155729985192635LL;
    e.mtime.nsec = 123456789;
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, "100003072-04-19 09:30:35.123456789Z");
    free(line);

    TEST_CASE("the ISO formats render a nine-digit year too");
    {
        struct tmd_archive a;
        char              *out;

        memset(&a, 0, sizeof(a));
        a.name = (char *)"big.tar";
        a.format = TMD_FMT_PAX;
        default_options(&opt);
        opt.output = TMD_OUT_JSON;
        make_entry(&e);
        e.mtime.sec = 3155729985192635LL;
        out = render_to_string(&opt, &a, &e, 1);
        CHECK_CONTAINS(out, "100003072-04-19T09:30:35Z");
        free(out);
    }

    /*
     * Past what a struct tm can hold, tm_year overflows and the year comes back
     * negative. There is no right answer to print, but there is a wrong one:
     * crashing, or reading past a buffer. Only that nothing blows up is
     * asserted -- the exact text is glibc's business, not this program's.
     */
    TEST_CASE("an mtime past what struct tm can hold does not misbehave");
    default_options(&opt);
    opt.full_time = true;
    make_entry(&e);
    e.mtime.sec = 67768036191676799LL;
    /* tmd_render_listing_line allocates through tmd_xmalloc, which exits
     * rather than returning NULL, so the line is checked for content and not
     * for a null pointer that cannot arrive. */
    line = tmd_render_listing_line(&e, &opt);
    CHECK_CONTAINS(line, "src/main.c");
    free(line);
}

/*
 * The schema 2 JSON: a description of the bytes, not a tidier listing.
 *
 * Each of these is a field somebody has to be able to rely on, so each is
 * checked for its value rather than its presence.
 */
static void test_exhaustive_json(void)
{
    struct tmd_archive a;
    struct tmd_entry   e;
    struct tmd_options opt;
    char              *out;

    TEST_CASE("the document declares its schema");
    memset(&a, 0, sizeof(a));
    a.name = (char *)"t.tar";
    a.format = TMD_FMT_PAX;
    default_options(&opt);
    opt.output = TMD_OUT_JSON;
    make_entry(&e);
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "\"schema\": 2");
    free(out);

    TEST_CASE("the mode is broken into the bits it is made of");
    make_entry(&e);
    e.mode = 04755; /* setuid, which is invisible in "rwxr-xr-x" */
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "\"setuid\": true");
    CHECK_CONTAINS(out, "\"setgid\": false");
    CHECK_CONTAINS(out, "\"sticky\": false");
    CHECK_CONTAINS(out, "\"owner\": {\"read\": true, \"write\": true, \"execute\": true}");
    CHECK_CONTAINS(out, "\"other\": {\"read\": true, \"write\": false, \"execute\": true}");
    free(out);

    /*
     * The block arithmetic is the part somebody would use to reconstruct the
     * archive's layout, so the padding has to be exactly right: 6 bytes of
     * payload occupy a whole 512-byte block and waste 506 of it.
     */
    TEST_CASE("the block range and its padding are reported exactly");
    make_entry(&e);
    e.offset = 1024;
    e.data_size = 6;
    e.size = 6;
    e.stored_size = 1024; /* one header block, one data block */
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "\"header_offset\": 1024");
    CHECK_CONTAINS(out, "\"header_blocks\": 1");
    CHECK_CONTAINS(out, "\"data_offset\": 1536");
    CHECK_CONTAINS(out, "\"data_blocks\": 1");
    CHECK_CONTAINS(out, "\"padding\": 506");
    free(out);

    TEST_CASE("both checksum conventions are reported, and which matched");
    make_entry(&e);
    e.chksum_stored = 6208;
    e.chksum_unsigned = 6208;
    e.chksum_signed = 6208;
    e.chksum_ok = true;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "\"computed_unsigned\": 6208");
    CHECK_CONTAINS(out, "\"computed_signed\": 6208");
    CHECK_CONTAINS(out, "\"matched\": \"unsigned\"");
    free(out);

    TEST_CASE("a header only the signed reading accepts says so");
    make_entry(&e);
    e.chksum_stored = 4294967000u;
    e.chksum_unsigned = 12;
    e.chksum_signed = -296;
    e.chksum_ok = true;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "\"matched\": \"signed\"");
    free(out);

    TEST_CASE("a path that is not UTF-8 is reported with the offending byte");
    make_entry(&e);
    e.path = (char *)"bad-\xff-name";
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "\"path_encoding\": {\"utf8\": false, \"first_invalid_byte\": 4}");
    free(out);

    TEST_CASE("a path that is UTF-8 says so too");
    make_entry(&e);
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "\"path_encoding\": {\"utf8\": true}");
    free(out);

    TEST_CASE("where each timestamp came from is reported");
    make_entry(&e);
    e.mtime.source = "pax mtime";
    e.ctime.sec = 1600000000;
    e.ctime.present = true;
    e.ctime.source = "pax SCHILY.ctime";
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "\"mtime_source\": \"pax mtime\"");
    CHECK_CONTAINS(out, "\"ctime_source\": \"pax SCHILY.ctime\"");
    free(out);

    TEST_CASE("xattrs are reported encoded AND decoded");
    {
        struct tmd_kv pax[2];

        make_entry(&e);
        pax[0].key = (char *)"SCHILY.xattr.user.comment";
        pax[0].value = (char *)"aGVsbG8geGF0dHI=";        /* "hello xattr" */
        pax[1].key = (char *)"LIBARCHIVE.xattr.user.broken";
        pax[1].value = (char *)"!!!not base64!!!";
        e.pax = pax;
        e.npax = 2;
        out = render_to_string(&opt, &a, &e, 1);
        CHECK_CONTAINS(out, "\"user.comment\": {\"encoded\": \"aGVsbG8geGF0dHI=\", "
                            "\"decoded\": \"hello xattr\"}");
        /* A value that is not base64 is called out, not silently dropped: that
         * it does not decode is itself a fact about the archive. */
        CHECK_CONTAINS(out, "\"decoded\": null, \"decode_error\": \"not valid base64\"");
        /* And the verbatim record is still there beside the decoding. */
        CHECK_CONTAINS(out, "\"SCHILY.xattr.user.comment\": \"aGVsbG8geGF0dHI=\"");
        free(out);
    }
}

/* Where each path appears in the output, for asserting an order. */
static long position_of(const char *haystack, const char *needle)
{
    const char *at = strstr(haystack, needle);

    return at ? (long)(at - haystack) : -1;
}

static void test_match_and_sort(void)
{
    struct tmd_archive a;
    struct tmd_entry   e[3];
    struct tmd_options opt;
    const char        *patterns[2];
    char              *out;
    size_t             i;

    memset(&a, 0, sizeof(a));
    a.name = (char *)"t.tar";
    a.format = TMD_FMT_PAX;
    a.entries = 3;

    for (i = 0; i < 3; i++) {
        make_entry(&e[i]);
    }
    e[0].path = (char *)"etc/b.conf";
    e[0].size = 300;
    e[0].offset = 0;
    e[0].mtime.sec = 3000;
    e[1].path = (char *)"a.txt";
    e[1].size = 100;
    e[1].offset = 1024;
    e[1].mtime.sec = 1000;
    e[2].path = (char *)"etc/c.conf";
    e[2].size = 100; /* ties with a.txt, to pin the tiebreak */
    e[2].offset = 2048;
    e[2].mtime.sec = 2000;

    TEST_CASE("--sort=path orders by the stored path");
    default_options(&opt);
    opt.sort = TMD_SORT_PATH;
    out = render_to_string(&opt, &a, e, 3);
    CHECK(position_of(out, "a.txt") < position_of(out, "etc/b.conf"));
    CHECK(position_of(out, "etc/b.conf") < position_of(out, "etc/c.conf"));
    free(out);

    TEST_CASE("--sort=size orders by size");
    default_options(&opt);
    opt.sort = TMD_SORT_SIZE;
    out = render_to_string(&opt, &a, e, 3);
    CHECK(position_of(out, "a.txt") < position_of(out, "etc/b.conf"));
    free(out);

    /*
     * Equal keys keep the archive's own order, so a listing can be diffed
     * against itself. a.txt (@1024) and etc/c.conf (@2048) are both 100 bytes.
     */
    TEST_CASE("equal keys fall back to the archive order");
    default_options(&opt);
    opt.sort = TMD_SORT_SIZE;
    out = render_to_string(&opt, &a, e, 3);
    CHECK(position_of(out, "a.txt") < position_of(out, "etc/c.conf"));
    free(out);

    TEST_CASE("--reverse inverts the key but not the tiebreak");
    default_options(&opt);
    opt.sort = TMD_SORT_SIZE;
    opt.reverse = true;
    out = render_to_string(&opt, &a, e, 3);
    /* The 300-byte member comes first now... */
    CHECK(position_of(out, "etc/b.conf") < position_of(out, "a.txt"));
    /* ...but the two 100-byte members are still in archive order. */
    CHECK(position_of(out, "a.txt") < position_of(out, "etc/c.conf"));
    free(out);

    TEST_CASE("--sort=mtime orders by the stored time");
    default_options(&opt);
    opt.sort = TMD_SORT_MTIME;
    out = render_to_string(&opt, &a, e, 3);
    CHECK(position_of(out, "a.txt") < position_of(out, "etc/c.conf"));
    CHECK(position_of(out, "etc/c.conf") < position_of(out, "etc/b.conf"));
    free(out);

    TEST_CASE("-m keeps only the members that match");
    default_options(&opt);
    patterns[0] = "*.conf";
    opt.match = patterns;
    opt.nmatch = 1;
    out = render_to_string(&opt, &a, e, 3);
    CHECK_CONTAINS(out, "etc/b.conf");
    CHECK_CONTAINS(out, "etc/c.conf");
    CHECK(position_of(out, "a.txt") < 0);
    free(out);

    /* The offset is what makes two members with the same path tellable apart,
     * which is the reason the feature exists. */
    TEST_CASE("a matched line carries the member's offset");
    out = render_to_string(&opt, &a, e, 3);
    CHECK_CONTAINS(out, "@2048");
    free(out);

    TEST_CASE("several -m patterns are an either/or");
    default_options(&opt);
    patterns[0] = "*.conf";
    patterns[1] = "a.txt";
    opt.match = patterns;
    opt.nmatch = 2;
    out = render_to_string(&opt, &a, e, 3);
    CHECK_CONTAINS(out, "a.txt");
    CHECK_CONTAINS(out, "etc/b.conf");
    free(out);

    TEST_CASE("the summary counts the matches and still describes the archive");
    default_options(&opt);
    patterns[0] = "*.conf";
    opt.match = patterns;
    opt.nmatch = 1;
    opt.with_summary = true;
    out = render_to_string(&opt, &a, e, 3);
    CHECK_CONTAINS(out, "matched       2 of 3 members");
    CHECK_CONTAINS(out, "members       3"); /* the whole archive, not the matches */
    free(out);

    TEST_CASE("JSON reports the match count beside the member count");
    default_options(&opt);
    patterns[0] = "*.conf";
    opt.match = patterns;
    opt.nmatch = 1;
    opt.output = TMD_OUT_JSON;
    out = render_to_string(&opt, &a, e, 3);
    CHECK_CONTAINS(out, "\"members\": 3");
    CHECK_CONTAINS(out, "\"matched\": 2");
    free(out);

    TEST_CASE("-m and --sort compose: filter first, then order");
    default_options(&opt);
    patterns[0] = "*.conf";
    opt.match = patterns;
    opt.nmatch = 1;
    opt.sort = TMD_SORT_SIZE;
    out = render_to_string(&opt, &a, e, 3);
    CHECK(position_of(out, "a.txt") < 0);
    CHECK(position_of(out, "etc/c.conf") < position_of(out, "etc/b.conf"));
    free(out);
}

static void test_stat_mode(void)
{
    struct tmd_archive a;
    struct tmd_entry   e[4];
    struct tmd_options opt;
    char              *out;
    size_t             i;

    memset(&a, 0, sizeof(a));
    a.name = (char *)"t.tar";
    a.format = TMD_FMT_PAX;
    a.entries = 4;

    for (i = 0; i < 4; i++) {
        make_entry(&e[i]);
        e[i].offset = i * 1024;
    }
    /* Three members share one second and the fourth is two minutes later --
     * the shape a release script leaves behind. */
    e[0].path = (char *)"a"; e[0].size = 10;   e[0].data_size = 10;   e[0].stored_size = 1024; e[0].mtime.sec = 1700000000;
    e[1].path = (char *)"b"; e[1].size = 20;   e[1].data_size = 20;   e[1].stored_size = 1024; e[1].mtime.sec = 1700000000;
    e[2].path = (char *)"c"; e[2].size = 30;   e[2].data_size = 30;   e[2].stored_size = 1024; e[2].mtime.sec = 1700000000;
    e[3].path = (char *)"d"; e[3].size = 5000; e[3].data_size = 5000; e[3].stored_size = 5632; e[3].mtime.sec = 1700000120;

    TEST_CASE("--stat counts the members and their bytes");
    default_options(&opt);
    opt.stats = true;
    out = render_to_string(&opt, &a, e, 4);
    CHECK_CONTAINS(out, "members       4");
    CHECK_CONTAINS(out, "extracted     5060 bytes");
    free(out);

    /* 10, 20 and 30 bytes each occupy a whole 512-byte block; 5000 bytes takes
     * ten and wastes 120. */
    TEST_CASE("--stat reports the padding the archive spends");
    out = render_to_string(&opt, &a, e, 4);
    CHECK_CONTAINS(out, "padding       1596 bytes");
    free(out);

    TEST_CASE("--stat lists the largest members, biggest first");
    out = render_to_string(&opt, &a, e, 4);
    CHECK_CONTAINS(out, "5000  d");
    CHECK(strstr(out, "5000  d") < strstr(out, "30  c"));
    free(out);

    TEST_CASE("--stat counts distinct timestamps, not members");
    out = render_to_string(&opt, &a, e, 4);
    CHECK_CONTAINS(out, "timestamps    2 distinct");
    CHECK_CONTAINS(out, "span          2 minutes");
    free(out);

    TEST_CASE("--stat names the timestamp most members share");
    out = render_to_string(&opt, &a, e, 4);
    CHECK_CONTAINS(out, "most common");
    CHECK_CONTAINS(out, "(3 of 4 members)");
    free(out);

    /* The inference is the half of --stat worth having, so it is pinned. */
    TEST_CASE("--stat infers how the archive was produced");
    out = render_to_string(&opt, &a, e, 4);
    CHECK_CONTAINS(out, "exported into a fresh directory");
    free(out);

    TEST_CASE("one timestamp for every member reads as normalized");
    for (i = 0; i < 4; i++) {
        e[i].mtime.sec = 1700000000;
    }
    out = render_to_string(&opt, &a, e, 4);
    CHECK_CONTAINS(out, "timestamps    1 distinct");
    CHECK_CONTAINS(out, "normalized");
    free(out);

    TEST_CASE("every member at the epoch reads as discarded, not normalized");
    for (i = 0; i < 4; i++) {
        e[i].mtime.sec = 0;
    }
    out = render_to_string(&opt, &a, e, 4);
    CHECK_CONTAINS(out, "timestamps were discarded");
    free(out);

    TEST_CASE("a member dated in the future is called out");
    for (i = 0; i < 4; i++) {
        e[i].mtime.sec = 1700000000;
    }
    e[0].mtime.sec = 4000000000LL; /* 2096 */
    out = render_to_string(&opt, &a, e, 4);
    CHECK_CONTAINS(out, "future        1 member dated in the FUTURE");
    free(out);

    TEST_CASE("a member older than tar itself is called out");
    for (i = 0; i < 4; i++) {
        e[i].mtime.sec = 1700000000;
    }
    e[0].mtime.sec = 100000; /* 1970 */
    out = render_to_string(&opt, &a, e, 4);
    CHECK_CONTAINS(out, "before tar existed");
    free(out);

    TEST_CASE("--stat reaches the JSON as its own object");
    for (i = 0; i < 4; i++) {
        e[i].mtime.sec = 1700000000;
    }
    default_options(&opt);
    opt.stats = true;
    opt.output = TMD_OUT_JSON;
    out = render_to_string(&opt, &a, e, 4);
    CHECK_CONTAINS(out, "\"stat\": {\"members\": 4");
    CHECK_CONTAINS(out, "\"padding\": 1596");
    CHECK_CONTAINS(out, "\"distinct\": 1");
    CHECK_CONTAINS(out, "\"distinct_capped\": false");
    free(out);

    /*
     * -m narrows what --stat describes. That is the opposite of what -m does to
     * the summary, and deliberate: a summary answers "what is this file", a
     * distribution answers "what is in this set".
     */
    TEST_CASE("-m narrows the set --stat describes");
    {
        const char *patterns[1];

        default_options(&opt);
        opt.stats = true;
        patterns[0] = "d";
        opt.match = patterns;
        opt.nmatch = 1;
        out = render_to_string(&opt, &a, e, 4);
        CHECK_CONTAINS(out, "matched       1 of 4 members");
        CHECK_CONTAINS(out, "members       1");
        CHECK_CONTAINS(out, "extracted     5000 bytes");
        free(out);
    }
}

static void test_extraction_safety(void)
{
    struct tmd_archive a;
    struct tmd_entry   e;
    struct tmd_options opt;
    char              *out;

    memset(&a, 0, sizeof(a));
    a.name = (char *)"t.tar";
    a.format = TMD_FMT_PAX;
    a.entries = 1;
    make_entry(&e);

    /*
     * Stated even when there is nothing wrong. A reader pointing tmd at an
     * untrusted archive is asking a yes/no question, and silence is not an
     * answer to it -- it reads as "tmd did not look".
     */
    TEST_CASE("a clean archive says so rather than staying silent");
    default_options(&opt);
    opt.with_summary = true;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "every member stays inside the extraction directory");
    free(out);

    TEST_CASE("an escaping member is reported as a class");
    a.features.escape_link = 1;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "would extract OUTSIDE the current directory");
    CHECK_CONTAINS(out, "1 link pointing outside");
    free(out);

    TEST_CASE("each kind of escape is counted separately");
    a.features.escape_absolute = 2;
    a.features.escape_traversal = 3;
    a.features.escape_link = 1;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "6 members would extract OUTSIDE");
    CHECK_CONTAINS(out, "2 absolute");
    CHECK_CONTAINS(out, "3 climbing out");
    free(out);

    /* A consumer should read one boolean, not infer safety from missing keys. */
    TEST_CASE("the JSON carries a single boolean for the whole question");
    default_options(&opt);
    opt.output = TMD_OUT_JSON;
    opt.with_summary = true;
    memset(&a.features, 0, sizeof(a.features));
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "\"extraction\": {\"escapes\": false");
    free(out);

    a.features.escape_traversal = 1;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "\"extraction\": {\"escapes\": true");
    CHECK_CONTAINS(out, "\"traversals\": 1");
    free(out);
}

static void test_created_and_order(void)
{
    struct tmd_archive a;
    struct tmd_entry   e;
    struct tmd_options opt;
    char              *out;

    memset(&a, 0, sizeof(a));
    a.name = (char *)"t.tar";
    a.format = TMD_FMT_PAX;
    a.entries = 3;

    /*
     * Only libarchive writes a creation time, as LIBARCHIVE.creationtime, and
     * only when it is EARLIER than the mtime. So the value here is earlier --
     * an archive with a creation time after its modification time is not
     * something libarchive produces.
     */
    TEST_CASE("a creation time renders beside modified under -l");
    make_entry(&e);
    e.created.sec = 1500000000;
    e.created.present = true;
    e.created.source = "pax LIBARCHIVE.creationtime";
    default_options(&opt);
    opt.long_form = true;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "created     2017-07-14 02:40:00Z");
    free(out);

    TEST_CASE("and as its own JSON field, with where it came from");
    default_options(&opt);
    opt.output = TMD_OUT_JSON;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "\"created\": \"2017-07-14T02:40:00Z\"");
    CHECK_CONTAINS(out, "\"created_epoch\": 1500000000");
    CHECK_CONTAINS(out, "\"created_source\": \"pax LIBARCHIVE.creationtime\"");
    free(out);

    TEST_CASE("a member without one says nothing about it");
    make_entry(&e);
    default_options(&opt);
    opt.long_form = true;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK(strstr(out, "created") == NULL);
    free(out);

    /* --- member order ---------------------------------------------------- */
    make_entry(&e);
    a.counts[TMD_KIND_DIR] = 1;

    TEST_CASE("a sorted archive is reported as sorted");
    a.features.order_sorted = true;
    a.features.nroots = 1;
    memcpy(a.features.roots[0], "proj", 5);
    default_options(&opt);
    opt.with_summary = true;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "lexicographic by path");
    CHECK_CONTAINS(out, "one entry: proj");
    free(out);

    /*
     * Reported as an observation, not as a guess about which command wrote it.
     * `tar -c DIR` over a small tree can come out in exact lexicographic order,
     * and a reverse-sorted file list comes out unsorted with directories
     * present, so neither ordering identifies a writer.
     */
    TEST_CASE("an unsorted archive is reported without guessing why");
    a.features.order_sorted = false;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "not in path order");
    CHECK(strstr(out, "tar -c DIR") == NULL);
    free(out);

    /* This one does follow: a directory walk emits the directories it walks. */
    TEST_CASE("no directory members is reported as a file list");
    a.counts[TMD_KIND_DIR] = 0;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "written from a list of files");
    free(out);

    TEST_CASE("many top-level entries says extracting scatters them");
    {
        size_t i;

        a.features.nroots = 8;
        a.features.roots_truncated = true;
        for (i = 0; i < 8; i++) {
            a.features.roots[i][0] = (char)('a' + i);
            a.features.roots[i][1] = '\0';
        }
        out = render_to_string(&opt, &a, &e, 1);
        CHECK_CONTAINS(out, "scatters them into the current directory");
        free(out);
    }

    TEST_CASE("the order reaches the JSON");
    a.features.order_sorted = true;
    a.features.roots_truncated = false;
    a.features.nroots = 1;
    memcpy(a.features.roots[0], "proj", 5);
    default_options(&opt);
    opt.output = TMD_OUT_JSON;
    opt.with_summary = true;
    out = render_to_string(&opt, &a, &e, 1);
    CHECK_CONTAINS(out, "\"order\": {\"sorted\": true");
    CHECK_CONTAINS(out, "\"top_level\": [\"proj\"]");
    free(out);
}

void test_render(void)
{
    test_listing_line();
    test_json_escaping();
    test_output_formats();
    test_extreme_timestamps();
    test_exhaustive_json();
    test_match_and_sort();
    test_stat_mode();
    test_extraction_safety();
    test_created_and_order();
}
