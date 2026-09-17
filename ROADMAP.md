<!--
Copyright (c) 2026 Bryan C. Everly
SPDX-License-Identifier: BSD-2-Clause
-->

# Roadmap

Ideas for future versions. Nothing here is committed to a release.

An item does not leave this file when it is resolved; it moves. Shipped work
goes to [Shipped](#shipped) with the version it went out in, and an idea decided
against goes to [Considered and declined](#considered-and-declined) with the
reason. A file that lists only what is left cannot answer "was this ever
considered?" or "when did that land?", which are the two questions actually
asked of a roadmap a year later.

## Status

| Item | Status |
|---|---|
| [Find a member by name: `-m` / `--match`](#find-a-member-by-name--m----match-pattern-v1300) | **Shipped** in v1.3.0.0 |
| [`--sort` for the listing](#--sort-for-the-listing-v1300) | **Shipped** in v1.3.0.0 |
| [Exhaustive JSON, and a `-t` spelling for it](#exhaustive-json-output-and-a--t-spelling-for-it-v1200) | **Shipped** in v1.2.0.0 |
| [Promote `LIBARCHIVE.creationtime` to a `created` line](#other-ideas) | Not started |
| [Reproduce the extension blocks, not just the member header](#other-ideas) | Not started — follow-up to the JSON work |
| [`--verify` against a manifest](#other-ideas) | Not started |
| [Read compressed archives directly](#other-ideas) | Not started |
| [`--diff` between two archives](#other-ideas) | Not started |
| [Report the *order* members appear in](#other-ideas) | Not started |
| [Detect tar bombs](#other-ideas) | Not started |
| [More architectures in CI](#other-ideas) | Not started |
| [A `--stat` mode](#other-ideas) | Not started |

Nine open, three shipped, none declined.

`-m` and `--sort` go out in v1.3.0.0. The minor moves because the command line
grew — the same rule v1.2.0.0 followed when `-t` was added.

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

## Shipped

### Find a member by name: `-m` / `--match PATTERN` (v1.3.0.0)

**What shipped**, as designed: fnmatch(3) globs following find(1)'s rule (a
pattern with a `/` matches the whole stored path, one without it the basename),
case-sensitive, repeatable, and reporting **every** occurrence rather than the
first — which is the reason the feature belongs here rather than in
`tar -t | grep`.

Each matched line carries `@offset`. That is the one deliberate exception to
"`-m` narrows the member set and changes nothing else": two lines for the same
path differing only in size and date tell you there are two copies, and the
offset tells you which is which. `-l` and the machine formats already had it.

The open questions, and how each was answered:

- *Does the summary describe the archive or the matches?* The archive, as
  suggested. Format, blocking and integrity are properties of the file and do
  not change because a pattern was supplied. One line is added:
  `matched 2 of 562 members`, in both the summary and the `-i` report.
- *Exit status when nothing matched?* **4**, as suggested, and only when nothing
  worse happened — an unreadable archive still exits 1, because reporting "no
  match" for a file that could not be read would be a lie.
- *Should `--info` respect it?* The roadmap floated a usage error. It is not one.
  `-i` describes the whole archive and still does; `-m` adds the matched line
  and nothing else. Erroring would have been a rule with nothing behind it,
  given `-s` has exactly the same shape and is fine.
- *A regex option later?* Still not now.

### `--sort` for the listing (v1.3.0.0)

**What shipped:** `--sort=KEY` over `path`, `size`, `mtime` or `offset`, with
`--reverse`. `name` and `time` are accepted as aliases for the first two,
because they are what a person types first.

The roadmap said it "requires buffering, so it should be opt-in and should say
so when the archive is large". All three are true of what shipped: it is opt-in,
it holds a clone of every entry that will be printed (the reader recycles one
entry, so nothing can be kept without copying it), and past 100,000 held members
it says so on stderr — a warning rather than a cap, because truncating a listing
because it got big would be worse than the memory.

Two decisions the sketch did not cover:

- **Ties keep the archive's own order.** Every comparison falls back to the byte
  offset, so the order is total and the output reproducible. Without it two
  members of the same size come out in whatever order qsort happened to leave
  them, and a listing cannot be diffed against itself.
- **`--reverse` inverts the key, not the fallback.** Under `--sort=size
  --reverse` the largest comes first, but two equal sizes stay in archive order.
  The exception is `--sort=offset`, where the offset *is* the key and so is
  reversed.

### Exhaustive JSON output, and a `-t` spelling for it (v1.2.0.0)

**What was wanted:** `-t` / `--output-type` taking `TXT` or `JSON`, and a JSON
mode that emits everything that can be determined about a member rather than a
tidier version of the listing.

**What shipped.** `-t` / `--output-type` accepts `TXT`, `JSON` and `CSV` in
either case; `--format` continues to work under its old name, because it is in
the manpage of a released version. Both are documented, with `-t` as the primary
spelling.

The JSON carries `"schema": 2` as its first field and adds, per member:

| Field | What it answers |
|---|---|
| `mode_bits` | setuid, setgid, sticky and each rwx triple, broken out |
| `blocks` | the exact block range the member occupies, and its padding |
| `checksum.computed_signed` / `.matched` | both historic conventions, and which one the archive agreed with |
| `mtime_source`, `atime_source`, `ctime_source` | header, GNU tail, or the named pax key that overrode it |
| `path_source`, `linkpath_source` | header, `prefix`, GNU `L`, or pax |
| `path_encoding`, `linkpath_encoding` | valid UTF-8, and the offset of the first byte that is not |
| `xattrs` | `SCHILY.xattr.*` / `LIBARCHIVE.xattr.*` decoded from base64, beside the verbatim record |
| `warnings[].code` | a stable name such as `checksum-mismatch`, instead of prose a consumer would have to pattern-match |
| `raw.block_base64`, `raw.block_offset` | under `-R`, the member's own 512-byte header, byte for byte |

**What the design notes asked for, and what happened to each.**

- *Stay streaming.* It does. Each member is still built in its own buffer and
  written as it is read; nothing accumulates across members.
- *Base64 for the raw block, `\u00XX` for strings.* Both as specified. The
  string rule was already right and was left alone; `path_encoding` now reports
  which of the two happened so a consumer never has to guess.
- *A `"schema"` marker.* Added, at 2. Schema 1 was the output through v1.1.0.x.
  The number moved because one existing shape changed — `warnings` went from an
  array of strings to an array of `{code, text}`. New keys will keep arriving
  within a schema without moving it.
- *Pull fields into `struct tmd_entry` so both renderers can use them.* Partly.
  `chksum_signed` and `path_source` were already there and simply unemitted;
  `tmd_time` gained a `source`, `tmd_raw` gained the block, and the warning
  became a `{code, text}` pair. `render.c` did not shrink — it grew by about a
  hundred lines, all of it new output rather than duplicated formatting.

**What did not ship.** The base64 block covers the member's own header only. A
member with a GNU `L` name or a pax `x` header occupies two or three more blocks,
and those are still described by their effects rather than reproduced — see
*Reproduce the extension blocks* under [Other ideas](#other-ideas).

**Worth knowing.** The round-trip is enforced end to end: a test decodes every
`raw.block_base64` and compares it against the archive file at
`raw.block_offset`. That test is what caught the one real design mistake in the
work — `blocks.header_offset` is the member's *first* block, which for a
long-name member is the `L` header and not the block `raw` describes, so
`raw.block_offset` exists to say where that block actually is.

---

## Considered and declined

*(nothing yet)*
