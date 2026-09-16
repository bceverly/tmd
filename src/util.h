/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * Small helpers: allocation that cannot fail, a growable buffer, and the
 * field decoders every tar header needs.
 */
#ifndef TMD_UTIL_H
#define TMD_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tmd.h"

/*
 * Allocation wrappers that abort rather than return NULL.
 *
 * This is a short-lived command-line tool reading one file: there is nothing
 * useful to do about a failed 40-byte allocation except die, and threading a
 * NULL check through every parser would add far more places to get the error
 * handling wrong than it removes. Everything that allocates a *caller-
 * controlled* amount — a pax header, a long name, a sparse map — is bounded
 * before it gets here, so a hostile archive cannot ask for a gigabyte.
 */
void *tmd_xmalloc(size_t n);
void *tmd_xcalloc(size_t count, size_t size);
void *tmd_xrealloc(void *p, size_t n);
char *tmd_xstrdup(const char *s);
char *tmd_xstrndup(const char *s, size_t n);
char *tmd_xasprintf(const char *fmt, ...) TMD_PRINTF(1, 2);
/*
 * The va_list form, which every "build a message" site in the program shares.
 *
 * It formats into a stack buffer first and only allocates when the message
 * does not fit. The obvious implementation — vsnprintf(NULL, 0, ...) to
 * measure, then again to fill — is a documented C99 idiom that glibc's
 * _FORTIFY_SOURCE headers warn about as a null destination, so with -Werror it
 * is also a build failure. This does the same job with one call in the common
 * case.
 */
char *tmd_xvasprintf(const char *fmt, va_list ap) TMD_PRINTF(1, 0);

struct tmd_buf {
    char  *data;
    size_t len;
    size_t cap;
};

void  tmd_buf_init(struct tmd_buf *b);
void  tmd_buf_free(struct tmd_buf *b);
void  tmd_buf_add(struct tmd_buf *b, const void *data, size_t n);
void  tmd_buf_addc(struct tmd_buf *b, char c);
void  tmd_buf_addstr(struct tmd_buf *b, const char *s);
void  tmd_buf_addf(struct tmd_buf *b, const char *fmt, ...) TMD_PRINTF(2, 3);
/* Hand the bytes to the caller and reset the buffer. Always NUL-terminated. */
char *tmd_buf_detach(struct tmd_buf *b);

/*
 * Decode a numeric header field.
 *
 * tar stores numbers as NUL- or space-terminated octal ASCII. When a value
 * does not fit — a size over 8 GB, a uid over 2097151 — GNU tar and star write
 * base-256 instead, flagged by the high bit of the first byte. Both are
 * handled here; `*out` is untouched and false is returned for anything else,
 * including the all-spaces field that some writers use to mean "absent".
 */
bool tmd_parse_num(const char *field, size_t len, uint64_t *out);
bool tmd_parse_num_signed(const char *field, size_t len, int64_t *out);
/* True when the field is entirely NUL and/or space, i.e. carries no value. */
bool tmd_field_empty(const char *field, size_t len);

/*
 * Copy a fixed-width header field out as a C string.
 *
 * Header strings are NUL-terminated only when they are shorter than the field;
 * a name that fills all 100 bytes has no terminator at all. `out` must hold
 * len + 1 bytes.
 */
void tmd_field_str(const char *field, size_t len, char *out);

/* True when the bytes are well-formed UTF-8 (used to decide how to render a
 * path in JSON, where an invalid byte would produce invalid output). */
bool tmd_utf8_valid(const char *s, size_t len);

/*
 * Render `mode` as the ten-character listing form, e.g. "-rw-r--r--".
 *
 * The leading character comes from the entry's kind rather than from the mode
 * word: tar stores only the permission bits, so S_IFDIR is simply not there to
 * be read, and a directory whose type character came from the mode would print
 * as a plain file.
 */
void tmd_mode_string(uint32_t mode, enum tmd_kind kind, char out[11]);

/* 1.4K / 23.7M / 4.0G, to three significant figures. */
const char *tmd_human_size(uint64_t bytes, char *buf, size_t bufsz);

/* Round up to the next multiple of TMD_BLOCK_SIZE, saturating rather than
 * wrapping: a size field of 2^64-1 must not become 0. */
uint64_t tmd_round_up_blocks(uint64_t n);

#endif /* TMD_UTIL_H */
