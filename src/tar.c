/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * tar header parsing.
 *
 * The 512-byte header has not changed since 1979, but what it means has: three
 * generations of tar wrote different things into the same bytes, and two of
 * them added whole extension mechanisms for the fields that would not fit.
 * What this file does, in one sentence: read a block, work out which dialect
 * wrote it, apply whatever extension blocks came before it, and hand back one
 * fully resolved member.
 *
 * The header layout, for reference:
 *
 *     0   100  name        156   1  typeflag    329   8  devmajor
 *   100     8  mode        157 100  linkname    337   8  devminor
 *   108     8  uid         257   6  magic       345 155  prefix    (ustar)
 *   116     8  gid         263   2  version     345  12  atime     (GNU)
 *   124    12  size        265  32  uname       357  12  ctime     (GNU)
 *   136    12  mtime       297  32  gname       386  96  sparse[4] (GNU)
 *   148     8  chksum                           483  12  realsize  (GNU)
 *
 * The two readings of offset 345 onward are what the magic field selects
 * between, and getting that wrong is how a GNU archive's atime is reported as
 * a path prefix.
 */
#include "tar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Field offsets and widths, named so the parsing below reads as prose. */
#define F_NAME      0
#define F_NAME_LEN  100
#define F_MODE      100
#define F_UID       108
#define F_GID       116
#define F_SIZE      124
#define F_MTIME     136
#define F_CHKSUM    148
#define F_TYPEFLAG  156
#define F_LINKNAME  157
#define F_MAGIC     257
#define F_VERSION   263
#define F_UNAME     265
#define F_GNAME     297
#define F_DEVMAJOR  329
#define F_DEVMINOR  337
#define F_PREFIX    345
#define F_PREFIX_LEN 155
/* GNU's reading of the same tail. */
#define F_GNU_ATIME    345
#define F_GNU_CTIME    357
#define F_GNU_OFFSET   369
#define F_GNU_SPARSE   386
#define F_GNU_ISEXT    482
#define F_GNU_REALSIZE 483
/* A GNU sparse continuation block: 21 pairs, then the "more follows" byte. */
#define SPARSE_EXT_COUNT 21
#define SPARSE_EXT_ISEXT 504

/*
 * Caps on anything an archive gets to size for us.
 *
 * Every one of these guards an allocation whose length comes from the file
 * being read. A tar header can claim a 16 exabyte long-name block; without a
 * ceiling, reading a hostile archive is an out-of-memory kill rather than a
 * diagnostic. The limits are far above anything a real archive contains — the
 * longest path Linux will produce is 4096 bytes, and the largest pax header
 * GNU tar writes for one member is a few kilobytes.
 */
#define MAX_LONGNAME  (16ULL * 1024 * 1024)
#define MAX_PAX       (16ULL * 1024 * 1024)
#define MAX_SPARSE_ENTRIES 1000000u
/* How many blocks of a pax 1.0 sparse map to read before giving up on it. */
#define MAX_SPARSE_MAP_BLOCKS 4096u

struct tmd_reader {
    struct tmd_source *src;
    struct tmd_archive archive;
    struct tmd_entry   entry;
    char              *error;

    /* Extension state that applies to the *next* real header. */
    char   *pending_name;      /* from a GNU 'L' block */
    char   *pending_linkname;  /* from a GNU 'K' block */
    struct tmd_kv *pending_pax;
    size_t         pending_npax;

    /* Where the member being assembled started, so its true cost in the
     * archive can be measured rather than guessed from the size field. */
    uint64_t member_start;

    bool saw_any_header;
    bool finished;
};

/* ------------------------------------------------------------------------- */
/* Small shared helpers                                                      */
/* ------------------------------------------------------------------------- */

static void warn_archive(struct tmd_reader *r, const char *fmt, ...)
    TMD_PRINTF(2, 3);
static void warn_archive(struct tmd_reader *r, const char *fmt, ...)
{
    va_list ap;

    r->archive.warnings = tmd_xrealloc(
        r->archive.warnings,
        (r->archive.nwarnings + 1) * sizeof(*r->archive.warnings));
    va_start(ap, fmt);
    r->archive.warnings[r->archive.nwarnings++] = tmd_xvasprintf(fmt, ap);
    va_end(ap);
}

static void warn_entry(struct tmd_entry *e, const char *fmt, ...)
    TMD_PRINTF(2, 3);
static void warn_entry(struct tmd_entry *e, const char *fmt, ...)
{
    va_list ap;

    e->warnings = tmd_xrealloc(e->warnings,
                               (e->nwarnings + 1) * sizeof(*e->warnings));
    va_start(ap, fmt);
    e->warnings[e->nwarnings++] = tmd_xvasprintf(fmt, ap);
    va_end(ap);
}

static void kv_free(struct tmd_kv *kv, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        free(kv[i].key);
        free(kv[i].value);
    }
    free(kv);
}

static void kv_add(struct tmd_kv **list, size_t *n, const char *key,
                   const char *value, size_t value_len)
{
    *list = tmd_xrealloc(*list, (*n + 1) * sizeof(**list));
    (*list)[*n].key = tmd_xstrdup(key);
    (*list)[*n].value = tmd_xstrndup(value, value_len);
    (*n)++;
}

/* True when a numeric field uses GNU's base-256 escape rather than octal. */
static bool is_base256(const char *field)
{
    return ((unsigned char)field[0] & 0x80) != 0;
}

/* Record a pax key once, for the --info report's list of what the archive
 * uses. Bounded, so a hostile archive cannot grow the list without limit. */
static void note_pax_key(struct tmd_features *f, const char *key)
{
    size_t i;

    for (i = 0; i < f->npax_keys; i++)
        if (strcmp(f->pax_keys[i], key) == 0)
            return;
    if (f->npax_keys >= sizeof(f->pax_keys) / sizeof(f->pax_keys[0])) {
        f->pax_keys_truncated = true;
        return;
    }
    f->pax_keys[f->npax_keys++] = tmd_xstrdup(key);
}

/* The last value for `key`, because a local pax record overrides a global one
 * and a repeated record overrides its earlier self. */
static const char *kv_get(const struct tmd_kv *list, size_t n, const char *key)
{
    const char *found = NULL;
    size_t      i;

    for (i = 0; i < n; i++)
        if (strcmp(list[i].key, key) == 0)
            found = list[i].value;
    return found;
}

/* ------------------------------------------------------------------------- */
/* Checksums                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * The header checksum is the sum of all 512 bytes with the checksum field
 * itself read as eight spaces.
 *
 * It is computed twice because tar's own history is ambiguous: the original
 * implementation summed `char`, which is signed on most platforms, and later
 * ones summed `unsigned char`. The two agree until a header contains a byte
 * over 127 — a non-ASCII name, or a base-256 numeric field — and then they
 * differ by 256 per such byte. An archive is accepted if it matches either,
 * because rejecting one of them means calling a perfectly good archive corrupt.
 */
uint32_t tmd_header_checksum_unsigned(const char block[TMD_BLOCK_SIZE])
{
    const unsigned char *p = (const unsigned char *)block;
    uint32_t sum = 0;
    size_t   i;

    for (i = 0; i < TMD_BLOCK_SIZE; i++)
        sum += (i >= F_CHKSUM && i < F_CHKSUM + 8) ? (uint32_t)' ' : p[i];
    return sum;
}

int32_t tmd_header_checksum_signed(const char block[TMD_BLOCK_SIZE])
{
    int32_t sum = 0;
    size_t  i;

    for (i = 0; i < TMD_BLOCK_SIZE; i++)
        sum += (i >= F_CHKSUM && i < F_CHKSUM + 8) ? (int32_t)' '
                                                   : (int32_t)(signed char)block[i];
    return sum;
}

static bool block_is_zero(const char block[TMD_BLOCK_SIZE])
{
    size_t i;

    for (i = 0; i < TMD_BLOCK_SIZE; i++)
        if (block[i] != '\0')
            return false;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Names for the enums                                                       */
/* ------------------------------------------------------------------------- */

const char *tmd_format_name(enum tmd_format f)
{
    switch (f) {
    case TMD_FMT_V7:    return "v7";
    case TMD_FMT_USTAR: return "ustar";
    case TMD_FMT_STAR:  return "star";
    case TMD_FMT_GNU:   return "gnu";
    case TMD_FMT_PAX:   return "pax";
    default:            return "unknown";
    }
}

const char *tmd_kind_name(enum tmd_kind k)
{
    switch (k) {
    case TMD_KIND_FILE:       return "file";
    case TMD_KIND_HARDLINK:   return "hardlink";
    case TMD_KIND_SYMLINK:    return "symlink";
    case TMD_KIND_CHARDEV:    return "chardev";
    case TMD_KIND_BLOCKDEV:   return "blockdev";
    case TMD_KIND_DIR:        return "directory";
    case TMD_KIND_FIFO:       return "fifo";
    case TMD_KIND_CONTIGUOUS: return "contiguous";
    case TMD_KIND_VOLUME:     return "volume-label";
    case TMD_KIND_MULTIVOL:   return "multivolume-part";
    case TMD_KIND_DUMPDIR:    return "dumpdir";
    case TMD_KIND_XATTR:      return "xattr";
    default:                  return "unknown";
    }
}

const char *tmd_typeflag_name(char typeflag)
{
    switch (typeflag) {
    case '\0': return "regular (old, NUL)";
    case '0':  return "regular";
    case '1':  return "hard link";
    case '2':  return "symbolic link";
    case '3':  return "character device";
    case '4':  return "block device";
    case '5':  return "directory";
    case '6':  return "FIFO";
    case '7':  return "contiguous file";
    case 'g':  return "pax global extended header";
    case 'x':  return "pax extended header";
    case 'A':  return "Solaris ACL";
    case 'D':  return "GNU directory dump";
    case 'E':  return "Solaris extended attribute";
    case 'I':  return "Solaris inode metadata";
    case 'K':  return "GNU long link name";
    case 'L':  return "GNU long file name";
    case 'M':  return "GNU multi-volume continuation";
    case 'N':  return "GNU old long name (obsolete)";
    case 'S':  return "GNU sparse file";
    case 'V':  return "volume label";
    case 'X':  return "star extended header";
    default:   return "unknown";
    }
}

/*
 * A directory can also arrive as a regular file whose name ends in a slash.
 *
 * That is not a curiosity: it is how several writers that only implement the
 * v7 typeflags mark directories, and how a `ustar` archive produced by an old
 * pax does it. Reporting such an entry as a zero-byte file would be wrong in
 * the one way that matters — it changes what the archive says it contains.
 */
enum tmd_kind tmd_kind_of(char typeflag, const char *name)
{
    size_t len;

    switch (typeflag) {
    case '1': return TMD_KIND_HARDLINK;
    case '2': return TMD_KIND_SYMLINK;
    case '3': return TMD_KIND_CHARDEV;
    case '4': return TMD_KIND_BLOCKDEV;
    case '5': return TMD_KIND_DIR;
    case '6': return TMD_KIND_FIFO;
    case '7': return TMD_KIND_CONTIGUOUS;
    case 'D': return TMD_KIND_DUMPDIR;
    case 'M': return TMD_KIND_MULTIVOL;
    case 'V': return TMD_KIND_VOLUME;
    case 'E':
    case 'X': return TMD_KIND_XATTR;
    case 'S':
    case '0':
    case '\0':
        break;
    default:
        return TMD_KIND_UNKNOWN;
    }

    len = name ? strlen(name) : 0;
    if (len > 0 && name[len - 1] == '/')
        return TMD_KIND_DIR;
    return TMD_KIND_FILE;
}

/* ------------------------------------------------------------------------- */
/* Entry lifecycle                                                           */
/* ------------------------------------------------------------------------- */

static void entry_reset(struct tmd_entry *e)
{
    size_t i;

    free(e->path);
    free(e->linkpath);
    free(e->uname);
    free(e->gname);
    free(e->sparse);
    kv_free(e->pax, e->npax);
    for (i = 0; i < e->nwarnings; i++)
        free(e->warnings[i]);
    free(e->warnings);
    memset(e, 0, sizeof(*e));
}

/* ------------------------------------------------------------------------- */
/* pax extended headers                                                      */
/* ------------------------------------------------------------------------- */

/*
 * A pax extended header is a payload of records, each
 *
 *     "%d %s=%s\n"
 *
 * where the leading number is the length of the whole record including itself,
 * the space, the newline and the value. The length prefix is what lets a value
 * contain newlines — so the records cannot simply be split on '\n', which is
 * the bug every first implementation of this has.
 */
static void parse_pax(struct tmd_reader *r, const char *data, size_t len,
                      struct tmd_kv **list, size_t *count, bool global)
{
    size_t pos = 0;

    while (pos < len) {
        size_t start = pos;
        size_t record_len = 0;
        size_t key_start, eq;

        /* The length prefix. Bounded by the data we hold, so a record that
         * claims to be longer than the header cannot walk off the end. */
        while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
            if (record_len > (SIZE_MAX - 9) / 10) {
                warn_archive(r, "pax record length at offset %zu overflows", start);
                return;
            }
            record_len = record_len * 10 + (size_t)(data[pos] - '0');
            pos++;
        }
        if (pos == start || pos >= len || data[pos] != ' ') {
            warn_archive(r, "malformed pax record at offset %zu (no \"<len> \" prefix)",
                         start);
            return;
        }
        pos++; /* the space */

        if (record_len <= pos - start || start + record_len > len) {
            warn_archive(r,
                         "pax record at offset %zu claims %zu bytes, %zu remain",
                         start, record_len, len - start);
            return;
        }

        key_start = pos;
        eq = key_start;
        while (eq < start + record_len && data[eq] != '=')
            eq++;
        if (eq >= start + record_len) {
            warn_archive(r, "pax record at offset %zu has no '='", start);
            return;
        }

        {
            char  *key = tmd_xstrndup(data + key_start, eq - key_start);
            size_t value_len = (start + record_len) - (eq + 1);

            /* The record ends with a newline that is part of its declared
             * length but not part of the value. */
            if (value_len > 0 && data[eq + value_len] == '\n')
                value_len--;
            kv_add(list, count, key, data + eq + 1, value_len);
            free(key);
        }

        pos = start + record_len;
    }

    /* Only the writer fingerprints are archive-level; the values themselves
     * belong to whichever member they applied to. */
    (void)global;
}

/* ------------------------------------------------------------------------- */
/* Reading blocks                                                            */
/* ------------------------------------------------------------------------- */

/* Returns true when a whole block was read. A partial block is end-of-file
 * with damage, and the caller says so. */
static bool read_block(struct tmd_reader *r, char block[TMD_BLOCK_SIZE],
                       size_t *got)
{
    *got = tmd_source_read(r->src, block, TMD_BLOCK_SIZE);
    return *got == TMD_BLOCK_SIZE;
}

/*
 * Read `len` bytes of payload plus its block padding into a fresh buffer.
 *
 * Used for the things whose content we actually need: GNU long names, pax
 * headers, sparse maps. `cap` is the ceiling above which we refuse rather than
 * allocate — see the MAX_* constants.
 */
static char *read_payload(struct tmd_reader *r, uint64_t len, uint64_t cap,
                          size_t *out_len, const char *what)
{
    uint64_t padded = tmd_round_up_blocks(len);
    uint64_t want = len;
    char    *buf;
    size_t   got;

    if (len > cap) {
        warn_archive(r, "%s is %llu bytes, over the %llu byte limit — reading the first %llu",
                     what, (unsigned long long)len, (unsigned long long)cap,
                     (unsigned long long)cap);
        want = cap;
    }

    buf = tmd_xmalloc((size_t)want + 1);
    got = tmd_source_read(r->src, buf, (size_t)want);
    buf[got] = '\0';
    *out_len = got;

    if (got < want) {
        warn_archive(r, "%s is truncated: wanted %llu bytes, got %zu",
                     what, (unsigned long long)want, got);
        return buf;
    }
    /* Step over whatever of the member we chose not to read, plus the padding
     * that brings it to a block boundary. */
    if (padded > want)
        (void)tmd_source_skip(r->src, padded - want);
    return buf;
}

/* ------------------------------------------------------------------------- */
/* Header interpretation                                                     */
/* ------------------------------------------------------------------------- */

static enum tmd_format detect_format(const char block[TMD_BLOCK_SIZE])
{
    const char *magic = block + F_MAGIC;

    if (memcmp(magic, "ustar\0" "00", 8) == 0) {
        /* star signs its headers in the last four bytes of the block, which
         * ustar leaves as padding. */
        if (memcmp(block + 508, "tar\0", 4) == 0)
            return TMD_FMT_STAR;
        return TMD_FMT_USTAR;
    }
    if (memcmp(magic, "ustar  \0", 8) == 0)
        return TMD_FMT_GNU;
    /* Some writers put "ustar" in with a stray version. Treat a recognizable
     * magic as ustar rather than falling all the way back to v7, which would
     * throw away the uname, gname and prefix fields that are demonstrably
     * there. */
    if (memcmp(magic, "ustar", 5) == 0)
        return TMD_FMT_USTAR;
    return TMD_FMT_V7;
}

static void note_format(struct tmd_reader *r, enum tmd_format f)
{
    if (f <= TMD_FMT_PAX)
        r->archive.formats[f] = true;
    /* The archive's format is the most expressive one in it: a pax archive
     * full of plain ustar headers is still a pax archive, because a reader
     * that cannot parse pax gets the one member with a long path wrong. */
    if (f > r->archive.format)
        r->archive.format = f;
}

/* Copy the fixed-width fields out verbatim for --headers. */
static void fill_raw(struct tmd_raw *raw, const char block[TMD_BLOCK_SIZE])
{
    tmd_field_str(block + F_NAME, F_NAME_LEN, raw->name);
    tmd_field_str(block + F_MODE, 8, raw->mode);
    tmd_field_str(block + F_UID, 8, raw->uid);
    tmd_field_str(block + F_GID, 8, raw->gid);
    tmd_field_str(block + F_SIZE, 12, raw->size);
    tmd_field_str(block + F_MTIME, 12, raw->mtime);
    tmd_field_str(block + F_CHKSUM, 8, raw->chksum);
    raw->typeflag = block[F_TYPEFLAG];
    tmd_field_str(block + F_LINKNAME, 100, raw->linkname);
    tmd_field_str(block + F_MAGIC, 6, raw->magic);
    tmd_field_str(block + F_VERSION, 2, raw->version);
    tmd_field_str(block + F_UNAME, 32, raw->uname);
    tmd_field_str(block + F_GNAME, 32, raw->gname);
    tmd_field_str(block + F_DEVMAJOR, 8, raw->devmajor);
    tmd_field_str(block + F_DEVMINOR, 8, raw->devminor);
    tmd_field_str(block + F_PREFIX, F_PREFIX_LEN, raw->prefix);
}

/* The ustar path is prefix + '/' + name, and the '/' is not stored. */
static char *join_prefix(const char *prefix, const char *name)
{
    if (prefix[0] == '\0')
        return tmd_xstrdup(name);
    return tmd_xasprintf("%s/%s", prefix, name);
}

static void parse_time_field(struct tmd_entry *e, struct tmd_time *t,
                             const char *field, size_t len, const char *what)
{
    int64_t value;

    if (tmd_field_empty(field, len))
        return;
    if (!tmd_parse_num_signed(field, len, &value)) {
        warn_entry(e, "%s field is not a valid number", what);
        return;
    }
    t->sec = value;
    t->nsec = 0;
    t->present = true;
}

/*
 * A pax time is decimal seconds with an optional fractional part, e.g.
 * "1758045071.123456789". Everything after the ninth fractional digit is
 * dropped rather than rounded: the field is nanoseconds and there is nothing
 * finer to carry it in.
 */
static bool parse_pax_time(const char *s, struct tmd_time *t)
{
    const char *p = s;
    bool        negative = false;
    int64_t     sec = 0;
    uint32_t    nsec = 0;

    if (*p == '-') {
        negative = true;
        p++;
    } else if (*p == '+') {
        p++;
    }
    if (*p < '0' || *p > '9')
        return false;
    while (*p >= '0' && *p <= '9') {
        if (sec > (INT64_MAX - 9) / 10)
            return false;
        sec = sec * 10 + (*p - '0');
        p++;
    }
    if (*p == '.') {
        int digits = 0;

        p++;
        while (*p >= '0' && *p <= '9') {
            if (digits < 9)
                nsec = nsec * 10 + (uint32_t)(*p - '0');
            digits++;
            p++;
        }
        while (digits < 9 && digits > 0) {
            nsec *= 10;
            digits++;
        }
    }
    if (*p != '\0')
        return false;

    t->sec = negative ? -sec : sec;
    t->nsec = nsec;
    t->present = true;
    return true;
}

static bool parse_pax_u64(const char *s, uint64_t *out)
{
    uint64_t value = 0;

    if (*s < '0' || *s > '9')
        return false;
    for (; *s >= '0' && *s <= '9'; s++) {
        if (value > (UINT64_MAX - 9) / 10)
            return false;
        value = value * 10 + (uint64_t)(*s - '0');
    }
    return *s == '\0' ? (*out = value, true) : false;
}

static bool parse_pax_i64(const char *s, int64_t *out)
{
    bool     negative = (*s == '-');
    uint64_t magnitude;

    if (negative || *s == '+')
        s++;
    if (!parse_pax_u64(s, &magnitude) || magnitude > (uint64_t)INT64_MAX)
        return false;
    *out = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Sparse maps                                                               */
/* ------------------------------------------------------------------------- */

static void sparse_add(struct tmd_entry *e, uint64_t offset, uint64_t numbytes)
{
    if (e->nsparse >= MAX_SPARSE_ENTRIES) {
        e->sparse_truncated = true;
        return;
    }
    e->sparse = tmd_xrealloc(e->sparse, (e->nsparse + 1) * sizeof(*e->sparse));
    e->sparse[e->nsparse].offset = offset;
    e->sparse[e->nsparse].numbytes = numbytes;
    e->nsparse++;
}

/* The four pairs a GNU header carries inline, and the same 21 pairs that each
 * continuation block carries. An all-zero pair ends the map. */
static void sparse_from_block(struct tmd_entry *e, const char *base,
                              size_t pairs)
{
    size_t i;

    for (i = 0; i < pairs; i++) {
        const char *offset_field = base + i * 24;
        const char *bytes_field = offset_field + 12;
        uint64_t    offset, numbytes;

        if (tmd_field_empty(offset_field, 12) && tmd_field_empty(bytes_field, 12))
            continue;
        if (!tmd_parse_num(offset_field, 12, &offset) ||
            !tmd_parse_num(bytes_field, 12, &numbytes)) {
            warn_entry(e, "sparse map entry %zu is not a valid number", e->nsparse);
            return;
        }
        if (offset == 0 && numbytes == 0)
            continue;
        sparse_add(e, offset, numbytes);
    }
}

/* Old-GNU sparse continues into extra blocks when four pairs are not enough. */
static void read_sparse_extensions(struct tmd_reader *r, struct tmd_entry *e)
{
    unsigned guard = 0;

    for (;;) {
        char   block[TMD_BLOCK_SIZE];
        size_t got;

        if (!read_block(r, block, &got)) {
            warn_entry(e, "sparse map is truncated");
            return;
        }
        sparse_from_block(e, block, SPARSE_EXT_COUNT);
        if (block[SPARSE_EXT_ISEXT] == '\0')
            return;
        if (++guard > MAX_SPARSE_MAP_BLOCKS) {
            e->sparse_truncated = true;
            warn_entry(e, "sparse map exceeds %u blocks — not reading further",
                       MAX_SPARSE_MAP_BLOCKS);
            return;
        }
    }
}

/*
 * pax sparse format 1.0 keeps the map in the member's own payload: a decimal
 * count, then that many offset/length pairs, each on its own line, padded out
 * to a block boundary. Reading it means consuming part of the payload, so the
 * number of bytes consumed is handed back to the caller to subtract from what
 * is left to skip.
 */
static uint64_t read_pax_sparse_map_1_0(struct tmd_reader *r,
                                        struct tmd_entry *e, uint64_t avail)
{
    struct tmd_buf text;
    uint64_t       consumed = 0;
    uint64_t       expected = 0;
    uint64_t       have = 0;
    size_t         pos = 0;
    unsigned       blocks = 0;
    bool           counted = false;

    tmd_buf_init(&text);

    for (;;) {
        char        block[TMD_BLOCK_SIZE];
        size_t      got;
        const char *p;

        if (consumed >= avail || blocks >= MAX_SPARSE_MAP_BLOCKS) {
            warn_entry(e, "sparse map did not end within the member");
            e->sparse_truncated = true;
            break;
        }
        if (!read_block(r, block, &got)) {
            warn_entry(e, "sparse map is truncated");
            break;
        }
        consumed += TMD_BLOCK_SIZE;
        blocks++;
        tmd_buf_add(&text, block, TMD_BLOCK_SIZE);

        /* Consume whole "<number>\n" lines out of whatever has arrived. */
        p = text.data;
        for (;;) {
            size_t   start = pos;
            uint64_t value = 0;
            bool     digits = false;

            while (pos < text.len && p[pos] >= '0' && p[pos] <= '9') {
                if (value > (UINT64_MAX - 9) / 10) {
                    warn_entry(e, "sparse map number overflows");
                    goto done;
                }
                value = value * 10 + (uint64_t)(p[pos] - '0');
                digits = true;
                pos++;
            }
            if (pos >= text.len || p[pos] != '\n') {
                pos = start; /* an incomplete line — wait for the next block */
                break;
            }
            pos++;
            if (!digits) {
                warn_entry(e, "sparse map has an empty field");
                goto done;
            }
            if (!counted) {
                expected = value;
                counted = true;
                if (expected > MAX_SPARSE_ENTRIES) {
                    warn_entry(e, "sparse map claims %llu entries — not reading it",
                               (unsigned long long)expected);
                    e->sparse_truncated = true;
                    goto done;
                }
            } else if (have % 2 == 0) {
                sparse_add(e, value, 0);
                have++;
            } else {
                if (e->nsparse > 0)
                    e->sparse[e->nsparse - 1].numbytes = value;
                have++;
            }
            if (counted && have >= expected * 2)
                goto done;
        }
    }

done:
    tmd_buf_free(&text);
    return consumed;
}

/* GNU.sparse.map is "offset,size,offset,size,..." in one pax value. */
static void sparse_from_pax_map(struct tmd_entry *e, const char *map)
{
    const char *p = map;

    while (*p) {
        uint64_t offset = 0, numbytes = 0;
        bool     digits = false;

        while (*p >= '0' && *p <= '9') {
            offset = offset * 10 + (uint64_t)(*p++ - '0');
            digits = true;
        }
        if (!digits || *p != ',') {
            warn_entry(e, "GNU.sparse.map is malformed");
            return;
        }
        p++;
        digits = false;
        while (*p >= '0' && *p <= '9') {
            numbytes = numbytes * 10 + (uint64_t)(*p++ - '0');
            digits = true;
        }
        if (!digits) {
            warn_entry(e, "GNU.sparse.map is malformed");
            return;
        }
        sparse_add(e, offset, numbytes);
        if (*p == ',')
            p++;
    }
}

/* ------------------------------------------------------------------------- */
/* Applying pax attributes                                                   */
/* ------------------------------------------------------------------------- */

static void apply_pax(struct tmd_reader *r, struct tmd_entry *e)
{
    const char *v;

    v = kv_get(e->pax, e->npax, "path");
    if (v != NULL) {
        free(e->path);
        e->path = tmd_xstrdup(v);
        e->path_source = "pax path";
    }
    v = kv_get(e->pax, e->npax, "linkpath");
    if (v != NULL) {
        free(e->linkpath);
        e->linkpath = tmd_xstrdup(v);
        e->linkpath_source = "pax linkpath";
    }
    v = kv_get(e->pax, e->npax, "size");
    if (v != NULL) {
        uint64_t size;
        if (parse_pax_u64(v, &size))
            e->size = size;
        else
            warn_entry(e, "pax size \"%s\" is not a number", v);
    }
    v = kv_get(e->pax, e->npax, "uid");
    if (v != NULL) {
        int64_t uid;
        if (parse_pax_i64(v, &uid))
            e->uid = uid;
    }
    v = kv_get(e->pax, e->npax, "gid");
    if (v != NULL) {
        int64_t gid;
        if (parse_pax_i64(v, &gid))
            e->gid = gid;
    }
    v = kv_get(e->pax, e->npax, "uname");
    if (v != NULL) {
        free(e->uname);
        e->uname = tmd_xstrdup(v);
    }
    v = kv_get(e->pax, e->npax, "gname");
    if (v != NULL) {
        free(e->gname);
        e->gname = tmd_xstrdup(v);
    }
    v = kv_get(e->pax, e->npax, "mtime");
    if (v != NULL)
        (void)parse_pax_time(v, &e->mtime);
    v = kv_get(e->pax, e->npax, "atime");
    if (v != NULL)
        (void)parse_pax_time(v, &e->atime);
    /* star and GNU disagree about which key carries the inode change time;
     * both are accepted, with the POSIX spelling winning when both appear. */
    v = kv_get(e->pax, e->npax, "SCHILY.ctime");
    if (v != NULL)
        (void)parse_pax_time(v, &e->ctime);
    v = kv_get(e->pax, e->npax, "ctime");
    if (v != NULL)
        (void)parse_pax_time(v, &e->ctime);
    /*
     * Writer fingerprints.
     *
     * Only keys that one implementation alone emits count. GNU.sparse.* looks
     * like an obvious GNU marker and is not one: libarchive writes the same
     * keys, because GNU's pax sparse format is the only one there is, and
     * trusting it made every bsdtar archive containing a sparse file report
     * itself as GNU tar. What is left is genuinely distinguishing, and the
     * summary still labels the whole line as an inference.
     */
    for (size_t i = 0; i < e->npax; i++) {
        const char *key = e->pax[i].key;

        if (r->archive.writer)
            break;
        if (strncmp(key, "LIBARCHIVE.", 11) == 0)
            r->archive.writer = "libarchive (bsdtar)";
        else if (strncmp(key, "SCHILY.", 7) == 0)
            r->archive.writer = "star or libarchive";
        else if (strncmp(key, "GNU.", 4) == 0 &&
                 strncmp(key, "GNU.sparse.", 11) != 0)
            r->archive.writer = "GNU tar";
    }
}

/* ------------------------------------------------------------------------- */
/* The main loop                                                             */
/* ------------------------------------------------------------------------- */

/*
 * What comes after the end-of-archive marker.
 *
 * tar writes in records, not blocks: the default GNU blocking factor is 20
 * blocks (10 KiB) and bsdtar's is 10, and the archive is padded with zeros up
 * to the next record boundary. Reading that padding tells us two useful
 * things — the writer's blocking factor, and whether the "padding" is actually
 * zeros. A second archive appended with `cat` shows up here as trailing
 * garbage, which is the difference between "this archive has 40 members" and
 * "this file has 40 members in the first of two archives".
 */
static void consume_trailer(struct tmd_reader *r)
{
    char     block[TMD_BLOCK_SIZE];
    uint64_t total_blocks;

    for (;;) {
        size_t got = tmd_source_read(r->src, block, TMD_BLOCK_SIZE);
        if (got == 0)
            break;
        r->archive.trailing_bytes += got;
        if (!block_is_zero(block) ||
            (got < TMD_BLOCK_SIZE && !tmd_field_empty(block, got)))
            r->archive.trailing_garbage = true;
        if (got < TMD_BLOCK_SIZE)
            break;
    }

    if (r->archive.trailing_garbage)
        warn_archive(r, "%llu bytes of non-zero data follow the end-of-archive marker "
                        "(a second archive appended, or damage)",
                     (unsigned long long)r->archive.trailing_bytes);

    /*
     * Infer the blocking factor from the archive's total length.
     *
     * This is an inference and is labeled as one: any factor that divides the
     * length is consistent with the file, so the common ones are tried from
     * the largest down and the first that fits is reported. It is right for
     * every archive a standard tool writes and merely unproven for the rest,
     * which is why nothing downstream depends on it.
     */
    total_blocks = tmd_source_offset(r->src) / TMD_BLOCK_SIZE;
    if (total_blocks > 0) {
        /* Only the two factors a standard tool actually uses. Trying more of
         * them finds a "factor" for every archive, including the ones that
         * were never padded to a record at all: a 24-block bsdtar archive is
         * divisible by 8, and reporting 8 as its blocking factor is a number
         * invented to fill the field. Nothing is reported when neither fits. */
        static const unsigned factors[] = { 20, 10 };
        size_t i;
        for (i = 0; i < sizeof(factors) / sizeof(factors[0]); i++) {
            if (total_blocks % factors[i] == 0) {
                r->archive.record_blocks = factors[i];
                break;
            }
        }
    }
}

struct tmd_reader *tmd_reader_new(struct tmd_source *src)
{
    struct tmd_reader *r = tmd_xcalloc(1, sizeof(*r));

    r->src = src;
    r->archive.name = tmd_xstrdup(tmd_source_name(src));
    r->archive.file_size = tmd_source_size(src);
    return r;
}

void tmd_reader_free(struct tmd_reader *r)
{
    size_t i;

    if (!r)
        return;
    entry_reset(&r->entry);
    free(r->pending_name);
    free(r->pending_linkname);
    kv_free(r->pending_pax, r->pending_npax);
    kv_free(r->archive.pax_global, r->archive.npax_global);
    for (i = 0; i < r->archive.features.npax_keys; i++)
        free(r->archive.features.pax_keys[i]);
    for (i = 0; i < r->archive.nwarnings; i++)
        free(r->archive.warnings[i]);
    free(r->archive.warnings);
    free(r->archive.name);
    free(r->error);
    free(r);
}

const struct tmd_archive *tmd_reader_archive(const struct tmd_reader *r)
{
    return &r->archive;
}

const char *tmd_reader_error(const struct tmd_reader *r) { return r->error; }

static int fail(struct tmd_reader *r, const char *fmt, ...) TMD_PRINTF(2, 3);
static int fail(struct tmd_reader *r, const char *fmt, ...)
{
    va_list ap;

    free(r->error);
    va_start(ap, fmt);
    r->error = tmd_xvasprintf(fmt, ap);
    va_end(ap);
    return -1;
}

struct wrapper {
    const char *magic;      /* the bytes, as a literal */
    size_t      magic_len;
    const char *name;
    const char *unpack;     /* the command that gets a tar stream out of it */
};

static const struct wrapper wrappers[] = {
    { "\x1f\x8b",             2, "gzip",         "gzip -dc"  },
    { "BZh",                   3, "bzip2",        "bzip2 -dc" },
    { "\xfd" "7zXZ\x00",       6, "xz",           "xz -dc"    },
    { "\x28\xb5\x2f\xfd",      4, "zstd",         "zstd -dc"  },
    { "\x04\x22\x4d\x18",      4, "lz4",          "lz4 -dc"   },
    { "LZIP",                  4, "lzip",         "lzip -dc"  },
    { "\x1f\x9d",             2, "compress",     "zcat"      },
    /* Not compression, but the same mistake: a container that is not tar and
     * is routinely confused with one. There is no pipe that helps, so the
     * message for these says only what the file is. */
    { "PK\x03\x04",            4, "zip",          NULL        },
    { "070701",                6, "cpio (newc)",  NULL        },
    { "070707",                6, "cpio (odc)",   NULL        },
    { "!<arch>\n",             8, "ar",           NULL        },
    { "\xed\xab\xee\xdb",      4, "RPM",          NULL        },
    { "SQLite format 3",      14, "SQLite",       NULL        },
    { "\x7f" "ELF",             4, "ELF",          NULL        }
};

static const struct wrapper *compression_hint(const char block[TMD_BLOCK_SIZE],
                                              size_t len)
{
    size_t i;

    for (i = 0; i < sizeof(wrappers) / sizeof(wrappers[0]); i++)
        if (len >= wrappers[i].magic_len &&
            memcmp(block, wrappers[i].magic, wrappers[i].magic_len) == 0)
            return &wrappers[i];
    return NULL;
}

/*
 * The diagnostic for a file that is plainly not a tar archive.
 *
 * "not a tar archive" for a .tar.gz is technically true and completely
 * unhelpful — it *is* a tar archive, inside a wrapper this tool does not open.
 * Naming the wrapper, and the one command that gets past it, turns a dead end
 * into the next thing to type. Checked before the header checksum is, because
 * a small compressed file is shorter than one header block and would otherwise
 * be reported as a truncated archive.
 */
static int fail_not_tar(struct tmd_reader *r, const char block[TMD_BLOCK_SIZE],
                        size_t len)
{
    const struct wrapper *w = compression_hint(block, len);

    if (w && w->unpack)
        return fail(r, "%s: %s-compressed data, not a plain tar archive "
                       "(try: %s %s | tmd -f -)",
                    r->archive.name, w->name, w->unpack, r->archive.name);
    if (w)
        return fail(r, "%s: %s data, not a tar archive", r->archive.name, w->name);
    if (len < TMD_BLOCK_SIZE)
        return fail(r, "%s: only %zu bytes long — too short to be a tar archive",
                    r->archive.name, len);
    return fail(r, "%s: not a tar archive (the header checksum at offset 0 is wrong)",
                r->archive.name);
}

int tmd_reader_next(struct tmd_reader *r, const struct tmd_entry **out)
{
    char     block[TMD_BLOCK_SIZE];
    size_t   got;
    unsigned extension_guard = 0;

    if (r->finished)
        return 0;

    entry_reset(&r->entry);
    r->member_start = tmd_source_offset(r->src);

    for (;;) {
        struct tmd_entry *e = &r->entry;
        enum tmd_format   format;
        uint64_t          payload_consumed = 0;
        char              name_field[F_NAME_LEN + 1];
        char              prefix_field[F_PREFIX_LEN + 1];
        uint64_t          value;

        /* A pathological archive could be nothing but extension headers; each
         * iteration that does not produce an entry is counted. */
        if (++extension_guard > 1000) {
            warn_archive(r, "over 1000 extension headers for one member — giving up on it");
            r->finished = true;
            return 0;
        }

        if (!read_block(r, block, &got)) {
            if (got == 0) {
                if (tmd_source_error(r->src))
                    return fail(r, "%s: read error", r->archive.name);
                if (!r->saw_any_header)
                    return fail(r, "%s: empty file, not a tar archive",
                                r->archive.name);
                warn_archive(r, "archive ends without the two-block end-of-archive marker");
            } else if (!r->saw_any_header) {
                /* A first block that is too short to be a header is not a
                 * truncated archive; nothing ever established that this was an
                 * archive at all. */
                return fail_not_tar(r, block, got);
            } else {
                warn_archive(r,
                             "archive ends mid-header: %zu of %d bytes at offset %llu",
                             got, TMD_BLOCK_SIZE,
                             (unsigned long long)r->member_start);
            }
            r->finished = true;
            return 0;
        }

        /* --- end of archive -------------------------------------------- */
        if (block_is_zero(block)) {
            char   second[TMD_BLOCK_SIZE];
            size_t second_got;

            if (!read_block(r, second, &second_got) || !block_is_zero(second)) {
                /*
                 * One zero block is not an end marker. GNU tar skips it and
                 * keeps reading, which is what makes archives concatenated by
                 * `cat` readable, so that is what happens here — but it is
                 * worth saying, because it is also what a corrupt block looks
                 * like.
                 */
                warn_archive(r, "a single zero block at offset %llu is not an end marker",
                             (unsigned long long)(tmd_source_offset(r->src) -
                                                  (uint64_t)TMD_BLOCK_SIZE -
                                                  (uint64_t)second_got));
                if (second_got == 0) {
                    r->finished = true;
                    return 0;
                }
                memcpy(block, second, TMD_BLOCK_SIZE);
                /* Fall through and read this block as a header. */
            } else {
                r->archive.eof_marker = true;
                r->finished = true;
                consume_trailer(r);
                return 0;
            }
        }

        /* --- checksum --------------------------------------------------- */
        e->chksum_unsigned = tmd_header_checksum_unsigned(block);
        e->chksum_signed = tmd_header_checksum_signed(block);
        {
            bool readable = tmd_parse_num(block + F_CHKSUM, 8, &value);

            e->chksum_stored = readable ? (uint32_t)value : 0;
            e->chksum_ok = readable &&
                           ((value == e->chksum_unsigned) ||
                            ((int64_t)value == (int64_t)e->chksum_signed));

            if (!e->chksum_ok) {
                if (!r->saw_any_header)
                    return fail_not_tar(r, block, TMD_BLOCK_SIZE);
                r->archive.bad_checksums++;
                /* An unreadable field and a wrong one are different damage and
                 * lead to different conclusions — "the archive was truncated
                 * and resumed on a non-header" versus "one byte flipped" — so
                 * they get different messages rather than a stored value of 0
                 * that was never in the file. */
                /* The offset of the damaged *block*, not of the member: a
                 * member can begin several extension headers earlier, and the
                 * number here is one somebody will take to `dd` or a hex
                 * editor. */
                uint64_t at = tmd_source_offset(r->src) - (uint64_t)TMD_BLOCK_SIZE;

                if (readable)
                    warn_entry(e, "header checksum mismatch at offset %llu: field says %u, "
                                  "block sums to %u (unsigned) / %d (signed)",
                               (unsigned long long)at,
                               e->chksum_stored, e->chksum_unsigned, e->chksum_signed);
                else
                    warn_entry(e, "header checksum field at offset %llu is not octal; "
                                  "block sums to %u",
                               (unsigned long long)at, e->chksum_unsigned);
            }
        }
        r->saw_any_header = true;

        /* --- the fields every dialect shares ---------------------------- */
        format = detect_format(block);
        fill_raw(&e->raw, block);
        tmd_field_str(block + F_NAME, F_NAME_LEN, name_field);
        tmd_field_str(block + F_PREFIX, F_PREFIX_LEN, prefix_field);
        e->typeflag = block[F_TYPEFLAG];
        e->offset = r->member_start;

        if (is_base256(block + F_SIZE) || is_base256(block + F_UID) ||
            is_base256(block + F_GID) || is_base256(block + F_MTIME))
            r->archive.features.base256_fields++;

        if (tmd_parse_num(block + F_SIZE, 12, &value))
            e->data_size = value;
        else if (!tmd_field_empty(block + F_SIZE, 12))
            warn_entry(e, "size field is not a valid number");
        e->size = e->data_size;

        /* --- extension headers ------------------------------------------ */
        switch (e->typeflag) {
        case 'L':
        case 'K': {
            size_t len;
            char  *text = read_payload(r, e->data_size, MAX_LONGNAME, &len,
                                       e->typeflag == 'L' ? "GNU long name"
                                                          : "GNU long link name");
            /* The stored name includes its own terminating NUL in the size,
             * and tmd_xstrndup stops at the first one either way. */
            if (e->typeflag == 'L') {
                free(r->pending_name);
                r->pending_name = tmd_xstrndup(text, len);
                r->archive.features.gnu_longname++;
            } else {
                free(r->pending_linkname);
                r->pending_linkname = tmd_xstrndup(text, len);
                r->archive.features.gnu_longlink++;
            }
            free(text);
            note_format(r, TMD_FMT_GNU);
            continue;
        }
        case 'x':
        case 'g': {
            size_t len;

            /*
             * Which tool wrote this, from the name it gave its own header.
             *
             * The extended header is a member like any other and needs a name
             * nothing will collide with, so every implementation invents one —
             * and they all invented a different one. GNU tar writes
             * "<dir>/PaxHeaders/<base>", libarchive writes "<dir>/PaxHeader/<base>"
             * (no 's'), and star writes neither. It is a naming convention
             * rather than a declaration, so it is reported as an inference,
             * but it is the only fingerprint a pax archive of ordinary files
             * carries at all: the attribute keys in one are just "path" and
             * "mtime", which everybody writes.
             */
            if (!r->archive.writer) {
                if (strstr(name_field, "PaxHeaders/") != NULL)
                    r->archive.writer = "GNU tar";
                else if (strstr(name_field, "PaxHeader/") != NULL)
                    r->archive.writer = "libarchive (bsdtar)";
            }

            char  *text = read_payload(r, e->data_size, MAX_PAX, &len,
                                       e->typeflag == 'x' ? "pax extended header"
                                                          : "pax global header");
            if (e->typeflag == 'x') {
                parse_pax(r, text, len, &r->pending_pax, &r->pending_npax, false);
                r->archive.features.pax_headers++;
            } else {
                r->archive.features.pax_globals++;
                parse_pax(r, text, len, &r->archive.pax_global,
                          &r->archive.npax_global, true);
            }
            free(text);
            note_format(r, TMD_FMT_PAX);
            continue;
        }
        default:
            break;
        }

        note_format(r, format);
        e->format = format;
        if (r->pending_npax > 0 || r->archive.npax_global > 0)
            e->format = TMD_FMT_PAX;

        /* --- the path --------------------------------------------------- */
        if (format == TMD_FMT_USTAR || format == TMD_FMT_STAR ||
            format == TMD_FMT_PAX) {
            e->path = join_prefix(prefix_field, name_field);
            e->path_source = prefix_field[0] ? "prefix+name" : "name";
            if (prefix_field[0])
                r->archive.features.prefix_used++;
        } else {
            e->path = tmd_xstrdup(name_field);
            e->path_source = "name";
        }
        e->linkpath = tmd_xstrdup(e->raw.linkname);
        e->linkpath_source = "linkname";

        if (r->pending_name) {
            free(e->path);
            e->path = r->pending_name;
            r->pending_name = NULL;
            e->path_source = "GNU long name";
        }
        if (r->pending_linkname) {
            free(e->linkpath);
            e->linkpath = r->pending_linkname;
            r->pending_linkname = NULL;
            e->linkpath_source = "GNU long link name";
        }

        /* --- the numeric fields ----------------------------------------- */
        if (tmd_parse_num(block + F_MODE, 8, &value))
            e->mode = (uint32_t)(value & 07777);
        else if (!tmd_field_empty(block + F_MODE, 8))
            warn_entry(e, "mode field is not a valid number");

        if (!tmd_parse_num_signed(block + F_UID, 8, &e->uid) &&
            !tmd_field_empty(block + F_UID, 8))
            warn_entry(e, "uid field is not a valid number");
        if (!tmd_parse_num_signed(block + F_GID, 8, &e->gid) &&
            !tmd_field_empty(block + F_GID, 8))
            warn_entry(e, "gid field is not a valid number");

        parse_time_field(e, &e->mtime, block + F_MTIME, 12, "mtime");

        if (format == TMD_FMT_GNU) {
            parse_time_field(e, &e->atime, block + F_GNU_ATIME, 12, "atime");
            parse_time_field(e, &e->ctime, block + F_GNU_CTIME, 12, "ctime");
        }

        if (format != TMD_FMT_V7) {
            e->uname = tmd_xstrdup(e->raw.uname);
            e->gname = tmd_xstrdup(e->raw.gname);
        } else {
            e->uname = tmd_xstrdup("");
            e->gname = tmd_xstrdup("");
        }

        e->kind = tmd_kind_of(e->typeflag, e->path);
        if (e->kind == TMD_KIND_CHARDEV || e->kind == TMD_KIND_BLOCKDEV) {
            uint64_t major = 0, minor = 0;
            if (tmd_parse_num(block + F_DEVMAJOR, 8, &major) &&
                tmd_parse_num(block + F_DEVMINOR, 8, &minor)) {
                e->has_dev = true;
                e->devmajor = (uint32_t)major;
                e->devminor = (uint32_t)minor;
            } else {
                warn_entry(e, "device numbers are missing or not octal");
            }
        }

        if (e->kind == TMD_KIND_UNKNOWN)
            warn_entry(e, "unrecognized typeflag '%c' (0x%02x)",
                       (e->typeflag >= 32 && e->typeflag < 127) ? e->typeflag : '?',
                       (unsigned char)e->typeflag);

        /* --- sparse ------------------------------------------------------ */
        if (e->typeflag == 'S' && format == TMD_FMT_GNU) {
            e->is_sparse = true;
            sparse_from_block(e, block + F_GNU_SPARSE, 4);
            if (tmd_parse_num(block + F_GNU_REALSIZE, 12, &value))
                e->realsize = value;
            if (block[F_GNU_ISEXT] != '\0')
                read_sparse_extensions(r, e);
            e->size = e->realsize ? e->realsize : e->data_size;
        }

        /* --- pax attributes, local over global --------------------------- */
        if (r->archive.npax_global > 0) {
            size_t i;
            for (i = 0; i < r->archive.npax_global; i++)
                kv_add(&e->pax, &e->npax, r->archive.pax_global[i].key,
                       r->archive.pax_global[i].value,
                       strlen(r->archive.pax_global[i].value));
        }
        if (r->pending_npax > 0) {
            size_t i;
            for (i = 0; i < r->pending_npax; i++)
                kv_add(&e->pax, &e->npax, r->pending_pax[i].key,
                       r->pending_pax[i].value, strlen(r->pending_pax[i].value));
            kv_free(r->pending_pax, r->pending_npax);
            r->pending_pax = NULL;
            r->pending_npax = 0;
        }
        if (e->npax > 0)
            apply_pax(r, e);

        /* --- pax sparse (GNU.sparse.*, formats 0.0, 0.1 and 1.0) --------- */
        {
            const char *major = kv_get(e->pax, e->npax, "GNU.sparse.major");
            const char *minor = kv_get(e->pax, e->npax, "GNU.sparse.minor");
            const char *map = kv_get(e->pax, e->npax, "GNU.sparse.map");
            const char *realsize = kv_get(e->pax, e->npax, "GNU.sparse.realsize");
            const char *size_v1 = kv_get(e->pax, e->npax, "GNU.sparse.size");
            const char *name = kv_get(e->pax, e->npax, "GNU.sparse.name");

            if (major || minor || map || realsize || size_v1) {
                e->is_sparse = true;
                if (realsize)
                    (void)parse_pax_u64(realsize, &e->realsize);
                else if (size_v1)
                    (void)parse_pax_u64(size_v1, &e->realsize);
                if (name) {
                    /* Format 0.x hides the real name here and puts a dummy in
                     * the header, so the header name is not the file's name. */
                    free(e->path);
                    e->path = tmd_xstrdup(name);
                    e->path_source = "GNU.sparse.name";
                }
                if (map)
                    sparse_from_pax_map(e, map);
                if (e->realsize)
                    e->size = e->realsize;
            }

            if (major && minor && strcmp(major, "1") == 0 &&
                strcmp(minor, "0") == 0) {
                uint64_t used = read_pax_sparse_map_1_0(r, e, e->data_size);
                /* The map came out of the payload, so that much less of it is
                 * left to step over. */
                payload_consumed += used;
            }
        }

        /* --- step over the payload --------------------------------------- */
        {
            uint64_t payload = tmd_round_up_blocks(e->data_size);

            if (payload > payload_consumed)
                payload -= payload_consumed;
            else
                payload = 0;

            /* Members that carry no data at all: a hard link's size field is
             * advisory in some dialects and has been seen to be non-zero, and
             * skipping that many bytes would eat the next header. */
            if (e->kind == TMD_KIND_DIR || e->kind == TMD_KIND_SYMLINK ||
                e->kind == TMD_KIND_HARDLINK || e->kind == TMD_KIND_CHARDEV ||
                e->kind == TMD_KIND_BLOCKDEV || e->kind == TMD_KIND_FIFO) {
                if (e->data_size != 0) {
                    warn_entry(e, "a %s carries a size of %llu; ignoring the payload",
                               tmd_kind_name(e->kind),
                               (unsigned long long)e->data_size);
                    payload = 0;
                    e->size = 0;
                    e->data_size = 0;
                }
            }

            if (payload > 0 && !tmd_source_skip(r->src, payload))
                warn_entry(e, "member data is truncated");
        }

        e->stored_size = tmd_source_offset(r->src) - r->member_start;

        /* --- archive-level accounting ------------------------------------ */
        {
            struct tmd_features *f = &r->archive.features;
            size_t               path_len = e->path ? strlen(e->path) : 0;
            size_t               k;

            if (path_len > f->max_path)
                f->max_path = path_len;
            /* What a reader that does not understand this archive's extensions
             * would get wrong. 100 is the v7 name field; 255 is the most a
             * ustar prefix and name can express between them. */
            if (path_len > 100)
                f->paths_over_100++;
            if (path_len > 255)
                f->paths_over_255++;

            if (e->uid > f->max_uid)
                f->max_uid = e->uid;
            if (e->gid > f->max_gid)
                f->max_gid = e->gid;
            if ((e->uname && e->uname[0]) || (e->gname && e->gname[0]))
                f->names_present++;
            if (e->mtime.nsec || e->atime.nsec || e->ctime.nsec)
                f->subsecond_times++;
            if (e->atime.present)
                f->atime_present++;
            if (e->ctime.present)
                f->ctime_present++;

            switch (e->kind) {
            case TMD_KIND_DUMPDIR:  f->dumpdirs++; break;
            case TMD_KIND_MULTIVOL: f->multivolume++; break;
            case TMD_KIND_VOLUME:   f->volume_labels++; break;
            case TMD_KIND_XATTR:    f->xattr_members++; break;
            case TMD_KIND_UNKNOWN:  f->unknown_typeflags++; break;
            default: break;
            }
            if (e->is_sparse) {
                if (e->typeflag == 'S')
                    f->sparse_gnu_old++;
                else
                    f->sparse_pax++;
            }
            for (k = 0; k < e->npax; k++)
                note_pax_key(f, e->pax[k].key);
        }

        r->archive.entries++;
        r->archive.total_size += e->size;
        r->archive.total_stored += e->stored_size;
        if (e->kind <= TMD_KIND_XATTR)
            r->archive.counts[e->kind]++;
        if (!r->archive.writer && format == TMD_FMT_GNU)
            r->archive.writer = "GNU tar";
        if (!r->archive.writer && format == TMD_FMT_STAR)
            r->archive.writer = "star";

        *out = e;
        return 1;
    }
}
