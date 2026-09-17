/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * Turning entries into output.
 *
 * Three renderers share one interface so that main.c never branches on the
 * output format: it opens a renderer, feeds it entries as they arrive, and
 * closes it. Streaming matters — a 200,000-member archive must not have to be
 * held in memory to be listed — which is why JSON is written incrementally
 * with the renderer tracking whether a comma is owed rather than built as a
 * tree and serialized at the end.
 */
#ifndef TMD_RENDER_H
#define TMD_RENDER_H

#include <stdio.h>

#include "tmd.h"
#include "util.h"

struct tmd_render;

struct tmd_render *tmd_render_new(FILE *out, const struct tmd_options *opt,
                                  int archive_count);
void tmd_render_free(struct tmd_render *rd);

void tmd_render_archive_begin(struct tmd_render *rd, const struct tmd_archive *a);
void tmd_render_entry(struct tmd_render *rd, const struct tmd_entry *e);
void tmd_render_archive_end(struct tmd_render *rd, const struct tmd_archive *a);
void tmd_render_finish(struct tmd_render *rd);

/* How many members -m let through, across every archive read. Zero with a
 * pattern in effect is TMD_EXIT_NOMATCH. */
uint64_t tmd_render_matched(const struct tmd_render *rd);

/* Exposed for the tests: the listing line an entry produces in default mode,
 * and the JSON string escaping, which is the part with the sharp edges. */
char *tmd_render_listing_line(const struct tmd_entry *e,
                              const struct tmd_options *opt);
void tmd_json_escape(struct tmd_buf *b, const char *s);

#endif /* TMD_RENDER_H */
