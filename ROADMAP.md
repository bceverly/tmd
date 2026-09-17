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

## Exhaustive JSON output, and a `-t` spelling for it

**Wanted:** `-t` / `--output-type`, taking `TXT` or `JSON`. In JSON mode, emit
*everything* that can possibly be determined about each member.

**Where this meets what already exists.** `--format=text|json|csv` already
selects the output. So this item is really two separable pieces:

1. **The spelling.** A short `-t`, and accepting `TXT`/`JSON` case-insensitively
   alongside the current lowercase names. Cheap; the option table in
   `src/options.def` is the only place it has to be added, and the help text and
   manpage follow from it. Worth deciding whether `-t`/`--output-type` becomes
   the documented name with `--format` kept as an undocumented alias, or whether
   both are documented. Breaking `--format` is not on the table — it is in the
   manpage of a released version.

2. **The exhaustive JSON.** This is the real work, and the more interesting
   half. Today's JSON reports what the reader already resolves. "Everything it
   can possibly find out" is a larger set:

   | Already emitted | Not yet |
   |---|---|
   | path, kind, typeflag, format | the raw 512-byte header as base64, for byte-exact inspection |
   | mode, uid/gid, uname/gname | the decoded mode bits broken out (setuid, setgid, sticky, each rwx triple) |
   | size, stored_size, offset | the exact block range the member occupies, and its padding bytes |
   | mtime/atime/ctime | which field each timestamp actually came from — header, GNU tail, or a named pax key |
   | linkpath, devmajor/minor | every pax attribute *including* ones tmd does not act on, with their raw bytes |
   | sparse map, checksum | both checksum conventions side by side, and which one matched |
   | pax attributes applied | per-member warnings with a machine-readable code rather than only prose |
   | | the path's encoding verdict (valid UTF-8, or which byte offsets are not) |
   | | whether the name arrived via prefix, GNU `L`, or pax, as a field rather than a sentence |
   | | SCHILY/LIBARCHIVE xattr keys decoded from base64 where they are base64 |

   The guiding rule should be that the JSON is a faithful, lossless
   *description of the bytes*, not a prettier version of the listing: someone
   should be able to reconstruct what the header said from the JSON alone, and
   diff two archives' JSON to see exactly what changed.

**Design notes for whoever picks this up**

- `--long` and `-R` already gather most of these fields for the text renderer.
  The gap is that some are formatted directly into prose rather than kept as
  values. Pulling them into `struct tmd_entry` first would let both renderers
  use them and would shrink `render.c`.
- The output must stay streaming. A "dump everything" mode is exactly where it
  is tempting to build the whole document in memory, and a 200,000-member
  archive is exactly where that stops being acceptable.
- Raw bytes in JSON need a decided policy. Base64 for the header block is
  obvious; for *strings* the existing rule (valid UTF-8 passes through, anything
  else escapes byte-by-byte as `\u00XX`) already round-trips and should not be
  replaced with base64, which would make every ordinary path unreadable.
- Adding fields is backwards compatible for consumers that select keys; adding
  a `"schema": 2` marker at the top of each archive object would let a consumer
  refuse output it does not understand, and costs nothing now.

---

## Other ideas

Unordered, and none of them thought through as far as the item above.

- **`--verify` against a manifest.** Read a list of expected paths and sizes and
  report what the archive is missing or has gained. A backup check that does not
  need extraction.
- **Read compressed archives directly.** Today a `.tar.gz` is recognized and the
  user is told which command to pipe through. Doing it internally means linking
  zlib, which costs the "libc only" property that keeps the attack surface
  small — so if it happens it should be `dlopen`-on-demand, or a compile-time
  option that is off by default, and never a hard dependency.
- **`--diff` between two archives.** Which members were added, removed, changed
  in size, changed in mode, changed in mtime. The JSON above is the enabler.
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
