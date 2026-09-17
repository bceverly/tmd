/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "source.h"
#include "tar.h"
#include "tarbuild.h"

/* Read one archive and hand back the first entry, still owned by the reader.
 * Every test here is about a single member, so the caller keeps the reader
 * alive for as long as it looks at the entry. */
static const struct tmd_entry *first_entry(const struct tarbuild *tb,
                                           struct tmd_source **src,
                                           struct tmd_reader **reader)
{
    const struct tmd_entry *e = NULL;

    *src = tmd_source_open_memory(tb->buf.data, tb->buf.len, "(test)");
    *reader = tmd_reader_new(*src);
    if (tmd_reader_next(*reader, &e) != 1) {
        return NULL;
    }
    return e;
}

static void done(struct tmd_source *src, struct tmd_reader *reader)
{
    tmd_reader_free(reader);
    tmd_source_close(src);
}

static const char *pax_value(const struct tmd_entry *e, const char *key)
{
    size_t i;

    for (i = 0; i < e->npax; i++) {
        if (strcmp(e->pax[i].key, key) == 0) {
            return e->pax[i].value;
        }
    }
    return NULL;
}

static void test_pax_overrides(void)
{
    struct tarbuild          tb;
    struct tmd_source       *src;
    struct tmd_reader       *reader;
    const struct tmd_entry  *e;
    static const char *const records[] = {
        "path=a/path/far/longer/than/the/header/could/ever/hold/file.txt",
        "uid=4294967296",
        "gid=70000",
        "uname=averyverylongusernamethatoverflowsthethirtytwobytefield",
        "size=1234567890123",
        "mtime=1600000000.123456789"
    };

    TEST_CASE("pax attributes override every header field they name");
    tb_init(&tb);
    tb_pax(&tb, 'x', records, sizeof(records) / sizeof(records[0]));
    tb_file(&tb, "short.txt", NULL, TB_USTAR);
    tb_end(&tb);

    e = first_entry(&tb, &src, &reader);
    CHECK(e != NULL);
    if (e) {
        CHECK_STR(e->path, "a/path/far/longer/than/the/header/could/ever/hold/file.txt");
        CHECK_STR(e->path_source, "pax path");
        CHECK_INT(e->uid, 4294967296LL);
        CHECK_INT(e->gid, 70000);
        CHECK_STR(e->uname, "averyverylongusernamethatoverflowsthethirtytwobytefield");
        CHECK_INT(e->size, 1234567890123LL);
        CHECK_INT(e->format, TMD_FMT_PAX);
        /* Sub-second precision only ever arrives this way. */
        CHECK_INT(e->mtime.sec, 1600000000);
        CHECK_INT(e->mtime.nsec, 123456789);
    }
    done(src, reader);
    tb_free(&tb);
}

static void test_pax_fractions(void)
{
    struct tarbuild         tb;
    struct tmd_source      *src;
    struct tmd_reader      *reader;
    const struct tmd_entry *e;

    TEST_CASE("a short fraction is scaled to nanoseconds, not read as one");
    {
        static const char *const records[] = { "mtime=1600000000.5" };
        tb_init(&tb);
        tb_pax(&tb, 'x', records, 1);
        tb_file(&tb, "f", NULL, TB_USTAR);
        tb_end(&tb);
        e = first_entry(&tb, &src, &reader);
        CHECK(e != NULL);
        if (e) {
            CHECK_INT(e->mtime.nsec, 500000000);
        }
        done(src, reader);
        tb_free(&tb);
    }

    TEST_CASE("more than nine fractional digits are dropped, not rounded up");
    {
        static const char *const records[] = { "mtime=1600000000.1234567891234" };
        tb_init(&tb);
        tb_pax(&tb, 'x', records, 1);
        tb_file(&tb, "f", NULL, TB_USTAR);
        tb_end(&tb);
        e = first_entry(&tb, &src, &reader);
        CHECK(e != NULL);
        if (e) {
            CHECK_INT(e->mtime.nsec, 123456789);
        }
        done(src, reader);
        tb_free(&tb);
    }

    TEST_CASE("a negative pax time (a file from before 1970) survives");
    {
        static const char *const records[] = { "mtime=-86400" };
        tb_init(&tb);
        tb_pax(&tb, 'x', records, 1);
        tb_file(&tb, "f", NULL, TB_USTAR);
        tb_end(&tb);
        e = first_entry(&tb, &src, &reader);
        CHECK(e != NULL);
        if (e) {
            CHECK_INT(e->mtime.sec, -86400);
        }
        done(src, reader);
        tb_free(&tb);
    }
}

static void test_pax_values_with_newlines(void)
{
    struct tarbuild         tb;
    struct tmd_source      *src;
    struct tmd_reader      *reader;
    const struct tmd_entry *e;
    static const char *const records[] = {
        "comment=first line\nsecond line",
        "path=after.txt"
    };

    TEST_CASE("a value containing a newline is read by its length prefix");
    /* Splitting the header on '\n' is the bug every first implementation of
     * pax has, and this is the archive that exposes it: the record after the
     * embedded newline would be lost, so `path` would never be applied. */
    tb_init(&tb);
    tb_pax(&tb, 'x', records, 2);
    tb_file(&tb, "before.txt", NULL, TB_USTAR);
    tb_end(&tb);

    e = first_entry(&tb, &src, &reader);
    CHECK(e != NULL);
    if (e) {
        CHECK_STR(e->path, "after.txt");
        CHECK_STR(pax_value(e, "comment"), "first line\nsecond line");
    }
    done(src, reader);
    tb_free(&tb);
}

static void test_pax_global(void)
{
    struct tarbuild          tb;
    struct tmd_source       *src;
    struct tmd_reader       *reader;
    const struct tmd_entry  *e;
    static const char *const global[] = { "uname=globaluser", "comment=global" };
    static const char *const local[] = { "uname=localuser" };

    TEST_CASE("a global header applies to every member; a local one overrides it");
    tb_init(&tb);
    tb_pax(&tb, 'g', global, 2);
    tb_file(&tb, "first.txt", NULL, TB_USTAR);
    tb_pax(&tb, 'x', local, 1);
    tb_file(&tb, "second.txt", NULL, TB_USTAR);
    tb_end(&tb);

    src = tmd_source_open_memory(tb.buf.data, tb.buf.len, "(test)");
    reader = tmd_reader_new(src);
    CHECK_INT(tmd_reader_next(reader, &e), 1);
    CHECK_STR(e->path, "first.txt");
    CHECK_STR(e->uname, "globaluser");
    CHECK_INT(tmd_reader_next(reader, &e), 1);
    CHECK_STR(e->path, "second.txt");
    /* The local record is added after the global one and wins because the
     * lookup takes the last value, not the first. */
    CHECK_STR(e->uname, "localuser");
    CHECK_STR(pax_value(e, "comment"), "global");
    CHECK_INT(tmd_reader_archive(reader)->npax_global, 2);
    done(src, reader);
    tb_free(&tb);
}

static void test_pax_malformed(void)
{
    struct tarbuild    tb;
    struct tmd_source *src;
    struct tmd_reader *reader;
    struct tb_hdr      h;
    const struct tmd_entry *e;

    TEST_CASE("a pax record with no length prefix is reported, not parsed");
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "PaxHeader/f";
    h.magic = TB_USTAR;
    h.typeflag = 'x';
    h.size = strlen("path=nope\n");
    tb_header(&tb, &h);
    tb_data(&tb, "path=nope\n", strlen("path=nope\n"));
    tb_file(&tb, "real.txt", NULL, TB_USTAR);
    tb_end(&tb);

    src = tmd_source_open_memory(tb.buf.data, tb.buf.len, "(test)");
    reader = tmd_reader_new(src);
    CHECK_INT(tmd_reader_next(reader, &e), 1);
    /* The header's own name stands, because nothing valid overrode it. */
    CHECK_STR(e->path, "real.txt");
    CHECK(tmd_reader_archive(reader)->nwarnings > 0);
    CHECK_CONTAINS(tmd_reader_archive(reader)->warnings[0].text, "pax record");
    done(src, reader);
    tb_free(&tb);

    TEST_CASE("a record whose declared length runs past the header is refused");
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "PaxHeader/f";
    h.magic = TB_USTAR;
    h.typeflag = 'x';
    h.size = strlen("9999 path=nope\n");
    tb_header(&tb, &h);
    tb_data(&tb, "9999 path=nope\n", strlen("9999 path=nope\n"));
    tb_file(&tb, "real.txt", NULL, TB_USTAR);
    tb_end(&tb);

    src = tmd_source_open_memory(tb.buf.data, tb.buf.len, "(test)");
    reader = tmd_reader_new(src);
    CHECK_INT(tmd_reader_next(reader, &e), 1);
    CHECK_STR(e->path, "real.txt");
    CHECK(tmd_reader_archive(reader)->nwarnings > 0);
    done(src, reader);
    tb_free(&tb);

    TEST_CASE("a record with no '=' is refused");
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "PaxHeader/f";
    h.magic = TB_USTAR;
    h.typeflag = 'x';
    h.size = strlen("10 nokeyval\n");
    tb_header(&tb, &h);
    tb_data(&tb, "10 nokeyval\n", strlen("10 nokeyval\n"));
    tb_file(&tb, "real.txt", NULL, TB_USTAR);
    tb_end(&tb);
    src = tmd_source_open_memory(tb.buf.data, tb.buf.len, "(test)");
    reader = tmd_reader_new(src);
    CHECK_INT(tmd_reader_next(reader, &e), 1);
    CHECK(tmd_reader_archive(reader)->nwarnings > 0);
    done(src, reader);
    tb_free(&tb);

    TEST_CASE("a pax size that is not a number leaves the header's size alone");
    {
        static const char *const records[] = { "size=not-a-number" };
        tb_init(&tb);
        tb_pax(&tb, 'x', records, 1);
        tb_file(&tb, "f.txt", "12345678", TB_USTAR);
        tb_end(&tb);
        src = tmd_source_open_memory(tb.buf.data, tb.buf.len, "(test)");
        reader = tmd_reader_new(src);
        CHECK_INT(tmd_reader_next(reader, &e), 1);
        CHECK_INT(e->size, 8);
        CHECK(e->nwarnings > 0);
        done(src, reader);
        tb_free(&tb);
    }
}

static void test_pax_sparse(void)
{
    struct tarbuild          tb;
    struct tmd_source       *src;
    struct tmd_reader       *reader;
    const struct tmd_entry  *e;

    TEST_CASE("GNU sparse 0.1: the map and the real name are pax attributes");
    {
        static const char *const records[] = {
            "GNU.sparse.major=0",
            "GNU.sparse.minor=1",
            "GNU.sparse.name=real/sparse.img",
            "GNU.sparse.realsize=1048576",
            "GNU.sparse.map=0,512,1048064,512"
        };
        tb_init(&tb);
        tb_pax(&tb, 'x', records, 5);
        tb_file(&tb, "GNUSparseFile.0/sparse.img", NULL, TB_USTAR);
        tb_end(&tb);

        e = first_entry(&tb, &src, &reader);
        CHECK(e != NULL);
        if (e) {
            CHECK(e->is_sparse);
            /* The header name is a placeholder; the real one is in the pax. */
            CHECK_STR(e->path, "real/sparse.img");
            CHECK_STR(e->path_source, "GNU.sparse.name");
            CHECK_INT(e->realsize, 1048576);
            CHECK_INT(e->size, 1048576);
            CHECK_INT(e->nsparse, 2);
        }
        done(src, reader);
        tb_free(&tb);
    }

    TEST_CASE("GNU sparse 1.0: the map is in the member's own payload");
    {
        static const char *const records[] = {
            "GNU.sparse.major=1",
            "GNU.sparse.minor=0",
            "GNU.sparse.name=big.img",
            "GNU.sparse.realsize=2097152"
        };
        struct tb_hdr h;
        char          map[TMD_BLOCK_SIZE];

        memset(map, 0, sizeof(map));
        /* Two segments: a count, then offset/length on their own lines. */
        (void)snprintf(map, sizeof(map), "2\n0\n1024\n2096128\n1024\n");

        tb_init(&tb);
        tb_pax(&tb, 'x', records, 4);
        memset(&h, 0, sizeof(h));
        h.name = "GNUSparseFile.0/big.img";
        h.magic = TB_USTAR;
        h.typeflag = '0';
        h.mode = 0644;
        /* The map block plus two blocks of data. */
        h.size = 3 * TMD_BLOCK_SIZE;
        tb_header(&tb, &h);
        tb_raw(&tb, map, sizeof(map));
        tb_zeros(&tb, 2 * TMD_BLOCK_SIZE);
        tb_file(&tb, "after.txt", "still here\n", TB_USTAR);
        tb_end(&tb);

        src = tmd_source_open_memory(tb.buf.data, tb.buf.len, "(test)");
        reader = tmd_reader_new(src);
        CHECK_INT(tmd_reader_next(reader, &e), 1);
        CHECK(e->is_sparse);
        CHECK_STR(e->path, "big.img");
        CHECK_INT(e->nsparse, 2);
        if (e->nsparse == 2) {
            CHECK_INT(e->sparse[0].offset, 0);
            CHECK_INT(e->sparse[0].numbytes, 1024);
            CHECK_INT(e->sparse[1].offset, 2096128);
            CHECK_INT(e->sparse[1].numbytes, 1024);
        }
        /* Reading the map out of the payload must not lose the reader's place:
         * the member after it has to be found exactly where it is. */
        CHECK_INT(tmd_reader_next(reader, &e), 1);
        CHECK_STR(e->path, "after.txt");
        done(src, reader);
        tb_free(&tb);
    }
}

static void test_writer_fingerprints(void)
{
    struct tarbuild         tb;
    struct tmd_source      *src;
    struct tmd_reader      *reader;
    const struct tmd_entry *e;

    TEST_CASE("a LIBARCHIVE.* key identifies bsdtar");
    {
        static const char *const records[] = { "LIBARCHIVE.xattr.user.test=dmFs" };
        tb_init(&tb);
        tb_pax(&tb, 'x', records, 1);
        tb_file(&tb, "f", NULL, TB_USTAR);
        tb_end(&tb);
        e = first_entry(&tb, &src, &reader);
        CHECK(e != NULL);
        CHECK_STR(tmd_reader_archive(reader)->writer, "libarchive (bsdtar)");
        done(src, reader);
        tb_free(&tb);
    }

    TEST_CASE("GNU.sparse.* alone does NOT identify GNU tar");
    /* libarchive writes the same keys, because GNU's is the only pax sparse
     * format there is. Trusting it reported every bsdtar archive containing a
     * sparse file as GNU tar. */
    {
        static const char *const records[] = {
            "GNU.sparse.major=1", "GNU.sparse.minor=0", "GNU.sparse.name=x"
        };
        tb_init(&tb);
        tb_pax(&tb, 'x', records, 3);
        tb_file(&tb, "f", NULL, TB_USTAR);
        tb_end(&tb);
        e = first_entry(&tb, &src, &reader);
        CHECK(e != NULL);
        CHECK(tmd_reader_archive(reader)->writer == NULL);
        done(src, reader);
        tb_free(&tb);
    }

    TEST_CASE("the name of the pax header block tells the writers apart");
    /* An ordinary pax archive's attribute keys are "path" and "mtime", which
     * everybody writes — the only fingerprint it carries is the name each
     * implementation invented for its own extended-header member. GNU tar
     * writes "PaxHeaders/", libarchive writes "PaxHeader/" without the s. */
    {
        static const char *const records[] = { "path=file.txt" };
        tb_init(&tb);
        tb_pax_named(&tb, 'x', "t/PaxHeaders/file.txt", records, 1);
        tb_file(&tb, "file.txt", NULL, TB_USTAR);
        tb_end(&tb);
        e = first_entry(&tb, &src, &reader);
        CHECK(e != NULL);
        CHECK_STR(tmd_reader_archive(reader)->writer, "GNU tar");
        done(src, reader);
        tb_free(&tb);

        tb_init(&tb);
        tb_pax_named(&tb, 'x', "t/PaxHeader/file.txt", records, 1);
        tb_file(&tb, "file.txt", NULL, TB_USTAR);
        tb_end(&tb);
        e = first_entry(&tb, &src, &reader);
        CHECK(e != NULL);
        CHECK_STR(tmd_reader_archive(reader)->writer, "libarchive (bsdtar)");
        done(src, reader);
        tb_free(&tb);
    }

    TEST_CASE("a GNU.* key that is not GNU.sparse.* does identify GNU tar");
    {
        static const char *const records[] = { "GNU.dumpdir=Yfile" };
        tb_init(&tb);
        tb_pax(&tb, 'x', records, 1);
        tb_file(&tb, "f", NULL, TB_USTAR);
        tb_end(&tb);
        e = first_entry(&tb, &src, &reader);
        CHECK(e != NULL);
        CHECK_STR(tmd_reader_archive(reader)->writer, "GNU tar");
        done(src, reader);
        tb_free(&tb);
    }
}

void test_pax(void)
{
    test_pax_overrides();
    test_pax_fractions();
    test_pax_values_with_newlines();
    test_pax_global();
    test_pax_malformed();
    test_pax_sparse();
    test_writer_fingerprints();
}
