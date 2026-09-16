/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * Where the archive bytes come from.
 *
 * A file, standard input, or a block of memory. The memory case is not a
 * convenience for tests that happen to be easier that way — it is what lets
 * the whole reader run under the fuzzer without touching the filesystem, and
 * what lets a unit test build a deliberately corrupt archive in a few lines.
 *
 * Standard input is the reason for the read/skip split: a pipe cannot seek, so
 * skipping a 4 GB member means reading and discarding it. Callers ask to skip
 * and the source decides how.
 */
#ifndef TMD_SOURCE_H
#define TMD_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tmd_source;

/* Returns NULL and sets *err (malloc'd, caller frees) when the file will not
 * open. "-" is accepted as a name for standard input. */
struct tmd_source *tmd_source_open(const char *path, char **err);
struct tmd_source *tmd_source_open_memory(const void *data, size_t len,
                                          const char *name);
void tmd_source_close(struct tmd_source *s);

/* Reads exactly `n` bytes unless the source ends first; returns how many were
 * read. A short count is end-of-file, not an error — a truncated archive is a
 * thing tmd reports rather than refuses. */
size_t tmd_source_read(struct tmd_source *s, void *buf, size_t n);

/* Discards `n` bytes. False means the source ended first. */
bool tmd_source_skip(struct tmd_source *s, uint64_t n);

/* How many bytes have been consumed so far — the offset of the next one. */
uint64_t tmd_source_offset(const struct tmd_source *s);

/* The total length when it is knowable (a regular file), 0 otherwise. A pipe
 * has no length until it ends. */
uint64_t tmd_source_size(const struct tmd_source *s);

const char *tmd_source_name(const struct tmd_source *s);

/* True once a read has hit the end. */
bool tmd_source_at_eof(const struct tmd_source *s);

/* True when the last read failed for a reason that is not end-of-file (an I/O
 * error on the underlying file). */
bool tmd_source_error(const struct tmd_source *s);

#endif /* TMD_SOURCE_H */
