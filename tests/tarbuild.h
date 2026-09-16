/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * Build tar archives in memory, byte by byte.
 *
 * The reader's job is to cope with archives that real tools do not produce —
 * a truncated sparse map, a pax record whose length prefix lies, a checksum
 * field full of spaces. None of those can be created with `tar`, so the tests
 * assemble the bytes themselves. Everything here writes exactly what it is
 * told to, including things that are wrong: that is the point.
 */
#ifndef TMD_TARBUILD_H
#define TMD_TARBUILD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

struct tarbuild {
    struct tmd_buf buf;
};

/* What goes into one 512-byte header. Anything left at its zero value is
 * written as an empty field, which is what a minimal writer produces. */
struct tb_hdr {
    const char        *name;
    const char        *linkname;
    const char        *prefix;
    const char        *uname;
    const char        *gname;
    /* "ustar\0" "00" for POSIX, "ustar  \0" for GNU, NULL for v7. */
    const char        *magic;
    unsigned           mode;
    long               uid;
    long               gid;
    unsigned long long size;
    long long          mtime;
    char               typeflag;
    unsigned           devmajor;
    unsigned           devminor;
    bool               has_dev;
    /* GNU's reading of the header tail. */
    long long          gnu_atime;
    long long          gnu_ctime;
    bool               has_gnu_times;
    unsigned long long gnu_realsize;
    bool               gnu_sparse_isext;
    /* Four inline sparse pairs, terminated by a zero pair. */
    unsigned long long sparse[8];
    size_t             nsparse;
    /* Write a size field in GNU's base-256 form rather than octal. */
    bool               base256_size;
    /* Damage, on purpose. */
    bool               bad_checksum;
    bool               garbage_checksum;
    bool               star_signature;
};

#define TB_USTAR "ustar\0" "00"
#define TB_GNU   "ustar  "

void tb_init(struct tarbuild *tb);
void tb_free(struct tarbuild *tb);

void tb_header(struct tarbuild *tb, const struct tb_hdr *h);
/* Payload, padded out to a block boundary the way tar writes it. */
void tb_data(struct tarbuild *tb, const void *data, size_t len);
/* Raw bytes with no padding, for building deliberately misaligned files. */
void tb_raw(struct tarbuild *tb, const void *data, size_t len);
/* `len` bytes of zero, padded to a block boundary: the payload of a member
 * whose contents do not matter to the test. Passing "" to tb_data() with a
 * length reads past the literal, which is what this exists to stop. */
void tb_zeros(struct tarbuild *tb, size_t len);
void tb_zero_block(struct tarbuild *tb);
void tb_end(struct tarbuild *tb);

/* The common shapes, so a test that is about something else stays short. */
void tb_file(struct tarbuild *tb, const char *name, const char *content,
             const char *magic);
/*
 * One pax extended header ('x') or global header ('g'), from "key=value"
 * strings, with the length prefixes computed.
 *
 * The header block itself is given a name that matches no real writer's
 * convention, because the reader fingerprints the writer from that name and a
 * test about attribute keys must not accidentally also be a test about naming.
 * tb_pax_named() is for the tests that ARE about the naming.
 */
void tb_pax(struct tarbuild *tb, char typeflag, const char *const *records,
            size_t count);
void tb_pax_named(struct tarbuild *tb, char typeflag, const char *name,
                  const char *const *records, size_t count);

#endif /* TMD_TARBUILD_H */
