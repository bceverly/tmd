<!--
Copyright (c) 2026 Bryan C. Everly
SPDX-License-Identifier: BSD-2-Clause
-->

# Roadmap

Ideas for future versions. Nothing here is committed to a release; an item
leaves this file when it ships or when it is decided against — and one decided
against is worth recording under [Considered and declined](#considered-and-declined)
rather than silently dropping, so it is not proposed again a year later.

---

## Find a member by name: `-m` / `--match PATTERN`

**Wanted:** name a file and have tmd report every member that matches, with its
full internal path and its metadata — including when the same name appears in
the archive more than once.

**The switch.** `-s` would have been the obvious short form for `--search`, and
it is taken: `-s` is `--summary` and `-S` is `--with-summary`, both shipped and
documented, so neither can move. `-m` is free, is not confusable with `-n`
(`--numeric-owner`), and "match" says the true thing — this is a *filter* over
the listing, not a separate mode. `--name` and `--path` are deliberately left
unused in case the matching ever needs to be narrowed explicitly.

**What it matches.** fnmatch(3) globs, following find(1)'s convention, which is
the one everybody already knows:

| pattern | matches |
|---|---|
| `nginx.conf` | any member whose **basename** is exactly that, at any depth |
| `*.conf` | any member whose basename ends in `.conf` |
| `etc/nginx/*` | contains a `/`, so it matches against the **full stored path** |
| `*/logs/*` | likewise |

Case-sensitive, because a tar path is a string of bytes and two members
differing only in case are two different members. Repeatable — `-m a.conf -m
b.conf` reports members matching either.

**Every occurrence, not the first.** This is the part worth building carefully,
and the reason the feature belongs in *this* tool rather than in `tar -t | grep`.
A tar archive can legitimately contain the same path twice: `tar -r` appends,
an incremental backup re-adds a changed file, and a concatenated archive can
carry two whole copies. Extraction silently keeps the last one. tmd should show
all of them, with the byte offset of each, because "which copy wins and what was
the other one" is a question nothing else answers:

```
$ tmd -f backup.tar -m nginx.conf
-rw-r--r--  root/root    2481  2026-03-01 09:14  etc/nginx/nginx.conf   @1536
-rw-r--r--  root/root    2604  2026-09-15 11:02  etc/nginx/nginx.conf   @884736  (extracted; supersedes @1536)
```

**It is a filter, so it composes.** `-m` should narrow the member set and change
nothing else: the default listing prints matching lines, `-l` prints a full
block per match, `--format=json` emits only matching entries, `--format=csv`
only matching rows. It must keep working on a pipe, which it does for free —
matching needs the path, which the reader already has, and nothing has to be
buffered.

**Open questions for whoever picks this up**

- *Does the summary describe the archive or the matches?* `-S` with `-m` is
  ambiguous. The archive's format, blocking and integrity are properties of the
  whole file and should not change because a filter was applied; the counts
  arguably should. Suggest: the summary keeps describing the whole archive, and
  a matched run adds one line — `matched 2 of 562 members`.
- *Exit status when nothing matches.* Genuinely useful for scripting
  (`tmd -f a.tar -m secrets.env || echo absent`), but 1 already means "could not
  read the archive" and 3 means "--check found damage", and conflating "no match"
  with either would be wrong. A new **4** for "no member matched" is the honest
  answer, and it is additive — nothing that exists today can return it.
- *Should `--info` respect it?* Probably not: `-i` describes the archive, and an
  archive's generation does not depend on a filter. Likely a usage error to
  combine them, or `-m` is simply ignored there.
- *Whether to grow a regex option later.* fnmatch covers the asked-for case and
  costs nothing; POSIX `regcomp` is in libc too, so `--regex` could be added
  without a dependency if globs ever prove too blunt. Not now.

---

## Other ideas

Unordered, and none of them thought through as far as the item above.

- **Promote `LIBARCHIVE.creationtime` to a first-class `created` line.** tmd
  already prints it, because it prints every pax attribute it finds whether or
  not it acts on one — but as a raw `pax LIBARCHIVE.creationtime = 1789650968`
  rather than a rendered date beside `modified` and `accessed`.

  This is a real case, not a hypothetical, and it took reading the source to
  establish. libarchive's pax writer does emit it:

  ```c
  /* Store birth/creationtime only if it's earlier than mtime */
  if (archive_entry_birthtime_is_set(entry_main) &&
      archive_entry_birthtime(entry_main) < archive_entry_mtime(entry_main))
          add_pax_attr_time(&(pax->pax_header), "LIBARCHIVE.creationtime", ...);
  ```

  Two conditions gate it, and both are easy to miss. The birth time must be
  *earlier* than the modification time — so a file created now and back-dated
  with `touch` never gets one. And `birthtime_is_set()` must be true, which
  depends on the platform: `archive_entry_copy_stat.c` fills it only from
  `struct stat.st_birthtime`, which FreeBSD, macOS and NetBSD have and Linux
  does not. libarchive's disk reader never calls `statx(STATX_BTIME)`, so
  **bsdtar on Linux silently never writes it** while bsdtar on FreeBSD or macOS
  does.

  The practical upshot: archives produced on a Mac or a BSD may carry a genuine
  creation time that tmd currently shows as an unlabelled attribute. GNU tar
  never writes one on any platform — its whole pax keyword table is `atime
  comment charset ctime gid gname linkpath mtime path size uid uname` plus the
  `GNU.*`, `RHT.security.selinux` and `SCHILY.*` extensions, and the string
  does not appear anywhere in its source.

  Worth rendering as `created` under `-l`, counting under `-i`'s "timestamps"
  line, and emitting as its own JSON field. Cheap: the value is already parsed
  and in the pax list, so this is presentation only.

- **Reproduce the extension blocks, not just the member header.** `-R` now emits
  `raw.block_base64`, the member's own 512-byte header, byte for byte. A member
  with a GNU `L` long name or a pax `x` header occupies two or three *more*
  blocks than that, and those are described by their effects — the path, and
  `path_source` — rather than reproduced. Emitting them too would make the JSON
  a complete account of every byte a member occupies, which is what "lossless"
  ought to mean. It needs somewhere to put a variable number of blocks, and a
  decision about whether a 200-block pax header is something to emit in full.

- **`--verify` against a manifest.** Read a list of expected paths and sizes and
  report what the archive is missing or has gained. A backup check that does not
  need extraction.
- **Read compressed archives directly.** Today a `.tar.gz` is recognized and the
  user is told which command to pipe through. Doing it internally means linking
  zlib, which costs the "libc only" property that keeps the attack surface
  small — so if it happens it should be `dlopen`-on-demand, or a compile-time
  option that is off by default, and never a hard dependency.
- **`--diff` between two archives.** Which members were added, removed, changed
  in size, changed in mode, changed in mtime. The schema 2 JSON is the
  enabler: it already describes each member field by field, so a diff is a
  comparison of two documents rather than a second parser.
- **Report the *order* members appear in.** A tar written by `find | tar -T -`
  has a different ordering fingerprint from one written by `tar -c dir`, which
  is occasionally the only clue about how an archive was made.
- **Detect tar bombs.** Count members that would extract outside the current
  directory (`../`, absolute paths, symlinks pointing outside the tree) and
  report them as a class. `tmd` never extracts, so this is pure reporting — but
  it is the question somebody pointing this tool at an untrusted archive most
  wants answered.
- **`--sort` for the listing.** By size, by mtime, by path. Requires buffering,
  so it should be opt-in and should say so when the archive is large.
- **More architectures in CI.** The code is endian-clean by construction (every
  field is parsed byte by byte) but nothing proves it. A `qemu`-based
  big-endian leg would.
- **A `--stat` mode.** Distribution of member sizes, the largest members, how
  much of the archive is padding. Useful for "why is this archive 40 GB".

  **Timestamp distribution belongs here too**, and may be the more useful half.
  How many *distinct* mtimes an archive contains, and the span between the
  earliest and the latest, says how the archive was produced — a question that
  comes up the first time somebody looks at a release tarball and wonders why
  every file appears to have been modified at the same instant:

  | distinct mtimes | what produced it |
  |---|---|
  | 1 | normalized, almost certainly a reproducible build clamping to `SOURCE_DATE_EPOCH` |
  | a handful, minutes apart | exported from version control into a fresh directory, then a few files regenerated by the release script |
  | hundreds, spread over months or years | real per-file mtimes, preserved from a working tree |

  nginx 1.31.6 is the second kind: 558 of its 562 members share one identical
  second, and the four that differ — `CHANGES`, `CHANGES.ru` and two
  directories — are two minutes later, which is the release script rebuilding
  them after the export.

  Today that takes a pipeline:

  ```bash
  tmd -f archive.tar --format=csv | tail -n +2 | awk -F, '{print $13}' \
    | sort -u | wc -l
  ```

  It should be a line of `--stat`, alongside the earliest and latest mtime, the
  modal timestamp and how many members share it. Cheap to compute — the reader
  already has every mtime — but it needs somewhere to accumulate, so it is the
  one statistic that cannot be streamed straight out. A count of distinct values
  needs a set; bounding that set (say 4096 entries, then "more than 4096
  distinct") keeps it honest on a hostile archive.

  Related: report whether any mtime is in the **future**, or before the first
  tar existed. Both are signs of a clock problem or a crafted header, and both
  are invisible in a listing that shows one member per line.

---

## Considered and declined

*(nothing yet)*
