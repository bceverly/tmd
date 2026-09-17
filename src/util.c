/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "util.h"

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Allocation                                                                */
/* ------------------------------------------------------------------------- */

static void oom(size_t n)
{
    (void)fprintf(stderr, "tmd: out of memory allocating %zu bytes\n", n);
    exit(70); /* EX_SOFTWARE */
}

void *tmd_xmalloc(size_t n)
{
    /* malloc(0) is allowed to return NULL, which callers would read as a
     * failure. Ask for a byte instead so a zero-length string still has an
     * address to point at. */
    void *p = malloc(n ? n : 1);
    if (!p) {
        oom(n);
    }
    return p;
}

void *tmd_xcalloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) {
        oom(count * size);
    }
    return p;
}

void *tmd_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        oom(n);
    }
    return q;
}

char *tmd_xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *p = tmd_xmalloc(n);
    memcpy(p, s, n);
    return p;
}

char *tmd_xstrndup(const char *s, size_t n)
{
    size_t len = 0;
    char  *p;

    while (len < n && s[len] != '\0') {
        len++;
    }
    p = tmd_xmalloc(len + 1);
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

char *tmd_xvasprintf(const char *fmt, va_list ap)
{
    char    stack[256];
    va_list copy;
    int     n;
    char   *out;

    va_copy(copy, ap);
    /* The format is checked by the compiler at every call site: this function
     * is declared TMD_PRINTF(1, 0) and -Wformat-nonliteral rejects any caller
     * passing a format that is not a literal it can verify. */
    n = vsnprintf(stack, sizeof(stack), fmt, copy); /* Flawfinder: ignore */
    va_end(copy);
    if (n < 0) {
        return tmd_xstrdup("");
    }
    if ((size_t)n < sizeof(stack)) {
        return tmd_xstrdup(stack);
    }

    out = tmd_xmalloc((size_t)n + 1);
    (void)vsnprintf(out, (size_t)n + 1, fmt, ap); /* Flawfinder: ignore */
    return out;
}

char *tmd_xasprintf(const char *fmt, ...)
{
    va_list ap;
    char   *out;

    va_start(ap, fmt);
    out = tmd_xvasprintf(fmt, ap);
    va_end(ap);
    return out;
}

/* ------------------------------------------------------------------------- */
/* Growable buffer                                                           */
/* ------------------------------------------------------------------------- */

void tmd_buf_init(struct tmd_buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void tmd_buf_free(struct tmd_buf *b)
{
    free(b->data);
    tmd_buf_init(b);
}

static void buf_reserve(struct tmd_buf *b, size_t extra)
{
    size_t want = b->len + extra + 1; /* +1 keeps room for the terminator */

    if (want <= b->cap) {
        return;
    }
    if (b->cap == 0) {
        b->cap = 64;
    }
    while (b->cap < want) {
        b->cap *= 2;
    }
    b->data = tmd_xrealloc(b->data, b->cap);
}

void tmd_buf_add(struct tmd_buf *b, const void *data, size_t n)
{
    if (n == 0) {
        return;
    }
    buf_reserve(b, n);
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void tmd_buf_addc(struct tmd_buf *b, char c)
{
    buf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void tmd_buf_addstr(struct tmd_buf *b, const char *s)
{
    tmd_buf_add(b, s, strlen(s));
}

void tmd_buf_addf(struct tmd_buf *b, const char *fmt, ...)
{
    va_list ap;
    char   *text;

    va_start(ap, fmt);
    text = tmd_xvasprintf(fmt, ap);
    va_end(ap);
    tmd_buf_addstr(b, text);
    free(text);
}

char *tmd_buf_detach(struct tmd_buf *b)
{
    char *out;

    /* A buffer nothing was ever added to has no allocation at all, and every
     * caller expects a string it can free. Give it an empty one. */
    if (!b->data) {
        b->data = tmd_xmalloc(1);
        b->data[0] = '\0';
    }
    out = b->data;
    tmd_buf_init(b);
    return out;
}

/* ------------------------------------------------------------------------- */
/* Header field decoding                                                     */
/* ------------------------------------------------------------------------- */

bool tmd_field_empty(const char *field, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (field[i] != '\0' && field[i] != ' ') {
            return false;
        }
    }
    return true;
}

/*
 * Base-256: the high bit of byte 0 marks it, the remaining bits of that byte
 * plus every byte after it are a big-endian two's-complement integer. GNU tar
 * writes 0x80 for positive and 0xff for negative, so the sign is carried by
 * bit 6 of the first byte once the marker bit is cleared.
 */
static bool parse_base256(const char *field, size_t len, int64_t *out)
{
    const unsigned char *p = (const unsigned char *)field;
    bool     negative = (p[0] & 0x40) != 0;
    uint64_t value = negative ? UINT64_MAX : 0;
    size_t   i;

    /* Only the low 8 bytes can survive in an int64_t. Everything above them
     * has to be pure sign extension, or the value does not fit and saying so
     * is better than reporting a truncated number as fact. */
    for (i = 0; i + 8 < len; i++) {
        unsigned char expect = negative ? 0xff : 0x00;
        unsigned char byte = (unsigned char)(i == 0 ? (p[0] & 0x7f) : p[i]);
        if (byte != (i == 0 ? (unsigned char)(expect & 0x7f) : expect)) {
            return false;
        }
    }

    for (; i < len; i++) {
        unsigned char byte = (i == 0) ? (unsigned char)(p[0] & 0x7f) : p[i];
        value = (value << 8) | byte;
    }

    *out = (int64_t)value;
    return true;
}

static bool parse_octal(const char *field, size_t len, uint64_t *out)
{
    uint64_t value = 0;
    size_t   i = 0;
    bool     digits = false;

    /* Leading whitespace is legal; some writers right-align the number. */
    while (i < len && (field[i] == ' ' || field[i] == '\t')) {
        i++;
    }

    for (; i < len && field[i] >= '0' && field[i] <= '7'; i++) {
        /* An 8-byte field of octal digits cannot overflow, but a caller may
         * hand us a longer one and a hostile archive will. */
        if (value > (UINT64_MAX >> 3)) {
            return false;
        }
        value = (value << 3) | (uint64_t)(field[i] - '0');
        digits = true;
    }

    /* Whatever is left has to be padding. A field like "0644x" is corrupt,
     * and quietly accepting the first four characters of it hides that. */
    for (; i < len; i++) {
        if (field[i] != '\0' && field[i] != ' ' && field[i] != '\t') {
            return false;
        }
    }

    if (!digits) {
        return false;
    }
    *out = value;
    return true;
}

bool tmd_parse_num(const char *field, size_t len, uint64_t *out)
{
    if (len == 0) {
        return false;
    }
    if ((unsigned char)field[0] & 0x80) {
        int64_t signed_value;
        if (!parse_base256(field, len, &signed_value) || signed_value < 0) {
            return false;
        }
        *out = (uint64_t)signed_value;
        return true;
    }
    return parse_octal(field, len, out);
}

bool tmd_parse_num_signed(const char *field, size_t len, int64_t *out)
{
    uint64_t unsigned_value;

    if (len == 0) {
        return false;
    }
    if ((unsigned char)field[0] & 0x80) {
        return parse_base256(field, len, out);
    }
    if (!parse_octal(field, len, &unsigned_value)) {
        return false;
    }
    if (unsigned_value > (uint64_t)INT64_MAX) {
        return false;
    }
    *out = (int64_t)unsigned_value;
    return true;
}

void tmd_field_str(const char *field, size_t len, char *out)
{
    size_t i;

    for (i = 0; i < len && field[i] != '\0'; i++) {
        out[i] = field[i];
    }
    out[i] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Presentation helpers                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Walk a string as UTF-8, reporting where it stops being valid.
 *
 * The offset matters. "this path is not UTF-8" tells a reader nothing they can
 * act on; "byte 14 is not UTF-8" points at the character, which is the
 * difference between a mojibake filename somebody can fix and a mystery.
 *
 * `bad` may be NULL when only the verdict is wanted.
 */
static bool utf8_scan(const char *s, size_t len, size_t *bad)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t               i = 0;

    while (i < len) {
        unsigned char c = p[i];
        size_t        extra;
        uint32_t      cp;

        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xe0) == 0xc0) {
            extra = 1;
            cp = c & 0x1fu;
        } else if ((c & 0xf0) == 0xe0) {
            extra = 2;
            cp = c & 0x0fu;
        } else if ((c & 0xf8) == 0xf0) {
            extra = 3;
            cp = c & 0x07u;
        } else {
            goto bad_at; /* a continuation byte or an over-long lead */
        }
        if (i + extra >= len) {
            goto bad_at; /* the continuation bytes run past the end */
        }
        for (size_t k = 1; k <= extra; k++) {
            if ((p[i + k] & 0xc0) != 0x80) {
                goto bad_at;
            }
            cp = (cp << 6) | (uint32_t)(p[i + k] & 0x3f);
        }
        /* Reject the encodings that are valid bytes but not valid text:
         * over-long forms, surrogates, and anything past U+10FFFF. Those are
         * exactly what a path crafted to slip past a filter looks like. */
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000)) {
            goto bad_at;
            }
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            goto bad_at;
        }
        i += extra + 1;
    }
    return true;

bad_at:
    if (bad) {
        *bad = i;
    }
    return false;
}

bool tmd_utf8_valid(const char *s, size_t len)
{
    return utf8_scan(s, len, NULL);
}

bool tmd_utf8_first_invalid(const char *s, size_t len, size_t *offset)
{
    return !utf8_scan(s, len, offset);
}

/* ------------------------------------------------------------------------- */
/* Base64                                                                     */
/*                                                                            */
/* Two directions, for two different jobs. Encoding puts a header's 512 raw    */
/* bytes into JSON, where they have to survive being read back byte for byte.  */
/* Decoding turns the xattr values libarchive and star write as base64 back    */
/* into the bytes they stand for, so that `SCHILY.xattr.user.tag` reads as its */
/* value rather than as the encoding of one.                                  */
/* ------------------------------------------------------------------------- */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *tmd_base64_encode(const void *data, size_t n)
{
    const unsigned char *p = data;
    struct tmd_buf       b;
    size_t               i;

    /*
     * Built into the growable buffer the rest of this program uses, rather
     * than into a hand-sized allocation.
     *
     * The arithmetic for "how big is the encoding of n bytes" is easy to get
     * right and hard to *show* is right: it has an integer division, a
     * multiplication that can overflow for a large n, and a degenerate empty
     * case. Written that way it was correct and the clang analyzer still could
     * not confirm it -- and an analyzer that cannot follow the bound is a fair
     * proxy for a reader who cannot either. tmd_buf grows itself, so there is
     * no bound left to argue about, and 512 bytes of header is not a size
     * where the difference costs anything.
     */
    tmd_buf_init(&b);
    for (i = 0; i < n; i += 3) {
        size_t   have = n - i; /* 3, or 1 or 2 in the final group */
        uint32_t v = (uint32_t)p[i] << 16;

        if (have > 1) {
            v |= (uint32_t)p[i + 1] << 8;
        }
        if (have > 2) {
            v |= p[i + 2];
        }

        tmd_buf_addc(&b, B64[(v >> 18) & 0x3f]);
        tmd_buf_addc(&b, B64[(v >> 12) & 0x3f]);
        /* The ternary's type is int, so the narrowing is named rather than
         * left to happen quietly. */
        tmd_buf_addc(&b, (char)(have > 1 ? B64[(v >> 6) & 0x3f] : '='));
        tmd_buf_addc(&b, (char)(have > 2 ? B64[v & 0x3f] : '='));
    }
    return tmd_buf_detach(&b);
}

/*
 * Decode, strictly.
 *
 * Strict because the result is shown to a reader as "this is what the
 * attribute says", and a decoder that quietly skips what it does not
 * understand would turn a malformed value into a plausible-looking one. A
 * value that is not base64 is reported as not base64, and the raw text is
 * shown instead.
 */
static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') { return c - 'A';
    }
    if (c >= 'a' && c <= 'z') { return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') { return c - '0' + 52;
    }
    if (c == '+') { return 62;
    }
    if (c == '/') { return 63;
    }
    return -1;
}

bool tmd_base64_decode(const char *s, struct tmd_buf *out)
{
    size_t   len = strlen(s);
    size_t   i;
    uint32_t acc = 0;
    unsigned bits = 0;

    if (len == 0 || (len % 4) != 0) {
        return false;
    }
    for (i = 0; i < len; i++) {
        int v;

        if (s[i] == '=') {
            /* Padding is only ever the last one or two characters. */
            if (i + 2 < len || (i + 2 == len && s[i + 1] != '=')) {
                return false;
            }
            break;
        }
        v = b64_value((unsigned char)s[i]);
        if (v < 0) {
            return false;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            tmd_buf_addc(out, (char)(unsigned char)((acc >> bits) & 0xff));
        }
    }
    return true;
}

/*
 * Would extracting this write outside the directory you are standing in?
 *
 * Three ways an archive can do it, and the reason tmd is worth asking rather
 * than `tar -t`: tar answers by extracting, which is too late.
 *
 *   /etc/passwd        an absolute path. GNU tar strips the leading slash by
 *                      default and honors it under -P; bsdtar likewise. So
 *                      whether this escapes depends on flags the person
 *                      extracting may not think about.
 *   ../../etc/passwd   a traversal. Counted by depth rather than by looking
 *                      for "..", because a/../b does NOT escape and a
 *                      substring match would say it does.
 *   link -> /etc       a symlink out of the tree. The dangerous form is two
 *                      members: a link pointing out, then a later member
 *                      written through it.
 *
 * Depth is the honest test. Start at zero, add one for each real component,
 * subtract one for each "..", and the moment it goes below zero the path has
 * left the tree it started in -- whatever it does afterwards.
 */
static bool walk_depth(const char *s, long *depth)
{
    const char *p = s;

    while (*p) {
        const char *start = p;
        size_t      len;

        while (*p && *p != '/') {
            p++;
        }
        len = (size_t)(p - start);
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            --(*depth);
            if (*depth < 0) {
                return false;
            }
        } else if (len != 0 && !(len == 1 && start[0] == '.')) {
            ++(*depth);
        }
        while (*p == '/') {
            p++;
        }
    }
    return true;
}

bool tmd_path_escapes(const char *path)
{
    long depth = 0;

    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '/') {
        return true;
    }
    return !walk_depth(path, &depth);
}

bool tmd_link_escapes(const char *path, const char *target)
{
    long        depth = 0;
    const char *slash;

    if (!target || !target[0]) {
        return false;
    }
    if (target[0] == '/') {
        return true;
    }
    if (!path) {
        return false;
    }
    /*
     * A relative target is resolved against the directory the link sits in, so
     * that is where the depth starts: "a/b/link -> ../../c" lands outside,
     * while "a/b/link -> ../c" stays in.
     */
    slash = strrchr(path, '/');
    if (slash) {
        char *dir = tmd_xstrndup(path, (size_t)(slash - path));
        bool  ok = walk_depth(dir, &depth);

        free(dir);
        if (!ok) {
            return true; /* the link's own path already escapes */
        }
    }
    return !walk_depth(target, &depth);
}

/*
 * find(1)'s rule, which is the one everybody already knows: a pattern with a
 * slash in it is matched against the whole stored path, a pattern without one
 * against the basename alone. So `nginx.conf` finds the file at any depth,
 * while a pattern like "etc/nginx/" followed by a star finds what is under that
 * directory. (Spelled out rather than written as a glob: the glob contains the
 * two characters that open a comment, and -Wcomment rightly objects.)
 *
 * Case-sensitive, because a tar path is a string of bytes: two members
 * differing only in case are two different members, and an archive can hold
 * both.
 */
bool tmd_path_matches(const char *path, const char *pattern)
{
    char        stack[256];
    char       *heap = NULL;
    const char *base;
    const char *end;
    size_t      len;
    bool        hit;

    if (strchr(pattern, '/') != NULL) {
        return fnmatch(pattern, path, 0) == 0;
    }

    /* A directory member is stored with a trailing slash ("etc/nginx/"), so its
     * basename is the component before that slash and not the empty string
     * after it. */
    end = path + strlen(path);
    while (end > path && end[-1] == '/') {
        end--;
    }
    base = end;
    while (base > path && base[-1] != '/') {
        base--;
    }
    len = (size_t)(end - base);

    if (len < sizeof(stack)) {
        memcpy(stack, base, len);
        stack[len] = '\0';
        return fnmatch(pattern, stack, 0) == 0;
    }
    heap = tmd_xstrndup(base, len);
    hit = fnmatch(pattern, heap, 0) == 0;
    free(heap);
    return hit;
}

void tmd_mode_string(uint32_t mode, enum tmd_kind kind, char out[11])
{
    static const char rwx[8][4] = { "---", "--x", "-w-", "-wx",
                                    "r--", "r-x", "rw-", "rwx" };
    char type;

    switch (kind) {
    case TMD_KIND_DIR:        type = 'd'; break;
    case TMD_KIND_SYMLINK:    type = 'l'; break;
    case TMD_KIND_HARDLINK:   type = 'h'; break;
    case TMD_KIND_CHARDEV:    type = 'c'; break;
    case TMD_KIND_BLOCKDEV:   type = 'b'; break;
    case TMD_KIND_FIFO:       type = 'p'; break;
    case TMD_KIND_CONTIGUOUS: type = 'C'; break;
    case TMD_KIND_VOLUME:     type = 'V'; break;
    case TMD_KIND_DUMPDIR:    type = 'D'; break;
    case TMD_KIND_MULTIVOL:   type = 'M'; break;
    case TMD_KIND_XATTR:      type = 'x'; break;
    default:                  type = '-'; break;
    }

    out[0] = type;
    memcpy(out + 1, rwx[(mode >> 6) & 7], 3);
    memcpy(out + 4, rwx[(mode >> 3) & 7], 3);
    memcpy(out + 7, rwx[mode & 7], 3);

    /* setuid/setgid/sticky replace the matching execute character, upper-case
     * when the execute bit underneath is not set — the same convention ls
     * uses, so a mode that is surprising looks surprising. */
    if (mode & 04000) {
        out[3] = (out[3] == 'x') ? 's' : 'S';
    }
    if (mode & 02000) {
        out[6] = (out[6] == 'x') ? 's' : 'S';
    }
    if (mode & 01000) {
        out[9] = (out[9] == 'x') ? 't' : 'T';
    }
    out[10] = '\0';
}

const char *tmd_human_size(uint64_t bytes, char *buf, size_t bufsz)
{
    static const char units[] = { 'B', 'K', 'M', 'G', 'T', 'P', 'E' };
    double value = (double)bytes;
    size_t unit = 0;

    while (value >= 1024.0 && unit + 1 < sizeof(units)) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) {
        (void)snprintf(buf, bufsz, "%uB", (unsigned)bytes);
    } else if (value < 10.0) {
        (void)snprintf(buf, bufsz, "%.1f%c", value, units[unit]);
    } else {
        (void)snprintf(buf, bufsz, "%.0f%c", value, units[unit]);
    }
    return buf;
}

uint64_t tmd_round_up_blocks(uint64_t n)
{
    uint64_t blocks;

    /* Saturate rather than wrap. A size field can say 2^64-1, and rounding
     * that up the obvious way yields 0 — which would turn "skip the payload"
     * into "skip nothing" and re-read the same header forever. The saturated
     * value is the largest multiple of the block size that fits, so a caller
     * that divides it by 512 still gets a whole number of blocks. */
    if (n > UINT64_MAX - (TMD_BLOCK_SIZE - 1)) {
        return (UINT64_MAX / TMD_BLOCK_SIZE) * TMD_BLOCK_SIZE;
    }
    blocks = (n + TMD_BLOCK_SIZE - 1) / TMD_BLOCK_SIZE;
    return blocks * TMD_BLOCK_SIZE;
}
