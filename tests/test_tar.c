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

/*
 * Every test here builds an archive, reads it back and checks what came out.
 * `read_all` is the shape they all share: hand it the bytes, get the entries.
 */
struct readback {
    struct tmd_source *src;
    struct tmd_reader *reader;
    struct tmd_entry  *entries;
    size_t             count;
    int                last_rc;
};

/* A shallow copy of each entry, because the reader reuses its own. Only the
 * fields the tests look at are copied; the strings are duplicated so they
 * survive. */
static void keep(struct readback *rb, const struct tmd_entry *e)
{
    struct tmd_entry *copy;

    rb->entries = tmd_xrealloc(rb->entries, (rb->count + 1) * sizeof(*rb->entries));
    copy = &rb->entries[rb->count++];
    *copy = *e;
    copy->path = tmd_xstrdup(e->path ? e->path : "");
    copy->linkpath = tmd_xstrdup(e->linkpath ? e->linkpath : "");
    copy->uname = tmd_xstrdup(e->uname ? e->uname : "");
    copy->gname = tmd_xstrdup(e->gname ? e->gname : "");
    /* The warning and pax arrays are not copied; tests that need them look at
     * the live entry inside the loop instead. */
    copy->warnings = NULL;
    copy->nwarnings = e->nwarnings;
    copy->pax = NULL;
    copy->npax = e->npax;
    copy->sparse = NULL;
}

static void read_all(struct readback *rb, const struct tarbuild *tb)
{
    const struct tmd_entry *e;
    int                     rc;

    memset(rb, 0, sizeof(*rb));
    rb->src = tmd_source_open_memory(tb->buf.data, tb->buf.len, "(test)");
    rb->reader = tmd_reader_new(rb->src);
    while ((rc = tmd_reader_next(rb->reader, &e)) == 1)
    {
        keep(rb, e);
    }
    rb->last_rc = rc;
}

static void read_free(struct readback *rb)
{
    size_t i;

    for (i = 0; i < rb->count; i++)
    {
        free(rb->entries[i].path);
        free(rb->entries[i].linkpath);
        free(rb->entries[i].uname);
        free(rb->entries[i].gname);
    }
    free(rb->entries);
    tmd_reader_free(rb->reader);
    tmd_source_close(rb->src);
}

/* ------------------------------------------------------------------------- */

static void test_checksums(void)
{
    char block[TMD_BLOCK_SIZE];

    TEST_CASE("an all-space header sums to 512 spaces");
    memset(block, ' ', sizeof(block));
    CHECK_INT(tmd_header_checksum_unsigned(block), 512 * ' ');
    CHECK_INT(tmd_header_checksum_signed(block), 512 * ' ');

    TEST_CASE("the checksum field itself is read as spaces, whatever it holds");
    memset(block, 0, sizeof(block));
    memcpy(block + 148, "01234567", 8);
    CHECK_INT(tmd_header_checksum_unsigned(block), 8 * ' ');

    TEST_CASE("the signed and unsigned sums differ exactly on high bytes");
    /* This is the whole reason both are computed: one byte over 127 puts the
     * two conventions 256 apart, and a reader that knows only one of them
     * calls a perfectly good archive corrupt. */
    memset(block, 0, sizeof(block));
    memset(block + 148, ' ', 8);
    block[0] = (char)0xff;
    CHECK_INT(tmd_header_checksum_unsigned(block), 8 * ' ' + 255);
    CHECK_INT(tmd_header_checksum_signed(block), 8 * ' ' - 1);
}

static void test_kind_mapping(void)
{
    TEST_CASE("typeflags map to kinds");
    CHECK_INT(tmd_kind_of('0', "file"), TMD_KIND_FILE);
    CHECK_INT(tmd_kind_of('\0', "file"), TMD_KIND_FILE);
    CHECK_INT(tmd_kind_of('1', "file"), TMD_KIND_HARDLINK);
    CHECK_INT(tmd_kind_of('2', "file"), TMD_KIND_SYMLINK);
    CHECK_INT(tmd_kind_of('3', "dev"), TMD_KIND_CHARDEV);
    CHECK_INT(tmd_kind_of('4', "dev"), TMD_KIND_BLOCKDEV);
    CHECK_INT(tmd_kind_of('5', "dir"), TMD_KIND_DIR);
    CHECK_INT(tmd_kind_of('6', "pipe"), TMD_KIND_FIFO);
    CHECK_INT(tmd_kind_of('7', "f"), TMD_KIND_CONTIGUOUS);
    CHECK_INT(tmd_kind_of('D', "d"), TMD_KIND_DUMPDIR);
    CHECK_INT(tmd_kind_of('V', "label"), TMD_KIND_VOLUME);
    CHECK_INT(tmd_kind_of('M', "part"), TMD_KIND_MULTIVOL);
    CHECK_INT(tmd_kind_of('z', "?"), TMD_KIND_UNKNOWN);

    TEST_CASE("a trailing slash makes a regular entry a directory");
    /* Several writers mark directories this way and give them typeflag '0'.
     * Reporting one as a zero-byte file changes what the archive says. */
    CHECK_INT(tmd_kind_of('0', "some/dir/"), TMD_KIND_DIR);
    CHECK_INT(tmd_kind_of('\0', "some/dir/"), TMD_KIND_DIR);
    CHECK_INT(tmd_kind_of('0', NULL), TMD_KIND_FILE);
    /* ...but a typeflag that already says what it is wins. */
    CHECK_INT(tmd_kind_of('2', "link/"), TMD_KIND_SYMLINK);
}

static void test_v7(void)
{
    struct tarbuild  tb;
    struct readback  rb;

    TEST_CASE("a v7 archive: no magic, no uname, one file");
    tb_init(&tb);
    tb_file(&tb, "hello.txt", "hello\n", NULL);
    tb_end(&tb);
    read_all(&rb, &tb);

    CHECK_INT(rb.count, 1);
    if (rb.count == 1)
    {
        CHECK_STR(rb.entries[0].path, "hello.txt");
        CHECK_INT(rb.entries[0].size, 6);
        CHECK_INT(rb.entries[0].mode, 0644);
        CHECK_INT(rb.entries[0].uid, 1000);
        CHECK_INT(rb.entries[0].format, TMD_FMT_V7);
        CHECK_INT(rb.entries[0].kind, TMD_KIND_FILE);
        CHECK(rb.entries[0].chksum_ok);
        CHECK_INT(rb.entries[0].mtime.present, 1);
        CHECK_INT(rb.entries[0].mtime.sec, 1600000000);
        /* v7 has no uname field at all, so there is nothing to report. */
        CHECK_STR(rb.entries[0].uname, "");
        /* One header plus one padded data block. */
        CHECK_INT(rb.entries[0].stored_size, 1024);
        CHECK_INT(rb.entries[0].offset, 0);
    }
    CHECK_INT(tmd_reader_archive(rb.reader)->format, TMD_FMT_V7);
    CHECK(tmd_reader_archive(rb.reader)->eof_marker);
    read_free(&rb);
    tb_free(&tb);
}

static void test_ustar_prefix(void)
{
    struct tarbuild tb;
    struct readback rb;
    struct tb_hdr   h;

    TEST_CASE("ustar joins prefix and name with the slash that is not stored");
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "deep/file.txt";
    h.prefix = "a/long/directory/path";
    h.magic = TB_USTAR;
    h.mode = 0640;
    h.size = 0;
    h.mtime = 1600000000;
    h.typeflag = '0';
    h.uname = "bceverly";
    h.gname = "staff";
    tb_header(&tb, &h);
    tb_end(&tb);
    read_all(&rb, &tb);

    CHECK_INT(rb.count, 1);
    if (rb.count == 1)
    {
        CHECK_STR(rb.entries[0].path, "a/long/directory/path/deep/file.txt");
        CHECK_STR(rb.entries[0].path_source, "prefix+name");
        CHECK_STR(rb.entries[0].uname, "bceverly");
        CHECK_STR(rb.entries[0].gname, "staff");
        CHECK_INT(rb.entries[0].format, TMD_FMT_USTAR);
    }
    read_free(&rb);
    tb_free(&tb);
}

static void test_star_and_gnu_detection(void)
{
    struct tarbuild tb;
    struct readback rb;
    struct tb_hdr   h;

    TEST_CASE("star is ustar plus a signature in the block's last four bytes");
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "f";
    h.magic = TB_USTAR;
    h.typeflag = '0';
    h.star_signature = true;
    tb_header(&tb, &h);
    tb_end(&tb);
    read_all(&rb, &tb);
    CHECK_INT(rb.count, 1);
    if (rb.count == 1)
    {
        CHECK_INT(rb.entries[0].format, TMD_FMT_STAR);
    }
    CHECK_STR(tmd_reader_archive(rb.reader)->writer, "star");
    read_free(&rb);
    tb_free(&tb);

    TEST_CASE("GNU's magic is \"ustar  \" and it reads the tail as times");
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "f";
    h.magic = TB_GNU;
    h.typeflag = '0';
    h.mtime = 1600000000;
    h.has_gnu_times = true;
    h.gnu_atime = 1600000001;
    h.gnu_ctime = 1600000002;
    tb_header(&tb, &h);
    tb_end(&tb);
    read_all(&rb, &tb);
    CHECK_INT(rb.count, 1);
    if (rb.count == 1)
    {
        CHECK_INT(rb.entries[0].format, TMD_FMT_GNU);
        CHECK_INT(rb.entries[0].atime.present, 1);
        CHECK_INT(rb.entries[0].atime.sec, 1600000001);
        CHECK_INT(rb.entries[0].ctime.sec, 1600000002);
    }
    read_free(&rb);
    tb_free(&tb);
}

static void test_gnu_long_names(void)
{
    struct tarbuild tb;
    struct readback rb;
    struct tb_hdr   h;
    char            longname[300];
    char            longlink[250];

    TEST_CASE("a GNU 'L' block supplies a path the header cannot hold");
    memset(longname, 'n', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    memset(longlink, 'k', sizeof(longlink) - 1);
    longlink[sizeof(longlink) - 1] = '\0';

    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "././@LongLink";
    h.magic = TB_GNU;
    h.typeflag = 'L';
    h.size = strlen(longname) + 1;
    tb_header(&tb, &h);
    tb_data(&tb, longname, strlen(longname) + 1);

    memset(&h, 0, sizeof(h));
    h.name = "././@LongLink";
    h.magic = TB_GNU;
    h.typeflag = 'K';
    h.size = strlen(longlink) + 1;
    tb_header(&tb, &h);
    tb_data(&tb, longlink, strlen(longlink) + 1);

    memset(&h, 0, sizeof(h));
    h.name = "truncated-name-in-the-header";
    h.linkname = "truncated-link";
    h.magic = TB_GNU;
    h.typeflag = '2';
    h.mode = 0777;
    tb_header(&tb, &h);
    tb_end(&tb);

    read_all(&rb, &tb);
    /* The two extension blocks are not members: one entry comes out. */
    CHECK_INT(rb.count, 1);
    if (rb.count == 1)
    {
        CHECK_STR(rb.entries[0].path, longname);
        CHECK_STR(rb.entries[0].linkpath, longlink);
        CHECK_STR(rb.entries[0].path_source, "GNU long name");
        CHECK_STR(rb.entries[0].linkpath_source, "GNU long link name");
        CHECK_INT(rb.entries[0].kind, TMD_KIND_SYMLINK);
        /* The member's cost includes both extension blocks. */
        CHECK_INT(rb.entries[0].stored_size, 5 * TMD_BLOCK_SIZE);
    }
    read_free(&rb);
    tb_free(&tb);
}

static void test_devices_and_links(void)
{
    struct tarbuild tb;
    struct readback rb;
    struct tb_hdr   h;

    TEST_CASE("device nodes carry numbers rather than a size");
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "dev/null";
    h.magic = TB_USTAR;
    h.typeflag = '3';
    h.mode = 0666;
    h.has_dev = true;
    h.devmajor = 1;
    h.devminor = 3;
    tb_header(&tb, &h);

    memset(&h, 0, sizeof(h));
    h.name = "dev/sda";
    h.magic = TB_USTAR;
    h.typeflag = '4';
    h.mode = 0660;
    h.has_dev = true;
    h.devmajor = 8;
    h.devminor = 0;
    tb_header(&tb, &h);
    tb_end(&tb);

    read_all(&rb, &tb);
    CHECK_INT(rb.count, 2);
    if (rb.count == 2)
    {
        CHECK_INT(rb.entries[0].kind, TMD_KIND_CHARDEV);
        CHECK(rb.entries[0].has_dev);
        CHECK_INT(rb.entries[0].devmajor, 1);
        CHECK_INT(rb.entries[0].devminor, 3);
        CHECK_INT(rb.entries[1].kind, TMD_KIND_BLOCKDEV);
        CHECK_INT(rb.entries[1].devmajor, 8);
    }
    CHECK_INT(tmd_reader_archive(rb.reader)->counts[TMD_KIND_CHARDEV], 1);
    CHECK_INT(tmd_reader_archive(rb.reader)->counts[TMD_KIND_BLOCKDEV], 1);
    read_free(&rb);
    tb_free(&tb);
}

static void test_base256_size(void)
{
    struct tarbuild tb;
    struct readback rb;
    struct tb_hdr   h;

    TEST_CASE("a file over 8 GB has a base-256 size field");
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "huge.img";
    h.magic = TB_GNU;
    h.typeflag = '0';
    h.size = 0x300000000ULL; /* 12 GB — well past what 11 octal digits hold */
    h.base256_size = true;
    tb_header(&tb, &h);
    /* No payload is written: the test is about reading the field, and the
     * reader reports the truncation it then finds. */
    tb_end(&tb);

    read_all(&rb, &tb);
    CHECK_INT(rb.count, 1);
    if (rb.count == 1)
    {
        CHECK_INT(rb.entries[0].size, 0x300000000ULL);
    }
    read_free(&rb);
    tb_free(&tb);
}

static void test_damage(void)
{
    struct tarbuild         tb;
    struct readback         rb;
    struct tb_hdr           h;
    const struct tmd_entry *e;
    struct tmd_source      *src;
    struct tmd_reader      *reader;
    int                     rc;

    TEST_CASE("a bad checksum after a good header is a warning, not the end");
    tb_init(&tb);
    tb_file(&tb, "good.txt", "ok\n", TB_USTAR);
    memset(&h, 0, sizeof(h));
    h.name = "damaged.txt";
    h.magic = TB_USTAR;
    h.typeflag = '0';
    h.bad_checksum = true;
    tb_header(&tb, &h);
    tb_end(&tb);

    src = tmd_source_open_memory(tb.buf.data, tb.buf.len, "(test)");
    reader = tmd_reader_new(src);
    rc = tmd_reader_next(reader, &e);
    CHECK_INT(rc, 1);
    CHECK(e->chksum_ok);
    rc = tmd_reader_next(reader, &e);
    CHECK_INT(rc, 1);
    CHECK(!e->chksum_ok);
    CHECK_INT(e->nwarnings, 1);
    if (e->nwarnings)
    {
        CHECK_CONTAINS(e->warnings[0].text, "checksum mismatch");
    }
    tmd_reader_free(reader);
    tmd_source_close(src);
    tb_free(&tb);

    TEST_CASE("a checksum field that is not octal says so specifically");
    tb_init(&tb);
    tb_file(&tb, "good.txt", "ok\n", TB_USTAR);
    memset(&h, 0, sizeof(h));
    h.name = "damaged.txt";
    h.magic = TB_USTAR;
    h.typeflag = '0';
    h.garbage_checksum = true;
    tb_header(&tb, &h);
    tb_end(&tb);
    src = tmd_source_open_memory(tb.buf.data, tb.buf.len, "(test)");
    reader = tmd_reader_new(src);
    (void)tmd_reader_next(reader, &e);
    (void)tmd_reader_next(reader, &e);
    CHECK(!e->chksum_ok);
    if (e->nwarnings)
    {
        CHECK_CONTAINS(e->warnings[0].text, "not octal");
    }
    tmd_reader_free(reader);
    tmd_source_close(src);
    tb_free(&tb);

    TEST_CASE("a bad checksum in the very first block is not a tar archive");
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "x";
    h.typeflag = '0';
    h.bad_checksum = true;
    tb_header(&tb, &h);
    tb_end(&tb);
    read_all(&rb, &tb);
    CHECK_INT(rb.last_rc, -1);
    CHECK_INT(rb.count, 0);
    CHECK_CONTAINS(tmd_reader_error(rb.reader), "not a tar archive");
    read_free(&rb);
    tb_free(&tb);

    TEST_CASE("an empty file is refused with a message that says so");
    src = tmd_source_open_memory("", 0, "(empty)");
    reader = tmd_reader_new(src);
    CHECK_INT(tmd_reader_next(reader, &e), -1);
    CHECK_CONTAINS(tmd_reader_error(reader), "empty file");
    tmd_reader_free(reader);
    tmd_source_close(src);

    TEST_CASE("a gzip stream names the wrapper and the command that opens it");
    src = tmd_source_open_memory("\x1f\x8b\x08\x00 rest of a gzip member", 27,
                                 "a.tar.gz");
    reader = tmd_reader_new(src);
    CHECK_INT(tmd_reader_next(reader, &e), -1);
    CHECK_CONTAINS(tmd_reader_error(reader), "gzip");
    CHECK_CONTAINS(tmd_reader_error(reader), "gzip -dc");
    tmd_reader_free(reader);
    tmd_source_close(src);

    TEST_CASE("an ELF binary is named for what it is");
    src = tmd_source_open_memory("\x7f" "ELF\x02\x01\x01", 7, "ls");
    reader = tmd_reader_new(src);
    CHECK_INT(tmd_reader_next(reader, &e), -1);
    CHECK_CONTAINS(tmd_reader_error(reader), "ELF");
    tmd_reader_free(reader);
    tmd_source_close(src);

    TEST_CASE("an archive with no end-of-archive marker is reported");
    tb_init(&tb);
    tb_file(&tb, "a.txt", "a\n", TB_USTAR);
    /* deliberately no tb_end() */
    read_all(&rb, &tb);
    CHECK_INT(rb.count, 1);
    CHECK(!tmd_reader_archive(rb.reader)->eof_marker);
    CHECK_INT(tmd_reader_archive(rb.reader)->nwarnings, 1);
    if (tmd_reader_archive(rb.reader)->nwarnings)
    {
        CHECK_CONTAINS(tmd_reader_archive(rb.reader)->warnings[0].text, "end-of-archive");
    }
    read_free(&rb);
    tb_free(&tb);

    TEST_CASE("a member whose data is cut short is reported as truncated");
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "big.bin";
    h.magic = TB_USTAR;
    h.typeflag = '0';
    h.size = 100000;
    tb_header(&tb, &h);
    tb_raw(&tb, "only a little data", 18);
    read_all(&rb, &tb);
    CHECK_INT(rb.count, 1);
    if (rb.count == 1)
    {
        CHECK_INT(rb.entries[0].nwarnings, 1);
    }
    read_free(&rb);
    tb_free(&tb);

    TEST_CASE("a header cut off mid-block is reported with its offset");
    tb_init(&tb);
    tb_file(&tb, "a.txt", "a\n", TB_USTAR);
    tb_raw(&tb, "half a header", 13);
    read_all(&rb, &tb);
    CHECK_INT(rb.count, 1);
    CHECK_CONTAINS(tmd_reader_archive(rb.reader)->warnings[0].text, "mid-header");
    read_free(&rb);
    tb_free(&tb);

    TEST_CASE("one zero block is not an end marker; the archive continues");
    /* This is what an archive concatenated with `cat` looks like, and GNU tar
     * reads straight through it, so tmd does too — loudly. */
    tb_init(&tb);
    tb_file(&tb, "first.txt", "1\n", TB_USTAR);
    tb_zero_block(&tb);
    tb_file(&tb, "second.txt", "2\n", TB_USTAR);
    tb_end(&tb);
    read_all(&rb, &tb);
    CHECK_INT(rb.count, 2);
    if (rb.count == 2)
    {
        CHECK_STR(rb.entries[1].path, "second.txt");
    }
    CHECK_CONTAINS(tmd_reader_archive(rb.reader)->warnings[0].text, "not an end marker");
    read_free(&rb);
    tb_free(&tb);

    TEST_CASE("data after the end marker is reported as trailing garbage");
    tb_init(&tb);
    tb_file(&tb, "a.txt", "a\n", TB_USTAR);
    tb_end(&tb);
    tb_raw(&tb, "this is not padding and not zero", 32);
    read_all(&rb, &tb);
    CHECK_INT(rb.count, 1);
    CHECK(tmd_reader_archive(rb.reader)->eof_marker);
    CHECK(tmd_reader_archive(rb.reader)->trailing_garbage);
    CHECK_INT(tmd_reader_archive(rb.reader)->trailing_bytes, 32);
    read_free(&rb);
    tb_free(&tb);

    TEST_CASE("an unrecognized typeflag is a warning, not a refusal");
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "odd";
    h.magic = TB_USTAR;
    h.typeflag = 'z';
    tb_header(&tb, &h);
    tb_end(&tb);
    read_all(&rb, &tb);
    CHECK_INT(rb.count, 1);
    if (rb.count == 1)
    {
        CHECK_INT(rb.entries[0].kind, TMD_KIND_UNKNOWN);
        CHECK_INT(rb.entries[0].nwarnings, 1);
    }
    read_free(&rb);
    tb_free(&tb);

    TEST_CASE("a directory carrying a payload size has it ignored, with a warning");
    /* Believing the size field here would consume the next header. */
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "dir/";
    h.magic = TB_USTAR;
    h.typeflag = '5';
    h.size = 4096;
    tb_header(&tb, &h);
    tb_file(&tb, "after.txt", "still here\n", TB_USTAR);
    tb_end(&tb);
    read_all(&rb, &tb);
    CHECK_INT(rb.count, 2);
    if (rb.count == 2)
    {
        CHECK_INT(rb.entries[0].size, 0);
        CHECK_INT(rb.entries[0].nwarnings, 1);
        CHECK_STR(rb.entries[1].path, "after.txt");
    }
    read_free(&rb);
    tb_free(&tb);
}

static void test_gnu_sparse(void)
{
    struct tarbuild tb;
    struct readback rb;
    struct tb_hdr   h;

    TEST_CASE("old GNU sparse: the map is in the header, realsize beside it");
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "sparse.img";
    h.magic = TB_GNU;
    h.typeflag = 'S';
    h.size = 1024;             /* what the archive actually carries */
    h.gnu_realsize = 1048576;  /* what it expands to */
    h.sparse[0] = 0;
    h.sparse[1] = 512;
    h.sparse[2] = 1048064;
    h.sparse[3] = 512;
    h.nsparse = 4;
    tb_header(&tb, &h);
    tb_zeros(&tb, 1024);
    tb_end(&tb);

    read_all(&rb, &tb);
    CHECK_INT(rb.count, 1);
    if (rb.count == 1)
    {
        CHECK(rb.entries[0].is_sparse);
        CHECK_INT(rb.entries[0].realsize, 1048576);
        /* The reported size is the expanded one: that is the size of the file
         * the archive describes, which is what a listing is about. */
        CHECK_INT(rb.entries[0].size, 1048576);
        CHECK_INT(rb.entries[0].data_size, 1024);
        CHECK_INT(rb.entries[0].nsparse, 2);
    }
    read_free(&rb);
    tb_free(&tb);

    TEST_CASE("a sparse map continued into an extension block is read");
    tb_init(&tb);
    memset(&h, 0, sizeof(h));
    h.name = "sparse2.img";
    h.magic = TB_GNU;
    h.typeflag = 'S';
    h.size = 512;
    h.gnu_realsize = 2048;
    h.sparse[0] = 0;
    h.sparse[1] = 128;
    h.nsparse = 2;
    h.gnu_sparse_isext = true;
    tb_header(&tb, &h);
    {
        /* The continuation block: 21 pairs then the "more follows" byte. Two
         * pairs are filled in and the rest left zero, which ends the map. */
        char ext[TMD_BLOCK_SIZE];
        memset(ext, 0, sizeof(ext));
        (void)snprintf(ext, 13, "%011o", 1024);
        (void)snprintf(ext + 12, 13, "%011o", 256);
        (void)snprintf(ext + 24, 13, "%011o", 1536);
        (void)snprintf(ext + 36, 13, "%011o", 128);
        tb_raw(&tb, ext, sizeof(ext));
    }
    tb_zeros(&tb, 512);
    tb_end(&tb);

    read_all(&rb, &tb);
    CHECK_INT(rb.count, 1);
    if (rb.count == 1)
    {
        CHECK(rb.entries[0].is_sparse);
        CHECK_INT(rb.entries[0].nsparse, 3);
    }
    read_free(&rb);
    tb_free(&tb);
}

void test_tar(void)
{
    test_checksums();
    test_kind_mapping();
    test_v7();
    test_ustar_prefix();
    test_star_and_gnu_detection();
    test_gnu_long_names();
    test_devices_and_links();
    test_base256_size();
    test_damage();
    test_gnu_sparse();
}
