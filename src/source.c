/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "source.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util.h"

struct tmd_source {
    FILE       *file;        /* NULL for a memory source            */
    bool        owns_file;   /* stdin is borrowed, a fopen'd one is not */
    const char *mem;
    size_t      mem_len;
    size_t      mem_pos;
    uint64_t    offset;
    uint64_t    size;
    char       *name;
    bool        eof;
    bool        error;
};

struct tmd_source *tmd_source_open(const char *path, char **err)
{
    struct tmd_source *s;
    FILE              *file;
    bool               owns = true;
    uint64_t           size = 0;

    if (strcmp(path, "-") == 0) {
        file = stdin;
        owns = false;
    } else {
        file = fopen(path, "rb");
        if (!file) {
            if (err) {
                *err = tmd_xasprintf("%s: %s", path, strerror(errno));
            }
            return NULL;
        }
    }

    if (owns) {
        /* The length is worth having for the summary line and for deciding
         * whether trailing bytes after the end-of-archive marker are padding
         * or something else. stat, not fseek/ftell: it costs one syscall and
         * works the same on a file opened for reading on any platform. */
        struct stat st;
        if (fstat(fileno(file), &st) == 0 && S_ISREG(st.st_mode)) {
            size = (uint64_t)st.st_size;
        }
    }

    s = tmd_xcalloc(1, sizeof(*s));
    s->file = file;
    s->owns_file = owns;
    s->size = size;
    s->name = tmd_xstrdup(owns ? path : "(stdin)");
    return s;
}

struct tmd_source *tmd_source_open_memory(const void *data, size_t len,
                                          const char *name)
{
    struct tmd_source *s = tmd_xcalloc(1, sizeof(*s));

    s->mem = (const char *)data;
    s->mem_len = len;
    s->size = len;
    s->name = tmd_xstrdup(name ? name : "(memory)");
    return s;
}

void tmd_source_close(struct tmd_source *s)
{
    if (!s) {
        return;
    }
    if (s->file && s->owns_file) {
        (void)fclose(s->file);
    }
    free(s->name);
    free(s);
}

size_t tmd_source_read(struct tmd_source *s, void *buf, size_t n)
{
    size_t got;

    if (n == 0) {
        return 0;
    }

    if (s->file) {
        got = fread(buf, 1, n, s->file);
        if (got < n) {
            if (ferror(s->file)) {
                s->error = true;
            }
            s->eof = true;
        }
    } else {
        /* Spelled as an if rather than `got = n < left ? n : left` followed by
         * a separate `got < n` test. The two together say the same thing less
         * directly: a reader has to re-derive that a short read is exactly the
         * case where the buffer ran out, and a static analyzer following the
         * ternary concludes the second test can never fire. */
        size_t left = s->mem_len - s->mem_pos;

        if (n > left) {
            got = left;
            s->eof = true;
        } else {
            got = n;
        }
        memcpy(buf, s->mem + s->mem_pos, got);
        s->mem_pos += got;
    }

    s->offset += got;
    return got;
}

bool tmd_source_skip(struct tmd_source *s, uint64_t n)
{
    char   scratch[8192];
    bool   complete = true;

    if (n == 0) {
        return true;
    }

    if (s->file) {
        /*
         * Seek when we can, read when we cannot.
         *
         * fseeko on a regular file is O(1) and is what makes `tmd` on a 40 GB
         * archive as fast as reading its headers. On a pipe it fails, and the
         * only way past a member is through it. The seek is deliberately NOT
         * trusted to have landed inside the file: seeking past the end of a
         * regular file succeeds, so the end has to be detected by the read
         * that follows rather than here.
         */
        if (s->size > 0) {
            if (s->offset + n > s->size) {
                /* Consume what is actually there, then report the shortfall,
                 * rather than seeking into nothing and reporting success. */
                uint64_t available = s->size - s->offset;
                if (fseeko(s->file, (off_t)s->size, SEEK_SET) == 0) {
                    s->offset = s->size;
                    s->eof = true;
                    return false;
                }
                n = available;
                complete = false;
            } else if (fseeko(s->file, (off_t)n, SEEK_CUR) == 0) {
                s->offset += n;
                return true;
            }
        }
    }

    while (n > 0) {
        size_t want = n < sizeof(scratch) ? (size_t)n : sizeof(scratch);
        size_t got = tmd_source_read(s, scratch, want);
        if (got == 0) {
            return false;
        }
        n -= got;
    }
    return complete;
}

uint64_t tmd_source_offset(const struct tmd_source *s) { return s->offset; }
uint64_t tmd_source_size(const struct tmd_source *s) { return s->size; }
const char *tmd_source_name(const struct tmd_source *s) { return s->name; }
bool tmd_source_at_eof(const struct tmd_source *s) { return s->eof; }
bool tmd_source_error(const struct tmd_source *s) { return s->error; }
