/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
/*
 * tmd — tar metadata dump
 *
 * The types every module shares: one archive member, the archive-level facts
 * that only become known by reading the whole stream, and the option block
 * that says how to render them.
 */
#ifndef TMD_H
#define TMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Set by the build (-DTMD_VERSION="..."), from the VERSION file. */
#ifndef TMD_VERSION
#define TMD_VERSION "0.0.0.0-dev"
#endif

/*
 * The copyright line the program prints.
 *
 * One definition, used by --help and --version. scripts/lint.sh audits every
 * source file for the same name and SPDX identifier, so the notice in the
 * comment headers, the one in the binary and the one in debian/copyright
 * cannot drift apart without something failing.
 */
#define TMD_COPYRIGHT "Copyright (c) 2026 Bryan C. Everly"

#define TMD_BLOCK_SIZE 512

/*
 * Mark a function as taking a printf-style format.
 *
 * This turns a promise into a check. Every wrapper in this program that takes
 * a format and forwards it to v*printf carries it, so the compiler verifies
 * the format against the arguments at each of the ~200 call sites — a class of
 * bug that otherwise surfaces as garbage output or a crash on one rare error
 * path. It also satisfies -Wformat-nonliteral, which is what proves no format
 * string here can be attacker-controlled: GCC understands that a function so
 * marked is forwarding its own checked parameter, and flags any that is not.
 *
 * `fmt` is the 1-based index of the format parameter; `first` is the index of
 * the first variadic argument, or 0 for a va_list function.
 */
#if defined(__GNUC__) || defined(__clang__)
/* Flawfinder matches the word "printf" lexically, including here, where it
 * names the attribute rather than calling anything. */
#define TMD_PRINTF(fmt, first) \
    __attribute__((format(printf, fmt, first))) /* Flawfinder: ignore */
#else
#define TMD_PRINTF(fmt, first)
#endif

/*
 * The formats a header can be written in.
 *
 * These are ordered by how much they can express, and tmd_format_max() relies
 * on that: an archive that contains one pax header and a thousand plain ustar
 * ones is a pax archive, because a reader that does not understand pax will
 * get the thousand right and the one wrong.
 */
enum tmd_format {
    TMD_FMT_UNKNOWN = 0,
    TMD_FMT_V7,      /* pre-POSIX: no magic, no uname/gname, no prefix   */
    TMD_FMT_USTAR,   /* POSIX.1-1988: magic "ustar\0", version "00"      */
    TMD_FMT_STAR,    /* Jörg Schilling's star: ustar plus a 'tar\0' tail */
    TMD_FMT_GNU,     /* GNU tar: magic "ustar  \0", L/K/S/D/M typeflags  */
    TMD_FMT_PAX      /* POSIX.1-2001: ustar plus 'x'/'g' attribute blobs */
};

/* What the typeflag resolves to once the format's extensions are applied. */
enum tmd_kind {
    TMD_KIND_UNKNOWN = 0,
    TMD_KIND_FILE,
    TMD_KIND_HARDLINK,
    TMD_KIND_SYMLINK,
    TMD_KIND_CHARDEV,
    TMD_KIND_BLOCKDEV,
    TMD_KIND_DIR,
    TMD_KIND_FIFO,
    TMD_KIND_CONTIGUOUS,
    TMD_KIND_VOLUME,     /* GNU volume label ('V')                   */
    TMD_KIND_MULTIVOL,   /* continuation of a file split across media */
    TMD_KIND_DUMPDIR,    /* GNU incremental directory listing ('D')  */
    TMD_KIND_XATTR       /* Solaris/star extended attribute member   */
};

/*
 * A timestamp as the archive stores it.
 *
 * `present` is not the same as "zero": a v7 header has no atime field at all,
 * and reporting 1970-01-01 for it would be inventing data. Sub-second
 * precision only ever arrives from a pax attribute.
 */
struct tmd_time {
    int64_t  sec;
    uint32_t nsec;
    bool     present;
};

/* One run of data in a sparse file: `numbytes` of payload at `offset`. */
struct tmd_sparse {
    uint64_t offset;
    uint64_t numbytes;
};

/* One pax extended-header attribute, kept in the order the archive wrote it. */
struct tmd_kv {
    char *key;
    char *value;
};

/*
 * The raw header fields, exactly as the 512-byte block spelled them.
 *
 * --headers prints these rather than the parsed values, because the whole
 * point of that mode is to see what is actually on disk: a mode field of
 * "0000644\0" and one of "644    " parse identically and are not the same
 * bytes, and which of the two a writer produced is often the only evidence of
 * what wrote the archive.
 */
struct tmd_raw {
    char name[101];
    char mode[9];
    char uid[9];
    char gid[9];
    char size[13];
    char mtime[13];
    char chksum[9];
    char typeflag;
    char linkname[101];
    char magic[7];
    char version[3];
    char uname[33];
    char gname[33];
    char devmajor[9];
    char devminor[9];
    char prefix[156];
};

/*
 * One archive member, fully resolved.
 *
 * "Resolved" means the caller never has to know that a 300-byte path arrived
 * as a GNU 'L' block or a pax `path` attribute: by the time an entry is handed
 * out, `path` is the real path and `path_source` records where it came from.
 */
struct tmd_entry {
    char *path;
    char *linkpath;

    /* The size the member's data would occupy once extracted. For a sparse
     * member this is the expanded size, which is much larger than the archive
     * spends on it — see `stored_size`. */
    uint64_t size;
    /* What the member costs inside the archive: the header, its extension
     * blocks, and the payload rounded up to a block boundary. */
    uint64_t stored_size;
    /* Payload bytes as the size field gives them, before sparse expansion. */
    uint64_t data_size;

    uint32_t mode;
    int64_t  uid;
    int64_t  gid;
    char    *uname;
    char    *gname;

    struct tmd_time mtime;
    struct tmd_time atime;
    struct tmd_time ctime;

    bool     has_dev;
    uint32_t devmajor;
    uint32_t devminor;

    char             typeflag;
    enum tmd_kind    kind;
    enum tmd_format  format;

    /* Byte offset of this member's first header block within the archive. */
    uint64_t offset;

    /* Header checksum: what the field said, and what the bytes add up to.
     * Both the signed and the unsigned sum are kept because historic tars
     * disagreed about whether the header bytes were signed, and an archive
     * written by one and checked by the other reports a false mismatch. */
    uint32_t chksum_stored;
    uint32_t chksum_unsigned;
    int32_t  chksum_signed;
    bool     chksum_ok;

    bool                is_sparse;
    uint64_t            realsize;   /* expanded size of a sparse member */
    struct tmd_sparse  *sparse;
    size_t              nsparse;
    bool                sparse_truncated; /* map continued past what we read */

    /* pax attributes that applied to this member, local ones last so a local
     * key visibly overrides the global of the same name. */
    struct tmd_kv *pax;
    size_t         npax;

    /* Where the path and linkpath actually came from, for --long. One of
     * "header", "prefix+name", "GNU 'L'", "pax path", ... */
    const char *path_source;
    const char *linkpath_source;

    /* Per-entry complaints: a bad checksum, a field that is not octal, a
     * sparse map that does not add up. Never fatal on their own. */
    char **warnings;
    size_t nwarnings;

    struct tmd_raw raw;
};

/*
 * What an archive actually uses, as opposed to what its magic field claims.
 *
 * --info is built on this. The magic bytes say which dialect a writer *meant*,
 * but what matters to somebody deciding whether an archive will survive a trip
 * through another tool is which of that dialect's extensions it actually
 * relies on: a GNU-magic archive that uses no GNU extension reads perfectly in
 * a ustar-only tool, and a ustar-magic archive with one pax header in it does
 * not. Every field here is counted while reading, never guessed.
 */
struct tmd_features {
    uint64_t gnu_longname;      /* 'L' blocks: a path too long for the header  */
    uint64_t gnu_longlink;      /* 'K' blocks: a link target too long for it   */
    uint64_t pax_headers;       /* 'x' blocks                                  */
    uint64_t pax_globals;       /* 'g' blocks                                  */
    uint64_t sparse_gnu_old;    /* typeflag 'S', map in the header             */
    uint64_t sparse_pax;        /* GNU.sparse.* attributes                     */
    uint64_t dumpdirs;          /* 'D': a GNU incremental backup               */
    uint64_t multivolume;       /* 'M': part of a split archive                */
    uint64_t volume_labels;     /* 'V'                                         */
    uint64_t xattr_members;     /* 'E'/'X': Solaris or star attribute members  */
    uint64_t unknown_typeflags;
    uint64_t prefix_used;       /* members whose path came from prefix+name    */
    uint64_t base256_fields;    /* numeric fields too large for octal          */
    uint64_t subsecond_times;   /* members with nanosecond precision           */
    uint64_t atime_present;
    uint64_t ctime_present;
    uint64_t names_present;     /* members carrying a uname or gname           */

    size_t   max_path;          /* the longest path in the archive             */
    size_t   paths_over_100;    /* would not fit a v7 header                   */
    size_t   paths_over_255;    /* would not fit a ustar prefix+name either    */
    int64_t  max_uid;
    int64_t  max_gid;

    /* The distinct pax keys seen, in first-appearance order. Bounded: an
     * archive with a hostile number of distinct keys must not be able to make
     * this grow without limit, and past a few dozen the list has stopped being
     * something a person reads anyway. */
    char   *pax_keys[64];
    size_t  npax_keys;
    bool    pax_keys_truncated;
};

/* Everything about the archive that is only knowable after reading it. */
struct tmd_archive {
    char *name;                 /* the file it was read from, or "(stdin)" */
    enum tmd_format format;     /* the most expressive format seen         */
    bool  formats[TMD_FMT_PAX + 1];

    uint64_t entries;
    /*
     * Headers whose checksum did not match, counted across the whole archive.
     *
     * Per-entry is not enough: a GNU long-name block and a pax extended header
     * are headers with checksums of their own, and they belong to the member
     * that follows rather than being members themselves. Counting here is what
     * lets --check notice damage to one of those, which per-entry reporting
     * missed entirely — the corrupt block was an 'L' header, and the entry it
     * produced carried the *next* header's perfectly good checksum.
     */
    uint64_t bad_checksums;
    uint64_t total_size;        /* sum of extracted sizes   */
    uint64_t total_stored;      /* what the archive spends  */
    uint64_t file_size;         /* the archive's own length, 0 if unknown  */
    uint64_t counts[TMD_KIND_XATTR + 1];

    /* The writer's blocking factor, inferred from where the end-of-archive
     * marker sits. 20 blocks (10 KiB) is tar's default; bsdtar writes 10. */
    unsigned record_blocks;
    bool     eof_marker;        /* the two zero blocks were present        */
    uint64_t trailing_bytes;    /* data after the marker, usually padding  */
    bool     trailing_garbage;  /* ...and it was not all zero              */

    /* Attributes from a pax global header ('g'), which apply to every member
     * after it. Kept separately because they are a property of the archive. */
    struct tmd_kv *pax_global;
    size_t         npax_global;

    /* A guess at what wrote it, from the fingerprints only one tool leaves:
     * SCHILY.* keys, LIBARCHIVE.* keys, GNU.sparse.*, the star tail. */
    const char *writer;

    struct tmd_features features;

    char **warnings;
    size_t nwarnings;
};

enum tmd_output {
    TMD_OUT_TEXT = 0,
    TMD_OUT_JSON,
    TMD_OUT_CSV
};

/* How the caller wants the archive rendered. Filled in by opts.c. */
struct tmd_options {
    enum tmd_output output;
    bool  long_form;    /* -l: every resolved field, one block per entry   */
    bool  headers;      /* -R: the raw 512-byte header fields as well      */
    bool  summary_only; /* -s: the archive summary and nothing else        */
    bool  info;         /* -i: the full report on the archive's structure  */
    bool  with_summary; /* -S: the listing and then the summary            */
    bool  numeric;      /* -n: numeric uid/gid even when names exist       */
    bool  human;        /* -H: 1.4K rather than 1434                       */
    bool  utc;          /* -u: render times as UTC                         */
    bool  full_time;    /* -T: seconds, nanoseconds and the zone offset    */
    bool  check;        /* -c: a checksum mismatch is an exit status       */
    bool  quiet;        /* -q: do not write warnings to stderr             */
    bool  color;        /* resolved from --color and isatty()              */
};

const char *tmd_format_name(enum tmd_format f);
const char *tmd_kind_name(enum tmd_kind k);
const char *tmd_typeflag_name(char typeflag);

#endif /* TMD_H */
