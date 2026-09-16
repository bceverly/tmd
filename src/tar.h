/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * The reader: turns a byte stream into a sequence of fully resolved members.
 *
 * Everything format-specific lives behind this interface. A caller asks for
 * the next entry and gets one whose path is the real path, whose times carry
 * whatever precision the archive actually recorded, and whose `format` field
 * says which dialect it was written in — without needing to know that the path
 * arrived in three pieces from two different extension mechanisms.
 *
 * The reader owns the entry it hands out. It is valid until the next call.
 */
#ifndef TMD_TAR_H
#define TMD_TAR_H

#include "source.h"
#include "tmd.h"

struct tmd_reader;

struct tmd_reader *tmd_reader_new(struct tmd_source *src);
void               tmd_reader_free(struct tmd_reader *r);

/*
 * 1  an entry was produced
 * 0  the archive ended (cleanly or not; the archive record says which)
 * -1 this is not something we can read at all — tmd_reader_error() says why
 */
int tmd_reader_next(struct tmd_reader *r, const struct tmd_entry **out);

/* The accumulated archive-level facts. Complete only once next() has returned
 * 0; before that the counts are partial and `format` is the best guess so far. */
const struct tmd_archive *tmd_reader_archive(const struct tmd_reader *r);

/* NULL unless next() returned -1. */
const char *tmd_reader_error(const struct tmd_reader *r);

/* Exposed for the unit tests: the two checksum conventions, and the typeflag
 * mapping. Both are pure functions over a 512-byte block. */
uint32_t tmd_header_checksum_unsigned(const char block[TMD_BLOCK_SIZE]);
int32_t  tmd_header_checksum_signed(const char block[TMD_BLOCK_SIZE]);
enum tmd_kind tmd_kind_of(char typeflag, const char *name);

#endif /* TMD_TAR_H */
