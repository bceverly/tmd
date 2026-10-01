/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * --diff and --verify.
 *
 * One machine underneath. Both build an index of what is EXPECTED — the first
 * archive for --diff, the manifest for --verify — then stream what is actually
 * FOUND past it, and report the three ways two sets of members can disagree:
 * something expected is absent, something present was not expected, and
 * something is in both but not the same.
 *
 * The index is one-sided on purpose. Buffering both archives in full would cost
 * twice the memory for no gain: the second side only ever needs to look its own
 * path up, which is a hash probe, so it streams. And what is kept per member is
 * a digest of the fields being compared rather than a whole entry — no raw
 * block, no pax list, no warnings — which is the difference between tens of
 * megabytes and hundreds on a large archive.
 *
 * Duplicates follow the rule the rest of the program follows: an archive can
 * hold the same path twice, extraction keeps the last one, so the last
 * occurrence is the one compared. That a path occurred more than once is
 * reported, because it changes what "the same" means.
 */
#include "compare.h"

#include <stdlib.h>
#include <string.h>

#include "hash.h"
#include "opts.h"
#include "render.h"
#include "source.h"
#include "tar.h"
#include "util.h"

/* Past this many members, say so once: the index is held in memory and
 * somebody comparing two very large archives should know why the machine went
 * quiet. Not a limit — a truncated comparison would be worse than the wait. */
#define COMPARE_LOUD_AT 100000

/* Which fields differ. Reported by name, so a reader knows whether a member
 * moved, was rewritten, or only had its clock changed. */
#define D_SIZE  (1u << 0)
#define D_MODE  (1u << 1)
#define D_MTIME (1u << 2)
#define D_KIND  (1u << 3)
#define D_LINK  (1u << 4)
#define D_HASH  (1u << 5)

/*
 * One side of a comparison.
 *
 * The `have_*` flags exist because a manifest need not state everything. A
 * line with no size means "this path should be here" and nothing about how big
 * it is, and comparing against a size nobody supplied would invent a failure.
 */
struct side {
    uint64_t      size;
    uint32_t      mode;
    int64_t       mtime;
    enum tmd_kind kind;
    char         *linkpath;
    uint64_t      offset;
    /* The content digest, under --hash or from a manifest line that carries
     * one. A member with no content -- or whose content could not all be
     * read -- has none. */
    char          digest[TMD_HASH_HEX_MAX];
    bool          have_digest;
    unsigned      count;
    bool          present;
    bool          have_size;
    bool          have_mode;
    bool          have_mtime;
    bool          have_kind;
};

struct pair {
    char        *path;
    struct side  expected;
    struct side  found;
};

struct index {
    struct pair *pairs;
    size_t       npairs;
    size_t       cap;
    /* Open addressing: slots hold 1-based indices into `pairs`, so 0 is empty
     * and no separate occupancy array is needed. */
    size_t      *slots;
    size_t       nslots;
    bool         warned_large;
};

/* ------------------------------------------------------------------------- */
/* The index                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Indices into the slot table are taken with % rather than & (nslots - 1).
 *
 * The mask is correct only while nslots is a power of two, which it is by
 * construction -- and which is stated nowhere that a compiler, an analyzer or
 * the next reader can check. The clang analyzer said so: it could not establish
 * the bound and reported the probe as a possible out-of-range access. A modulo
 * is provably in range, and a division per member is immaterial beside the
 * strcmp it sits next to.
 */
static size_t hash_path(const char *s)
{
    /* FNV-1a. Short, well spread over path-shaped strings, and there is no
     * reason to be cleverer: a collision costs one extra strcmp. */
    uint64_t h = 1469598103934665603ULL;

    while (*s)
    {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return (size_t)h;
}

static void index_init(struct index *ix)
{
    memset(ix, 0, sizeof(*ix));
    ix->nslots = 1024;
    ix->slots = tmd_xcalloc(ix->nslots, sizeof(*ix->slots));
}

static void index_free(struct index *ix)
{
    size_t i;

    for (i = 0; i < ix->npairs; i++)
    {
        free(ix->pairs[i].path);
        free(ix->pairs[i].expected.linkpath);
        free(ix->pairs[i].found.linkpath);
    }
    free(ix->pairs);
    free(ix->slots);
    memset(ix, 0, sizeof(*ix));
}

static void index_rehash(struct index *ix)
{
    size_t  n = ix->nslots * 2;
    size_t *fresh = tmd_xcalloc(n, sizeof(*fresh));
    size_t  i;

    for (i = 0; i < ix->npairs; i++)
    {
        size_t slot = hash_path(ix->pairs[i].path) % n;

        while (fresh[slot])
        {
            slot = (slot + 1) % n;
        }
        fresh[slot] = i + 1;
    }
    free(ix->slots);
    ix->slots = fresh;
    ix->nslots = n;
}

/* The pair for this path, creating it if this is the first time it is seen. */
static struct pair *index_get(struct index *ix, const char *path)
{
    size_t slot;

    if (ix->npairs * 2 >= ix->nslots)
    {
        index_rehash(ix);
    }
    /*
     * NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) -- see below.
     *
     * The analyzer reports this index as tainted, and it is: for --verify the
     * path comes from a manifest file, which is untrusted input. What it cannot
     * follow is that `% ix->nslots` bounds the result, and nslots is never zero
     * (index_init sets it, index_rehash only doubles it). The warning is a
     * useful one in general -- it is what sent me to the over-long-line bug in
     * read_manifest above -- so it is suppressed here and nowhere else.
     */
    slot = hash_path(path) % ix->nslots;
    /* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
    while (ix->slots[slot] != 0)
    {
        /* The slot holds a 1-based index into `pairs`. The bound is checked
         * rather than assumed: it is an invariant of this file and not of the
         * type, so nothing but this loop enforces it -- which is also what the
         * clang analyzer said when it could not follow the reasoning. */
        size_t at = ix->slots[slot] - 1;

        if (at < ix->npairs && strcmp(ix->pairs[at].path, path) == 0)
        {
            return &ix->pairs[at];
        }
        slot = (slot + 1) % ix->nslots;
    }

    if (ix->npairs == ix->cap)
    {
        ix->cap = ix->cap ? ix->cap * 2 : 256;
        ix->pairs = tmd_xrealloc(ix->pairs, ix->cap * sizeof(*ix->pairs));
    }
    memset(&ix->pairs[ix->npairs], 0, sizeof(ix->pairs[ix->npairs]));
    ix->pairs[ix->npairs].path = tmd_xstrdup(path);
    ix->slots[slot] = ix->npairs + 1;
    ix->npairs++;

    if (ix->npairs == COMPARE_LOUD_AT && !ix->warned_large)
    {
        (void)fprintf(stderr, "tmd: comparing %d members; the index is held in "
                              "memory\n", COMPARE_LOUD_AT);
        ix->warned_large = true;
    }
    return &ix->pairs[ix->npairs - 1];
}

/* Fill one side from an archive member. The last occurrence of a path wins,
 * because that is the one extraction would leave on disk. */
static void side_from_entry(struct side *s, const struct tmd_entry *e)
{
    s->present = true;
    s->count++;
    s->size = e->size;
    s->have_size = true;
    s->mode = e->mode;
    s->have_mode = true;
    s->mtime = e->mtime.sec;
    s->have_mtime = e->mtime.present;
    s->kind = e->kind;
    s->have_kind = true;
    s->offset = e->offset;
    free(s->linkpath);
    s->linkpath = tmd_xstrdup(e->linkpath ? e->linkpath : "");
    s->have_digest = e->content_hashed;
    if (e->content_hashed)
    {
        (void)snprintf(s->digest, sizeof(s->digest), "%s", e->content_hash);
    }
}

/* Only the fields both sides actually state. */
static unsigned side_differences(const struct side *want,
                                 const struct side *got)
{
    unsigned d = 0;

    if (want->have_size && got->have_size && want->size != got->size)
    {
        d |= D_SIZE;
    }
    if (want->have_mode && got->have_mode && want->mode != got->mode)
    {
        d |= D_MODE;
    }
    if (want->have_mtime && got->have_mtime && want->mtime != got->mtime)
    {
        d |= D_MTIME;
    }
    if (want->have_kind && got->have_kind && want->kind != got->kind)
    {
        d |= D_KIND;
    }
    if (want->linkpath && got->linkpath &&
        strcmp(want->linkpath, got->linkpath) != 0)
    {
        d |= D_LINK;
    }
    /*
     * The digest is compared whenever the EXPECTED side states one. If the
     * found side then has none -- the member is a directory now, or its data
     * was cut short -- that is a difference too: the claim was "this content",
     * and it could not be shown to hold.
     */
    if (want->have_digest &&
        (!got->have_digest || strcmp(want->digest, got->digest) != 0))
    {
        d |= D_HASH;
    }
    return d;
}

static int pair_cmp(const void *a, const void *b)
{
    const struct pair *x = a;
    const struct pair *y = b;

    return strcmp(x->path, y->path);
}

/* ------------------------------------------------------------------------- */
/* Reading                                                                   */
/* ------------------------------------------------------------------------- */

static bool selected(const struct tmd_options *opt, const char *path)
{
    size_t i;

    if (opt->nmatch == 0)
    {
        return true;
    }
    for (i = 0; i < opt->nmatch; i++)
    {
        if (tmd_path_matches(path, opt->match[i]))
        {
            return true;
        }
    }
    return false;
}

/*
 * Read an archive into one side of the index.
 *
 * `which` picks the side, so the same function fills the expected side from the
 * first archive and the found side from the second.
 */
/* --hash digests exactly the members the comparison will look at. */
static bool want_member(const struct tmd_entry *e, const void *ctx)
{
    return tmd_entry_selected((const struct tmd_options *)ctx, e);
}

static bool read_into(struct index *ix, const char *path, bool as_found,
                      const struct tmd_options *opt, enum tmd_hash hash)
{
    struct tmd_source      *src;
    struct tmd_reader      *reader;
    const struct tmd_entry *e;
    char                   *err = NULL;
    int                     rc;
    bool                    ok = true;

    src = tmd_source_open(path, &err);
    if (!src)
    {
        (void)fprintf(stderr, "tmd: %s\n",
                      err ? err : "cannot open the archive");
        free(err);
        return false;
    }
    reader = tmd_reader_new(src);
    if (hash != TMD_HASH_NONE)
    {
        tmd_reader_hash(reader, hash, want_member, opt);
    }

    while ((rc = tmd_reader_next(reader, &e)) == 1)
    {
        struct pair *p;

        if (!e->path || !tmd_entry_selected(opt, e))
        {
            continue;
        }
        p = index_get(ix, e->path);
        side_from_entry(as_found ? &p->found : &p->expected, e);
    }
    if (rc < 0)
    {
        (void)fprintf(stderr, "tmd: %s\n", tmd_reader_error(reader));
        ok = false;
    }
    /* A comparison against half an archive is worse than no comparison. */
    if (tmd_source_codec_failed(src))
    {
        (void)fprintf(stderr, "tmd: %s: the %s stream ended badly; not "
                              "comparing a partial archive\n",
                      path, tmd_source_codec(src));
        ok = false;
    }
    tmd_reader_free(reader);
    tmd_source_close(src);
    return ok;
}

/*
 * Read a manifest into the expected side.
 *
 * The format is deliberately the simplest thing that can be produced by any
 * tool: one member per line, `SIZE PATH`, where SIZE may be "-" to mean "this
 * path should be here and I am not saying how big it is". Blank lines and lines
 * beginning with # are skipped.
 *
 * The first field counts as a size only when it is entirely digits or a single
 * "-". Otherwise the whole line is taken as the path, so a file actually named
 * "2481 notes.txt" still works as long as no size is given for it.
 */
/*
 * "md5:<32 hex>" or "sha256:<64 hex>", as --manifest writes them. False for
 * anything else, which leaves the token to be read as the start of the path.
 */
static bool parse_digest_token(const char *tok, size_t len, enum tmd_hash *algo,
                               char digest[TMD_HASH_HEX_MAX])
{
    static const enum tmd_hash algos[] = { TMD_HASH_MD5, TMD_HASH_SHA256 };
    size_t i;

    for (i = 0; i < sizeof(algos) / sizeof(algos[0]); i++)
    {
        const char *name = tmd_hash_name(algos[i]);
        size_t      nlen = strlen(name);
        size_t      hlen = tmd_hash_hex_len(algos[i]);
        size_t      k;

        if (len != nlen + 1 + hlen || strncmp(tok, name, nlen) != 0 ||
            tok[nlen] != ':')
        {
            continue;
        }
        for (k = 0; k < hlen; k++)
        {
            char c = tok[nlen + 1 + k];

            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F')))
            {
                return false;
            }
            /* Lowercase, because that is what tmd prints and compares. */
            if (c >= 'A' && c <= 'F')
            {
                c = (char)(c - 'A' + 'a');
            }
            digest[k] = c;
        }
        digest[hlen] = '\0';
        *algo = algos[i];
        return true;
    }
    return false;
}

static bool read_manifest(struct index *ix, const char *path,
                          const struct tmd_options *opt, enum tmd_hash *hash)
{
    FILE   *f;
    char    line[8192];
    size_t  lineno = 0;
    bool    ok = true;
    bool    skipping = false;

    f = fopen(path, "r"); /* Flawfinder: ignore */
    if (!f)
    {
        (void)fprintf(stderr, "tmd: %s: cannot open the manifest\n", path);
        return false;
    }

    while (fgets(line, sizeof(line), f))
    {
        char       *p = line;
        char       *end;
        const char *name;
        size_t      len = strlen(line);
        uint64_t    size = 0;
        bool        have_size = false;
        char        digest[TMD_HASH_HEX_MAX];
        bool        have_digest = false;
        /*
         * A line that did not fit.
         *
         * fgets stops at a newline or when the buffer is full, so a FULL buffer
         * with no newline is the one case that did not fit -- and a shorter
         * line with no newline is simply the last line of a file that does not
         * end in one, which is fine. Testing the buffer rather than asking the
         * stream means one read call and no feof, which is both simpler to
         * reason about and what the analyzer wanted.
         *
         * Without this the remainder came back as the NEXT line, turning one
         * over-long path into two plausible-looking ones: a silently wrong
         * answer from a file the caller may not have written.
         */
        bool overlong = (len == sizeof(line) - 1 && line[len - 1] != '\n');

        if (skipping)
        {
            skipping = overlong; /* still inside the long line */
            continue;
        }
        lineno++;
        if (overlong)
        {
            (void)fprintf(stderr, "tmd: %s:%zu: line is longer than %zu bytes; "
                                  "skipped\n", path, lineno, sizeof(line) - 1);
            ok = false;
            skipping = true;
            continue;
        }
        end = p + len;

        while (end > p && (end[-1] == '\n' || end[-1] == '\r'))
        {
            *--end = '\0';
        }
        while (*p == ' ' || *p == '\t')
        {
            p++;
        }
        if (*p == '\0' || *p == '#')
        {
            continue;
        }

        /* Is the first field a size? */
        {
            const char *first = p;
            char *sep = p;
            bool  digits = true;

            while (*sep && *sep != ' ' && *sep != '\t')
            {
                if (*sep < '0' || *sep > '9')
                {
                    digits = false;
                }
                sep++;
            }
            if (*sep && (digits || (sep - first == 1 && *first == '-')))
            {
                char *rest = sep;

                while (*rest == ' ' || *rest == '\t')
                {
                    rest++;
                }
                if (*rest)
                {
                    if (digits)
                    {
                        size = strtoull(first, NULL, 10);
                        have_size = true;
                    }
                    p = rest;
                }
            }
        }

        /*
         * Then, optionally, the digest --manifest --hash writes. One algorithm
         * per manifest: the archive is read once, so it can be hashed one way,
         * and a manifest that mixes them is not one tmd wrote.
         */
        {
            char         *sep = p;
            enum tmd_hash algo = TMD_HASH_NONE;

            while (*sep && *sep != ' ' && *sep != '\t')
            {
                sep++;
            }
            if (*sep && parse_digest_token(p, (size_t)(sep - p), &algo, digest))
            {
                char *rest = sep;

                while (*rest == ' ' || *rest == '\t')
                {
                    rest++;
                }
                if (*rest)
                {
                    if (*hash != TMD_HASH_NONE && *hash != algo)
                    {
                        (void)fprintf(stderr, "tmd: %s:%zu: a %s digest in a "
                                              "manifest of %s digests\n",
                                      path, lineno, tmd_hash_name(algo),
                                      tmd_hash_name(*hash));
                        ok = false;
                        continue;
                    }
                    *hash = algo;
                    have_digest = true;
                    p = rest;
                }
            }
        }

        name = p;
        if (!*name)
        {
            (void)fprintf(stderr, "tmd: %s:%zu: no path on this line\n",
                          path, lineno);
            ok = false;
            continue;
        }
        if (!selected(opt, name))
        {
            continue;
        }
        {
            struct pair *pr = index_get(ix, name);

            pr->expected.present = true;
            pr->expected.count++;
            pr->expected.size = size;
            pr->expected.have_size = have_size;
            pr->expected.have_digest = have_digest;
            if (have_digest)
            {
                (void)snprintf(pr->expected.digest, sizeof(pr->expected.digest),
                               "%s", digest);
            }
        }
    }
    (void)fclose(f);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Reporting                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * The differing fields, named.
 *
 * Worded for the comparison being made: a diff runs left to right in time, so
 * "size 6 -> 26" reads correctly, while a verify is checking reality against a
 * claim, where "size 99 expected, 3 found" is what a person means.
 */
static void field_list(struct tmd_buf *b, unsigned d, const struct side *want,
                       const struct side *got, const struct tmd_options *opt,
                       bool verify)
{
    const char *arrow = verify ? " expected, " : " -> ";
    const char *tail = verify ? " found" : "";

    if (d & D_KIND)
    {
        tmd_buf_addf(b, "%skind %s%s%s%s", b->len ? ", " : "",
                     tmd_kind_name(want->kind), arrow,
                     tmd_kind_name(got->kind), tail);
    }
    if (d & D_SIZE)
    {
        tmd_buf_addf(b, "%ssize %llu%s%llu%s", b->len ? ", " : "",
                     (unsigned long long)want->size, arrow,
                     (unsigned long long)got->size, tail);
    }
    if (d & D_MODE)
    {
        tmd_buf_addf(b, "%smode %04o%s%04o%s", b->len ? ", " : "",
                     want->mode, arrow, got->mode, tail);
    }
    if (d & D_MTIME)
    {
        struct tmd_time a = { want->mtime, 0, true, NULL };
        struct tmd_time c = { got->mtime, 0, true, NULL };
        char            before[64];
        char            after[64];

        tmd_render_time(&a, opt, before, sizeof(before));
        tmd_render_time(&c, opt, after, sizeof(after));
        tmd_buf_addf(b, "%smtime %s%s%s%s", b->len ? ", " : "", before, arrow,
                     after, tail);
    }
    if (d & D_LINK)
    {
        tmd_buf_addf(b, "%starget %s%s%s%s", b->len ? ", " : "",
                     want->linkpath, arrow, got->linkpath, tail);
    }
    if (d & D_HASH)
    {
        tmd_buf_addf(b, "%s%s %s%s%s%s", b->len ? ", " : "",
                     tmd_hash_name(opt->hash != TMD_HASH_NONE ? opt->hash
                                   : (strlen(want->digest) == 32 ? TMD_HASH_MD5
                                                                 : TMD_HASH_SHA256)),
                     want->digest, arrow,
                     got->have_digest ? got->digest : "(content not read)", tail);
    }
}

struct tally {
    uint64_t added;
    uint64_t removed;
    uint64_t changed;
    uint64_t same;
    uint64_t duplicated;
};

static void report_text(FILE *out, struct index *ix, const char *want_name,
                        const char *got_name, const struct tmd_options *opt,
                        struct tally *t, bool verify)
{
    size_t i;

    (void)fprintf(out, "--- %s\n", want_name);
    (void)fprintf(out, "+++ %s\n", got_name);

    for (i = 0; i < ix->npairs; i++)
    {
        struct pair *p = &ix->pairs[i];
        unsigned     d;

        if (p->expected.present && !p->found.present)
        {
            t->removed++;
            (void)fprintf(out, "- %s%s\n", p->path,
                          verify ? "   expected, not in the archive" : "");
            continue;
        }
        if (!p->expected.present && p->found.present)
        {
            t->added++;
            (void)fprintf(out, "+ %s%s\n", p->path,
                          verify ? "   in the archive, not expected" : "");
            continue;
        }
        d = side_differences(&p->expected, &p->found);
        if (p->expected.count > 1 || p->found.count > 1)
        {
            t->duplicated++;
        }
        if (d == 0)
        {
            t->same++;
            continue;
        }
        t->changed++;
        {
            struct tmd_buf b;

            tmd_buf_init(&b);
            field_list(&b, d, &p->expected, &p->found, opt, verify);
            (void)fprintf(out, "~ %s   %s\n", p->path, b.data ? b.data : "");
            tmd_buf_free(&b);
        }
    }

    (void)fprintf(out, "  %llu identical, %llu changed, %llu %s, %llu %s\n",
                  (unsigned long long)t->same,
                  (unsigned long long)t->changed,
                  (unsigned long long)t->added,
                  verify ? "unexpected" : "added",
                  (unsigned long long)t->removed,
                  verify ? "missing" : "removed");
    if (t->duplicated)
    {
        (void)fprintf(out, "  %llu path%s appear more than once; the last "
                           "occurrence is the one compared\n",
                      (unsigned long long)t->duplicated,
                      t->duplicated == 1 ? "" : "s");
    }
}

static void report_json(FILE *out, struct index *ix, const char *want_name,
                        const char *got_name, const struct tmd_options *opt,
                        struct tally *t, bool verify)
{
    struct tmd_buf b;
    size_t         i;
    bool           first = true;
    /* A diff runs from one archive to another; a verify checks what was found
     * against what was claimed. The keys say which, because the shapes are read
     * by different code for different reasons. */
    const char    *k_from = verify ? "expected" : "from";
    const char    *k_to = verify ? "found" : "to";

    tmd_buf_init(&b);
    tmd_buf_addf(&b, "{\n  \"schema\": 2,\n  \"%s\": {",
                 verify ? "verify" : "diff");
    tmd_buf_addstr(&b, verify ? "\"manifest\": " : "\"from\": ");
    tmd_json_escape(&b, want_name);
    tmd_buf_addstr(&b, verify ? ", \"archive\": " : ", \"to\": ");
    tmd_json_escape(&b, got_name);

    tmd_buf_addstr(&b, ", \"members\": [");
    for (i = 0; i < ix->npairs; i++)
    {
        struct pair *p = &ix->pairs[i];
        unsigned     d;
        const char  *state;

        if (p->expected.present && !p->found.present)
        {
            state = verify ? "missing" : "removed";
            t->removed++;
        } else if (!p->expected.present && p->found.present)
        {
            state = verify ? "unexpected" : "added";
            t->added++;
        } else
        {
            d = side_differences(&p->expected, &p->found);
            if (p->expected.count > 1 || p->found.count > 1)
            {
                t->duplicated++;
            }
            if (d == 0)
            {
                t->same++;
                continue; /* identical members are counted, not listed */
            }
            t->changed++;
            state = "changed";
        }

        tmd_buf_addstr(&b, first ? "" : ", ");
        first = false;
        tmd_buf_addstr(&b, "{\"path\": ");
        tmd_json_escape(&b, p->path);
        tmd_buf_addf(&b, ", \"state\": \"%s\"", state);

        if (strcmp(state, "changed") == 0)
        {
            d = side_differences(&p->expected, &p->found);
            tmd_buf_addstr(&b, ", \"fields\": {");
            if (d & D_SIZE)
            {
                tmd_buf_addf(&b, "\"size\": {\"%s\": %llu, \"%s\": %llu}",
                             k_from, (unsigned long long)p->expected.size,
                             k_to, (unsigned long long)p->found.size);
            }
            if (d & D_MODE)
            {
                tmd_buf_addf(&b, "%s\"mode\": {\"%s\": \"%04o\", "
                                 "\"%s\": \"%04o\"}",
                             (d & D_SIZE) ? ", " : "",
                             k_from, p->expected.mode, k_to, p->found.mode);
            }
            if (d & D_MTIME)
            {
                tmd_buf_addf(&b, "%s\"mtime\": {\"%s\": %lld, \"%s\": %lld}",
                             (d & (D_SIZE | D_MODE)) ? ", " : "",
                             k_from, (long long)p->expected.mtime,
                             k_to, (long long)p->found.mtime);
            }
            if (d & D_KIND)
            {
                tmd_buf_addf(&b, "%s\"kind\": {\"%s\": \"%s\", \"%s\": \"%s\"}",
                             (d & (D_SIZE | D_MODE | D_MTIME)) ? ", " : "",
                             k_from, tmd_kind_name(p->expected.kind),
                             k_to, tmd_kind_name(p->found.kind));
            }
            if (d & D_LINK)
            {
                tmd_buf_addf(&b, "%s\"linkpath\": {\"%s\": ",
                             (d & (D_SIZE | D_MODE | D_MTIME | D_KIND))
                                 ? ", " : "",
                             k_from);
                tmd_json_escape(&b, p->expected.linkpath);
                tmd_buf_addf(&b, ", \"%s\": ", k_to);
                tmd_json_escape(&b, p->found.linkpath);
                tmd_buf_addc(&b, '}');
            }
            if (d & D_HASH)
            {
                tmd_buf_addf(&b, "%s\"content_hash\": {\"%s\": \"%s\", \"%s\": ",
                             (d & (D_SIZE | D_MODE | D_MTIME | D_KIND | D_LINK))
                                 ? ", " : "",
                             k_from, p->expected.digest, k_to);
                if (p->found.have_digest)
                {
                    tmd_buf_addf(&b, "\"%s\"}", p->found.digest);
                } else
                {
                    tmd_buf_addstr(&b, "null}");
                }
            }
            tmd_buf_addc(&b, '}');
        }
        tmd_buf_addc(&b, '}');
    }
    tmd_buf_addstr(&b, "]");

    tmd_buf_addf(&b, ", \"identical\": %llu", (unsigned long long)t->same);
    tmd_buf_addf(&b, ", \"changed\": %llu", (unsigned long long)t->changed);
    tmd_buf_addf(&b, ", \"%s\": %llu", verify ? "unexpected" : "added",
                 (unsigned long long)t->added);
    tmd_buf_addf(&b, ", \"%s\": %llu", verify ? "missing" : "removed",
                 (unsigned long long)t->removed);
    tmd_buf_addf(&b, ", \"duplicated_paths\": %llu",
                 (unsigned long long)t->duplicated);
    tmd_buf_addf(&b, ", \"matches\": %s",
                 (t->added || t->removed || t->changed) ? "false" : "true");
    tmd_buf_addstr(&b, "}\n}\n");
    (void)fputs(b.data, out);
    tmd_buf_free(&b);
    (void)opt;
}

static int finish(FILE *out, struct index *ix, const char *want_name,
                  const char *got_name, const struct tmd_options *opt,
                  bool verify)
{
    struct tally t;

    memset(&t, 0, sizeof(t));
    /*
     * Always by path. A comparison that came out in a different order each time
     * could not be diffed against itself, which is most of what it is for.
     *
     * Guarded, because `pairs` is NULL until something is added and qsort's
     * first parameter is declared nonnull -- so two empty archives, or two
     * archives filtered to nothing by -m, would be undefined behavior rather
     * than a trivially empty report.
     */
    if (ix->npairs > 0)
    {
        qsort(ix->pairs, ix->npairs, sizeof(*ix->pairs), pair_cmp);
    }
    /* The slots index into `pairs` and the sort just moved them, so it must not
     * be used again; nothing below does. */

    if (opt->output == TMD_OUT_JSON)
    {
        report_json(out, ix, want_name, got_name, opt, &t, verify);
    } else
    {
        report_text(out, ix, want_name, got_name, opt, &t, verify);
    }
    if (t.added || t.removed || t.changed)
    {
        return TMD_EXIT_DIFFER;
    }
    return TMD_EXIT_OK;
}

/* ------------------------------------------------------------------------- */
/* Entry points                                                              */
/* ------------------------------------------------------------------------- */

int tmd_diff_archives(const char *from, const char *to, FILE *out,
                      const struct tmd_options *opt)
{
    struct index ix;
    int          status;

    index_init(&ix);
    if (!read_into(&ix, from, false, opt, opt->hash) ||
        !read_into(&ix, to, true, opt, opt->hash))
    {
        index_free(&ix);
        return TMD_EXIT_ERROR;
    }
    status = finish(out, &ix, from, to, opt, false);
    index_free(&ix);
    return status;
}

int tmd_verify_archive(const char *archive, const char *manifest, FILE *out,
                       const struct tmd_options *opt)
{
    struct index ix;
    int          status;

    enum tmd_hash hash = TMD_HASH_NONE;

    index_init(&ix);
    if (!read_manifest(&ix, manifest, opt, &hash))
    {
        index_free(&ix);
        return TMD_EXIT_ERROR;
    }
    /*
     * A manifest that carries digests decides how the archive is hashed; one
     * that carries none is hashed only if --hash asks. A --hash that disagrees
     * with the manifest is refused rather than quietly overridden: two
     * algorithms cannot be compared, and either choice would ignore what
     * somebody typed.
     */
    if (hash != TMD_HASH_NONE && opt->hash != TMD_HASH_NONE && opt->hash != hash)
    {
        (void)fprintf(stderr, "tmd: %s carries %s digests; --hash=%s cannot be "
                              "checked against them\n",
                      manifest, tmd_hash_name(hash), tmd_hash_name(opt->hash));
        index_free(&ix);
        return TMD_EXIT_USAGE;
    }
    if (hash == TMD_HASH_NONE)
    {
        hash = opt->hash;
    }
    if (!read_into(&ix, archive, true, opt, hash))
    {
        index_free(&ix);
        return TMD_EXIT_ERROR;
    }
    status = finish(out, &ix, manifest, archive, opt, true);
    index_free(&ix);
    return status;
}
