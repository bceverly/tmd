/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * --hash and the things that ride along with it: the digests against their
 * published test vectors, hashing through the reader, the owner tally, the
 * nested-archive hint and the date range.
 */
#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "hash.h"
#include "source.h"
#include "tar.h"
#include "tarbuild.h"
#include "util.h"

static void digest_of(enum tmd_hash algo, const char *s, char hex[TMD_HASH_HEX_MAX])
{
    struct tmd_hash_ctx ctx;

    tmd_hash_init(&ctx, algo);
    tmd_hash_update(&ctx, s, strlen(s));
    tmd_hash_final(&ctx, hex);
}

/* Accept every member except the one named in ctx. */
static bool all_but(const struct tmd_entry *e, const void *ctx)
{
    return strcmp(e->path, (const char *)ctx) != 0;
}

static void test_vectors(void)
{
    char hex[TMD_HASH_HEX_MAX];

    /* RFC 1321, appendix A.5. */
    TEST_CASE("md5 matches the RFC 1321 test suite");
    digest_of(TMD_HASH_MD5, "", hex);
    CHECK_STR(hex, "d41d8cd98f00b204e9800998ecf8427e");
    digest_of(TMD_HASH_MD5, "abc", hex);
    CHECK_STR(hex, "900150983cd24fb0d6963f7d28e17f72");
    digest_of(TMD_HASH_MD5, "message digest", hex);
    CHECK_STR(hex, "f96b697d7cb7938d525a2f31aaf161d0");
    digest_of(TMD_HASH_MD5, "12345678901234567890123456789012345678901234567890"
                            "123456789012345678901234567890", hex);
    CHECK_STR(hex, "57edf4a22be3c955ac49da2e2107b67a");

    /* FIPS 180-4 examples. */
    TEST_CASE("sha256 matches the FIPS 180-4 examples");
    digest_of(TMD_HASH_SHA256, "", hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    digest_of(TMD_HASH_SHA256, "abc", hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    digest_of(TMD_HASH_SHA256,
              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", hex);
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /*
     * The same bytes fed in uneven pieces, across block boundaries, must give
     * the same digest -- the reader hands over whatever a read returned.
     */
    TEST_CASE("a digest does not depend on how the input was split");
    {
        static const enum tmd_hash algos[] = { TMD_HASH_MD5, TMD_HASH_SHA256 };
        char   data[1000];
        char   whole[TMD_HASH_HEX_MAX];
        char   parts[TMD_HASH_HEX_MAX];
        size_t a;
        size_t i;

        for (i = 0; i < sizeof(data); i++)
        {
            data[i] = (char)(i * 7 + 3);
        }
        for (a = 0; a < 2; a++)
        {
            struct tmd_hash_ctx ctx;
            size_t              at = 0;
            size_t              step = 1;

            tmd_hash_init(&ctx, algos[a]);
            tmd_hash_update(&ctx, data, sizeof(data));
            tmd_hash_final(&ctx, whole);

            tmd_hash_init(&ctx, algos[a]);
            while (at < sizeof(data))
            {
                size_t n = step < sizeof(data) - at ? step : sizeof(data) - at;

                tmd_hash_update(&ctx, data + at, n);
                at += n;
                step = step * 3 % 97 + 1;
            }
            tmd_hash_final(&ctx, parts);
            CHECK_STR(parts, whole);
        }
    }

    /* Lengths either side of the 56-byte padding boundary, where the length
     * field spills into a second block. */
    TEST_CASE("the padding boundary at 55, 56 and 64 bytes is handled");
    digest_of(TMD_HASH_SHA256,
              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", hex);
    CHECK_STR(hex, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    digest_of(TMD_HASH_SHA256,
              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", hex);
    CHECK_STR(hex, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    digest_of(TMD_HASH_MD5,
              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", hex);
    CHECK_STR(hex, "014842d480b571495a4a0363793f7367");

    TEST_CASE("algorithm names parse in any case, and nothing else does");
    {
        enum tmd_hash h = TMD_HASH_NONE;

        CHECK(tmd_hash_parse("MD5", &h) && h == TMD_HASH_MD5);
        CHECK(tmd_hash_parse("sha256", &h) && h == TMD_HASH_SHA256);
        CHECK(tmd_hash_parse("SHA-256", &h) && h == TMD_HASH_SHA256);
        CHECK(!tmd_hash_parse("sha1", &h));
        CHECK(!tmd_hash_parse("md", &h));
        CHECK_INT(tmd_hash_hex_len(TMD_HASH_MD5), 32);
        CHECK_INT(tmd_hash_hex_len(TMD_HASH_SHA256), 64);
    }
}

static void test_reader_hashing(void)
{
    struct tarbuild          tb;
    struct tmd_source       *src;
    struct tmd_reader       *r;
    const struct tmd_entry  *e;
    char                     expect[TMD_HASH_HEX_MAX];
    int                      n = 0;

    TEST_CASE("the reader hashes what it is asked to, and the next header still parses");
    tb_init(&tb);
    tb_file(&tb, "a.txt", "hello\n", TB_USTAR);
    tb_file(&tb, "skip.txt", "not hashed\n", TB_USTAR);
    tb_file(&tb, "empty", "", TB_USTAR);
    tb_file(&tb, "b.txt", "the last one\n", TB_USTAR);
    tb_end(&tb);
    src = tmd_source_open_memory(tb.buf.data, tb.buf.len, "(test)");
    r = tmd_reader_new(src);
    tmd_reader_hash(r, TMD_HASH_MD5, all_but, "skip.txt");
    while (tmd_reader_next(r, &e) == 1)
    {
        n++;
        if (strcmp(e->path, "a.txt") == 0)
        {
            digest_of(TMD_HASH_MD5, "hello\n", expect);
            CHECK(e->content_hashed);
            CHECK_STR(e->content_hash, expect);
        } else if (strcmp(e->path, "skip.txt") == 0)
        {
            CHECK(!e->content_hashed);
        } else if (strcmp(e->path, "empty") == 0)
        {
            /* An empty file has a digest, and it is the empty-input one. */
            CHECK(e->content_hashed);
            CHECK_STR(e->content_hash, "d41d8cd98f00b204e9800998ecf8427e");
        } else
        {
            digest_of(TMD_HASH_MD5, "the last one\n", expect);
            CHECK_STR(e->path, "b.txt");
            CHECK_STR(e->content_hash, expect);
            /* The offsets still line up after content was read rather than
             * skipped: a.txt, skip.txt and empty took 2 + 2 + 1 blocks. */
            CHECK_INT(e->offset, 5 * 512);
        }
    }
    CHECK_INT(n, 4);
    CHECK_INT(tmd_reader_archive(r)->hashed, 3);
    CHECK(tmd_reader_archive(r)->eof_marker);
    tmd_reader_free(r);
    tmd_source_close(src);
    tb_free(&tb);

    TEST_CASE("a member cut short gets no digest, and says why");
    {
        struct tb_hdr h;
        bool          warned = false;

        tb_init(&tb);
        memset(&h, 0, sizeof(h));
        h.name = "big.bin";
        h.magic = TB_USTAR;
        h.typeflag = '0';
        h.size = 100000;
        tb_header(&tb, &h);
        tb_raw(&tb, "only a little data", 18);
        src = tmd_source_open_memory(tb.buf.data, tb.buf.len, "(test)");
        r = tmd_reader_new(src);
        tmd_reader_hash(r, TMD_HASH_SHA256, NULL, NULL);
        if (tmd_reader_next(r, &e) == 1)
        {
            size_t i;

            CHECK(!e->content_hashed);
            for (i = 0; i < e->nwarnings; i++)
            {
                if (strcmp(e->warnings[i].code, "hash-data-truncated") == 0)
                {
                    warned = true;
                }
            }
        }
        CHECK(warned);
        tmd_reader_free(r);
        tmd_source_close(src);
        tb_free(&tb);
    }

    TEST_CASE("without --hash, nothing is hashed and the archive says so");
    tb_init(&tb);
    tb_file(&tb, "a.txt", "hello\n", TB_USTAR);
    tb_end(&tb);
    src = tmd_source_open_memory(tb.buf.data, tb.buf.len, "(test)");
    r = tmd_reader_new(src);
    if (tmd_reader_next(r, &e) == 1)
    {
        CHECK(!e->content_hashed);
    }
    CHECK(tmd_reader_archive(r)->hash == TMD_HASH_NONE);
    CHECK_INT(tmd_reader_archive(r)->hashed, 0);
    tmd_reader_free(r);
    tmd_source_close(src);
    tb_free(&tb);
}

static void test_owners_and_nesting(void)
{
    struct tarbuild            tb;
    struct tmd_source         *src;
    struct tmd_reader         *r;
    const struct tmd_entry    *e;
    const struct tmd_features *f;
    struct tb_hdr              h;
    int                        i;

    TEST_CASE("owners are tallied, and the odd one out is visible");
    tb_init(&tb);
    for (i = 0; i < 3; i++)
    {
        memset(&h, 0, sizeof(h));
        h.name = i == 0 ? "home/is/a" : i == 1 ? "home/is/b" : "home/mdounin/.procmail/rc";
        h.magic = TB_USTAR;
        h.typeflag = '0';
        h.uname = i == 2 ? "aershov" : "is";
        h.gname = i == 2 ? "staff" : "is";
        h.mode = 0644;
        tb_header(&tb, &h);
    }
    /* No names at all: the tally falls back to the numbers. */
    memset(&h, 0, sizeof(h));
    h.name = "numeric";
    h.magic = TB_USTAR;
    h.typeflag = '0';
    h.uid = 1108;
    h.gid = 20;
    tb_header(&tb, &h);
    tb_file(&tb, "deps/inner.tar.gz", "x", TB_USTAR);
    tb_end(&tb);
    src = tmd_source_open_memory(tb.buf.data, tb.buf.len, "(test)");
    r = tmd_reader_new(src);
    while (tmd_reader_next(r, &e) == 1)
    {
        if (strcmp(e->path, "deps/inner.tar.gz") == 0)
        {
            CHECK(e->nested_archive);
        }
    }
    f = &tmd_reader_archive(r)->features;
    /* Four: the three above, and whatever tb_file gives deps/inner.tar.gz. */
    CHECK_INT(f->nowners, 4);
    if (f->nowners == 4)
    {
        CHECK_STR(f->owners[0].owner, "is/is");
        CHECK_INT(f->owners[0].count, 2);
        CHECK_STR(f->owners[1].owner, "aershov/staff");
        CHECK_STR(f->owners[2].owner, "1108/20");
    }
    TEST_CASE("a member named like an archive is counted as nested");
    CHECK_INT(f->nested_archives, 1);
    tmd_reader_free(r);
    tmd_source_close(src);
    tb_free(&tb);

    TEST_CASE("archive names are recognized by extension, case-insensitively");
    CHECK(tmd_looks_like_archive("a/b.tar"));
    CHECK(tmd_looks_like_archive("b.TAR.GZ"));
    CHECK(tmd_looks_like_archive("x.tgz"));
    CHECK(tmd_looks_like_archive("pkg.deb"));
    CHECK(tmd_looks_like_archive("bundle.zip"));
    CHECK(!tmd_looks_like_archive("notes.txt.gz")); /* a compressed file */
    CHECK(!tmd_looks_like_archive(".tar"));          /* a dotfile */
    CHECK(!tmd_looks_like_archive("dir/.tar"));
    CHECK(!tmd_looks_like_archive("guitar"));
    CHECK(!tmd_looks_like_archive(NULL));
}

static void test_date_range(void)
{
    struct tmd_options opt;
    struct tmd_entry   e;

    memset(&e, 0, sizeof(e));
    e.path = (char *)"x";
    e.mtime.present = true;

    TEST_CASE("--mtime-before is strict and --mtime-after inclusive: [after, before)");
    memset(&opt, 0, sizeof(opt));
    opt.have_mtime_before = true;
    opt.mtime_before = 1000;
    e.mtime.sec = 999;
    CHECK(tmd_entry_selected(&opt, &e));
    e.mtime.sec = 1000;
    CHECK(!tmd_entry_selected(&opt, &e));
    opt.have_mtime_after = true;
    opt.mtime_after = 500;
    e.mtime.sec = 500;
    CHECK(tmd_entry_selected(&opt, &e));
    e.mtime.sec = 499;
    CHECK(!tmd_entry_selected(&opt, &e));
    CHECK(tmd_options_filtering(&opt));

    TEST_CASE("a member with no mtime matches no date range");
    e.mtime.present = false;
    e.mtime.sec = 700;
    CHECK(!tmd_entry_selected(&opt, &e));

    TEST_CASE("a date range and a pattern must both match");
    {
        const char *patterns[1] = { "y" };

        e.mtime.present = true;
        opt.match = patterns;
        opt.nmatch = 1;
        CHECK(!tmd_entry_selected(&opt, &e)); /* in range, wrong name */
        e.path = (char *)"y";
        CHECK(tmd_entry_selected(&opt, &e));
    }
}

static void test_exclude_and_size(void)
{
    struct tmd_options opt;
    struct tmd_entry   e;
    const char        *ex[2];
    const char        *pat[1];

    TEST_CASE("--exclude with a bare name drops that name at any depth, and all beneath it");
    CHECK(tmd_path_excluded("acct/Logs/", "Logs"));
    CHECK(tmd_path_excluded("acct/Logs/2016/msg1", "Logs"));
    CHECK(tmd_path_excluded("Logs", "Logs"));
    CHECK(tmd_path_excluded("a/b/c.tmp", "*.tmp"));
    CHECK(!tmd_path_excluded("acct/Logsheet.txt", "Logs"));
    CHECK(!tmd_path_excluded("acct/Inbox/a.eml", "Logs"));

    TEST_CASE("--exclude with a slash takes -m's subtree rule");
    CHECK(tmd_path_excluded("acct/Logs/2016/msg1", "*/Logs/*"));
    CHECK(!tmd_path_excluded("acct/Logs/", "*/Logs/*")); /* the contents, not the dir */
    CHECK(tmd_path_excluded("acct/Logs/", "*/Logs"));
    CHECK(tmd_path_excluded("/acct/Logs/x", "acct/Logs"));
    CHECK(!tmd_path_excluded("acct/Logsheet", "acct/Logs"));

    memset(&opt, 0, sizeof(opt));
    memset(&e, 0, sizeof(e));
    TEST_CASE("--exclude wins over -m");
    pat[0] = "acct/";
    ex[0] = "Logs";
    opt.match = pat;
    opt.nmatch = 1;
    opt.exclude = ex;
    opt.nexclude = 1;
    e.path = (char *)"acct/Inbox/a.eml";
    CHECK(tmd_entry_selected(&opt, &e));
    e.path = (char *)"acct/Logs/msg";
    CHECK(!tmd_entry_selected(&opt, &e));
    CHECK(!tmd_path_selected(&opt, "acct/Logs/msg"));
    CHECK(tmd_options_filtering(&opt));

    TEST_CASE("--min-size and --max-size are inclusive bounds on the extracted size");
    memset(&opt, 0, sizeof(opt));
    e.path = (char *)"f";
    opt.have_min_size = true;
    opt.min_size = 1024;
    opt.have_max_size = true;
    opt.max_size = 2048;
    e.size = 1024;
    CHECK(tmd_entry_selected(&opt, &e));
    e.size = 2048;
    CHECK(tmd_entry_selected(&opt, &e));
    e.size = 1023;
    CHECK(!tmd_entry_selected(&opt, &e));
    e.size = 2049;
    CHECK(!tmd_entry_selected(&opt, &e));
    CHECK(tmd_options_filtering(&opt));
}

void test_hash(void)
{
    test_vectors();
    test_reader_hashing();
    test_owners_and_nesting();
    test_date_range();
    test_exclude_and_size();
}
