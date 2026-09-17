/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "render.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tar.h"
#include "util.h"

struct stats;
struct tmd_render;

/* Defined with the rest of --stat, further down; declared here because the
 * renderer's free and its JSON summary both come earlier in the file. */
static void stat_free(struct stats *s);
static void json_stat(const struct tmd_render *rd, struct tmd_buf *b);

struct tmd_render {
    FILE                     *out;
    const struct tmd_options *opt;
    int                       archive_count;
    int                       archive_index;
    bool                      entry_written;  /* a comma is owed (JSON) */
    bool                      csv_header_written;

    /* -m: how much of the archive the filter let through, for the summary
     * line and for the exit status. Cumulative across archives, because
     * "nothing matched" has to mean nothing in any of them. */
    uint64_t                  matched;
    uint64_t                  seen;
    uint64_t                  matched_total;

    /*
     * --sort: every entry that will be printed, held until the archive ends.
     *
     * This is the one place the program stops streaming, and it is why --sort
     * is opt-in. The entries are clones: the reader recycles its own.
     */
    struct tmd_entry        **held;
    size_t                    nheld;
    size_t                    held_cap;
    bool                      warned_large;

    /* --stat: allocated only when asked for; it is a few hundred kilobytes. */
    struct stats             *stat;
};

/* Past this many held members, say so once. Not a limit -- truncating a
 * listing because it got big would be worse than the memory -- but somebody
 * who pointed --sort at a 200,000-member archive should be told why their
 * machine went quiet. */
#define TMD_SORT_LOUD_AT 100000

/*
 * The options the comparator needs.
 *
 * C11's qsort takes no context pointer, and qsort_r is spelled differently on
 * glibc and the BSDs with the arguments in a different order. A file-scope
 * pointer set immediately before the sort and cleared immediately after is the
 * portable answer; the program is single-threaded and does not sort while
 * sorting.
 */
static const struct tmd_options *compare_options;

/* ANSI colors, used only when the output is a terminal and --color allows it.
 * The same three distinctions ls makes, and no more: anything further turns a
 * listing into decoration. */
#define C_RESET "\033[0m"
#define C_DIR   "\033[1;34m"
#define C_LINK  "\033[1;36m"
#define C_EXEC  "\033[1;32m"

/* ------------------------------------------------------------------------- */
/* Time                                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Render a timestamp.
 *
 * Three things can go wrong and all three are things real archives do: the
 * field can be absent (v7 has no atime), it can be negative (a file from
 * before 1970, which happens in archives of archives), and it can be far
 * enough in the future that the platform's time_t cannot hold it. In the last
 * two cases the raw seconds are printed rather than a wrong date or a blank —
 * the number is what the archive actually says, and hiding it would lose the
 * only evidence that the field is wrong.
 */
/*
 * strftime(3), with a result that is safe to print.
 *
 * strftime returns 0 when the conversion does not fit, and C11 7.27.3.5 leaves
 * the buffer's contents *unspecified* in that case. Not truncated and
 * terminated, not emptied: unspecified, with no terminator promised. Every
 * caller below hands the buffer straight to "%s", so a conversion that did not
 * fit read off the end of a 24-byte stack array and kept going until it found
 * a zero byte -- a 29-byte read out of a 24-byte `stamp`, which is what
 * libFuzzer eventually caught.
 *
 * It needs no exotic archive. "%Y" is four digits only for the years anybody
 * expects; a tar header carries a 64-bit mtime, GNU base-256 numeric fields
 * make a huge one easy to write, and GNU tar itself will list such a member
 * without complaint. Whether the overflow actually fired depended on what
 * happened to be on the stack underneath, which is why it took a long fuzz run
 * to surface rather than a single case.
 *
 * Returns false when nothing was written. A zero return is unambiguous here
 * because none of the formats in this file can legitimately produce an empty
 * string.
 */
/*
 * A macro rather than a function, for two reasons.
 *
 * The format stays a literal at every call site, so both compilers keep
 * checking it -- which is not a thing to give up in the one function this bug
 * was in. And a wrapper function cannot satisfy -Wformat-nonliteral here
 * anyway: GCC does not honor format(strftime, N, 0) the way it honors the
 * printf archetype, and warns inside the wrapper wherever the attribute is
 * placed (checked on GCC 15; clang accepts it).
 *
 * The buffer is deliberately left alone when the conversion does not fit.
 * Every caller below replaces the destination with something honest -- the raw
 * seconds, or an empty string -- so a buffer that is never read does not need
 * terminating, and terminating it here would put an assignment inside an `if`
 * condition, which this file avoids: it reads as a comparison typo even when it
 * is not one.
 *
 * What must never happen is printing a buffer this returned false for. That is
 * precisely what the bug was.
 */
#define TIME_FITS(buf, size, fmt, tm) (strftime((buf), (size), (fmt), (tm)) != 0)

static void format_time(const struct tmd_time *t, const struct tmd_options *opt,
                        char *buf, size_t bufsz)
{
    struct tm        tmbuf;
    const struct tm *tm;
    time_t           seconds;

    if (!t->present)
    {
        (void)snprintf(buf, bufsz, "%s", "-");
        return;
    }

    seconds = (time_t)t->sec;
    if ((int64_t)seconds != t->sec)
    {
        (void)snprintf(buf, bufsz, "@%lld", (long long)t->sec);
        return;
    }

    tm = opt->local ? localtime_r(&seconds, &tmbuf) : gmtime_r(&seconds, &tmbuf);
    if (!tm)
    {
        (void)snprintf(buf, bufsz, "@%lld", (long long)t->sec);
        return;
    }

    /*
     * The zone marker is part of the timestamp, always.
     *
     * "2024-03-15 16:32" tells a reader nothing about which clock it is on, and
     * the whole reason to read an archive's timestamps is usually to place its
     * contents in time. A trailing Z says UTC; a numeric offset says the
     * reader asked for local and here is what that was worth.
     */
    if (opt->full_time)
    {
        /* Sized for the widest struct tm a platform can hold: tm_year is an
         * int, so the year runs to 10 digits and the stamp to 25 characters.
         * The guard below stays regardless -- the buffer being large enough is
         * a property of this file, and the terminator is not. */
        char stamp[32];
        char zone[16];  /* " +HHMM", or " +HH:MM" where a platform emits it */
        /* Reduced modulo a second so the compiler can see that %09u is nine
         * digits and not ten. A nanosecond field over 999999999 is malformed
         * anyway, and showing it as a second's worth of nanoseconds is closer
         * to the truth than showing it as a wider number. */
        unsigned nsec = t->nsec % 1000000000u;

        if (!TIME_FITS(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm))
        {
            (void)snprintf(buf, bufsz, "@%lld", (long long)t->sec);
            return;
        }
        if (opt->local)
        {
            /* No offset is better than a wrong one. */
            if (!TIME_FITS(zone, sizeof(zone), " %z", tm))
            {
                zone[0] = '\0';
            }
        } else
        {
            (void)snprintf(zone, sizeof(zone), "Z");
        }

        if (nsec)
        {
            (void)snprintf(buf, bufsz, "%s.%09u%s", stamp, nsec, zone);
        } else
        {
            (void)snprintf(buf, bufsz, "%s%s", stamp, zone);
        }
    } else if (opt->local)
    {
        char stamp[32];

        if (!TIME_FITS(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm))
        {
            (void)snprintf(buf, bufsz, "@%lld", (long long)t->sec);
        } else
        {
            (void)snprintf(buf, bufsz, "%s", stamp);
        }
    } else if (!TIME_FITS(buf, bufsz, "%Y-%m-%d %H:%M:%SZ", tm))
    {
        (void)snprintf(buf, bufsz, "@%lld", (long long)t->sec);
    }
}

/* ISO 8601 for the machine-readable formats. Always UTC there, because a JSON
 * consumer has no way to know which machine's local time it was written on. */
static void format_time_iso(const struct tmd_time *t, char *buf, size_t bufsz)
{
    struct tm        tmbuf;
    const struct tm *tm;
    time_t           seconds = (time_t)t->sec;

    if (!t->present || (int64_t)seconds != t->sec)
    {
        buf[0] = '\0';
        return;
    }
    /* Only now is `seconds` known to round-trip, which is what makes it safe
     * to hand to gmtime_r at all. Kept as its own statement rather than a third
     * term of the condition above: an assignment inside an `if` reads as a
     * comparison typo even when it is not one. */
    tm = gmtime_r(&seconds, &tmbuf);
    if (!tm)
    {
        buf[0] = '\0';
        return;
    }
    if (t->nsec)
    {
        char stamp[32]; /* a 10-digit year makes this 25 characters */

        /* An empty string is this function's existing way of saying "cannot be
         * represented"; see the !present branch above. */
        if (!TIME_FITS(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", tm))
        {
            buf[0] = '\0';
        } else
        {
            /* Reduced modulo a second so the compiler can see that %09u is
             * nine digits and not ten. */
            unsigned nsec = t->nsec % 1000000000u;

            (void)snprintf(buf, bufsz, "%s.%09uZ", stamp, nsec);
        }
    } else if (!TIME_FITS(buf, bufsz, "%Y-%m-%dT%H:%M:%SZ", tm))
    {
        buf[0] = '\0';
    }
}

/* ------------------------------------------------------------------------- */
/* Shared pieces                                                             */
/* ------------------------------------------------------------------------- */

/* "bceverly/bceverly", or "1000/1000" when the names are absent or -n is set.
 * The mixed case is deliberate: an archive can carry a uname and no gname. */
static void owner_string(const struct tmd_entry *e, const struct tmd_options *opt,
                         char *buf, size_t bufsz)
{
    const char *user = (!opt->numeric && e->uname && e->uname[0]) ? e->uname : NULL;
    const char *group = (!opt->numeric && e->gname && e->gname[0]) ? e->gname : NULL;
    char        user_buf[32];
    char        group_buf[32];

    if (!user)
    {
        (void)snprintf(user_buf, sizeof(user_buf), "%lld", (long long)e->uid);
        user = user_buf;
    }
    if (!group)
    {
        (void)snprintf(group_buf, sizeof(group_buf), "%lld", (long long)e->gid);
        group = group_buf;
    }
    (void)snprintf(buf, bufsz, "%s/%s", user, group);
}

/* The size column. A device has no size — it has a device number, and ls
 * prints that in the same column, so this does too. */
static void size_string(const struct tmd_entry *e, const struct tmd_options *opt,
                        char *buf, size_t bufsz)
{
    if (e->has_dev)
    {
        (void)snprintf(buf, bufsz, "%u,%u", e->devmajor, e->devminor);
    } else if (opt->human)
    {
        char human[32];
        (void)snprintf(buf, bufsz, "%s", tmd_human_size(e->size, human, sizeof(human)));
    } else
    {
        (void)snprintf(buf, bufsz, "%llu", (unsigned long long)e->size);
    }
}

static const char *entry_color(const struct tmd_entry *e,
                               const struct tmd_options *opt)
{
    if (!opt->color)
    {
        return NULL;
    }
    switch (e->kind)
    {
    case TMD_KIND_DIR:
        return C_DIR;
    case TMD_KIND_SYMLINK:
    case TMD_KIND_HARDLINK:
        return C_LINK;
    default:
        return (e->mode & 0111) ? C_EXEC : NULL;
    }
}

/*
 * One listing line, in the shape `ls -l` and `tar -tvf` both produce.
 *
 *   -rw-r--r--  bceverly/bceverly      1234  2026-09-16 14:11  src/main.c
 *   lrwxrwxrwx  bceverly/bceverly         0  2026-09-16 14:11  link -> target
 *
 * The column widths are fixed rather than fitted to the widest row, because
 * the output streams: fitting them would mean holding the whole archive in
 * memory to discover how wide the size column needs to be. Fields that overrun
 * push the line out rather than being truncated — a wrong number is worse than
 * a ragged column.
 */
void tmd_render_time(const struct tmd_time *t, const struct tmd_options *opt,
                     char *buf, size_t bufsz)
{
    format_time(t, opt, buf, bufsz);
}

char *tmd_render_listing_line(const struct tmd_entry *e,
                              const struct tmd_options *opt)
{
    struct tmd_buf line;
    char           mode[11];
    char           owner[80];
    char           size[48];
    char           when[64];
    const char    *color;

    tmd_buf_init(&line);
    tmd_mode_string(e->mode, e->kind, mode);
    owner_string(e, opt, owner, sizeof(owner));
    size_string(e, opt, size, sizeof(size));
    format_time(&e->mtime, opt, when, sizeof(when));

    tmd_buf_addf(&line, "%s  %-17s %10s  %-20s  ", mode, owner, size, when);

    color = entry_color(e, opt);
    if (color)
    {
        tmd_buf_addstr(&line, color);
    }
    tmd_buf_addstr(&line, e->path);
    if (color)
    {
        tmd_buf_addstr(&line, C_RESET);
    }

    if (e->kind == TMD_KIND_SYMLINK && e->linkpath && e->linkpath[0])
    {
        tmd_buf_addf(&line, " -> %s", e->linkpath);
    } else if (e->kind == TMD_KIND_HARDLINK && e->linkpath && e->linkpath[0])
    {
        tmd_buf_addf(&line, " link to %s", e->linkpath);
    }

    /*
     * Under -m, where the member is.
     *
     * A deliberate exception to "-m changes nothing but which lines appear".
     * The reason the feature exists is that an archive can hold the same path
     * more than once -- `tar -r` appends, an incremental backup re-adds a
     * changed file, a concatenated archive carries two whole copies -- and
     * extraction silently keeps the last. Two matching lines that differ only
     * in size and date tell you there are two; the offset tells you where each
     * one is, which is the part nothing else answers. -l and the machine
     * formats already carry it, so this only fills in the default listing.
     */
    if (opt->nmatch > 0)
    {
        tmd_buf_addf(&line, "   @%llu", (unsigned long long)e->offset);
    }

    return tmd_buf_detach(&line);
}

/* ------------------------------------------------------------------------- */
/* --stat                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * How many distinct timestamps to track, and how many slots to track them in.
 *
 * Bounded on purpose. A count of distinct values needs somewhere to put them,
 * and an archive is a thing somebody else wrote: 200,000 members with 200,000
 * different seconds must not turn a report into an allocation. Past the limit
 * the answer becomes "more than 4096 distinct", which is honest and is all the
 * reader needed anyway -- the interesting answers are 1, a handful, and many.
 *
 * SLOTS is twice LIMIT and a power of two, so the table never exceeds half
 * full and linear probing stays short.
 */
#define STAT_TIME_LIMIT 4096
#define STAT_TIME_SLOTS 8192
#define STAT_TOP        5

/* The first tar shipped with Seventh Edition Unix in 1979. A timestamp older
 * than that was not written by a clock that was working. */
#define TAR_EPOCH 283996800L /* 1979-01-01T00:00:00Z */

struct stat_time {
    int64_t  sec;
    uint64_t count;
    bool     used;
};

struct stats {
    uint64_t members;
    uint64_t extracted;
    uint64_t stored;
    uint64_t padding;

    struct {
        uint64_t size;
        char    *path;
    } top[STAT_TOP];
    size_t ntop;

    struct stat_time times[STAT_TIME_SLOTS];
    size_t           ndistinct;
    bool             distinct_overflow;

    int64_t  earliest;
    int64_t  latest;
    bool     have_time;
    uint64_t no_time;
    uint64_t zero_time;
    uint64_t negative_time;
    uint64_t future_time;
    uint64_t pre_tar_time;
};

/*
 * A scramble, not a hash function anybody should rely on.
 *
 * Timestamps in one archive cluster tightly -- often consecutive seconds, often
 * one value repeated -- and taking the low bits of a clustered key straight
 * into a probe sequence is how a hash table turns into a linked list. Mixing
 * the high bits down first spreads them.
 */
static size_t time_slot(int64_t sec)
{
    uint64_t x = (uint64_t)sec;

    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (size_t)(x & (STAT_TIME_SLOTS - 1));
}

static void stat_note_time(struct stats *s, const struct tmd_time *mt)
{
    size_t slot;
    size_t probes;

    if (!mt->present)
    {
        s->no_time++;
        return;
    }

    if (!s->have_time)
    {
        s->earliest = mt->sec;
        s->latest = mt->sec;
        s->have_time = true;
    } else
    {
        if (mt->sec < s->earliest)
        {
            s->earliest = mt->sec;
        }
        if (mt->sec > s->latest)
        {
            s->latest = mt->sec;
        }
    }

    if (mt->sec == 0)
    {
        s->zero_time++;
    } else if (mt->sec < 0)
    {
        s->negative_time++;
    } else if (mt->sec < TAR_EPOCH)
    {
        s->pre_tar_time++;
    }
    if (mt->sec > (int64_t)time(NULL))
    {
        s->future_time++;
    }

    slot = time_slot(mt->sec);
    for (probes = 0; probes < STAT_TIME_SLOTS; probes++)
    {
        struct stat_time *e = &s->times[slot];

        if (e->used && e->sec == mt->sec)
        {
            e->count++;
            return;
        }
        if (!e->used)
        {
            if (s->ndistinct >= STAT_TIME_LIMIT)
            {
                s->distinct_overflow = true;
                return;
            }
            e->used = true;
            e->sec = mt->sec;
            e->count = 1;
            s->ndistinct++;
            return;
        }
        slot = (slot + 1) & (STAT_TIME_SLOTS - 1);
    }
    s->distinct_overflow = true;
}

/* The largest few, kept in order, so the array is never longer than STAT_TOP
 * whatever the archive contains. */
static void stat_note_size(struct stats *s, const struct tmd_entry *e)
{
    size_t i;
    size_t at;

    if (s->ntop == STAT_TOP && e->size <= s->top[s->ntop - 1].size)
    {
        return;
    }
    at = s->ntop;
    for (i = 0; i < s->ntop; i++)
    {
        if (e->size > s->top[i].size)
        {
            at = i;
            break;
        }
    }
    if (s->ntop == STAT_TOP)
    {
        free(s->top[STAT_TOP - 1].path);
        s->ntop--;
    }
    for (i = s->ntop; i > at; i--)
    {
        s->top[i] = s->top[i - 1];
    }
    s->top[at].size = e->size;
    s->top[at].path = tmd_xstrdup(e->path ? e->path : "");
    s->ntop++;
}

static void stat_note(struct stats *s, const struct tmd_entry *e)
{
    uint64_t padded = tmd_round_up_blocks(e->data_size);

    s->members++;
    s->extracted += e->size;
    s->stored += e->stored_size;
    s->padding += padded - e->data_size;
    stat_note_size(s, e);
    stat_note_time(s, &e->mtime);
}

static void stat_free(struct stats *s)
{
    size_t i;

    if (!s)
    {
        return;
    }
    for (i = 0; i < s->ntop; i++)
    {
        free(s->top[i].path);
    }
    free(s);
}

/* ------------------------------------------------------------------------- */
/* JSON                                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Escape a string for JSON.
 *
 * Paths in a tar archive are bytes, not text: there is no encoding field in a
 * ustar header, and an archive written on a machine using Latin-1 filenames is
 * perfectly legal and not UTF-8. Emitting those bytes raw produces JSON that a
 * strict parser rejects, so anything that is not valid UTF-8 is escaped byte by
 * byte as \u00XX. That round-trips through any parser and, unlike replacing
 * the byte with U+FFFD, does not destroy the value.
 */
void tmd_json_escape(struct tmd_buf *b, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t               len = strlen(s);
    bool                 utf8 = tmd_utf8_valid(s, len);
    size_t               i;

    tmd_buf_addc(b, '"');
    for (i = 0; i < len; i++)
    {
        unsigned char c = p[i];
        switch (c)
        {
        case '"':  tmd_buf_addstr(b, "\\\""); break;
        case '\\': tmd_buf_addstr(b, "\\\\"); break;
        case '\b': tmd_buf_addstr(b, "\\b"); break;
        case '\f': tmd_buf_addstr(b, "\\f"); break;
        case '\n': tmd_buf_addstr(b, "\\n"); break;
        case '\r': tmd_buf_addstr(b, "\\r"); break;
        case '\t': tmd_buf_addstr(b, "\\t"); break;
        default:
            if (c < 0x20 || (c >= 0x80 && !utf8))
            {
                tmd_buf_addf(b, "\\u%04x", c);
            } else
            {
                tmd_buf_addc(b, (char)c);
            }
            break;
        }
    }
    tmd_buf_addc(b, '"');
}

/*
 * The mode, broken into the bits it is made of.
 *
 * "mode": "0755" is the archive's answer and "mode_string": "-rwxr-xr-x" is the
 * one a person reads; neither is a thing a program can test without decoding it
 * again. setuid in particular is the bit somebody auditing an archive actually
 * cares about, and it is invisible in the octal unless you already know to look
 * at the fourth digit.
 */
static void json_mode_bits(struct tmd_buf *b, uint32_t mode)
{
    static const char *const who[3] = { "owner", "group", "other" };
    unsigned                 i;

    tmd_buf_addf(b, ", \"mode_bits\": {\"setuid\": %s, \"setgid\": %s, \"sticky\": %s",
                 (mode & 04000) ? "true" : "false",
                 (mode & 02000) ? "true" : "false",
                 (mode & 01000) ? "true" : "false");
    for (i = 0; i < 3; i++)
    {
        unsigned shift = 6 - i * 3;

        tmd_buf_addf(b, ", \"%s\": {\"read\": %s, \"write\": %s, \"execute\": %s}",
                     who[i],
                     (mode & (04u << shift)) ? "true" : "false",
                     (mode & (02u << shift)) ? "true" : "false",
                     (mode & (01u << shift)) ? "true" : "false");
    }
    tmd_buf_addc(b, '}');
}

/*
 * Exactly which blocks this member occupies.
 *
 * Derived rather than stored: `stored_size` is the header, its extension
 * blocks and the padded payload together, so subtracting the padded payload
 * leaves the headers. Reporting it lets someone reconstruct the archive's
 * layout from the JSON alone -- and makes the padding visible, which is the
 * only way to answer "why is this archive bigger than its contents".
 */
static uint64_t entry_header_bytes(const struct tmd_entry *e)
{
    uint64_t data_padded = tmd_round_up_blocks(e->data_size);

    return e->stored_size > data_padded ? e->stored_size - data_padded : 0;
}

static void json_blocks(struct tmd_buf *b, const struct tmd_entry *e)
{
    /* Each value computed at its own width first, then widened once for
     * printing. Casting the arithmetic instead widens the result of a uint64_t
     * expression, which says nothing about the operands. */
    uint64_t data_padded  = tmd_round_up_blocks(e->data_size);
    uint64_t header_bytes = entry_header_bytes(e);
    uint64_t header_blocks = header_bytes / TMD_BLOCK_SIZE;
    uint64_t data_offset  = e->offset + header_bytes;
    uint64_t data_blocks  = data_padded / TMD_BLOCK_SIZE;
    uint64_t padding      = data_padded - e->data_size;

    tmd_buf_addf(b, ", \"blocks\": {\"block_size\": %d", TMD_BLOCK_SIZE);
    tmd_buf_addf(b, ", \"header_offset\": %llu", (unsigned long long)e->offset);
    tmd_buf_addf(b, ", \"header_blocks\": %llu", (unsigned long long)header_blocks);
    tmd_buf_addf(b, ", \"data_offset\": %llu", (unsigned long long)data_offset);
    tmd_buf_addf(b, ", \"data_bytes\": %llu", (unsigned long long)e->data_size);
    tmd_buf_addf(b, ", \"data_blocks\": %llu", (unsigned long long)data_blocks);
    tmd_buf_addf(b, ", \"padding\": %llu", (unsigned long long)padding);
    tmd_buf_addf(b, ", \"total_bytes\": %llu}",
                 (unsigned long long)e->stored_size);
}

/*
 * Whether a string is text, and where it stops being text if it is not.
 *
 * A tar path is a string of bytes with no declared encoding. Most are UTF-8;
 * the ones that are not are usually a filename from a machine with a different
 * locale, and occasionally a path built to slip past something that only
 * checks strings it can decode. Either way the reader wants to know, and
 * wants the offset rather than a yes/no.
 */
static void json_encoding(struct tmd_buf *b, const char *key, const char *s)
{
    size_t bad = 0;

    if (!s)
    {
        return;
    }
    if (tmd_utf8_first_invalid(s, strlen(s), &bad))
    {
        tmd_buf_addf(b, ", \"%s\": {\"utf8\": false, \"first_invalid_byte\": %zu}",
                     key, bad);
    } else
    {
        tmd_buf_addf(b, ", \"%s\": {\"utf8\": true}", key);
    }
}

/*
 * The xattrs, decoded.
 *
 * libarchive and star both write extended attributes as pax records whose
 * value is base64 -- SCHILY.xattr.user.tag, LIBARCHIVE.xattr.user.tag. The
 * pax object above reports them verbatim, because that is what the archive
 * says; this reports what they mean. Both are kept: the encoded form is the
 * evidence, and the decoded form is the answer.
 *
 * A value that does not decode is reported as not decoding rather than
 * quietly skipped, because "this xattr is not valid base64" is itself a
 * finding about the archive.
 */
static void json_xattrs(struct tmd_buf *b, const struct tmd_entry *e)
{
    static const char *const prefixes[] = { "SCHILY.xattr.", "LIBARCHIVE.xattr." };
    size_t                   i;
    bool                     any = false;

    for (i = 0; i < e->npax; i++)
    {
        const char *key = e->pax[i].key;
        const char *name = NULL;
        size_t      k;

        for (k = 0; k < sizeof(prefixes) / sizeof(*prefixes); k++)
        {
            size_t n = strlen(prefixes[k]);

            if (strncmp(key, prefixes[k], n) == 0 && key[n] != '\0')
            {
                name = key + n;
            }
        }
        if (!name)
        {
            continue;
        }

        tmd_buf_addstr(b, any ? ", " : ", \"xattrs\": {");
        any = true;
        tmd_json_escape(b, name);
        tmd_buf_addstr(b, ": {\"encoded\": ");
        tmd_json_escape(b, e->pax[i].value);

        {
            struct tmd_buf decoded;

            tmd_buf_init(&decoded);
            if (tmd_base64_decode(e->pax[i].value, &decoded))
            {
                tmd_buf_addstr(b, ", \"decoded\": ");
                tmd_json_escape(b, decoded.data ? decoded.data : "");
            } else
            {
                tmd_buf_addstr(b, ", \"decoded\": null, \"decode_error\": "
                                  "\"not valid base64\"");
            }
            tmd_buf_free(&decoded);
        }
        tmd_buf_addc(b, '}');
    }
    if (any)
    {
        tmd_buf_addc(b, '}');
    }
}

static void json_entry(struct tmd_render *rd, const struct tmd_entry *e)
{
    const struct tmd_options *opt = rd->opt;
    struct tmd_buf            b;
    char                      mode[11];
    char                      stamp[64];
    size_t                    i;

    tmd_buf_init(&b);
    tmd_mode_string(e->mode, e->kind, mode);

    tmd_buf_addstr(&b, "    {");
    tmd_buf_addstr(&b, "\"path\": ");
    tmd_json_escape(&b, e->path);
    tmd_buf_addf(&b, ", \"kind\": \"%s\"", tmd_kind_name(e->kind));
    tmd_buf_addf(&b, ", \"typeflag\": \"%c\"",
                 (e->typeflag >= 32 && e->typeflag < 127) ? e->typeflag : '0');
    tmd_buf_addf(&b, ", \"format\": \"%s\"", tmd_format_name(e->format));
    tmd_buf_addf(&b, ", \"mode\": \"%04o\"", e->mode);
    tmd_buf_addf(&b, ", \"mode_string\": \"%s\"", mode);
    json_mode_bits(&b, e->mode);
    tmd_buf_addf(&b, ", \"uid\": %lld", (long long)e->uid);
    tmd_buf_addf(&b, ", \"gid\": %lld", (long long)e->gid);
    tmd_buf_addstr(&b, ", \"uname\": ");
    tmd_json_escape(&b, e->uname ? e->uname : "");
    tmd_buf_addstr(&b, ", \"gname\": ");
    tmd_json_escape(&b, e->gname ? e->gname : "");
    tmd_buf_addf(&b, ", \"size\": %llu", (unsigned long long)e->size);
    tmd_buf_addf(&b, ", \"stored_size\": %llu", (unsigned long long)e->stored_size);
    tmd_buf_addf(&b, ", \"data_size\": %llu", (unsigned long long)e->data_size);
    tmd_buf_addf(&b, ", \"offset\": %llu", (unsigned long long)e->offset);
    json_blocks(&b, e);

    format_time_iso(&e->mtime, stamp, sizeof(stamp));
    if (stamp[0])
    {
        tmd_buf_addf(&b, ", \"mtime\": \"%s\"", stamp);
        tmd_buf_addf(&b, ", \"mtime_epoch\": %lld", (long long)e->mtime.sec);
    } else
    {
        tmd_buf_addstr(&b, ", \"mtime\": null");
        if (e->mtime.present)
        {
            tmd_buf_addf(&b, ", \"mtime_epoch\": %lld", (long long)e->mtime.sec);
        }
    }
    if (e->mtime.source)
    {
        tmd_buf_addf(&b, ", \"mtime_source\": \"%s\"", e->mtime.source);
    }
    if (e->atime.present)
    {
        format_time_iso(&e->atime, stamp, sizeof(stamp));
        tmd_buf_addf(&b, ", \"atime\": \"%s\"", stamp);
        tmd_buf_addf(&b, ", \"atime_epoch\": %lld", (long long)e->atime.sec);
        if (e->atime.source)
        {
            tmd_buf_addf(&b, ", \"atime_source\": \"%s\"", e->atime.source);
        }
    }
    if (e->ctime.present)
    {
        format_time_iso(&e->ctime, stamp, sizeof(stamp));
        tmd_buf_addf(&b, ", \"ctime\": \"%s\"", stamp);
        tmd_buf_addf(&b, ", \"ctime_epoch\": %lld", (long long)e->ctime.sec);
        if (e->ctime.source)
        {
            tmd_buf_addf(&b, ", \"ctime_source\": \"%s\"", e->ctime.source);
        }
    }
    if (e->created.present)
    {
        format_time_iso(&e->created, stamp, sizeof(stamp));
        tmd_buf_addf(&b, ", \"created\": \"%s\"", stamp);
        tmd_buf_addf(&b, ", \"created_epoch\": %lld", (long long)e->created.sec);
        if (e->created.source)
        {
            tmd_buf_addf(&b, ", \"created_source\": \"%s\"", e->created.source);
        }
    }

    if (e->path_source)
    {
        tmd_buf_addf(&b, ", \"path_source\": \"%s\"", e->path_source);
    }
    json_encoding(&b, "path_encoding", e->path);
    if (e->linkpath && e->linkpath[0])
    {
        tmd_buf_addstr(&b, ", \"linkpath\": ");
        tmd_json_escape(&b, e->linkpath);
        if (e->linkpath_source)
        {
            tmd_buf_addf(&b, ", \"linkpath_source\": \"%s\"", e->linkpath_source);
        }
        json_encoding(&b, "linkpath_encoding", e->linkpath);
    }
    if (e->has_dev)
    {
        tmd_buf_addf(&b, ", \"devmajor\": %u, \"devminor\": %u",
                     e->devmajor, e->devminor);
    }
    if (e->is_sparse)
    {
        tmd_buf_addf(&b, ", \"sparse\": {\"realsize\": %llu, \"segments\": [",
                     (unsigned long long)e->realsize);
        for (i = 0; i < e->nsparse; i++)
        {
            tmd_buf_addf(&b, "%s{\"offset\": %llu, \"bytes\": %llu}",
                         i ? ", " : "",
                         (unsigned long long)e->sparse[i].offset,
                         (unsigned long long)e->sparse[i].numbytes);
        }
        tmd_buf_addf(&b, "], \"truncated\": %s}",
                     e->sparse_truncated ? "true" : "false");
    }

    /*
     * Both conventions, side by side, and which one the archive agreed with.
     *
     * Historic tars disagreed about whether a header's bytes were signed, so an
     * archive written by one and checked by the other reports a false mismatch.
     * tmd accepts either -- but "valid" alone hides which, and an archive whose
     * checksums only match the signed reading says something real about the tool
     * that wrote it.
     */
    tmd_buf_addf(&b, ", \"checksum\": {\"stored\": %u, \"computed\": %u"
                     ", \"computed_unsigned\": %u, \"computed_signed\": %ld"
                     ", \"valid\": %s, \"matched\": ",
                 e->chksum_stored, e->chksum_unsigned,
                 e->chksum_unsigned, (long)e->chksum_signed,
                 e->chksum_ok ? "true" : "false");
    if (e->chksum_ok && e->chksum_stored == e->chksum_unsigned)
    {
        tmd_buf_addstr(&b, "\"unsigned\"}");
    } else if (e->chksum_ok && (int32_t)e->chksum_stored == e->chksum_signed)
    {
        tmd_buf_addstr(&b, "\"signed\"}");
    } else
    {
        tmd_buf_addstr(&b, "null}");
    }

    if (e->npax > 0)
    {
        tmd_buf_addstr(&b, ", \"pax\": {");
        for (i = 0; i < e->npax; i++)
        {
            if (i)
            {
                tmd_buf_addstr(&b, ", ");
            }
            tmd_json_escape(&b, e->pax[i].key);
            tmd_buf_addstr(&b, ": ");
            tmd_json_escape(&b, e->pax[i].value);
        }
        tmd_buf_addc(&b, '}');
    }
    json_xattrs(&b, e);

    if (opt->headers)
    {
        tmd_buf_addstr(&b, ", \"raw\": {");
        tmd_buf_addstr(&b, "\"name\": ");
        tmd_json_escape(&b, e->raw.name);
        tmd_buf_addstr(&b, ", \"prefix\": ");
        tmd_json_escape(&b, e->raw.prefix);
        tmd_buf_addstr(&b, ", \"mode\": ");
        tmd_json_escape(&b, e->raw.mode);
        tmd_buf_addstr(&b, ", \"uid\": ");
        tmd_json_escape(&b, e->raw.uid);
        tmd_buf_addstr(&b, ", \"gid\": ");
        tmd_json_escape(&b, e->raw.gid);
        tmd_buf_addstr(&b, ", \"size\": ");
        tmd_json_escape(&b, e->raw.size);
        tmd_buf_addstr(&b, ", \"mtime\": ");
        tmd_json_escape(&b, e->raw.mtime);
        tmd_buf_addstr(&b, ", \"chksum\": ");
        tmd_json_escape(&b, e->raw.chksum);
        tmd_buf_addstr(&b, ", \"magic\": ");
        tmd_json_escape(&b, e->raw.magic);
        tmd_buf_addstr(&b, ", \"version\": ");
        tmd_json_escape(&b, e->raw.version);
        if (e->raw.block_present)
        {
            char    *encoded = tmd_base64_encode(e->raw.block, sizeof(e->raw.block));
            uint64_t hdr = entry_header_bytes(e);

            /*
             * Where this block actually is, which is not always the member's
             * first block.
             *
             * A member with a GNU 'L' long name occupies three header blocks --
             * the 'L' header, the name it carries, then the member's own ustar
             * header -- and this is the last of them, the one the fields above
             * were decoded from. Reporting the offset rather than leaving the
             * reader to derive it is the difference between the JSON describing
             * the bytes and merely implying them; a check that the block matches
             * the file at "blocks.header_offset" fails on exactly these members,
             * which is how this came up.
             *
             * The extension blocks are not reproduced. What they carried is
             * already here, in the path and in "path_source".
             */
            uint64_t block_offset = e->offset +
                (hdr >= TMD_BLOCK_SIZE ? hdr - TMD_BLOCK_SIZE : 0);

            tmd_buf_addf(&b, ", \"block_offset\": %llu",
                         (unsigned long long)block_offset);
            tmd_buf_addf(&b, ", \"block_base64\": \"%s\"", encoded);
            free(encoded);
        }

        /*
         * The blocks before it: the 'L'/'K'/'x'/'g' headers and their payloads,
         * padding included. With these the JSON accounts for every byte the
         * member occupies, which is what the raw object was always trying to
         * be -- `block_base64` alone described the last block and left the two
         * or three before it to be inferred from their effects.
         */
        if (e->n_ext_blocks > 0)
        {
            tmd_buf_addstr(&b, ", \"extension_blocks\": [");
            for (i = 0; i < e->n_ext_blocks; i++)
            {
                char *enc = tmd_base64_encode(e->ext_blocks[i].bytes,
                                              sizeof(e->ext_blocks[i].bytes));

                tmd_buf_addf(&b, "%s{\"offset\": %llu, \"kind\": ", i ? ", " : "",
                             (unsigned long long)e->ext_blocks[i].offset);
                if (e->ext_blocks[i].kind)
                {
                    tmd_buf_addf(&b, "\"%c\"", e->ext_blocks[i].kind);
                } else
                {
                    /* A payload block of the header above it, not a header. */
                    tmd_buf_addstr(&b, "null");
                }
                tmd_buf_addf(&b, ", \"base64\": \"%s\"}", enc);
                free(enc);
            }
            tmd_buf_addf(&b, "], \"extension_blocks_truncated\": %s",
                         e->ext_truncated ? "true" : "false");
        }
        tmd_buf_addc(&b, '}');
    }

    if (e->nwarnings > 0)
    {
        tmd_buf_addstr(&b, ", \"warnings\": [");
        for (i = 0; i < e->nwarnings; i++)
        {
            if (i)
            {
                tmd_buf_addstr(&b, ", ");
            }
            tmd_buf_addf(&b, "{\"code\": \"%s\", \"text\": ", e->warnings[i].code);
            tmd_json_escape(&b, e->warnings[i].text);
            tmd_buf_addc(&b, '}');
        }
        tmd_buf_addc(&b, ']');
    }

    tmd_buf_addc(&b, '}');
    (void)fputs(b.data, rd->out);
    tmd_buf_free(&b);
}

/* ------------------------------------------------------------------------- */
/* CSV                                                                       */
/* ------------------------------------------------------------------------- */

/* RFC 4180: double the quotes, wrap anything containing a comma, a quote or a
 * newline. A path may legitimately contain all three. */
static void csv_field(struct tmd_buf *b, const char *s)
{
    bool   needs_quotes = false;
    size_t i;

    if (!s)
    {
        s = "";
    }
    for (i = 0; s[i]; i++)
    {
        if (s[i] == ',' || s[i] == '"' || s[i] == '\n' || s[i] == '\r')
        {
            needs_quotes = true;
        }
    }

    if (!needs_quotes)
    {
        tmd_buf_addstr(b, s);
        return;
    }
    tmd_buf_addc(b, '"');
    for (i = 0; s[i]; i++)
    {
        if (s[i] == '"')
        {
            tmd_buf_addc(b, '"');
        }
        tmd_buf_addc(b, s[i]);
    }
    tmd_buf_addc(b, '"');
}

static void csv_entry(struct tmd_render *rd, const struct tmd_entry *e)
{
    struct tmd_buf b;
    char           mode[11];
    char           stamp[64];

    tmd_buf_init(&b);
    tmd_mode_string(e->mode, e->kind, mode);
    format_time_iso(&e->mtime, stamp, sizeof(stamp));

    csv_field(&b, e->path);
    tmd_buf_addf(&b, ",%s,%s,%04o,%s,%lld,%lld,",
                 tmd_kind_name(e->kind), mode, e->mode,
                 tmd_format_name(e->format), (long long)e->uid,
                 (long long)e->gid);
    csv_field(&b, e->uname);
    tmd_buf_addc(&b, ',');
    csv_field(&b, e->gname);
    tmd_buf_addf(&b, ",%llu,%llu,%llu,",
                 (unsigned long long)e->size,
                 (unsigned long long)e->stored_size,
                 (unsigned long long)e->offset);
    csv_field(&b, stamp);
    tmd_buf_addf(&b, ",%lld,", (long long)(e->mtime.present ? e->mtime.sec : 0));
    csv_field(&b, e->linkpath);
    tmd_buf_addf(&b, ",%s", e->chksum_ok ? "ok" : "bad");
    tmd_buf_addc(&b, '\n');

    (void)fputs(b.data, rd->out);
    tmd_buf_free(&b);
}

/* ------------------------------------------------------------------------- */
/* Long text form                                                            */
/* ------------------------------------------------------------------------- */

static void text_long_entry(struct tmd_render *rd, const struct tmd_entry *e)
{
    const struct tmd_options *opt = rd->opt;
    FILE                     *out = rd->out;
    char                      mode[11];
    char                      stamp[64];
    char                      human[32];
    size_t                    i;

    tmd_mode_string(e->mode, e->kind, mode);

    (void)fprintf(out, "%s\n", e->path);
    (void)fprintf(out, "  type        %s (typeflag '%c' — %s)\n",
                  tmd_kind_name(e->kind),
                  (e->typeflag >= 32 && e->typeflag < 127) ? e->typeflag : '0',
                  tmd_typeflag_name(e->typeflag));
    (void)fprintf(out, "  format      %s\n", tmd_format_name(e->format));
    (void)fprintf(out, "  mode        %04o  %s\n", e->mode, mode);
    (void)fprintf(out, "  owner       %s (%lld) / %s (%lld)\n",
                  (e->uname && e->uname[0]) ? e->uname : "-", (long long)e->uid,
                  (e->gname && e->gname[0]) ? e->gname : "-", (long long)e->gid);

    if (e->has_dev)
    {
        (void)fprintf(out, "  device      %u, %u\n", e->devmajor, e->devminor);
    } else
    {
        (void)fprintf(out, "  size        %llu bytes (%s)\n",
                      (unsigned long long)e->size,
                      tmd_human_size(e->size, human, sizeof(human)));
    }

    (void)fprintf(out, "  in archive  %llu bytes at offset %llu\n",
                  (unsigned long long)e->stored_size,
                  (unsigned long long)e->offset);

    format_time(&e->mtime, opt, stamp, sizeof(stamp));
    (void)fprintf(out, "  modified    %s\n", stamp);
    if (e->atime.present)
    {
        format_time(&e->atime, opt, stamp, sizeof(stamp));
        (void)fprintf(out, "  accessed    %s\n", stamp);
    }
    if (e->ctime.present)
    {
        format_time(&e->ctime, opt, stamp, sizeof(stamp));
        (void)fprintf(out, "  changed     %s\n", stamp);
    }
    if (e->created.present)
    {
        format_time(&e->created, opt, stamp, sizeof(stamp));
        (void)fprintf(out, "  created     %s\n", stamp);
    }

    if (e->linkpath && e->linkpath[0])
    {
        (void)fprintf(out, "  links to    %s (%s)\n", e->linkpath,
                      e->linkpath_source ? e->linkpath_source : "header");
    }
    (void)fprintf(out, "  path from   %s\n",
                  e->path_source ? e->path_source : "header");

    (void)fprintf(out, "  checksum    %s (stored %06o, computed %06o)\n",
                  e->chksum_ok ? "ok" : "MISMATCH", e->chksum_stored,
                  e->chksum_unsigned);

    if (e->is_sparse)
    {
        (void)fprintf(out, "  sparse      %zu data segment%s, expands to %llu bytes\n",
                      e->nsparse, e->nsparse == 1 ? "" : "s",
                      (unsigned long long)(e->realsize ? e->realsize : e->size));
        for (i = 0; i < e->nsparse && i < 16; i++)
        {
            (void)fprintf(out, "                offset %llu, %llu bytes\n",
                          (unsigned long long)e->sparse[i].offset,
                          (unsigned long long)e->sparse[i].numbytes);
        }
        if (e->nsparse > 16)
        {
            (void)fprintf(out, "                ... and %zu more\n", e->nsparse - 16);
        }
    }

    for (i = 0; i < e->npax; i++)
    {
        (void)fprintf(out, "  pax         %s = %s\n", e->pax[i].key, e->pax[i].value);
    }

    if (opt->headers)
    {
        (void)fprintf(out, "  raw header\n");
        (void)fprintf(out, "    name      \"%s\"\n", e->raw.name);
        (void)fprintf(out, "    prefix    \"%s\"\n", e->raw.prefix);
        (void)fprintf(out, "    mode      \"%s\"\n", e->raw.mode);
        (void)fprintf(out, "    uid       \"%s\"\n", e->raw.uid);
        (void)fprintf(out, "    gid       \"%s\"\n", e->raw.gid);
        (void)fprintf(out, "    size      \"%s\"\n", e->raw.size);
        (void)fprintf(out, "    mtime     \"%s\"\n", e->raw.mtime);
        (void)fprintf(out, "    chksum    \"%s\"\n", e->raw.chksum);
        (void)fprintf(out, "    typeflag  '%c'\n",
                      (e->raw.typeflag >= 32 && e->raw.typeflag < 127)
                          ? e->raw.typeflag : '0');
        (void)fprintf(out, "    linkname  \"%s\"\n", e->raw.linkname);
        (void)fprintf(out, "    magic     \"%s\" version \"%s\"\n",
                      e->raw.magic, e->raw.version);
        (void)fprintf(out, "    uname     \"%s\"\n", e->raw.uname);
        (void)fprintf(out, "    gname     \"%s\"\n", e->raw.gname);
        (void)fprintf(out, "    devmajor  \"%s\"  devminor \"%s\"\n",
                      e->raw.devmajor, e->raw.devminor);
    }

    for (i = 0; i < e->nwarnings; i++)
    {
        (void)fprintf(out, "  warning     %s\n", e->warnings[i].text);
    }

    (void)fputc('\n', out);
}

/* ------------------------------------------------------------------------- */
/* Summary                                                                   */
/* ------------------------------------------------------------------------- */

static const char *format_long_name(enum tmd_format f)
{
    switch (f)
    {
    case TMD_FMT_V7:    return "v7 (pre-POSIX)";
    case TMD_FMT_USTAR: return "POSIX ustar (POSIX.1-1988)";
    case TMD_FMT_STAR:  return "star";
    case TMD_FMT_GNU:   return "GNU tar";
    case TMD_FMT_PAX:   return "POSIX pax (POSIX.1-2001)";
    default:            return "unrecognized";
    }
}

/* ------------------------------------------------------------------------- */
/* --info: what this archive is                                              */
/* ------------------------------------------------------------------------- */

/*
 * The generations of the tar format.
 *
 * tar has been revised four times in forty-five years and every revision kept
 * the same 512-byte header, so an archive does not announce its generation —
 * it has to be worked out from which fields are filled in and which extension
 * blocks appear. Naming the generation is the single most useful thing --info
 * says, because it is what decides which tools can read the file.
 */
static const char *generation_name(enum tmd_format f)
{
    switch (f)
    {
    case TMD_FMT_V7:    return "1st — Seventh Edition Unix tar (1979)";
    case TMD_FMT_USTAR: return "2nd — POSIX ustar, IEEE 1003.1-1988";
    case TMD_FMT_GNU:   return "2nd, GNU branch — GNU tar's own extensions (1990s)";
    case TMD_FMT_STAR:  return "2nd, star branch — Jörg Schilling's star (1985-)";
    case TMD_FMT_PAX:   return "3rd — POSIX pax, IEEE 1003.1-2001";
    default:            return "unrecognized";
    }
}

static const char *generation_note(enum tmd_format f)
{
    switch (f)
    {
    case TMD_FMT_V7:
        return "100-byte paths, no magic, no owner names, no device numbers.\n"
               "                    Only regular files, hard links and symlinks "
               "can be expressed.";
    case TMD_FMT_USTAR:
        return "Adds the \"ustar\" magic, a 155-byte path prefix, owner and\n"
               "                    group names, device numbers, and directory, "
               "FIFO and\n                   contiguous-file types.";
    case TMD_FMT_GNU:
        return "ustar's layout with GNU's own extensions: 'L' and 'K' blocks "
               "for\n                   unlimited paths, 'S' for sparse files, "
               "base-256 numbers,\n                   and atime/ctime written "
               "over ustar's prefix field.";
    case TMD_FMT_STAR:
        return "ustar plus star's signature and its SCHILY.* attributes for "
               "ACLs,\n                   extended attributes and sub-second "
               "times.";
    case TMD_FMT_PAX:
        return "ustar headers plus 'x' and 'g' blocks carrying arbitrary\n"
               "                    key/value attributes: paths and link "
               "targets of any length,\n                   64-bit ids, and "
               "timestamps to the nanosecond.";
    default:
        return "No header in this archive matched a known dialect.";
    }
}

/*
 * What it takes to read this archive correctly.
 *
 * Deliberately about *this file* rather than about its magic bytes: an archive
 * written by GNU tar that happens to use no GNU extension reads perfectly in a
 * ustar-only tool, and saying otherwise would send somebody converting an
 * archive that did not need converting.
 */
static void print_portability(FILE *out, const struct tmd_archive *a)
{
    const struct tmd_features *f = &a->features;
    bool  needs_pax = f->pax_headers > 0 || f->pax_globals > 0 || f->sparse_pax > 0;
    bool  needs_gnu = f->gnu_longname > 0 || f->gnu_longlink > 0 ||
                      f->sparse_gnu_old > 0 || f->dumpdirs > 0 ||
                      f->multivolume > 0 || f->base256_fields > 0;
    /*
     * What genuinely requires a ustar header: the 155-byte path prefix, the
     * owner and group *names*, and device numbers. Those fields do not exist
     * in a v7 header at all, so an archive using any of them cannot be read
     * correctly without one.
     *
     * Directory and FIFO typeflags deliberately do not count. They postdate v7
     * itself, but they live in a byte v7 already had and every tar written
     * since 1988 understands them — so listing them as "needs ustar" would
     * send somebody converting an archive that nothing has trouble with.
     */
    bool  needs_ustar = f->prefix_used > 0 || f->names_present > 0 ||
                        a->counts[TMD_KIND_CHARDEV] > 0 ||
                        a->counts[TMD_KIND_BLOCKDEV] > 0;
    bool  beyond_v7_types = a->counts[TMD_KIND_DIR] > 0 ||
                            a->counts[TMD_KIND_FIFO] > 0 ||
                            a->counts[TMD_KIND_CONTIGUOUS] > 0;

    if (needs_pax)
    {
        (void)fprintf(out, "  needs a reader   that understands pax extended headers\n"
                           "                   (GNU tar 1.14+, bsdtar, star, POSIX pax)\n");
    } else if (needs_gnu)
    {
        (void)fprintf(out, "  needs a reader   that understands GNU's extensions\n"
                           "                   (GNU tar, bsdtar, star)\n");
    } else if (needs_ustar)
    {
        (void)fprintf(out, "  needs a reader   any POSIX ustar tar; nothing here is "
                           "an extension\n");
    } else
    {
        (void)fprintf(out, "  needs a reader   any tar at all — this uses nothing "
                           "beyond v7\n");
        if (beyond_v7_types)
        {
            (void)fprintf(out, "                   (the directory and FIFO "
                               "typeflags postdate v7 itself,\n"
                               "                   but every tar since 1988 reads "
                               "them)\n");
        }
    }

    if (f->paths_over_255)
    {
        (void)fprintf(out, "                   %zu path%s cannot be expressed in a "
                           "ustar header at all\n",
                      f->paths_over_255, f->paths_over_255 == 1 ? "" : "s");
    } else if (f->paths_over_100)
    {
        (void)fprintf(out, "                   %zu path%s over 100 bytes; a v7 "
                           "reader would truncate %s\n",
                      f->paths_over_100, f->paths_over_100 == 1 ? "" : "s",
                      f->paths_over_100 == 1 ? "it" : "them");
    }
}

/*
 * What the filter let through.
 *
 * The summary keeps describing the WHOLE archive -- format, blocking and
 * integrity are properties of the file and do not change because a pattern was
 * supplied -- so this one line is the only thing -m adds to it. Without it a
 * filtered run gives no way to tell "two members matched" from "the archive has
 * two members".
 */
/*
 * How the members are ordered, and what extracting them would create.
 *
 * Reported as observations, and deliberately not as a guess about which
 * command wrote the archive. The first draft here did guess -- "lexicographic,
 * so it came from a sorted list" and "unsorted with directories, so it was a
 * directory walk" -- and both were caught being wrong within minutes of being
 * written: `tar -c DIR` over a small tree came out in exact lexicographic order
 * because readdir happened to return it that way, and a reverse-sorted file
 * list came out unsorted with directories present. The ordering is a clue, and
 * a clue is what it is reported as.
 *
 * One inference IS sound and is stated: an archive with no directory members
 * was written from a list of files, because a directory walk emits the
 * directories it walks through unless it was told not to.
 *
 * The top-level count answers the older sense of "tar bomb" -- not a path that
 * escapes, but an archive that unpacks two hundred entries into whatever
 * directory you happened to be standing in.
 */
static void print_order(FILE *out, const struct tmd_archive *a, int width)
{
    const struct tmd_features *f = &a->features;
    bool has_dirs = a->counts[TMD_KIND_DIR] > 0;

    if (a->entries < 2)
    {
        return; /* one member is in every order at once */
    }

    if (a->codec)
    {
        (void)fprintf(out, "  %-*s%s\n", width, "compression", a->codec);
    }
    (void)fprintf(out, "  %-*s%s%s\n", width, "member order",
                  f->order_sorted ? "lexicographic by path"
                                  : "not in path order",
                  has_dirs ? ""
                           : "; no directory members, so it was written from a "
                             "list of files");

    if (f->nroots == 1 && !f->roots_truncated)
    {
        (void)fprintf(out, "  %-*sone entry: %s\n", width, "top level",
                      f->roots[0]);
    } else if (f->roots_truncated)
    {
        (void)fprintf(out, "  %-*smore than %zu entries — extracting scatters "
                           "them into the current directory\n",
                      width, "top level",
                      sizeof(f->roots) / sizeof(f->roots[0]));
    } else
    {
        struct tmd_buf list;
        size_t         i;

        tmd_buf_init(&list);
        for (i = 0; i < f->nroots; i++)
        {
            tmd_buf_addf(&list, "%s%s", list.len ? ", " : "", f->roots[i]);
        }
        (void)fprintf(out, "  %-*s%zu entries: %s\n", width, "top level",
                      f->nroots, list.data ? list.data : "");
        tmd_buf_free(&list);
    }
}

/*
 * The extraction-safety verdict, as a class.
 *
 * Stated even when it is clean, which is unusual for this report -- most lines
 * appear only when there is something to say. This one is different because a
 * reader pointing tmd at an untrusted archive is asking a yes/no question, and
 * the absence of a line is not an answer: it reads as "tmd did not look".
 */
static void print_extraction(FILE *out, const struct tmd_archive *a, int width)
{
    const struct tmd_features *f = &a->features;
    uint64_t total = f->escape_absolute + f->escape_traversal + f->escape_link;
    struct tmd_buf parts;

    if (total == 0)
    {
        (void)fprintf(out, "  %-*s%s\n", width, "extraction",
                      "every member stays inside the extraction directory");
        return;
    }

    tmd_buf_init(&parts);
    if (f->escape_absolute)
    {
        tmd_buf_addf(&parts, "%llu absolute",
                     (unsigned long long)f->escape_absolute);
    }
    if (f->escape_traversal)
    {
        tmd_buf_addf(&parts, "%s%llu climbing out with \"..\"",
                     parts.len ? ", " : "",
                     (unsigned long long)f->escape_traversal);
    }
    if (f->escape_link)
    {
        tmd_buf_addf(&parts, "%s%llu link%s pointing outside",
                     parts.len ? ", " : "",
                     (unsigned long long)f->escape_link,
                     f->escape_link == 1 ? "" : "s");
    }
    (void)fprintf(out, "  %-*s%llu member%s would extract OUTSIDE the current "
                       "directory\n", width, "extraction",
                  (unsigned long long)total, total == 1 ? "" : "s");
    (void)fprintf(out, "  %-*s%s\n", width, "", parts.data ? parts.data : "");
    tmd_buf_free(&parts);
}

static void text_matched_line(struct tmd_render *rd, int label_width)
{
    if (rd->opt->nmatch == 0)
    {
        return;
    }
    /* The label column is passed in because the summary and the -i report use
     * different widths, and a line that does not line up with the ones around
     * it reads as a different kind of thing. */
    (void)fprintf(rd->out, "  %-*s%llu of %llu member%s\n",
                  label_width, "matched",
                  (unsigned long long)rd->matched,
                  (unsigned long long)rd->seen,
                  rd->seen == 1 ? "" : "s");
}

static void text_info(struct tmd_render *rd, const struct tmd_archive *a)
{
    const struct tmd_features *f = &a->features;
    FILE *out = rd->out;
    char  human[32];
    bool  first = true;
    int   k;
    /* Summed once. Spelled inline these appeared three times per line — in the
     * test, in the value and in the plural — which is three places for the two
     * halves to stop agreeing. */
    uint64_t devices = a->counts[TMD_KIND_CHARDEV] + a->counts[TMD_KIND_BLOCKDEV];
    uint64_t sparse_members = f->sparse_gnu_old + f->sparse_pax;

    (void)fprintf(out, "\n%s\n", a->name);

    if (a->file_size)
    {
        (void)fprintf(out, "  size             %llu bytes (%s)\n",
                      (unsigned long long)a->file_size,
                      tmd_human_size(a->file_size, human, sizeof(human)));
    }

    (void)fprintf(out, "  format           %s\n", format_long_name(a->format));
    (void)fprintf(out, "  generation       %s\n", generation_name(a->format));
    (void)fprintf(out, "                   %s\n", generation_note(a->format));

    (void)fprintf(out, "  dialects seen    ");
    for (k = TMD_FMT_V7; k <= TMD_FMT_PAX; k++)
    {
        if (!a->formats[k])
        {
            continue;
        }
        (void)fprintf(out, "%s%s", first ? "" : ", ",
                      tmd_format_name((enum tmd_format)k));
        first = false;
    }
    (void)fprintf(out, "%s\n", first ? "none" : "");

    if (a->writer)
    {
        (void)fprintf(out, "  written by       %s (inferred)\n", a->writer);
    } else
    {
        (void)fprintf(out, "  written by       no tool-specific fingerprint in this "
                           "archive\n");
    }

    /* --- the extensions this archive actually relies on ------------------ */
    (void)fprintf(out, "  extensions used  ");
    {
        struct tmd_buf list;

        tmd_buf_init(&list);
        if (f->gnu_longname)
        {
            tmd_buf_addf(&list, "%sGNU long name blocks (%llu)",
                         list.len ? ", " : "", (unsigned long long)f->gnu_longname);
        }
        if (f->gnu_longlink)
        {
            tmd_buf_addf(&list, "%sGNU long link blocks (%llu)",
                         list.len ? ", " : "", (unsigned long long)f->gnu_longlink);
        }
        if (f->pax_headers)
        {
            tmd_buf_addf(&list, "%spax extended headers (%llu)",
                         list.len ? ", " : "", (unsigned long long)f->pax_headers);
        }
        if (f->pax_globals)
        {
            tmd_buf_addf(&list, "%spax global headers (%llu)",
                         list.len ? ", " : "", (unsigned long long)f->pax_globals);
        }
        if (f->sparse_gnu_old)
        {
            tmd_buf_addf(&list, "%sGNU sparse, old format (%llu)",
                         list.len ? ", " : "", (unsigned long long)f->sparse_gnu_old);
        }
        if (f->sparse_pax)
        {
            tmd_buf_addf(&list, "%sGNU sparse, pax format (%llu)",
                         list.len ? ", " : "", (unsigned long long)f->sparse_pax);
        }
        if (f->prefix_used)
        {
            tmd_buf_addf(&list, "%sustar path prefix (%llu)",
                         list.len ? ", " : "", (unsigned long long)f->prefix_used);
        }
        if (f->base256_fields)
        {
            tmd_buf_addf(&list, "%sbase-256 numeric fields (%llu)",
                         list.len ? ", " : "", (unsigned long long)f->base256_fields);
        }
        if (f->dumpdirs)
        {
            tmd_buf_addf(&list, "%sGNU incremental dumpdirs (%llu)",
                         list.len ? ", " : "", (unsigned long long)f->dumpdirs);
        }
        if (f->multivolume)
        {
            tmd_buf_addf(&list, "%smulti-volume continuations (%llu)",
                         list.len ? ", " : "", (unsigned long long)f->multivolume);
        }
        if (f->volume_labels)
        {
            tmd_buf_addf(&list, "%svolume labels (%llu)",
                         list.len ? ", " : "", (unsigned long long)f->volume_labels);
        }
        if (f->xattr_members)
        {
            tmd_buf_addf(&list, "%sextended-attribute members (%llu)",
                         list.len ? ", " : "", (unsigned long long)f->xattr_members);
        }
        (void)fprintf(out, "%s\n", list.len ? list.data : "none — plain headers only");
        tmd_buf_free(&list);
    }

    if (f->npax_keys)
    {
        size_t i;

        (void)fprintf(out, "  pax keys         ");
        for (i = 0; i < f->npax_keys; i++)
        {
            (void)fprintf(out, "%s%s", i ? ", " : "", f->pax_keys[i]);
        }
        (void)fprintf(out, "%s\n", f->pax_keys_truncated ? ", ..." : "");
    }

    /* --- what the metadata can express ----------------------------------- */
    (void)fprintf(out, "  paths            longest %zu bytes", f->max_path);
    if (f->paths_over_255)
    {
        (void)fprintf(out, "; %zu over the ustar limit", f->paths_over_255);
    } else if (f->paths_over_100)
    {
        (void)fprintf(out, "; %zu over the v7 limit", f->paths_over_100);
    }
    (void)fputc('\n', out);

    (void)fprintf(out, "  timestamps       mtime%s%s%s%s\n",
                  f->subsecond_times ? " to nanosecond precision" : " to the second",
                  f->atime_present ? ", atime" : "",
                  f->ctime_present ? ", ctime" : "",
                  f->created_present ? ", creation time (written by libarchive "
                                       "on a BSD or a Mac)" : "");

    (void)fprintf(out, "  ownership        %s; highest uid %lld, gid %lld\n",
                  f->names_present ? "names and numbers" : "numbers only (no names stored)",
                  (long long)f->max_uid, (long long)f->max_gid);

    /* --- physical layout -------------------------------------------------- */
    if (a->record_blocks)
    {
        (void)fprintf(out, "  blocking         %u blocks of %d bytes (%u), inferred "
                           "from the length\n",
                      a->record_blocks, TMD_BLOCK_SIZE,
                      a->record_blocks * TMD_BLOCK_SIZE);
    } else
    {
        (void)fprintf(out, "  blocking         not a multiple of a standard record "
                           "size\n");
    }

    (void)fprintf(out, "  end marker       %s\n",
                  a->eof_marker ? "present (two zero blocks)"
                                : "MISSING — the archive is truncated");
    if (a->trailing_bytes)
    {
        (void)fprintf(out, "  after the marker %llu bytes%s\n",
                      (unsigned long long)a->trailing_bytes,
                      a->trailing_garbage
                          ? " that are NOT zero — a second archive, or damage"
                          : " of zero padding");
    }

    /* --- contents --------------------------------------------------------- */
    text_matched_line(rd, 17);
    (void)fprintf(out, "  members          %llu", (unsigned long long)a->entries);
    {
        struct tmd_buf parts;

        tmd_buf_init(&parts);
        if (a->counts[TMD_KIND_FILE])
        {
            tmd_buf_addf(&parts, "%llu file%s",
                         (unsigned long long)a->counts[TMD_KIND_FILE],
                         a->counts[TMD_KIND_FILE] == 1 ? "" : "s");
        }
        if (a->counts[TMD_KIND_DIR])
        {
            tmd_buf_addf(&parts, "%s%llu director%s", parts.len ? ", " : "",
                         (unsigned long long)a->counts[TMD_KIND_DIR],
                         a->counts[TMD_KIND_DIR] == 1 ? "y" : "ies");
        }
        if (a->counts[TMD_KIND_SYMLINK])
        {
            tmd_buf_addf(&parts, "%s%llu symlink%s", parts.len ? ", " : "",
                         (unsigned long long)a->counts[TMD_KIND_SYMLINK],
                         a->counts[TMD_KIND_SYMLINK] == 1 ? "" : "s");
        }
        if (a->counts[TMD_KIND_HARDLINK])
        {
            tmd_buf_addf(&parts, "%s%llu hard link%s", parts.len ? ", " : "",
                         (unsigned long long)a->counts[TMD_KIND_HARDLINK],
                         a->counts[TMD_KIND_HARDLINK] == 1 ? "" : "s");
        }
        if (devices)
        {
            tmd_buf_addf(&parts, "%s%llu device node%s", parts.len ? ", " : "",
                         (unsigned long long)devices, devices == 1 ? "" : "s");
        }
        if (a->counts[TMD_KIND_FIFO])
        {
            tmd_buf_addf(&parts, "%s%llu FIFO%s", parts.len ? ", " : "",
                         (unsigned long long)a->counts[TMD_KIND_FIFO],
                         a->counts[TMD_KIND_FIFO] == 1 ? "" : "s");
        }
        if (parts.len)
        {
            (void)fprintf(out, " (%s)", parts.data);
        }
        tmd_buf_free(&parts);
    }
    (void)fputc('\n', out);

    (void)fprintf(out, "  content          %llu bytes (%s) when extracted\n",
                  (unsigned long long)a->total_size,
                  tmd_human_size(a->total_size, human, sizeof(human)));
    if (sparse_members > 0)
    {
        (void)fprintf(out, "                   %llu of those members %s sparse, so "
                           "the archive is much smaller\n",
                      (unsigned long long)sparse_members,
                      sparse_members == 1 ? "is" : "are");
    }

    /* --- integrity -------------------------------------------------------- */
    if (a->bad_checksums)
    {
        (void)fprintf(out, "  integrity        %llu header checksum%s did not match\n",
                      (unsigned long long)a->bad_checksums,
                      a->bad_checksums == 1 ? "" : "s");
    } else if (a->eof_marker && !a->trailing_garbage)
    {
        (void)fprintf(out, "  integrity        every header checksum matched\n");
    } else
    {
        (void)fprintf(out, "  integrity        checksums matched, but see the end "
                           "marker above\n");
    }

    if (f->unknown_typeflags)
    {
        (void)fprintf(out, "  unknown types    %llu member%s carry a typeflag this "
                           "does not recognize\n",
                      (unsigned long long)f->unknown_typeflags,
                      f->unknown_typeflags == 1 ? "" : "s");
    }

    print_extraction(out, a, 17);
    print_order(out, a, 17);

    print_portability(out, a);

    if (a->npax_global)
    {
        size_t g;
        for (g = 0; g < a->npax_global; g++)
        {
            (void)fprintf(out, "  pax global       %s = %s\n",
                          a->pax_global[g].key, a->pax_global[g].value);
        }
    }
    (void)fputc('\n', out);
}

static void text_summary(struct tmd_render *rd, const struct tmd_archive *a)
{
    FILE    *out = rd->out;
    char     human[32];
    char     stored[32];
    int      i;
    bool     first = true;
    uint64_t devices = a->counts[TMD_KIND_CHARDEV] + a->counts[TMD_KIND_BLOCKDEV];

    (void)fprintf(out, "\n%s\n", a->name);
    (void)fprintf(out, "  format        %s\n", format_long_name(a->format));

    (void)fprintf(out, "  dialects      ");
    for (i = TMD_FMT_V7; i <= TMD_FMT_PAX; i++)
    {
        if (!a->formats[i])
        {
            continue;
        }
        (void)fprintf(out, "%s%s", first ? "" : ", ",
                      tmd_format_name((enum tmd_format)i));
        first = false;
    }
    if (first)
    {
        (void)fprintf(out, "none");
    }
    (void)fputc('\n', out);

    if (a->writer)
    {
        (void)fprintf(out, "  written by    %s (inferred)\n", a->writer);
    }

    text_matched_line(rd, 14);
    (void)fprintf(out, "  members       %llu\n", (unsigned long long)a->entries);
    if (a->counts[TMD_KIND_FILE])
    {
        (void)fprintf(out, "                %llu file%s\n",
                      (unsigned long long)a->counts[TMD_KIND_FILE],
                      a->counts[TMD_KIND_FILE] == 1 ? "" : "s");
    }
    if (a->counts[TMD_KIND_DIR])
    {
        (void)fprintf(out, "                %llu director%s\n",
                      (unsigned long long)a->counts[TMD_KIND_DIR],
                      a->counts[TMD_KIND_DIR] == 1 ? "y" : "ies");
    }
    if (a->counts[TMD_KIND_SYMLINK])
    {
        (void)fprintf(out, "                %llu symlink%s\n",
                      (unsigned long long)a->counts[TMD_KIND_SYMLINK],
                      a->counts[TMD_KIND_SYMLINK] == 1 ? "" : "s");
    }
    if (a->counts[TMD_KIND_HARDLINK])
    {
        (void)fprintf(out, "                %llu hard link%s\n",
                      (unsigned long long)a->counts[TMD_KIND_HARDLINK],
                      a->counts[TMD_KIND_HARDLINK] == 1 ? "" : "s");
    }
    if (devices)
    {
        (void)fprintf(out, "                %llu device node%s\n",
                      (unsigned long long)devices, devices == 1 ? "" : "s");
    }
    if (a->counts[TMD_KIND_FIFO])
    {
        (void)fprintf(out, "                %llu FIFO%s\n",
                      (unsigned long long)a->counts[TMD_KIND_FIFO],
                      a->counts[TMD_KIND_FIFO] == 1 ? "" : "s");
    }

    (void)fprintf(out, "  content       %llu bytes (%s)\n",
                  (unsigned long long)a->total_size,
                  tmd_human_size(a->total_size, human, sizeof(human)));
    (void)fprintf(out, "  archive       %llu bytes (%s)%s\n",
                  (unsigned long long)(a->file_size ? a->file_size : a->total_stored),
                  tmd_human_size(a->file_size ? a->file_size : a->total_stored,
                                 stored, sizeof(stored)),
                  a->file_size ? "" : " read");

    if (a->record_blocks)
    {
        (void)fprintf(out, "  blocking      %u blocks (%u bytes), inferred\n",
                      a->record_blocks, a->record_blocks * TMD_BLOCK_SIZE);
    }
    if (a->bad_checksums)
    {
        (void)fprintf(out, "  checksums     %llu header%s did not match\n",
                      (unsigned long long)a->bad_checksums,
                      a->bad_checksums == 1 ? "" : "s");
    }
    (void)fprintf(out, "  end marker    %s\n",
                  a->eof_marker ? "present" : "MISSING (archive is truncated)");
    if (a->trailing_bytes)
    {
        (void)fprintf(out, "  after marker  %llu bytes%s\n",
                      (unsigned long long)a->trailing_bytes,
                      a->trailing_garbage ? " — NOT all zero" : " of padding");
    }

    print_extraction(out, a, 14);
    print_order(out, a, 14);

    if (a->npax_global)
    {
        size_t k;
        for (k = 0; k < a->npax_global; k++)
        {
            (void)fprintf(out, "  pax global    %s = %s\n",
                          a->pax_global[k].key, a->pax_global[k].value);
        }
    }
}

static void json_summary(struct tmd_render *rd, const struct tmd_archive *a)
{
    struct tmd_buf b;
    char           human[32];

    tmd_buf_init(&b);
    tmd_buf_addstr(&b, "  \"summary\": {");
    tmd_buf_addf(&b, "\"format\": \"%s\"", tmd_format_name(a->format));
    tmd_buf_addf(&b, ", \"format_name\": \"%s\"", format_long_name(a->format));
    if (a->writer)
    {
        tmd_buf_addstr(&b, ", \"written_by\": ");
        tmd_json_escape(&b, a->writer);
    }
    tmd_buf_addf(&b, ", \"members\": %llu", (unsigned long long)a->entries);
    if (rd->opt->nmatch > 0)
    {
        tmd_buf_addf(&b, ", \"matched\": %llu", (unsigned long long)rd->matched);
    }
    tmd_buf_addf(&b, ", \"files\": %llu", (unsigned long long)a->counts[TMD_KIND_FILE]);
    tmd_buf_addf(&b, ", \"directories\": %llu",
                 (unsigned long long)a->counts[TMD_KIND_DIR]);
    tmd_buf_addf(&b, ", \"symlinks\": %llu",
                 (unsigned long long)a->counts[TMD_KIND_SYMLINK]);
    tmd_buf_addf(&b, ", \"hardlinks\": %llu",
                 (unsigned long long)a->counts[TMD_KIND_HARDLINK]);
    tmd_buf_addf(&b, ", \"content_bytes\": %llu", (unsigned long long)a->total_size);
    tmd_buf_addf(&b, ", \"content_human\": \"%s\"",
                 tmd_human_size(a->total_size, human, sizeof(human)));
    tmd_buf_addf(&b, ", \"archive_bytes\": %llu",
                 (unsigned long long)(a->file_size ? a->file_size : a->total_stored));
    if (a->record_blocks)
    {
        tmd_buf_addf(&b, ", \"record_blocks\": %u", a->record_blocks);
    }
    tmd_buf_addf(&b, ", \"bad_checksums\": %llu",
                 (unsigned long long)a->bad_checksums);
    tmd_buf_addf(&b, ", \"end_marker\": %s", a->eof_marker ? "true" : "false");
    tmd_buf_addf(&b, ", \"trailing_bytes\": %llu",
                 (unsigned long long)a->trailing_bytes);
    tmd_buf_addf(&b, ", \"trailing_garbage\": %s",
                 a->trailing_garbage ? "true" : "false");
    /*
     * Extraction safety as its own object, present whether or not -i was asked
     * for.
     *
     * A consumer checking whether an archive is safe to unpack should read one
     * field and get a boolean, not have to pass an extra switch and then infer
     * the answer from which keys are absent.
     */
    {
        const struct tmd_features *ef = &a->features;
        uint64_t escapes = ef->escape_absolute + ef->escape_traversal +
                           ef->escape_link;

        tmd_buf_addf(&b, ", \"extraction\": {\"escapes\": %s",
                     escapes ? "true" : "false");
        tmd_buf_addf(&b, ", \"absolute_paths\": %llu",
                     (unsigned long long)ef->escape_absolute);
        tmd_buf_addf(&b, ", \"traversals\": %llu",
                     (unsigned long long)ef->escape_traversal);
        tmd_buf_addf(&b, ", \"links_outside\": %llu}",
                     (unsigned long long)ef->escape_link);

        if (a->codec)
        {
            tmd_buf_addstr(&b, ", \"compression\": ");
            tmd_json_escape(&b, a->codec);
        } else
        {
            tmd_buf_addstr(&b, ", \"compression\": null");
        }
        tmd_buf_addf(&b, ", \"order\": {\"sorted\": %s",
                     ef->order_sorted ? "true" : "false");
        tmd_buf_addstr(&b, ", \"top_level\": [");
        {
            size_t k;

            for (k = 0; k < ef->nroots; k++)
            {
                tmd_buf_addstr(&b, k ? ", " : "");
                tmd_json_escape(&b, ef->roots[k]);
            }
        }
        tmd_buf_addf(&b, "], \"top_level_capped\": %s}",
                     ef->roots_truncated ? "true" : "false");
    }

    if (rd->opt->info)
    {
        const struct tmd_features *f = &a->features;
        size_t                     i;

        tmd_buf_addstr(&b, ", \"features\": {");
        tmd_buf_addf(&b, "\"generation\": \"%s\"", generation_name(a->format));
        tmd_buf_addf(&b, ", \"gnu_longname_blocks\": %llu",
                     (unsigned long long)f->gnu_longname);
        tmd_buf_addf(&b, ", \"gnu_longlink_blocks\": %llu",
                     (unsigned long long)f->gnu_longlink);
        tmd_buf_addf(&b, ", \"pax_headers\": %llu",
                     (unsigned long long)f->pax_headers);
        tmd_buf_addf(&b, ", \"pax_global_headers\": %llu",
                     (unsigned long long)f->pax_globals);
        tmd_buf_addf(&b, ", \"sparse_gnu_old\": %llu",
                     (unsigned long long)f->sparse_gnu_old);
        tmd_buf_addf(&b, ", \"sparse_pax\": %llu",
                     (unsigned long long)f->sparse_pax);
        tmd_buf_addf(&b, ", \"ustar_prefix_used\": %llu",
                     (unsigned long long)f->prefix_used);
        tmd_buf_addf(&b, ", \"base256_fields\": %llu",
                     (unsigned long long)f->base256_fields);
        tmd_buf_addf(&b, ", \"subsecond_times\": %llu",
                     (unsigned long long)f->subsecond_times);
        tmd_buf_addf(&b, ", \"max_path_length\": %zu", f->max_path);
        tmd_buf_addf(&b, ", \"paths_over_100\": %zu", f->paths_over_100);
        tmd_buf_addf(&b, ", \"paths_over_255\": %zu", f->paths_over_255);
        tmd_buf_addf(&b, ", \"max_uid\": %lld", (long long)f->max_uid);
        tmd_buf_addf(&b, ", \"max_gid\": %lld", (long long)f->max_gid);
        tmd_buf_addstr(&b, ", \"pax_keys\": [");
        for (i = 0; i < f->npax_keys; i++)
        {
            if (i)
            {
                tmd_buf_addstr(&b, ", ");
            }
            tmd_json_escape(&b, f->pax_keys[i]);
        }
        tmd_buf_addstr(&b, "]}");
    }

    if (rd->opt->stats && rd->stat)
    {
        json_stat(rd, &b);
    }

    tmd_buf_addc(&b, '}');
    (void)fputs(b.data, rd->out);
    tmd_buf_free(&b);
}

/* ------------------------------------------------------------------------- */
/* The renderer itself                                                       */
/* ------------------------------------------------------------------------- */

struct tmd_render *tmd_render_new(FILE *out, const struct tmd_options *opt,
                                  int archive_count)
{
    struct tmd_render *rd = tmd_xcalloc(1, sizeof(*rd));

    rd->out = out;
    rd->opt = opt;
    rd->archive_count = archive_count;

    /* More than one archive in JSON means a top-level array; one archive means
     * a bare object, so that `tmd -f x.tar --format=json | jq .summary` works
     * without the caller having to index into a single-element list. */
    if (opt->output == TMD_OUT_JSON && archive_count > 1)
    {
        (void)fputs("[\n", out);
    }
    return rd;
}

void tmd_render_free(struct tmd_render *rd)
{
    stat_free(rd->stat);
    free(rd);
}

/* "2 minutes", "3 days" — enough to see the shape of a span at a glance. */
static void span_string(int64_t seconds, char *buf, size_t bufsz)
{
    static const struct {
        int64_t     unit;
        const char *name;
    } scale[] = {
        { INT64_C(86400) * 365, "year" },
        { INT64_C(86400) * 30,  "month" },
        { 86400,       "day" },
        { 3600,        "hour" },
        { 60,          "minute" },
        { 1,           "second" },
    };
    size_t i;

    if (seconds == 0)
    {
        (void)snprintf(buf, bufsz, "none — every member shares one second");
        return;
    }
    for (i = 0; i < sizeof(scale) / sizeof(*scale); i++)
    {
        if (seconds >= scale[i].unit)
        {
            int64_t n = seconds / scale[i].unit;

            (void)snprintf(buf, bufsz, "%lld %s%s", (long long)n,
                           scale[i].name, n == 1 ? "" : "s");
            return;
        }
    }
    (void)snprintf(buf, bufsz, "%lld seconds", (long long)seconds);
}

/* The timestamp the most members share, and how many. */
static const struct stat_time *stat_modal(const struct stats *s)
{
    const struct stat_time *best = NULL;
    size_t                  i;

    for (i = 0; i < STAT_TIME_SLOTS; i++)
    {
        if (s->times[i].used && (!best || s->times[i].count > best->count))
        {
            best = &s->times[i];
        }
    }
    return best;
}

/*
 * What the spread of timestamps says about how the archive was made.
 *
 * This is the half of --stat worth having. "562 members, 2 distinct mtimes two
 * minutes apart" is a fact; "exported from version control and then partly
 * regenerated" is what the fact means, and it is the thing somebody trying to
 * date a tarball actually wants. Stated as an inference, not a verdict,
 * because it is one.
 */
static const char *stat_verdict(const struct stats *s)
{
    int64_t span;

    if (!s->have_time || s->members == 0)
    {
        return NULL;
    }
    span = s->latest - s->earliest;

    if (s->ndistinct == 1 && !s->distinct_overflow)
    {
        if (s->zero_time)
        {
            return "every member is dated at the epoch — timestamps were "
                   "discarded, not preserved";
        }
        return "one timestamp for every member — normalized, as a reproducible "
               "build does with SOURCE_DATE_EPOCH";
    }
    if (!s->distinct_overflow && s->ndistinct <= 8 && span <= 3600)
    {
        return "a handful of timestamps within an hour — exported into a fresh "
               "directory, then a few files regenerated";
    }
    if (span >= INT64_C(86400) * 30)
    {
        return "timestamps spread over months or more — real per-file times, "
               "preserved from a working tree";
    }
    return NULL;
}

static void text_stat(struct tmd_render *rd, const struct tmd_archive *a)
{
    FILE              *out = rd->out;
    const struct stats *s = rd->stat;
    char               human[32];
    char               stamp[64];
    char               span[64];
    const char        *verdict;
    size_t             i;

    (void)fprintf(out, "\n%s\n", a->name);
    text_matched_line(rd, 14);
    (void)fprintf(out, "  %-14s%llu\n", "members", (unsigned long long)s->members);

    /* --- sizes ------------------------------------------------------------ */
    (void)fprintf(out, "  %-14s%llu bytes (%s)\n", "extracted",
                  (unsigned long long)s->extracted,
                  tmd_human_size(s->extracted, human, sizeof(human)));
    (void)fprintf(out, "  %-14s%llu bytes (%s)\n", "stored",
                  (unsigned long long)s->stored,
                  tmd_human_size(s->stored, human, sizeof(human)));
    if (s->stored > 0)
    {
        /* Integer arithmetic to a tenth of a percent: this is a report, and
         * pulling in floating point for one line of it is not worth it. */
        uint64_t tenths = (s->padding * 1000u) / s->stored;

        (void)fprintf(out, "  %-14s%llu bytes (%llu.%llu%% of the archive)\n",
                      "padding", (unsigned long long)s->padding,
                      (unsigned long long)(tenths / 10),
                      (unsigned long long)(tenths % 10));
    }

    for (i = 0; i < s->ntop; i++)
    {
        (void)fprintf(out, "  %-14s%10llu  %s\n", i == 0 ? "largest" : "",
                      (unsigned long long)s->top[i].size, s->top[i].path);
    }

    /* --- timestamps ------------------------------------------------------- */
    if (!s->have_time)
    {
        (void)fprintf(out, "  %-14sno member carries one\n", "timestamps");
        return;
    }
    if (s->distinct_overflow)
    {
        (void)fprintf(out, "  %-14smore than %d distinct\n", "timestamps",
                      STAT_TIME_LIMIT);
    } else
    {
        (void)fprintf(out, "  %-14s%zu distinct\n", "timestamps", s->ndistinct);
    }

    {
        struct tmd_time first = { s->earliest, 0, true, NULL };
        struct tmd_time last = { s->latest, 0, true, NULL };

        format_time(&first, rd->opt, stamp, sizeof(stamp));
        (void)fprintf(out, "  %-14s%s\n", "earliest", stamp);
        format_time(&last, rd->opt, stamp, sizeof(stamp));
        (void)fprintf(out, "  %-14s%s\n", "latest", stamp);
    }
    span_string(s->latest - s->earliest, span, sizeof(span));
    (void)fprintf(out, "  %-14s%s\n", "span", span);

    {
        const struct stat_time *modal = stat_modal(s);

        if (modal && modal->count > 1)
        {
            struct tmd_time mt = { modal->sec, 0, true, NULL };

            format_time(&mt, rd->opt, stamp, sizeof(stamp));
            (void)fprintf(out, "  %-14s%s (%llu of %llu members)\n",
                          "most common", stamp,
                          (unsigned long long)modal->count,
                          (unsigned long long)s->members);
        }
    }

    /* The ones that are invisible one-member-per-line. */
    if (s->no_time)
    {
        (void)fprintf(out, "  %-14s%llu member%s carry no mtime at all\n",
                      "missing", (unsigned long long)s->no_time,
                      s->no_time == 1 ? "" : "s");
    }
    if (s->future_time)
    {
        (void)fprintf(out, "  %-14s%llu member%s dated in the FUTURE\n",
                      "future", (unsigned long long)s->future_time,
                      s->future_time == 1 ? "" : "s");
    }
    if (s->pre_tar_time)
    {
        (void)fprintf(out, "  %-14s%llu member%s dated before tar existed "
                           "(earlier than 1979)\n", "implausible",
                      (unsigned long long)s->pre_tar_time,
                      s->pre_tar_time == 1 ? "" : "s");
    }
    if (s->negative_time)
    {
        (void)fprintf(out, "  %-14s%llu member%s dated before 1970\n",
                      "negative", (unsigned long long)s->negative_time,
                      s->negative_time == 1 ? "" : "s");
    }

    verdict = stat_verdict(s);
    if (verdict)
    {
        (void)fprintf(out, "  %-14s%s\n", "produced by", verdict);
    }
}

static void json_stat(const struct tmd_render *rd, struct tmd_buf *b)
{
    const struct stats     *s = rd->stat;
    const struct stat_time *modal = stat_modal(s);
    size_t                  i;

    tmd_buf_addf(b, ", \"stat\": {\"members\": %llu",
                 (unsigned long long)s->members);
    tmd_buf_addf(b, ", \"extracted\": %llu", (unsigned long long)s->extracted);
    tmd_buf_addf(b, ", \"stored\": %llu", (unsigned long long)s->stored);
    tmd_buf_addf(b, ", \"padding\": %llu", (unsigned long long)s->padding);

    tmd_buf_addstr(b, ", \"largest\": [");
    for (i = 0; i < s->ntop; i++)
    {
        tmd_buf_addf(b, "%s{\"size\": %llu, \"path\": ", i ? ", " : "",
                     (unsigned long long)s->top[i].size);
        tmd_json_escape(b, s->top[i].path);
        tmd_buf_addc(b, '}');
    }
    tmd_buf_addc(b, ']');

    tmd_buf_addstr(b, ", \"timestamps\": {");
    tmd_buf_addf(b, "\"distinct\": %zu", s->ndistinct);
    tmd_buf_addf(b, ", \"distinct_capped\": %s",
                 s->distinct_overflow ? "true" : "false");
    if (s->have_time)
    {
        struct tmd_time first = { s->earliest, 0, true, NULL };
        struct tmd_time last = { s->latest, 0, true, NULL };
        char            stamp[64];

        format_time_iso(&first, stamp, sizeof(stamp));
        tmd_buf_addf(b, ", \"earliest\": \"%s\"", stamp);
        tmd_buf_addf(b, ", \"earliest_epoch\": %lld", (long long)s->earliest);
        format_time_iso(&last, stamp, sizeof(stamp));
        tmd_buf_addf(b, ", \"latest\": \"%s\"", stamp);
        tmd_buf_addf(b, ", \"latest_epoch\": %lld", (long long)s->latest);
        tmd_buf_addf(b, ", \"span_seconds\": %lld",
                     (long long)(s->latest - s->earliest));
    }
    if (modal)
    {
        tmd_buf_addf(b, ", \"most_common_epoch\": %lld, \"most_common_count\": %llu",
                     (long long)modal->sec, (unsigned long long)modal->count);
    }
    tmd_buf_addf(b, ", \"missing\": %llu", (unsigned long long)s->no_time);
    tmd_buf_addf(b, ", \"future\": %llu", (unsigned long long)s->future_time);
    tmd_buf_addf(b, ", \"before_tar_existed\": %llu",
                 (unsigned long long)s->pre_tar_time);
    tmd_buf_addf(b, ", \"before_1970\": %llu",
                 (unsigned long long)s->negative_time);
    tmd_buf_addc(b, '}');

    {
        const char *verdict = stat_verdict(s);

        if (verdict)
        {
            tmd_buf_addstr(b, ", \"produced_by\": ");
            tmd_json_escape(b, verdict);
        }
    }
    tmd_buf_addc(b, '}');
}

void tmd_render_archive_begin(struct tmd_render *rd, const struct tmd_archive *a)
{
    /* Per archive; matched_total is not reset, because "nothing matched" has
     * to mean nothing in any of the archives named on the command line. */
    rd->matched = 0;
    rd->seen = 0;

    if (rd->opt->stats)
    {
        stat_free(rd->stat);
        rd->stat = tmd_xcalloc(1, sizeof(*rd->stat));
    }

    struct tmd_buf b;

    switch (rd->opt->output)
    {
    case TMD_OUT_JSON:
        if (rd->archive_index > 0)
        {
            (void)fputs(",\n", rd->out);
        }
        tmd_buf_init(&b);
        /*
         * The schema number, first, so a consumer can decide whether it
         * understands this document before reading it.
         *
         * 1 was the output up to and including v1.1.0.x. 2 adds the fields
         * that make the JSON a description of the bytes rather than a tidier
         * listing, and changes "warnings" from an array of strings to an array
         * of {code, text} -- the one shape change, and the reason this is a new
         * number rather than a silent addition. New keys will keep arriving
         * within a schema; a consumer that selects the keys it wants is
         * unaffected by those, which is what the number is for.
         */
        tmd_buf_addstr(&b, "{\n  \"schema\": 2,\n  \"archive\": ");
        tmd_json_escape(&b, a->name);
        tmd_buf_addstr(&b, ",\n  \"entries\": [\n");
        (void)fputs(b.data, rd->out);
        tmd_buf_free(&b);
        rd->entry_written = false;
        break;
    case TMD_OUT_CSV:
        if (!rd->csv_header_written)
        {
            (void)fputs("path,kind,mode_string,mode,format,uid,gid,uname,gname,"
                        "size,stored_size,offset,mtime,mtime_epoch,linkpath,"
                        "checksum\n",
                        rd->out);
            rd->csv_header_written = true;
        }
        break;
    case TMD_OUT_TEXT:
        /* A banner only when there is more than one archive to tell apart.
         * With one, the listing is the whole output and a header would just be
         * something a pipeline has to strip. */
        if (rd->archive_count > 1 && !rd->opt->summary_only && !rd->opt->info)
        {
            (void)fprintf(rd->out, "%s==> %s <==\n", rd->archive_index ? "\n" : "",
                          a->name);
        }
        break;
    }
    rd->archive_index++;
}

/* True when no -m was given, or when one of the patterns matches. */
static bool entry_selected(const struct tmd_render *rd, const struct tmd_entry *e)
{
    size_t i;

    if (rd->opt->nmatch == 0)
    {
        return true;
    }
    for (i = 0; i < rd->opt->nmatch; i++)
    {
        if (tmd_path_matches(e->path, rd->opt->match[i]))
        {
            return true;
        }
    }
    return false;
}

static void render_one(struct tmd_render *rd, const struct tmd_entry *e);

static void hold_entry(struct tmd_render *rd, const struct tmd_entry *e)
{
    if (rd->nheld == rd->held_cap)
    {
        rd->held_cap = rd->held_cap ? rd->held_cap * 2 : 128;
        /*
         * `held` is an array OF POINTERS, so the element size is the size of a
         * pointer and sizeof(*rd->held) is exactly right.
         *
         * Suppressed rather than rewritten because there is no other way to
         * spell it: the check objects to taking sizeof of any pointer-to-struct
         * type, which is what an array of pointers needs by definition. It fires
         * only on clang-tidy 18 (Ubuntu 24.04, which CI runs); 19 and later
         * refined the check and say nothing, so this is invisible locally.
         */
        /* NOLINTNEXTLINE(bugprone-sizeof-expression) */
        rd->held = tmd_xrealloc(rd->held, rd->held_cap * sizeof(*rd->held));
    }
    rd->held[rd->nheld++] = tmd_entry_clone(e);

    if (rd->nheld == TMD_SORT_LOUD_AT && !rd->warned_large && !rd->opt->quiet)
    {
        (void)fprintf(stderr,
                      "tmd: --sort is holding %d members in memory; "
                      "a listing cannot be ordered until it has been read "
                      "in full\n", TMD_SORT_LOUD_AT);
        rd->warned_large = true;
    }
}

void tmd_render_entry(struct tmd_render *rd, const struct tmd_entry *e)
{
    rd->seen++;
    if (!entry_selected(rd, e))
    {
        return;
    }
    rd->matched++;
    rd->matched_total++;

    /*
     * Before the mode checks below, so --stat measures the members it was asked
     * about. That means -m narrows what --stat describes, which is the opposite
     * of what -m does to the summary -- deliberately. A summary answers "what
     * is this file", which a filter cannot change; a distribution answers "what
     * is in this set", which is exactly what a filter selects.
     */
    if (rd->stat)
    {
        stat_note(rd->stat, e);
    }

    if (rd->opt->summary_only || rd->opt->info || rd->opt->stats)
    {
        return;
    }
    if (rd->opt->sort != TMD_SORT_NONE)
    {
        hold_entry(rd, e);
        return;
    }
    render_one(rd, e);
}

static void render_one(struct tmd_render *rd, const struct tmd_entry *e)
{
    switch (rd->opt->output)
    {
    case TMD_OUT_JSON:
        if (rd->entry_written)
        {
            (void)fputs(",\n", rd->out);
        }
        json_entry(rd, e);
        rd->entry_written = true;
        break;
    case TMD_OUT_CSV:
        csv_entry(rd, e);
        break;
    case TMD_OUT_TEXT:
        if (rd->opt->long_form || rd->opt->headers)
        {
            text_long_entry(rd, e);
        } else
        {
            char *line = tmd_render_listing_line(e, rd->opt);
            (void)fprintf(rd->out, "%s\n", line);
            free(line);
        }
        break;
    }
}

/*
 * The comparators.
 *
 * Every one falls back to the archive offset when the keys are equal, so the
 * order is total and the output is reproducible. Without that, two members of
 * the same size appear in whatever order qsort happened to leave them, which
 * makes a listing that cannot be diffed against itself.
 */
static int cmp_u64(uint64_t x, uint64_t y)
{
    if (x < y)
    {
        return -1;
    }
    if (x > y)
    {
        return 1;
    }
    return 0;
}

static int compare_held(const void *pa, const void *pb)
{
    const struct tmd_entry *a = *(const struct tmd_entry *const *)pa;
    const struct tmd_entry *b = *(const struct tmd_entry *const *)pb;
    const struct tmd_options *opt = compare_options;
    int                       r = 0;

    switch (opt->sort)
    {
    case TMD_SORT_PATH:
        r = strcmp(a->path ? a->path : "", b->path ? b->path : "");
        break;
    case TMD_SORT_SIZE:
        r = cmp_u64(a->size, b->size);
        break;
    case TMD_SORT_MTIME:
        /* A member with no mtime at all sorts before every member that has
         * one, rather than being treated as the epoch -- "absent" and "1970"
         * are different findings about an archive. */
        if (a->mtime.present != b->mtime.present)
        {
            r = a->mtime.present ? 1 : -1;
        } else if (a->mtime.sec != b->mtime.sec)
        {
            r = a->mtime.sec < b->mtime.sec ? -1 : 1;
        } else
        {
            r = cmp_u64(a->mtime.nsec, b->mtime.nsec);
        }
        break;
    case TMD_SORT_OFFSET:
    case TMD_SORT_NONE:
        break;
    }
    if (r == 0)
    {
        r = cmp_u64(a->offset, b->offset);
        /*
         * Under --sort=offset the offset IS the key, so --reverse applies to
         * it. Under every other key it is only the tiebreak, and is NOT
         * inverted: two members of the same size should stay in the archive's
         * own order whichever direction the sizes run.
         */
        if (opt->reverse && opt->sort == TMD_SORT_OFFSET)
        {
            return -r;
        }
        return r;
    }
    return opt->reverse ? -r : r;
}

static void flush_held(struct tmd_render *rd)
{
    size_t i;

    if (rd->nheld > 0)
    {
        compare_options = rd->opt;
        /* The element is a pointer; see hold_entry above. */
        /* NOLINTNEXTLINE(bugprone-sizeof-expression) */
        qsort(rd->held, rd->nheld, sizeof(*rd->held), compare_held);
        compare_options = NULL;
    }
    for (i = 0; i < rd->nheld; i++)
    {
        render_one(rd, rd->held[i]);
        tmd_entry_free(rd->held[i]);
    }
    free(rd->held);
    rd->held = NULL;
    rd->nheld = 0;
    rd->held_cap = 0;
}

void tmd_render_archive_end(struct tmd_render *rd, const struct tmd_archive *a)
{
    /* The held listing goes out before the summary, exactly where the
     * streamed lines would have been. */
    flush_held(rd);

    switch (rd->opt->output)
    {
    case TMD_OUT_JSON:
        (void)fputs(rd->entry_written ? "\n  ],\n" : "  ],\n", rd->out);
        json_summary(rd, a);
        (void)fputs("\n}", rd->out);
        break;
    case TMD_OUT_CSV:
        break;
    case TMD_OUT_TEXT:
        if (rd->opt->stats)
        {
            text_stat(rd, a);
        }
        if (rd->opt->info)
        {
            text_info(rd, a);
        } else if (rd->opt->summary_only || rd->opt->with_summary)
        {
            text_summary(rd, a);
        }
        break;
    }
}

void tmd_render_finish(struct tmd_render *rd)
{
    if (rd->opt->output == TMD_OUT_JSON)
    {
        if (rd->archive_count > 1)
        {
            (void)fputs("\n]\n", rd->out);
        } else
        {
            (void)fputc('\n', rd->out);
        }
    }
}

uint64_t tmd_render_matched(const struct tmd_render *rd)
{
    return rd->matched_total;
}
