/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "tarbuild.h"

#include <stdio.h>
#include <string.h>

#include "tmd.h"

void tb_init(struct tarbuild *tb) { tmd_buf_init(&tb->buf); }
void tb_free(struct tarbuild *tb) { tmd_buf_free(&tb->buf); }

/* Octal, NUL-terminated, right-aligned with leading zeros — the form GNU tar
 * writes and the one every reader is happiest with. */
static void put_octal(char *field, size_t len, unsigned long long value)
{
    size_t i;

    if (len == 0)
        return;
    field[len - 1] = '\0';
    for (i = len - 1; i-- > 0;) {
        field[i] = (char)('0' + (value & 7));
        value >>= 3;
    }
}

/* GNU's escape hatch for values that do not fit in octal: high bit set on the
 * first byte, big-endian binary in the rest. */
static void put_base256(char *field, size_t len, unsigned long long value)
{
    size_t i;

    memset(field, 0, len);
    for (i = len; i-- > 1;) {
        field[i] = (char)(value & 0xff);
        value >>= 8;
    }
    field[0] = (char)0x80;
}

static void put_str(char *field, size_t len, const char *s)
{
    size_t n;

    memset(field, 0, len);
    if (!s)
        return;
    n = strlen(s);
    if (n > len)
        n = len; /* a name that fills the field has no terminator */
    memcpy(field, s, n);
}

void tb_header(struct tarbuild *tb, const struct tb_hdr *h)
{
    char     block[TMD_BLOCK_SIZE];
    uint32_t sum = 0;
    size_t   i;

    memset(block, 0, sizeof(block));
    put_str(block + 0, 100, h->name);
    put_octal(block + 100, 8, h->mode);
    put_octal(block + 108, 8, (unsigned long long)h->uid);
    put_octal(block + 116, 8, (unsigned long long)h->gid);
    if (h->base256_size)
        put_base256(block + 124, 12, h->size);
    else
        put_octal(block + 124, 12, h->size);
    put_octal(block + 136, 12, (unsigned long long)h->mtime);
    block[156] = h->typeflag ? h->typeflag : '0';
    put_str(block + 157, 100, h->linkname);
    if (h->magic)
        memcpy(block + 257, h->magic, 8);
    put_str(block + 265, 32, h->uname);
    put_str(block + 297, 32, h->gname);
    if (h->has_dev) {
        put_octal(block + 329, 8, h->devmajor);
        put_octal(block + 337, 8, h->devminor);
    }

    if (h->has_gnu_times) {
        put_octal(block + 345, 12, (unsigned long long)h->gnu_atime);
        put_octal(block + 357, 12, (unsigned long long)h->gnu_ctime);
    } else {
        put_str(block + 345, 155, h->prefix);
    }

    if (h->nsparse > 0) {
        size_t pairs = h->nsparse / 2;
        if (pairs > 4)
            pairs = 4;
        for (i = 0; i < pairs; i++) {
            put_octal(block + 386 + i * 24, 12, h->sparse[i * 2]);
            put_octal(block + 386 + i * 24 + 12, 12, h->sparse[i * 2 + 1]);
        }
    }
    if (h->gnu_sparse_isext)
        block[482] = 1;
    if (h->gnu_realsize)
        put_octal(block + 483, 12, h->gnu_realsize);
    if (h->star_signature)
        memcpy(block + 508, "tar\0", 4);

    /* The checksum is computed over the block with its own field read as
     * spaces, so the field is filled with spaces first. */
    memset(block + 148, ' ', 8);
    for (i = 0; i < TMD_BLOCK_SIZE; i++)
        sum += (unsigned char)block[i];
    if (h->bad_checksum)
        sum += 1;

    if (h->garbage_checksum)
        memcpy(block + 148, "99999999", 8);
    else
        (void)snprintf(block + 148, 8, "%06o", sum);

    tmd_buf_add(&tb->buf, block, TMD_BLOCK_SIZE);
}

void tb_raw(struct tarbuild *tb, const void *data, size_t len)
{
    tmd_buf_add(&tb->buf, data, len);
}

void tb_data(struct tarbuild *tb, const void *data, size_t len)
{
    static const char padding[TMD_BLOCK_SIZE] = { 0 };
    size_t            remainder;

    tmd_buf_add(&tb->buf, data, len);
    remainder = len % TMD_BLOCK_SIZE;
    if (remainder)
        tmd_buf_add(&tb->buf, padding, TMD_BLOCK_SIZE - remainder);
}

void tb_zeros(struct tarbuild *tb, size_t len)
{
    char   chunk[512];
    size_t written = 0;

    memset(chunk, 0, sizeof(chunk));
    while (written < len) {
        size_t n = len - written;
        if (n > sizeof(chunk))
            n = sizeof(chunk);
        tmd_buf_add(&tb->buf, chunk, n);
        written += n;
    }
    if (len % TMD_BLOCK_SIZE)
        tmd_buf_add(&tb->buf, chunk, TMD_BLOCK_SIZE - (len % TMD_BLOCK_SIZE));
}

void tb_zero_block(struct tarbuild *tb)
{
    static const char zeros[TMD_BLOCK_SIZE] = { 0 };

    tmd_buf_add(&tb->buf, zeros, TMD_BLOCK_SIZE);
}

void tb_end(struct tarbuild *tb)
{
    tb_zero_block(tb);
    tb_zero_block(tb);
}

void tb_file(struct tarbuild *tb, const char *name, const char *content,
             const char *magic)
{
    struct tb_hdr h;
    size_t        len = content ? strlen(content) : 0;

    memset(&h, 0, sizeof(h));
    h.name = name;
    h.magic = magic;
    h.mode = 0644;
    h.uid = 1000;
    h.gid = 1000;
    h.size = len;
    h.mtime = 1600000000;
    h.typeflag = '0';
    if (magic) {
        h.uname = "bceverly";
        h.gname = "bceverly";
    }
    tb_header(tb, &h);
    if (len)
        tb_data(tb, content, len);
}

void tb_pax(struct tarbuild *tb, char typeflag, const char *const *records,
            size_t count)
{
    tb_pax_named(tb, typeflag,
                 (typeflag == 'g') ? "PaxAttrs.global" : "PaxAttrs/file",
                 records, count);
}

void tb_pax_named(struct tarbuild *tb, char typeflag, const char *name,
                  const char *const *records, size_t count)
{
    struct tmd_buf payload;
    struct tb_hdr  h;
    size_t         i;

    tmd_buf_init(&payload);
    for (i = 0; i < count; i++) {
        /* The length prefix counts itself, which makes it a fixed point: try
         * a width, and if adding the digits pushes the total over, try again
         * one digit wider. Two iterations settle it for any real record. */
        size_t body = strlen(records[i]) + 2; /* the space and the newline */
        size_t width = 1;
        size_t total;

        for (;;) {
            size_t candidate = body + width;
            size_t needed = 1;
            size_t n = candidate;
            while (n >= 10) {
                n /= 10;
                needed++;
            }
            if (needed == width) {
                total = candidate;
                break;
            }
            width = needed;
        }
        tmd_buf_addf(&payload, "%zu %s\n", total, records[i]);
    }

    memset(&h, 0, sizeof(h));
    h.name = name;
    h.magic = TB_USTAR;
    h.mode = 0644;
    h.size = payload.len;
    h.mtime = 1600000000;
    h.typeflag = typeflag;
    tb_header(tb, &h);
    tb_data(tb, payload.data ? payload.data : "", payload.len);
    tmd_buf_free(&payload);
}
