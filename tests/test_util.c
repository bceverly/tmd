/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

static void test_numeric_fields(void)
{
    uint64_t value;
    int64_t  signed_value;
    char     field[12];

    TEST_CASE("octal fields in every padding style tar has used");
    CHECK(tmd_parse_num("0000644\0", 8, &value) && value == 0644);
    CHECK(tmd_parse_num("644    ", 7, &value) && value == 0644);
    CHECK(tmd_parse_num("   644 ", 7, &value) && value == 0644);
    CHECK(tmd_parse_num("0000000000\0 ", 12, &value) && value == 0);

    TEST_CASE("a field that is not octal is rejected rather than truncated");
    /* The failure that matters: "0644x" must not silently become 0644. */
    CHECK(!tmd_parse_num("0644x   ", 8, &value));
    CHECK(!tmd_parse_num("99999999", 8, &value));
    CHECK(!tmd_parse_num("        ", 8, &value));
    CHECK(!tmd_parse_num("", 0, &value));

    TEST_CASE("an empty field is recognized as carrying no value");
    CHECK(tmd_field_empty("        ", 8));
    CHECK(tmd_field_empty("\0\0\0\0\0\0\0\0", 8));
    CHECK(!tmd_field_empty("0000644\0", 8));

    TEST_CASE("base-256 fields, positive and negative");
    /* 0x80 marks it; the rest is big-endian. 12 bytes holding 8589934592. */
    memset(field, 0, sizeof(field));
    field[0] = (char)0x80;
    field[7] = 0x02;
    CHECK(tmd_parse_num(field, 12, &value) && value == 0x200000000ULL);

    /* All-ones is -1: GNU writes 0xff in every byte. */
    memset(field, (char)0xff, sizeof(field));
    CHECK(tmd_parse_num_signed(field, 12, &signed_value) && signed_value == -1);
    /* ...and an unsigned caller must refuse it rather than report 2^64-1. */
    CHECK(!tmd_parse_num(field, 12, &value));

    TEST_CASE("a base-256 value too large for 64 bits is refused, not truncated");
    memset(field, 0, sizeof(field));
    field[0] = (char)0x80;
    field[1] = 0x01; /* a bit above the 64 bits an int64_t can hold */
    CHECK(!tmd_parse_num(field, 12, &value));
}

static void test_field_strings(void)
{
    char out[101];

    TEST_CASE("a field that fills its width has no terminator to find");
    {
        char full[8];
        memset(full, 'a', sizeof(full));
        tmd_field_str(full, 8, out);
        CHECK_INT(strlen(out), 8);
        CHECK_STR(out, "aaaaaaaa");
    }

    TEST_CASE("a short field stops at its NUL");
    tmd_field_str("abc\0defg", 8, out);
    CHECK_STR(out, "abc");
}

static void test_buffer(void)
{
    struct tmd_buf b;
    char          *taken;

    TEST_CASE("the buffer grows and stays NUL-terminated");
    tmd_buf_init(&b);
    tmd_buf_addstr(&b, "hello");
    tmd_buf_addc(&b, ' ');
    tmd_buf_add(&b, "world", 5);
    tmd_buf_addf(&b, " %d", 42);
    CHECK_STR(b.data, "hello world 42");
    CHECK_INT(b.len, 14);
    CHECK_INT(b.data[b.len], 0);
    tmd_buf_free(&b);

    TEST_CASE("growing past the initial capacity keeps the contents");
    tmd_buf_init(&b);
    for (int i = 0; i < 500; i++) {
        tmd_buf_addc(&b, 'x');
    }
    CHECK_INT(b.len, 500);
    CHECK_INT(strlen(b.data), 500);
    tmd_buf_free(&b);

    TEST_CASE("detaching a buffer nothing was added to still yields a string");
    tmd_buf_init(&b);
    taken = tmd_buf_detach(&b);
    CHECK_STR(taken, "");
    CHECK(b.data == NULL);
    free(taken);

    TEST_CASE("embedded NULs are kept by add() and counted in len");
    tmd_buf_init(&b);
    tmd_buf_add(&b, "a\0b", 3);
    CHECK_INT(b.len, 3);
    CHECK_INT(b.data[1], 0);
    CHECK_INT(b.data[2], 'b');
    tmd_buf_free(&b);
}

static void test_strings(void)
{
    char *s;

    TEST_CASE("xstrndup stops at the first NUL or the limit");
    s = tmd_xstrndup("abcdef", 3);
    CHECK_STR(s, "abc");
    free(s);
    s = tmd_xstrndup("ab\0cdef", 6);
    CHECK_STR(s, "ab");
    free(s);

    TEST_CASE("xasprintf sizes the result exactly");
    s = tmd_xasprintf("%s/%d", "path", 7);
    CHECK_STR(s, "path/7");
    free(s);
}

static void test_utf8(void)
{
    TEST_CASE("valid UTF-8 is accepted");
    CHECK(tmd_utf8_valid("plain ascii", 11));
    CHECK(tmd_utf8_valid("\xc3\xa9", 2));                 /* e-acute      */
    CHECK(tmd_utf8_valid("\xe2\x82\xac", 3));             /* euro sign    */
    CHECK(tmd_utf8_valid("\xf0\x9f\x92\xa9", 4));         /* an emoji     */
    CHECK(tmd_utf8_valid("", 0));

    TEST_CASE("the encodings that are valid bytes but not valid text are refused");
    CHECK(!tmd_utf8_valid("\xc3", 1));             /* truncated sequence   */
    CHECK(!tmd_utf8_valid("\x80", 1));             /* a bare continuation  */
    CHECK(!tmd_utf8_valid("\xc0\xaf", 2));         /* over-long '/'        */
    CHECK(!tmd_utf8_valid("\xed\xa0\x80", 3));     /* a surrogate half     */
    CHECK(!tmd_utf8_valid("\xf5\x80\x80\x80", 4)); /* past U+10FFFF        */
    CHECK(!tmd_utf8_valid("\xe2\x82", 2));         /* one byte short       */
    CHECK(!tmd_utf8_valid("\xff\xfe", 2));         /* not a lead byte      */
}

static void test_mode_string(void)
{
    char mode[11];

    TEST_CASE("the type character comes from the kind, not from the mode word");
    tmd_mode_string(0644, TMD_KIND_FILE, mode);
    CHECK_STR(mode, "-rw-r--r--");
    tmd_mode_string(0755, TMD_KIND_DIR, mode);
    CHECK_STR(mode, "drwxr-xr-x");
    tmd_mode_string(0777, TMD_KIND_SYMLINK, mode);
    CHECK_STR(mode, "lrwxrwxrwx");
    tmd_mode_string(0644, TMD_KIND_HARDLINK, mode);
    CHECK_STR(mode, "hrw-r--r--");
    tmd_mode_string(0660, TMD_KIND_BLOCKDEV, mode);
    CHECK_STR(mode, "brw-rw----");
    tmd_mode_string(0660, TMD_KIND_CHARDEV, mode);
    CHECK_STR(mode, "crw-rw----");
    tmd_mode_string(0644, TMD_KIND_FIFO, mode);
    CHECK_STR(mode, "prw-r--r--");

    TEST_CASE("setuid, setgid and sticky, upper-case when execute is not set");
    tmd_mode_string(04755, TMD_KIND_FILE, mode);
    CHECK_STR(mode, "-rwsr-xr-x");
    tmd_mode_string(04644, TMD_KIND_FILE, mode);
    CHECK_STR(mode, "-rwSr--r--");
    tmd_mode_string(02755, TMD_KIND_FILE, mode);
    CHECK_STR(mode, "-rwxr-sr-x");
    tmd_mode_string(01777, TMD_KIND_DIR, mode);
    CHECK_STR(mode, "drwxrwxrwt");
    tmd_mode_string(01666, TMD_KIND_DIR, mode);
    CHECK_STR(mode, "drw-rw-rwT");
}

static void test_human_size(void)
{
    char buf[32];

    TEST_CASE("sizes are rendered the way ls -h does");
    CHECK_STR(tmd_human_size(0, buf, sizeof(buf)), "0B");
    CHECK_STR(tmd_human_size(1023, buf, sizeof(buf)), "1023B");
    CHECK_STR(tmd_human_size(1024, buf, sizeof(buf)), "1.0K");
    CHECK_STR(tmd_human_size(1536, buf, sizeof(buf)), "1.5K");
    CHECK_STR(tmd_human_size(1024ULL * 1024, buf, sizeof(buf)), "1.0M");
    CHECK_STR(tmd_human_size(50ULL * 1024 * 1024, buf, sizeof(buf)), "50M");
    CHECK_STR(tmd_human_size(1024ULL * 1024 * 1024 * 1024, buf, sizeof(buf)), "1.0T");
}

static void test_rounding(void)
{
    TEST_CASE("rounding to a block boundary");
    CHECK_INT(tmd_round_up_blocks(0), 0);
    CHECK_INT(tmd_round_up_blocks(1), 512);
    CHECK_INT(tmd_round_up_blocks(512), 512);
    CHECK_INT(tmd_round_up_blocks(513), 1024);

    TEST_CASE("a size field of 2^64-1 saturates rather than wrapping to zero");
    /* Wrapping here would turn "skip the payload" into "skip nothing", and the
     * reader would read the same header forever. */
    CHECK(tmd_round_up_blocks(UINT64_MAX) > 0);
    CHECK_INT(tmd_round_up_blocks(UINT64_MAX) % 512, 0);
}

static void test_base64(void)
{
    static const struct {
        const char *plain;
        const char *encoded;
    } vectors[] = {
        /* RFC 4648 section 10, which exists precisely so an implementation can
         * be checked rather than believed. */
        { "",       ""         },
        { "f",      "Zg=="     },
        { "fo",     "Zm8="     },
        { "foo",    "Zm9v"     },
        { "foob",   "Zm9vYg==" },
        { "fooba",  "Zm9vYmE=" },
        { "foobar", "Zm9vYmFy" },
    };
    size_t i;

    TEST_CASE("base64 encodes the RFC 4648 test vectors");
    for (i = 0; i < sizeof(vectors) / sizeof(*vectors); i++) {
        char *got = tmd_base64_encode(vectors[i].plain, strlen(vectors[i].plain));

        CHECK_STR(got, vectors[i].encoded);
        free(got);
    }

    TEST_CASE("base64 decodes them back");
    for (i = 1; i < sizeof(vectors) / sizeof(*vectors); i++) {
        struct tmd_buf out;

        tmd_buf_init(&out);
        CHECK(tmd_base64_decode(vectors[i].encoded, &out));
        CHECK_STR(out.data ? out.data : "", vectors[i].plain);
        tmd_buf_free(&out);
    }

    TEST_CASE("every byte value survives the round trip");
    {
        unsigned char all[256];
        struct tmd_buf out;
        char *enc;
        size_t k;

        for (k = 0; k < sizeof(all); k++) {
            all[k] = (unsigned char)k;
        }
        enc = tmd_base64_encode(all, sizeof(all));
        tmd_buf_init(&out);
        CHECK(tmd_base64_decode(enc, &out));
        CHECK(out.len == sizeof(all));
        CHECK(out.data != NULL && memcmp(out.data, all, sizeof(all)) == 0);
        tmd_buf_free(&out);
        free(enc);
    }

    /* Strict on the way in: a value that is not base64 has to be reported as
     * such, so the caller can show the raw text instead of a plausible-looking
     * decode of something that was never encoded. */
    TEST_CASE("malformed base64 is rejected rather than half-decoded");
    {
        static const char *bad[] = {
            "Zm9vYmFy=",   /* padding where none belongs           */
            "Zg=",         /* length not a multiple of four        */
            "Zm9!",        /* a character outside the alphabet     */
            "=m9v",        /* padding at the front                 */
            "Z=9v",        /* padding in the middle                */
        };
        size_t k;

        for (k = 0; k < sizeof(bad) / sizeof(*bad); k++) {
            struct tmd_buf out;

            tmd_buf_init(&out);
            CHECK(!tmd_base64_decode(bad[k], &out));
            tmd_buf_free(&out);
        }
    }
}

static void test_utf8_offset(void)
{
    size_t at;

    TEST_CASE("a valid string reports no invalid byte");
    CHECK(!tmd_utf8_first_invalid("h\xc3\xa9llo", 6, &at));

    TEST_CASE("the reported offset points at the byte that breaks it");
    at = 999;
    CHECK(tmd_utf8_first_invalid("abc\xff", 4, &at));
    CHECK(at == 3);

    TEST_CASE("a truncated sequence is reported where it starts");
    at = 999;
    CHECK(tmd_utf8_first_invalid("ab\xc3", 3, &at));
    CHECK(at == 2);
}

static void test_path_matching(void)
{
    /*
     * find(1)'s rule, which is the whole of the design: a pattern with a slash
     * is matched against the path, one without it against the basename.
     */
    TEST_CASE("a bare name matches the basename at any depth");
    CHECK(tmd_path_matches("etc/nginx/nginx.conf", "nginx.conf"));
    CHECK(tmd_path_matches("nginx.conf", "nginx.conf"));
    CHECK(!tmd_path_matches("etc/nginx/nginx.conf", "nginx"));

    TEST_CASE("a glob matches the basename");
    CHECK(tmd_path_matches("etc/nginx/nginx.conf", "*.conf"));
    CHECK(!tmd_path_matches("etc/nginx/nginx.conf", "*.txt"));

    TEST_CASE("a pattern containing a slash matches the whole path");
    CHECK(tmd_path_matches("etc/nginx/nginx.conf", "etc/nginx/*"));
    CHECK(tmd_path_matches("var/logs/a.log", "*/logs/*"));
    /* Without the slash rule this would match on the basename and be true. */
    CHECK(!tmd_path_matches("etc/nginx/nginx.conf", "nginx/*.txt"));

    /*
     * A directory is stored with a trailing slash, so its basename is the
     * component before that slash. Getting this wrong makes every directory
     * match the empty string and nothing else.
     */
    TEST_CASE("a directory matches on the name before its trailing slash");
    CHECK(tmd_path_matches("etc/nginx/", "nginx"));
    CHECK(tmd_path_matches("etc/nginx/", "ngin*"));
    CHECK(!tmd_path_matches("etc/nginx/", "etc"));

    TEST_CASE("matching is case-sensitive");
    CHECK(!tmd_path_matches("etc/NGINX.conf", "nginx.conf"));
    CHECK(tmd_path_matches("etc/NGINX.conf", "NGINX.conf"));

    TEST_CASE("a basename longer than the stack buffer still matches");
    {
        char path[600];
        char pattern[600];
        size_t i;

        memcpy(path, "deep/", 5);
        for (i = 5; i < sizeof(path) - 1; i++) {
            path[i] = 'x';
        }
        path[sizeof(path) - 1] = '\0';
        memcpy(pattern, path + 5, sizeof(path) - 5);
        CHECK(tmd_path_matches(path, pattern));
    }
}

static void test_path_escapes(void)
{
    TEST_CASE("an ordinary path stays put");
    CHECK(!tmd_path_escapes("etc/nginx/nginx.conf"));
    CHECK(!tmd_path_escapes("./a/b"));
    CHECK(!tmd_path_escapes("a/"));

    TEST_CASE("an absolute path escapes");
    CHECK(tmd_path_escapes("/etc/passwd"));
    CHECK(tmd_path_escapes("//etc/passwd"));

    TEST_CASE("a traversal that leaves the tree escapes");
    CHECK(tmd_path_escapes("../etc/passwd"));
    CHECK(tmd_path_escapes("a/../../etc/passwd"));
    CHECK(tmd_path_escapes(".."));

    /*
     * The case a substring search for ".." gets wrong. `a/../b` extracts to
     * `b`, squarely inside the current directory, and calling it a bomb would
     * make the check cry wolf on archives that do this legitimately.
     */
    TEST_CASE("a traversal that comes back does not escape");
    CHECK(!tmd_path_escapes("a/../b"));
    CHECK(!tmd_path_escapes("a/b/../c"));
    CHECK(!tmd_path_escapes("a/b/../.."));

    TEST_CASE("a link target that leaves the tree escapes");
    CHECK(tmd_link_escapes("link", "/etc/passwd"));
    CHECK(tmd_link_escapes("link", "../outside"));
    CHECK(tmd_link_escapes("a/link", "../../outside"));

    /* A relative target is resolved against the directory the link is in, so
     * how deep the link sits decides whether the same target escapes. */
    TEST_CASE("a link target is resolved from the link's own directory");
    CHECK(!tmd_link_escapes("a/b/link", "../c"));
    CHECK(!tmd_link_escapes("a/b/link", "../../c"));
    CHECK(tmd_link_escapes("a/b/link", "../../../c"));
    CHECK(!tmd_link_escapes("a/link", "sibling"));

    TEST_CASE("a member with no link target does not escape through one");
    CHECK(!tmd_link_escapes("a/b", ""));
    CHECK(!tmd_link_escapes("a/b", NULL));
}

void test_util(void)
{
    test_numeric_fields();
    test_field_strings();
    test_buffer();
    test_strings();
    test_utf8();
    test_utf8_offset();
    test_base64();
    test_mode_string();
    test_human_size();
    test_rounding();
    test_path_matching();
    test_path_escapes();
}
