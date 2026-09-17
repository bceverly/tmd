<div align="center">

# tmd

**Dump the metadata out of a tar archive — BSD, GNU or v7 — without extracting
anything.**

[![CI](https://github.com/bceverly/tmd/actions/workflows/ci.yml/badge.svg)](https://github.com/bceverly/tmd/actions/workflows/ci.yml)
[![Security](https://github.com/bceverly/tmd/actions/workflows/security.yml/badge.svg)](https://github.com/bceverly/tmd/actions/workflows/security.yml)
[![Coverage](docs/badges/coverage.svg)](#testing)

[![License: BSD 2-Clause](https://img.shields.io/badge/license-BSD--2--Clause-1B4B8F.svg)](LICENSE)
[![Language: C11](https://img.shields.io/badge/language-C11-0A2240.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Links: libc only](https://img.shields.io/badge/links-libc%20only-1e7a46.svg)](#what-it-links-and-what-it-runs)
[![Ubuntu: 22.04 · 24.04 · 26.04](https://img.shields.io/badge/ubuntu-22.04%20%C2%B7%2024.04%20%C2%B7%2026.04-E95420.svg)](#installing)

[![cppcheck](https://img.shields.io/badge/cppcheck-clean-1e7a46.svg)](#security)
[![clang-tidy](https://img.shields.io/badge/clang--tidy-clean-1e7a46.svg)](#security)
[![gcc -fanalyzer](https://img.shields.io/badge/gcc%20--fanalyzer-clean-1e7a46.svg)](#security)
[![flawfinder](https://img.shields.io/badge/flawfinder-clean-1e7a46.svg)](#security)
[![semgrep](https://img.shields.io/badge/semgrep-clean-1e7a46.svg)](#security)
[![CodeQL](https://img.shields.io/badge/codeql-security--extended-1e7a46.svg)](#security)

[![ASan · UBSan · LSan](https://img.shields.io/badge/asan%20%C2%B7%20ubsan%20%C2%B7%20lsan-clean-1e7a46.svg)](#testing)
[![valgrind](https://img.shields.io/badge/valgrind-clean-1e7a46.svg)](#testing)
[![Fuzzed](https://img.shields.io/badge/fuzzed-libFuzzer%20%2B%20in--process-1e7a46.svg)](#testing)
[![Hardened](https://img.shields.io/badge/hardened-PIE%20%C2%B7%20RELRO%20%C2%B7%20CET%20%C2%B7%20fortify-1e7a46.svg)](#building)

</div>

---

`tar -tvf` tells you what is in an archive. It does not tell you what the
archive *is* — which dialect wrote it, whether the paths came from a GNU long-name
block or a pax attribute, whether the header checksums still match, whether the
end-of-archive marker is even there. `tmd` answers both questions: a listing in
the shape you already know, and a report on the file itself.

```console
$ gzip -dc attachment.tgz | tmd
-rw-r--r--  jsmith/staff           18244  2017-01-30 06:15:00Z  docs/old/draft.txt
-rw-r--r--  jsmith/staff            4096  2019-04-12 14:23:07Z  docs/proposal.doc
-rw-r--r--  jsmith/staff           92160  2021-11-03 09:01:44Z  docs/budget.xls
-rw-r--r--  jsmith/staff            1102  2024-07-19 22:58:12Z  docs/notes.txt
drwxr-xr-x  jsmith/staff               0  2026-09-15 11:40:02Z  docs/
lrwxrwxrwx  jsmith/staff               0  2024-07-19 22:58:12Z  docs/latest -> notes.txt
```

## Contents

- [What it does](#what-it-does)
- [Quick start](#quick-start)
- [Usage](#usage)
- [Output modes](#output-modes)
- [What it understands](#what-it-understands)
- [Building](#building)
- [Testing](#testing)
- [Security](#security)
- [Make targets](#make-targets)
- [Installing](#installing)
- [Releasing](#releasing)
- [GitHub configuration](#github-configuration)
- [Project layout](#project-layout)
- [Design notes](#design-notes)
- [Roadmap](ROADMAP.md)

---

## What it does

- **A listing in `ls -l` shape.** Permission bits, owner and group, size, the
  modification time the archive recorded, and the path. The same columns
  `tar -tvf` produces, and the end-to-end tests check member by member that the
  two agree on every format both can read.
- **Every header field, when you want it.** `-l` prints the whole resolved
  member — format, typeflag, both owner names and numbers, the size in the
  archive as well as the extracted size, the byte offset of the header, the
  checksum, the pax attributes that applied, the sparse map. `-R` adds the raw
  512-byte fields exactly as they are spelled on disk.
- **A report on the archive itself.** `-i` says which generation of the tar
  format it belongs to, which extensions it actually relies on, what wrote it,
  how it is blocked, whether it is intact, and what it would take to read it
  somewhere else.
- **JSON and CSV**, written as the archive is read rather than built in memory,
  so a 200,000-member archive costs no more than a small one.
- **An integrity check.** `-c` verifies every header checksum and the
  end-of-archive marker, and exits 3 if anything is wrong.
- **It never writes.** The archive is opened read-only, no member is extracted,
  and only headers are read — member data is seeked over, so the cost is
  proportional to how many members an archive has rather than to how large it is.

## Quick start

```bash
git clone https://github.com/bceverly/tmd.git
cd tmd
make build          # compiles into ./bin/tmd and regenerates the manpage
./bin/tmd -f some.tar
```

That needs a C11 compiler and GNU make and nothing else. For the test suite,
the analyzers and the packaging tools:

```bash
make install-dev    # prompts for sudo; installs valgrind, cppcheck, clang-tidy,
                    # shellcheck, gcovr, flawfinder, gitleaks, semgrep,
                    # debhelper, devscripts, dput, lintian and the git hooks
make test           # unit + end-to-end tests, sanitizers, 80% coverage gate
```

## Usage

```
Usage: tmd -f FILE [OPTION]...
       tmd [OPTION]... < FILE      (or: ... | tmd)

Reads a tar archive and reports what is inside it without extracting
anything: one ls -l style line per member by default, or every header
field in long, JSON or CSV form. BSD archives (ustar and pax, as bsdtar
and libarchive write them), GNU tar archives including long names and
sparse files, and pre-POSIX v7 archives are all understood. The archive
is only ever opened for reading.

Options:
  -f, --file=FILE          archive to read; repeatable. Without it, standard input is read
  -o, --output=FILE        write the report to FILE instead of standard output
  -l, --long               one block per member with every field, rather than one line
  -R, --raw-headers        also show the raw 512-byte header fields (implies --long)
  -i, --info               report what the archive IS: format generation, extensions, integrity
  -s, --summary            print only the archive summary, no member listing
  -S, --with-summary       print the listing and then the summary
  -n, --numeric-owner      show numeric uid/gid instead of the names stored in the archive
  -H, --human              print sizes as 1.4K and 23M rather than in bytes
  -L, --local              render timestamps in this machine's zone instead of UTC
  -u, --utc                render timestamps as UTC (the default; accepted for scripts)
  -T, --full-time          include seconds, nanoseconds and the zone offset in timestamps
  -c, --check              check the archive for damage (checksums, truncation); exit 3 on any
  -q, --quiet              do not write warnings about damaged headers to standard error
  -m, --match=PATTERN      show only members matching PATTERN; repeatable. A pattern with a '/' matches the whole path, one without it the basename
      --diff               compare two archives given with -f and report what changed
      --verify=FILE        check the archive against a manifest of expected "SIZE PATH" lines
      --stat               report distributions instead of a listing: sizes, padding, and how the archive's timestamps are spread
      --sort=KEY           order the listing by path, size, mtime or offset (reads it all first)
      --reverse            reverse the --sort order
  -t, --output-type=TYPE   output type: TXT (the default), JSON or CSV; case does not matter
      --format=FMT         the same thing, spelled the way releases before 1.2 spelled it
      --color[=WHEN]       colorize the listing: auto (the default), always or never
  -h, --help               show this help and exit
  -V, --version            show the version and exit
```

Typing `tmd` with no arguments **at a terminal** prints that same help and
exits 0. With standard input redirected or piped it reads that instead, which
is what makes it work in a pipeline:

```bash
gzip -dc backup.tar.gz | tmd          # the common case
zstd -dc backup.tar.zst | tmd -i
tmd < backup.tar                      # a plain redirect
ssh host 'cat /backups/nightly.tar' | tmd -s
```

The distinction is whether stdin is a terminal, which is the same rule `cat`,
`grep` and `wc` use. Typing the name of the tool to see what it does still
shows the help; piping something in never prints help at you and exits 0
having done nothing, which would make a broken pipeline look as though it had
worked. `-f -` remains valid and means exactly the same thing, for scripts that
would rather be explicit.

**Exit status.** `0` the archive was read · `1` it could not be read · `2` the
command line was wrong · `3` `--check` found damage · `4` `--match` found no
member · `5` `--diff` or `--verify` found differences. Distinct statuses rather
than one, because the difference matters to a script: a mistyped flag is the
caller's bug, an unreadable archive is the file's problem, a bad checksum is a
*finding* — the tool worked perfectly and the archive is damaged — and the last
two are not failures at all, just answers.

**`-f` and `-o`.** `-f` may be given more than once to read several archives in
one run; with none given, standard input is read. `-o` redirects the report; warnings about damaged
headers always go to standard error, so `tmd -f x.tar -o report.txt` produces a
clean report and still tells you that three headers were damaged.

Compressed archives are not opened directly — but they are recognized, and the
message names the command that gets past the wrapper:

```console
$ tmd -f backup.tar.gz
tmd: backup.tar.gz: gzip-compressed data, not a plain tar archive
     (try: gzip -dc backup.tar.gz | tmd -f -)
```

A piped archive is read exactly as a file is, with one difference worth knowing:
a pipe cannot seek, so member data is read and discarded rather than skipped
over. The listing is identical either way — including the archive's own byte
count, which is measured while reading when there is no file to stat.

## Output modes

### The default listing

```console
$ gzip -dc attachment.tgz | tmd
-rw-r--r--  jsmith/staff           18244  2017-01-30 06:15:00Z  docs/old/draft.txt
-rw-r--r--  jsmith/staff            4096  2019-04-12 14:23:07Z  docs/proposal.doc
-rw-r--r--  jsmith/staff           92160  2021-11-03 09:01:44Z  docs/budget.xls
-rw-r--r--  jsmith/staff            1102  2024-07-19 22:58:12Z  docs/notes.txt
drwxr-xr-x  jsmith/staff               0  2026-09-15 11:40:02Z  docs/
lrwxrwxrwx  jsmith/staff               0  2024-07-19 22:58:12Z  docs/latest -> notes.txt
```

One line per member, and the dates are the ones the files carried on the disk
they came from — not the date the archive was built. Here the tarball was made
on 2026-09-15 (which is what the *directory* mtime shows, since a directory's
timestamp moves whenever something is added to it) while the files inside go
back to 2017.

Nothing but the listing goes to standard output: no banner, no summary unless
one is asked for, so it pipes into `awk` with nothing to strip. The leading
character of the mode string comes from the member's *type* rather than from
the mode word, because a tar header stores no type bits — a directory whose
type character came from the mode would print as a plain file. A device node
shows its major and minor numbers where a file shows its size, as `ls` does.

### `-m`, finding a member

```console
$ tmd -f backup.tar -m nginx.conf
-rw-r--r--  root/root    2481  2026-03-01 09:14:00Z  etc/nginx/nginx.conf   @1536
-rw-r--r--  root/root    2604  2026-09-15 11:02:31Z  etc/nginx/nginx.conf   @884736
```

`-m` / `--match` takes an fnmatch(3) glob and follows find(1)'s rule, which is
the one everybody already knows:

| pattern | matches |
|---|---|
| `nginx.conf` | any member whose **basename** is exactly that, at any depth |
| `*.conf` | any member whose basename ends in `.conf` |
| `etc/nginx/*` | contains a `/`, so it matches against the **full stored path** |
| `*/logs/*` | likewise |

Case-sensitive, because a tar path is a string of bytes and two members
differing only in case are two different members. Repeatable: `-m a.conf -m
b.conf` reports members matching either.

**Every occurrence, not the first.** That is the reason this is in tmd rather
than `tar -t | grep`. An archive can legitimately hold the same path twice —
`tar -r` appends, an incremental backup re-adds a changed file, a concatenated
archive carries two whole copies — and extraction silently keeps the last one.
The two lines above are the same path at two offsets: the second is what you
get if you extract, and the first is the one you would never otherwise know was
there.

The `@offset` is the one thing `-m` adds to a line; everything else about the
listing is unchanged. `-l` and the machine formats already carry the offset.

It composes. `-l` prints a full block per match, `-t JSON` emits only matching
entries, `-t CSV` only matching rows, and it works on a pipe, because matching
needs the path and nothing has to be buffered.

The **summary keeps describing the whole archive** — format, blocking and
integrity are properties of the file and do not change because a pattern was
supplied — with one line added:

```console
$ tmd -f backup.tar -m '*.conf' -S | grep matched
  matched       2 of 562 members
```

**Exit status 4** means the archive was read and nothing matched, which is its
own answer and not to be confused with `1` (could not read it) or `3` (`--check`
found damage):

```console
$ tmd -f backup.tar -m secrets.env || echo "not in this archive"
not in this archive
```

### `--sort`, ordering the listing

```console
$ tmd -f backup.tar --sort=size --reverse | head -3
-rw-r--r--  root/root  40140288  2026-09-15 11:02:31Z  var/lib/db/store.sqlite
-rw-r--r--  root/root   2104320  2026-09-15 10:58:02Z  var/log/app.log
-rw-r--r--  root/root    884736  2026-03-01 09:14:00Z  usr/share/data.bin
```

`--sort=KEY` takes `path`, `size`, `mtime` or `offset`, and `--reverse` inverts
it. It answers "what is actually big in here" without a pipeline.

Two things worth knowing:

- **It reads the whole archive before printing anything.** The last member of an
  archive can sort first, so nothing can be written until everything has been
  read. This is the one place tmd stops streaming, which is why it is opt-in and
  why a run holding more than 100,000 members says so on stderr.
- **Ties keep the archive's own order.** Two members of the same size come out
  in the order they appear in the file, and `--reverse` does not invert that —
  it reverses the key, not the fallback. So the output is reproducible and a
  listing can be diffed against itself.

`-m` and `--sort` compose in the obvious direction: filter first, then order
what survived.

### `--diff`, what changed between two archives

```console
$ tmd -f old.tar -f new.tar --diff
--- old.tar
+++ new.tar
+ tree/added.txt
~ tree/changes.txt   size 6 -> 26
~ tree/keep.txt   mode 0664 -> 0600
- tree/removed.txt
  3 identical, 2 changed, 1 added, 1 removed
```

`-` was in the first archive and is not in the second, `+` is the other way
round, and `~` is in both but not the same — naming the fields that differ:
size, mode, mtime, kind, and a link's target.

Always ordered by path, whatever order the archives are in, so a comparison can
be diffed against itself. Identical members are counted, not listed.

**It reads one archive into memory and streams the other past it.** Buffering
both would cost twice the memory for nothing: the second side only ever needs to
look its own path up. What is kept per member is a digest of the compared
fields, not a whole entry — no raw block, no pax list — which on a large archive
is the difference between tens of megabytes and hundreds.

Duplicated paths follow the same rule as everywhere else in tmd: extraction
keeps the last one, so the last occurrence is what gets compared, and the report
says how many paths appeared more than once.

### `--verify`, checking an archive against a manifest

```console
$ tmd -f backup.tar --verify=manifest.txt
--- manifest.txt
+++ backup.tar
~ etc/nginx/nginx.conf   size 2481 expected, 2604 found
- etc/missing.conf   expected, not in the archive
+ etc/extra.conf   in the archive, not expected
  559 identical, 1 changed, 1 unexpected, 1 missing
```

A backup check that never extracts anything.

The manifest is the simplest thing any tool can produce — one member per line,
`SIZE PATH`, where `SIZE` may be `-` to mean "this path should be here and I am
not saying how big it is". Blank lines and lines starting with `#` are ignored:

```
# what the nightly backup should contain
2481 etc/nginx/nginx.conf
-    etc/nginx/conf.d/
```

tmd can produce one from an archive you trust, which makes the round trip a
one-liner:

```console
$ tmd -f good.tar -t CSV | tail -n +2 | awk -F, '{print $10, $1}' > manifest.txt
$ tmd -f suspect.tar --verify=manifest.txt
```

The first field counts as a size only when it is entirely digits or a single
`-`. Otherwise the whole line is the path, so a file actually named
`2481 notes.txt` still works as long as no size is given for it. A line too long
to fit in the reader is **refused rather than split** — silently turning one
over-long path into two plausible-looking ones would be a wrong answer from a
file you may not have written.

### Both report the same way

`-m` narrows what is compared. `-t JSON` gives the whole comparison as a
document with a `matches` boolean, so a script reads one field:

```console
$ tmd -f old.tar -f new.tar --diff -t JSON | jq '.diff.matches'
false
```

**Exit status 5** means the comparison ran and found differences — distinct from
`1` (an archive could not be read) and `2` (the command line was wrong).
`diff(1)` uses `1` for this; that is already taken here by the unreadable case:

```console
$ tmd -f nightly.tar --verify=manifest.txt || echo "backup does not match"
```

### Extraction safety — would this archive escape?

Reported on every run, without being asked, because it is the question somebody
pointing this at an untrusted archive most wants answered — and tmd can answer
it **before** anything is written to disk, which `tar` cannot:

```console
$ tmd -f untrusted.tar
tmd: untrusted.tar: out-abs: link target leaves the extraction directory: /etc/passwd
tmd: untrusted.tar: ../../etc/cron.d/x: path climbs out of the extraction directory
drwxr-xr-x  root/root     0  2026-09-15 11:02:31Z  safe/
lrwxrwxrwx  root/root     0  2026-09-15 11:02:31Z  out-abs -> /etc/passwd
```

Three classes, because they are three different mistakes to make when
extracting:

| class | example | who honors it |
|---|---|---|
| absolute path | `/etc/passwd` | GNU tar and bsdtar strip the leading `/` by default and honor it under `-P` |
| traversal | `../../etc/passwd` | refused by modern tar, accepted by old ones |
| link out of the tree | `link -> /etc`, `link -> ../..` | the dangerous one: a link out, then a later member written *through* it |

Traversals are counted by **depth**, not by looking for `..` in the string.
`a/../b` extracts to `b`, squarely inside the current directory, and flagging it
would make the check cry wolf on archives that do it legitimately. A path is
only reported when its depth actually goes below where it started.

The verdict appears in the summary and the `-i` report, and — this is the part
worth knowing — **it is stated even when the archive is clean**:

```console
$ tmd -f release.tar -i | grep extraction
  extraction       every member stays inside the extraction directory
```

Most lines in that report appear only when there is something to say. This one
is different, because silence is not an answer to a yes/no question: it reads as
"tmd did not look". In JSON it is one boolean, so a script does not have to
infer safety from which keys are missing:

```console
$ tmd -f untrusted.tar -S -t JSON | jq .summary.extraction
{
  "escapes": true,
  "absolute_paths": 0,
  "traversals": 1,
  "links_outside": 1
}
```

tmd never extracts anything, so this is pure reporting. It does not refuse, and
it does not change the exit status.

### Member order, and what extracting would create

```console
$ tmd -f nginx-1.31.6.tar -i | grep -A1 'member order'
  member order     lexicographic by path
  top level        one entry: nginx-1.31.6/
```

Two observations, reported as observations. The ordering is a clue to how an
archive was built — a sorted input list looks different from a directory walk —
but it is **only** a clue, and tmd does not guess a command from it. An earlier
draft did, claiming "lexicographic, so it came from a sorted list", and it was
wrong within minutes: `tar -c DIR` over a small tree came out in exact
lexicographic order because `readdir` happened to return it that way.

One inference does follow and is stated: an archive with **no directory
members** was written from a list of files, because a directory walk emits the
directories it walks through unless told otherwise.

The `top level` line answers the older sense of "tar bomb" — not a path that
escapes, but an archive that unpacks two hundred entries into whatever directory
you were standing in:

```console
$ tmd -f scatter.tar -i | grep -A1 'top level'
  top level        more than 8 entries — extracting scatters them into the current directory
```

### `--stat`, what is actually in here

```console
$ tmd -f nginx-1.31.6.tar --stat

nginx-1.31.6.tar
  members       562
  extracted     8123456 bytes (7.7M)
  stored        8388608 bytes (8.0M)
  padding       265152 bytes (3.1% of the archive)
  largest           94208  src/http/ngx_http_core_module.c
                    61440  src/core/ngx_string.c
  timestamps    2 distinct
  earliest      2026-03-01 09:14:00Z
  latest        2026-03-01 09:16:12Z
  span          2 minutes
  most common   2026-03-01 09:14:00Z (558 of 562 members)
  produced by   a handful of timestamps within an hour — exported into a fresh
                directory, then a few files regenerated
```

The sizes answer "why is this archive 40 GB", including how much of it is
padding that no member's contents occupy.

**The timestamps are the more useful half.** How many *distinct* mtimes an
archive holds says how it was produced, which is exactly what you need when a
tarball arrives with no other provenance:

| distinct mtimes | what produced it |
|---|---|
| 1 | normalized — a reproducible build clamping to `SOURCE_DATE_EPOCH` |
| 1, and it is the epoch | timestamps discarded rather than normalized |
| a handful within an hour | exported into a fresh directory, then a few files regenerated by a release script |
| many, spread over months | real per-file times, preserved from a working tree |

tmd states that inference on the `produced by` line. It is an inference, not a
verdict, and it is phrased as one.

It also reports what a one-line-per-member listing hides: members with **no**
mtime, members dated in the **future**, members dated before tar existed (1979),
and members dated before 1970.

Two things worth knowing:

- **The distinct count is capped at 4096**, after which it reports "more than
  4096 distinct". An archive is a thing somebody else wrote, and counting
  distinct values needs somewhere to put them; past the cap the answer the
  reader needed — one, a handful, or many — is already settled.
- **`-m` narrows what `--stat` describes.** That is the opposite of what `-m`
  does to the summary, and deliberate: a summary answers "what is this file",
  which a filter cannot change, while a distribution answers "what is in this
  set", which is precisely what a filter selects. The `matched N of M` line
  makes which one you are reading unambiguous.

### `created`, when the archive knows it

```console
$ tmd -f from-a-mac.tar -l | grep -E 'modified|created'
  modified    2023-11-14 22:13:20Z
  created     2023-11-03 08:26:40Z
```

Usually absent, and worth knowing why. **Only libarchive writes it**, as the pax
attribute `LIBARCHIVE.creationtime`, and only under two conditions:

- the birth time must be **earlier** than the modification time — so a file
  created now and back-dated with `touch` never gets one;
- the platform must fill `struct stat.st_birthtime`, which FreeBSD, macOS and
  NetBSD do and **Linux does not**. libarchive's disk reader never calls
  `statx(STATX_BTIME)`, so `bsdtar` on Linux silently never writes it while
  `bsdtar` on a Mac does.

GNU tar never writes one on any platform. So a `created` line means the archive
came off a Mac or a BSD — which is itself a fact about its provenance, and the
reason it is rendered plainly rather than left as a raw attribute.

tmd accepts only that one spelling. star's and GNU's keyword tables have no
birth-time key, and inventing a second spelling would mean rendering a field no
writer produces.

### Timestamps are UTC, and say so

The default rendering is `2019-04-12 14:23:07Z` — to the second, marked UTC.

That is deliberate, for the job this tool is usually doing: an archive arrives
from somewhere else, and what its timestamps mean is a question about an
absolute instant, not about the wall clock of whoever is reading it. A bare
`2019-04-12 10:23` is ambiguous the moment the archive leaves the machine that
wrote it, and shifts by an hour after a DST change.

Nothing is lost by this. A tar header stores mtime as **seconds since the Unix
epoch** — an absolute count from `1970-01-01 00:00:00 UTC`. No tar format
records a timezone, and none needs to: the zone was already applied when the
filesystem wrote the timestamp, so what is stored is the result. Two files that
each read `12:32` to their own user, one in New York and one in Los Angeles,
hold epoch values exactly three hours apart and render here as `16:32:00Z` and
`19:32:00Z`.

`--local` renders in the reader's zone instead, which is what `tar -tvf` does.
`-T` adds nanoseconds, which only pax archives carry. `-u` still works and now
means "yes, really, the default".

### `-i`, the archive report

```console
$ tmd -f backup.tar -i

backup.tar
  size             12288 bytes (12K)
  format           POSIX pax (POSIX.1-2001)
  generation       3rd — POSIX pax, IEEE 1003.1-2001
                   ustar headers plus 'x' and 'g' blocks carrying arbitrary
                   key/value attributes: paths and link targets of any length,
                   64-bit ids, and timestamps to the nanosecond.
  dialects seen    ustar, pax
  written by       libarchive (bsdtar) (inferred)
  extensions used  pax extended headers (12), GNU sparse, pax format (1)
  pax keys         mtime, path, GNU.sparse.major, GNU.sparse.name
  paths            longest 203 bytes; 1 over the v7 limit
  timestamps       mtime to nanosecond precision
  ownership        names and numbers; highest uid 1000, gid 1000
  blocking         20 blocks of 512 bytes (10240), inferred from the length
  end marker       present (two zero blocks)
  members          8 (3 files, 3 directories, 1 symlink, 1 hard link)
  content          10005012 bytes (9.5M) when extracted
                   1 of those members is sparse, so the archive is much smaller
  integrity        every header checksum matched
  needs a reader   that understands pax extended headers
                   (GNU tar 1.14+, bsdtar, star, POSIX pax)
```

Two things there are worth calling out because they are inferences rather than
facts the archive states, and both are labeled as such:

- **"written by"** comes from fingerprints only one implementation leaves.
  `LIBARCHIVE.*` attribute keys mean bsdtar; `SCHILY.*` means star or
  libarchive; the GNU magic string means GNU tar. The strongest signal for an
  ordinary pax archive is the *name* each implementation gives its own extended
  header member — GNU writes `PaxHeaders/`, libarchive writes `PaxHeader/`
  without the `s`. `GNU.sparse.*` looks like an obvious GNU marker and is
  deliberately not treated as one: libarchive writes the same keys, because
  GNU's is the only pax sparse format there is.
- **"needs a reader"** is about *this file*, not about its magic bytes. An
  archive written by GNU tar that happens to use no GNU extension reads
  perfectly in a ustar-only tool, and saying otherwise would send somebody
  converting an archive that nothing has trouble with.

### JSON and CSV

```console
$ tmd -f backup.tar -t JSON | jq '.summary.members'
8
$ tmd -f backup.tar -t CSV | head -2
path,kind,mode_string,mode,format,uid,gid,uname,gname,size,stored_size,offset,mtime,mtime_epoch,linkpath,checksum
src/main.c,file,-rw-r--r--,0644,pax,1000,50,bceverly,staff,1234,1536,0,2026-09-16T18:11:33Z,1789668693,,ok
```

`-t` / `--output-type` takes `TXT`, `JSON` or `CSV` in either case. `--format`
is the same switch under the name releases before 1.2 used, and keeps working.

One `-f` produces a bare JSON object; several produce an array of them, so
`tmd -f x.tar -t JSON | jq .summary` works without indexing into a
single-element list.

#### The JSON describes the bytes

The JSON is not a tidier listing. It aims to be a faithful description of what
is actually in the header, so that two archives can be diffed field by field and
a reader can check tmd's interpretation against the bytes it interpreted:

```console
$ tmd -f backup.tar -t JSON | jq '.entries[0] | {path, mode_bits, blocks, checksum}'
{
  "path": "src/main.c",
  "mode_bits": {
    "setuid": false, "setgid": false, "sticky": false,
    "owner": {"read": true, "write": true, "execute": false},
    "group": {"read": true, "write": false, "execute": false},
    "other": {"read": true, "write": false, "execute": false}
  },
  "blocks": {
    "block_size": 512, "header_offset": 0, "header_blocks": 1,
    "data_offset": 512, "data_bytes": 1234, "data_blocks": 3,
    "padding": 302, "total_bytes": 2048
  },
  "checksum": {
    "stored": 6208, "computed": 6208,
    "computed_unsigned": 6208, "computed_signed": 6208,
    "valid": true, "matched": "unsigned"
  }
}
```

What that buys, field by field:

| Field | Answers |
|---|---|
| `schema` | which version of this document you are reading (currently `2`) |
| `mode_bits` | is anything setuid — a question `"0755"` hides in a digit |
| `blocks` | exactly which bytes the member occupies, and how much is padding |
| `checksum.matched` | `unsigned`, `signed`, or `null` — historic tars disagreed, and which one an archive agrees with says something about the tool that wrote it |
| `mtime_source` | `header`, `GNU tail`, `pax mtime` … — a pax record can override the header, and two tars can legitimately disagree about the same member |
| `path_source` | whether the name came from the header, a `prefix`, a GNU `L` block or a pax record |
| `path_encoding` | whether the path is valid UTF-8, and the offset of the first byte that is not |
| `xattrs` | `SCHILY.xattr.*` / `LIBARCHIVE.xattr.*` values decoded from base64, kept **beside** the verbatim record rather than replacing it |
| `warnings[].code` | a stable name like `checksum-mismatch`, so a consumer matches on that instead of on English |
| `raw.block_base64` | under `-R`, the member's own 512-byte header, byte for byte, with `raw.block_offset` saying where it is |

`raw.block_offset` is not always `blocks.header_offset`: a member with a GNU `L`
long name occupies three header blocks, and `block_base64` is the last of them —
the one the decoded fields came from.

The **other** blocks are there too, under `raw.extension_blocks`:

```console
$ tmd -f long-names.tar -R -t JSON | jq '.entries[1].raw.extension_blocks[] | {offset, kind}'
{ "offset": 512,  "kind": "L" }     # the GNU long-name header
{ "offset": 1024, "kind": null }    # the name it carries
```

`kind` is the typeflag of the header that introduced the block (`L`, `K`, `x`,
`g`), or `null` for one of its payload blocks. The payload's **padding is
included**, deliberately: it is the one place in an archive where bytes can sit
that no field accounts for.

With those, the JSON accounts for every byte a member occupies, which is what
"a faithful description of the bytes" ought to mean. It is checkable, and the
test suite checks it: each block must equal the archive at its own offset, and
the captured count plus the member's own header must equal `blocks.header_blocks`.

The list is capped at 32 blocks per member (`extension_blocks_truncated` says
when it was cut) — a pax header is as large as its writer chose, and an archive
is a thing somebody else wrote.

Both are emitted only under `-R`. The member header alone is 684 base64
characters, a 200,000-member archive is a normal thing to point this at, and the
reader does not even keep the extension blocks unless `-R` asked for them.

A path in a tar archive is a string of bytes with no declared encoding. In JSON,
paths that are valid UTF-8 are emitted as themselves; anything else is escaped
byte by byte as `\u00XX`, which round-trips through any parser and does not
destroy the value the way replacing it with U+FFFD would. `path_encoding` says
which of the two happened, so a consumer never has to guess.

#### Schema versions

`"schema": 2` is the first field of every archive object. New keys keep arriving
within a schema — a consumer that selects the keys it wants is unaffected — and
the number changes only when an existing shape does. Schema 1 was the output up
to v1.1.0.x; schema 2 adds the fields above and changes `warnings` from an array
of strings to an array of `{code, text}`.

## What it understands

| Generation | Written by | What tmd reads |
|---|---|---|
| **v7** (1979) | Seventh Edition tar | 100-byte names, no magic, no owner names. Directories marked by a trailing slash are recognized as directories rather than reported as empty files. |
| **ustar** (POSIX.1-1988) | `pax`, most tars | The `ustar` magic, the 155-byte path prefix joined to the name, owner and group names, device numbers, the full typeflag set. |
| **star** | Schilling's star | ustar plus star's block signature and its `SCHILY.*` attributes. |
| **GNU** | GNU tar | `L` and `K` blocks for unlimited paths and link targets, `S` sparse files with the in-header map *and* its continuation blocks, `D` dumpdirs, `M` multi-volume parts, `V` volume labels, base-256 numeric fields, and atime/ctime read from the header tail rather than mistaken for a path prefix. |
| **pax** (POSIX.1-2001) | bsdtar, GNU tar `--format=posix` | `x` and `g` extended headers with correct length-prefixed record parsing, `path`/`linkpath`/`size`/`uid`/`gid`/`uname`/`gname`/`mtime`/`atime` overrides, nanosecond timestamps, and GNU sparse formats 0.0, 0.1 and 1.0 — including the 1.0 map that lives in the member's own payload. |

### Compressed archives

Read directly. A `.tar.gz`, `.tar.xz`, `.tar.bz2`, `.tar.zst` and several more
are recognized **by content, not by extension**, and unpacked on the way in:

```console
$ tmd -f nginx-1.31.6.tar.gz -i | grep compression
  compression      gzip
$ cat backup.tar.zst | tmd          # through a pipe, where nothing can be rewound
```

| magic | format | tool run |
|---|---|---|
| `1f 8b` | gzip | `gzip -dc` |
| `fd 37 7a 58 5a 00` | xz | `xz -dc` |
| `42 5a 68` | bzip2 | `bzip2 -dc` |
| `28 b5 2f fd` | zstd | `zstd -dc` |
| `04 22 4d 18` | lz4 | `lz4 -dc` |
| `LZIP` | lzip | `lzip -dc` |
| `1f 9d` | compress | `gzip -dc` |

tmd **runs** those tools rather than linking a compression library — see
[What it links, and what it runs](#what-it-links-and-what-it-runs). Three
processes are involved: a feeder, the decompressor, and tmd. The feeder exists
because tmd has already read the first bytes to find out what the file *is*, and
those bytes have to reach the decompressor too; a seekable file could be rewound
instead, but standard input cannot, and one path that always works beats two
that each work sometimes.

Two things it will not do quietly:

- **A missing tool is named.** Not an empty archive, not a parse error:
  `tmd: a.tgz: gzip-compressed, and gzip is not installed`.
- **A truncated stream is an error.** A corrupt `.tar.gz` decompresses part way
  and stops, and what reaches the reader is a perfectly well-formed *prefix* of
  an archive. Reporting that as a complete listing with exit 0 would be a silent
  wrong answer, so tmd checks the decompressor's exit status and fails.

A container that is not a compressed tar — a zip, an RPM, a cpio archive — is
named for what it is rather than guessed at, because no amount of piping turns
one into a tar archive.

The format is worked out **per member**, not per archive, because a real archive
mixes them: a pax archive is plain ustar headers with an occasional extended
header in front of the member that needed one. The summary reports the most
expressive dialect present, because that is the one a reader has to understand
to get every member right.

Damage is reported rather than fatal. A bad checksum, a pax record whose length
prefix lies, a sparse map that runs off the end, a size field that is not octal —
each is a warning on standard error and the read continues, so one bad header
does not hide the two hundred good ones after it. Both the signed and the
unsigned checksum convention are accepted, because tar's own history is
ambiguous about whether the header bytes were signed and rejecting one of them
means calling a perfectly good archive corrupt.

## Building

```bash
make build        # ./bin/tmd, plus the manpage if it is out of date
make debug        # unoptimized with -g3, for a debugger
make clean        # remove every intermediate file
make distclean    # clean, plus the packaging output
```

`CC`, `CFLAGS`, `CPPFLAGS` and `LDFLAGS` are all left to the caller, because a
distribution build passes its own — a package that ignores `dpkg-buildflags`
ships without the hardening the distribution promises. Everything this project
insists on is appended rather than substituted, so both survive:

```bash
make build CC=clang CFLAGS="-O3 -march=native"
```

The build is clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Wcast-qual -Wwrite-strings -Wformat=2` and several more, and
CI builds it a second time with `-Werror`. `-Werror` is deliberately **not** on
by default: a newer compiler inventing a new warning must not stop somebody
building the package from source.

The binary is hardened — PIE, stack protector, `_FORTIFY_SOURCE`, stack-clash
protection, CET, full RELRO and BIND_NOW — and `make security` checks the
*binary* for all of it rather than trusting that the flags did something.

Each optional hardening flag is **probed**, not assumed, because the PPA builds
for 22.04 LTS as well as 24.04 and 26.04:

- `-fstack-clash-protection` and `-fcf-protection=full` exist on x86 and not
  everywhere, so a hardening flag that stops the build on somebody's
  architecture is worse than one that is absent there.
- `_FORTIFY_SOURCE=3` needs GCC 12 or clang 9. 22.04 ships GCC 11, where glibc
  answers a request for level 3 with `#warning _FORTIFY_SOURCE > 2 is treated
  like 2 on this platform` — harmless until `-Werror` is on, and `-Werror` is on
  in CI and in `make lint`. The level is chosen by compiling something that
  actually includes a header, because a flag probe never reaches `features.h`.
  Level 3 where it works, level 2 where it does not.

That is also why `-Werror` is off by default. CI builds with it on a current
compiler, where a new warning *should* fail; a Launchpad builder on 22.04 builds
without it, where a warning from a four-year-old GCC should not stop a package.

### What version you are running

```console
$ tmd --version
tmd 1.0.0.6
Copyright (c) 2026 Bryan C. Everly
Licensed under the BSD 2-Clause License. There is NO WARRANTY, to the extent permitted by law.
```

A plain number means the build **is** the release: the working tree is clean and
`HEAD` is exactly the `v1.0.0.6` tag. Anything else reports a `-dev` suffix:

```console
$ tmd --version | head -1
tmd 1.0.0.6-dev
```

`1.0.0.6` names a build somebody else can download. `1.0.0.6-dev` names a build
only the person running it has — which, in a bug report, is the difference
between reproducing a problem and chasing it.

A tree is "not the release" if a tracked file has been modified, if there are
commits after the tag, or if there is no tag for this version yet. **Untracked**
files deliberately do not count: a scratch archive or an editor swap file does
not change what was built, and counting it would leave every working tree
permanently `-dev` for reasons unrelated to the code.

The suffix never reaches a package. `debian/rules` passes the released number
explicitly, and a source package has no `.git` to compare against anyway — so
the binary inside a `.deb` and the version on the `.deb` always agree. The
manpage carries the released number too, because it is a committed file and a
suffix that moved with the working tree would make it differ from its committed
copy on every edit.

See `scripts/version.sh`, which is the whole of this in forty lines.

### What it links, and what it runs

Two different questions, and tmd answers them differently.

**It links nothing but libc.** `ldd bin/tmd` reports the C runtime and nothing
else, and `make security` checks that rather than asserting it — anything beyond
libc fails the build. That is a design decision, not an accident: this program
parses files that arrive from other people, and every library linked into it is
more attacker-reachable code and another CVE feed to watch. It also means a
Launchpad builder — which has no network — can build the package from source
alone, with no vendoring.

**It runs the system's decompressor.** Since v1.6.0.0 tmd reads `.tar.gz`,
`.tar.xz`, `.tar.bz2`, `.tar.zst` and several more directly, by executing
`gzip -dc`, `xz -dc` and friends as separate processes — the same thing GNU tar
does when you pass it `-z`. So those tools are **runtime** dependencies, and the
Debian package declares them: `xz-utils` as a `Depends`, `bzip2` and `zstd` as
`Recommends`, `lz4` and `lzip` as `Suggests`. `gzip` is deliberately absent
because it is `Essential` on every Debian system, and depending on an essential
package is a Policy violation.

Running them rather than linking them is the better trade for this program, not
merely the easier one: the code that parses hostile *compressed* bytes then sits
in a different process from the code that parses hostile *tar* headers, and a
bug in zlib is a bug in something tmd talks to rather than a bug in tmd's
address space. Without any of those tools installed tmd still works perfectly on
uncompressed archives, and says plainly which tool is missing when it meets one
it cannot open.

## Testing

```bash
make test           # everything below, in order
make test-unit      # the C unit tests
make test-cli       # end-to-end, against real GNU and BSD archives
make test-memory    # ASan, UBSan, LeakSanitizer and valgrind
make coverage       # line coverage, fails below 80%
make fuzz           # 30 seconds of mutated archives (FUZZ_SECONDS to change)
```

**Unit tests** build archives byte by byte, because the headers worth testing
are the ones no real tool would ever write: a truncated sparse map, a pax record
whose length prefix lies, a checksum field full of spaces, a directory that
claims a 4096-byte payload. There is no external framework — a test runner that
needs a package installed before `make test` works would undo the point of a C
program that links nothing.

**Architectures.** Every job runs on x86-64, where three things are true that
are not true everywhere: the machine is little-endian, `time_t` is 64 bits, and
plain `char` is signed. tmd is written not to care — every header field is
decoded byte by byte, so there is not a single multi-byte load in the program —
and since v1.6.0.0 a `Tests (aarch64)` job on GitHub's native arm runners proves
part of it: `char` is unsigned there, and the ABI is different. It is still
little-endian, so a genuine byte-order bug would survive it. A qemu-based
big-endian leg would catch one and was
[considered and declined](ROADMAP.md#a-big-endian-ci-leg): there is not a single
multi-byte load in the program, so the property is true by construction, and an
emulated leg runs too slowly to sit on every push.

**End-to-end tests** do the opposite: they build archives with the real `tar`
and `bsdtar`, in every format each can write, and check that `tmd`'s listing
agrees with `tar -tvf` member by member. Then they check the things `tar` cannot
tell you — the format report, the JSON and CSV output, the exit statuses, and
what happens to a `.tar.gz`, a truncated archive, a corrupted header and a text
file.

**Memory safety** runs both suites again under AddressSanitizer,
UndefinedBehaviorSanitizer and LeakSanitizer, and then under valgrind, which
catches uninitialised reads that ASan does not. The sanitizers are pointed at a
log file rather than stderr, because the end-to-end tests assert on what the
program writes to stderr.

**Coverage** rebuilds instrumented and runs *both* suites against that one
build, then regenerates the README badge from the number it measured — red
below the 80% gate, amber for a pass with no headroom, green above 85%. The
badge is a committed SVG rather than a shields.io URL, so the README renders
with no network round trip (including inside a source package, where there is
none) and reading the page does not report a visit to a third party. A coverage
number typed into a README by hand is a coverage number that is eventually
wrong. That matters: the unit tests never touch `main.c` or `opts.c`, and the
end-to-end tests are what cover them, so measuring either alone reports a number
that is wrong in a way more tests would not fix. The gate is 80%; the current
figure is 86%.

**Fuzzing** uses libFuzzer when clang is installed and a built-in in-process
mutation loop otherwise, so fuzzing happens on every machine rather than only
where clang is. The mutation operators aim at the fields that decide control
flow — the typeflag, the size, the magic — and re-checksum the block afterwards,
because random byte flips almost never produce a valid checksum and would spend
the whole run being rejected at the first block. Either engine runs under the
sanitizers, which are what decide whether something is a bug.

Every test script skips a tool it cannot find rather than failing, which is what
makes them usable on a fresh checkout — and also what makes it possible to have
a green local run that CI then fails, because CI has all of them. `make
install-dev` closes that gap: it installs valgrind, cppcheck, clang-tidy,
clang-tools, shellcheck, gcovr, flawfinder, clang, gitleaks, semgrep and the
Debian packaging tools, then installs the git hooks. After it, nothing skips
locally either.

## Security

```bash
make security
```

Nine checks, ordered by how close they sit to the threat — this program parses
a file somebody else produced, so every byte of a tar header is
attacker-controlled:

| Check | Looks for |
|---|---|
| `hardening-check` | whether the shipped binary actually got the hardening the flags asked for |
| `gcc -fanalyzer` | leaks, double frees and null dereferences along a specific path |
| `cppcheck` + CERT C | the rule-based second opinion |
| `scan-build` | the clang static analyzer, a third |
| `flawfinder` | known-dangerous API use |
| `semgrep` | pattern-based SAST |
| the sanitizers | the real thing, at runtime |
| a fuzz run | the real thing, on inputs nobody wrote |
| `gitleaks` | committed secrets, across the whole history |

CI runs the same script, plus CodeQL with the `security-extended` query pack and
a longer fuzz run (15 minutes on the weekly schedule).

The internal limits are worth knowing about, because they are what stops a
hostile archive from turning a diagnostic into an out-of-memory kill: a GNU long
name or a pax header is read up to 16 MiB and then truncated with a warning, a
sparse map is capped at a million entries, and the reader gives up on a member
after a thousand extension headers. All are far above anything a real archive
contains — the longest path Linux will produce is 4096 bytes.

## Make targets

Run `make` with no arguments for the full list. The ones you will use:

| Target | What it does |
|---|---|
| `make build` | compile into `./bin`, regenerating the manpage if needed |
| `make test` | unit + end-to-end tests, sanitizers, and the coverage gate |
| `make lint` | the compiler with `-Werror`, `-fanalyzer`, cppcheck, clang-tidy, shellcheck, the copyright audit, and the manpage and README freshness checks |
| `make security` | the security scanners, locally |
| `make man` | force the manpage to be regenerated from `--help` |
| `make docs` | regenerate the manpage **and** the README's Usage block |
| `make coverage` | measure coverage and refresh the README badge |
| `make install` | build the installable `.deb` |
| `make install-tree` | staged install into `DESTDIR` (what the package build uses) |
| `make release` | bump the version, tag it and push |
| `make install-dev` | install every tool the above wants, so nothing skips |
| `make clean` | remove every intermediate file |

### The documented options cannot go stale

`man/tmd.1` and the [Usage](#usage) block above are both generated from the
program's own `--help`, which is itself generated from `src/options.def` — one
table that produces the `getopt_long` array, the short option string and the
help text. Adding an option updates all five together.

`make build` regenerates the manpage when it is out of date, `make docs`
refreshes both, `make lint` fails if either committed copy does not match, and
CI runs that check on every pull request.

The README was the one that proved this necessary. Its option list was pasted
by hand and checked by nobody, and it went on presenting `--format` as the only
way to choose an output type for a whole release after `-t` existed. Three
copies of one list is fine; two of them being generated is what makes it fine.

### The git hooks

`make install-hooks` installs two:

- **pre-commit** runs `make lint` and refuses the commit on any finding — *and*
  refuses it if lint **skipped** anything. A scanner that is not installed
  reports "skipped" and passes, which is a clean result that proves nothing;
  at commit time that is the wrong trade, and `make install-dev` is the fix.
- **pre-push** runs `make test` — both suites, the sanitizers, valgrind and the
  coverage gate.

The split is deliberate. Lint takes seconds and belongs at the save point;
the full test run takes minutes, and interrupting every commit for it is how
people learn to pass `--no-verify`, which switches off the push check too.
`TMD_SKIP_HOOK=1` bypasses either.

### Brace style, and why lint checks it

Two rules, both enforced by `make lint` rather than left to good intentions.

**Every body is braced**, even a single statement:

```c
if (!path)
{
    return false;
}
```

Not a matter of taste. An unbraced body is one careless edit away from a second
statement that looks guarded and is not, which is exactly CVE-2014-1266 —
Apple's "goto fail", a duplicated `goto fail;` under an unbraced `if` that
skipped the rest of a TLS signature check and shipped. clang-tidy's
`readability-braces-around-statements` is enabled by name for this.

**The opening brace of a statement goes on its own line.** A house rule, adopted
because the attached form is harder to read, and checked by lint's `brace style`
section — which reports file and line, so a slip is caught rather than merged.

It is deliberately narrow: this is about *statements*. A `struct`, `union` or
`enum` body and an initializer keep their brace attached, because those declare
a shape rather than open a block, and moving theirs would make a table of
constants twice as tall for no gain.

The check is awk rather than clang-format, because clang-format cannot be asked
for one rule — it reformats everything, and the alignment and comment wrapping
in these files were done by hand and are meant to stay.

### The copyright audit

Every source file carries the same notice, and `make lint` is what keeps that
true. It is not bureaucracy: the license grant on a BSD-2 project *is* the file
header, and a file that reaches somebody without one has no license at all. The
audit checks `src/`, `include/`, `tests/` and `scripts/` for

```c
/*
 * Copyright (c) 2026 Bryan C. Everly
 * SPDX-License-Identifier: BSD-2-Clause
 */
```

and separately checks that `LICENSE`, `debian/copyright`, the manpage and the
binary's own `--version` output all name the same person. The year may be a
single year or a range, so bumping it does not mean touching every file.

## Installing

### From the PPA (Ubuntu)

```bash
sudo add-apt-repository ppa:bceverly/tmd
sudo apt update
sudo apt install tmd
```

### Build the package yourself

```bash
make install        # or: make deb — builds ../tmd_<version>~<series>1_<arch>.deb
sudo dpkg -i ../tmd_*.deb
```

`make install` builds a `.deb` rather than copying files into `/usr`, which is
what this project's owner asked for. The staged install a package build needs is
`make install-tree DESTDIR=... prefix=/usr`, and it is deliberately not called
`install`: a target that quietly writes into `/usr` when somebody expected a
package is the wrong surprise to hand anybody.

### Without a package

```bash
make build
sudo make install-tree prefix=/usr/local
```

## Releasing

```bash
make release                    # bump the last digit of the highest tag
make release VERSION=1.2.0.0    # or set it explicitly
make release VERSION=v1.2.0.0   # the "v" is optional
```

Versions are four-part and tags carry a lowercase `v`, e.g. `v1.2.3.4`. With no
version tags in the repository at all, the first release is `v1.0.0.0`.

The script asks for one confirmation and then: writes `VERSION`, rebuilds so the
binary and the manpage carry the new number, checks that `tmd --version` agrees,
commits, pushes, tags and pushes the tag. **Pushing the tag is what triggers the
release**, so the confirmation is the point of no return — answer `n` and
nothing at all happens.

The version lives in exactly one file. The Makefile reads `VERSION` and compiles
it in with `-DTMD_VERSION`; `scripts/build-deb.sh` generates `debian/changelog`
from it. There is no second copy to forget.

What the tag sets off:

1. **gate** — waits for the CI and Security runs that are already going on that
   exact commit. A tag on a commit that was never pushed to a branch fails here
   in five minutes rather than building something untested.
2. **build** — imports the signing key, builds the binary `.deb`, runs lintian
   with `--fail-on error,warning`, then builds one *signed source package per
   Ubuntu series* — 22.04, 24.04 and 26.04 LTS by default.
3. **verify** — installs the `.deb` in a clean `ubuntu:24.04` container with no
   compiler in it, and puts it through every mode, including a member-by-member
   `diff` against `tar -tvf`.
4. **publish** — `dput`s the source packages to the PPA.

`publish` needs `verify`, so a package that builds but does not install never
reaches the PPA. That ordering is the point: **a bad upload to a PPA cannot be
withdrawn, only superseded by a higher version.**

Launchpad builds asynchronously after the upload, so a green workflow means
"accepted", not "published". The PPA's own page is the last word.

## GitHub configuration

Everything below is set in the repository's **Settings → Secrets and variables
→ Actions**. Only the first two are required, and only for releasing — CI,
security scanning and coverage need nothing configured at all.

### Secrets

| Secret | What it is |
|---|---|
| `LAUNCHPAD_GPG_PRIVATE_KEY` | The ASCII-armored **private** key that signs the source packages. Its public half must be registered on your Launchpad account. |
| `LAUNCHPAD_GPG_PASSPHRASE` | That key's passphrase. |

Producing them, once:

```bash
# 1. Make a signing key, if you do not already have one. RSA rather than gpg's
#    ECC default: Launchpad's OpenPGP handling has a long history of trouble
#    with newer algorithms, and this is not the place to discover it.
#
#    Give it the same name and email as the Maintainer in debian/control. Not
#    a Launchpad requirement -- see below -- but it keeps the changelog trailer
#    and the signature telling one story, and it stops debsign picking the
#    wrong key when several of yours share an address.
gpg --full-generate-key       # (1) RSA and RSA, 4096, 0 = never expires
#                             # leave Comment BLANK: it becomes part of the UID

# 2. Find its fingerprint -- the 40-character hex line, not the short key id.
gpg --list-secret-keys --keyid-format=long

# 3. Publish the public half where Launchpad looks for it.
gpg --send-keys --keyserver keyserver.ubuntu.com <FINGERPRINT>
#
#    Then paste the same fingerprint into
#    https://launchpad.net/~<you>/+editpgpkeys
#
#    If Launchpad answers "could not import the OpenPGP key", the upload has
#    almost certainly worked and the keyserver has not finished indexing it.
#    Launchpad fetches BY FINGERPRINT, so wait until this returns a key block
#    rather than "Not Found" and retry -- it took about three minutes:
#      curl -s 'https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x<FINGERPRINT>'
#
#    Registration is two steps. Launchpad then emails a PGP-ENCRYPTED
#    confirmation, and the key is attached to the account only once you decrypt
#    it and open the link inside:
#      gpg -d <the-saved-mail>
#
#    Confirm it landed (public API, no credentials needed):
#      curl -s -H 'Accept: application/json' \
#        https://api.launchpad.net/1.0/~<you>/gpg_keys

# 4. Export the private half for the secret. This is the whole key -- treat the
#    output the way you would a password-manager export, and paste it straight
#    into the browser rather than by way of a file.
gpg --armor --export-secret-keys <FINGERPRINT>
#    All of it, BEGIN and END lines included, into LAUNCHPAD_GPG_PRIVATE_KEY.

# 5. Point local signed builds at it.
export DEBSIGN_KEYID=<FINGERPRINT>
```

### Backing the key up

Losing it means losing the ability to publish to that PPA under that key, and
the key above never expires — so the revocation certificate is the only way to
retire it. Four artifacts, not one:

```bash
FPR=<FINGERPRINT>
OUT=$(mktemp -d -p /dev/shm) && chmod 700 "$OUT"   # tmpfs: never touches a disk

gpg --armor --export-secret-keys "$FPR" > "$OUT/secret.asc"      # the key itself
gpg --armor --export             "$FPR" > "$OUT/public.asc"      # convenience
gpg --export-ownertrust | grep "^$FPR"  > "$OUT/ownertrust.txt"  # or it imports untrusted
cp ~/.gnupg/openpgp-revocs.d/"$FPR".rev   "$OUT/revocation.asc"  # the one people forget
```

All four plus the passphrase go in a password vault, then
`shred -u "$OUT"/*; rm -rf "$OUT"`. Verify the backup restores *before* you need
it, in a throwaway keyring:

```bash
export GNUPGHOME=$(mktemp -d -p /dev/shm)
gpg --import secret.asc && gpg --import-ownertrust ownertrust.txt
echo test | gpg --local-user "$FPR" --armor --detach-sign -o /dev/null  # proves the passphrase
rm -rf "$GNUPGHOME"; unset GNUPGHOME
```

### Variables

All optional; the defaults are shown.

| Variable | Default | What it is |
|---|---|---|
| `LAUNCHPAD_PPA` | `ppa:bceverly/tmd` | The PPA to upload to. Create one at `https://launchpad.net/~<you>/+activate-ppa` if it does not exist. |
| `LAUNCHPAD_SERIES` | `jammy noble resolute` | The Ubuntu series to build source packages for, space separated — 22.04, 24.04 and 26.04 LTS. One signed source package is built per series. |

### Identity is pinned in the repository, not in your shell

Two things that a Debian build normally reads from the environment are read
from the repository instead:

| What | Where | Why |
|---|---|---|
| the changelog trailer | `Maintainer` in `debian/control` | |
| the signing key | `debian/signing-fingerprint` | |

`DEBFULLNAME`, `DEBEMAIL` and `DEBSIGN_KEYID` are **deliberately ignored**, with
a warning when they are set to something different. They are global shell
variables, and a developer who maintains more than one Debian package has them
exported in their profile for whichever project they set up first — so tmd's
first clean-chroot build produced a package whose `Changed-By` named a
different project's signing identity entirely, and signed it with that
project's key. Which person a package is changed by, and which key signs it,
are properties of the package.

For a one-off override without editing anything:

```bash
TMD_SIGNING_KEY=<fingerprint> make deb-source
TMD_DEBFULLNAME="Someone Else" TMD_DEBEMAIL=someone@example.com make deb
```

Only the **source** package is signed — that signature is what Launchpad
authenticates the upload with. A local binary `.deb` is a test artifact nothing
verifies, so signing it only prompts for a passphrase nobody needed to type.

### What Launchpad actually requires

Worth stating plainly, because it is easy to assume more than is true and then
wait on something that was never needed:

- The `.changes` must be signed by a key **registered to a Launchpad account**
  with upload rights to the PPA. That is the real gate.
- That account must have signed the Ubuntu Code of Conduct.
- The changelog trailer — `DEBFULLNAME <DEBEMAIL>` — is where Launchpad sends
  the ACCEPTED or REJECTED mail. It does **not** have to equal the signing
  key's UID.

Matching them anyway is worth doing, for two reasons that have nothing to do
with Launchpad: a package whose changelog and signature name different people
is confusing a year later, and `debsign` selects a key by matching the address
when it is not told which one to use. `scripts/build-deb.sh` always passes
`-k$DEBSIGN_KEYID` explicitly, precisely because one developer commonly has
several keys sharing one email address and the wrong one gets picked silently.

### An environment, if you want a hand on the lever

The `publish` job declares `environment: launchpad`. Create an environment of
that name under **Settings → Environments** and add yourself as a required
reviewer, and every upload waits for a click. Leave it unconfigured and uploads
are automatic. Given that a PPA upload cannot be withdrawn, the manual approval
is worth considering.

### What is *not* needed

No `GITHUB_TOKEN` to create — Actions provides it. No package-registry token, no
Snyk or Sonar account, no Docker Hub credentials. The release does not create a
GitHub Release or push an image; it uploads source packages to Launchpad and
nothing else.

## Project layout

```
include/tmd.h        the types every module shares: an entry, an archive, the options
src/
  options.def        THE option table — expanded three times, for getopt, the
                     help text and the manpage generator
  opts.c             command-line parsing and the help
  source.c           where the bytes come from: a file, stdin, or memory
  tar.c              the reader — header parsing, every dialect, pax, sparse
  render.c           the four output modes
  util.c             allocation, a growable buffer, the field decoders
  main.c             open each archive, pump entries into the renderer
tests/
  tarbuild.c         builds tar archives byte by byte, including invalid ones
  test_*.c           the unit tests
  cli/run.sh         end-to-end, against real tar and bsdtar
  fuzz/fuzz_tar.c    the fuzz target: libFuzzer, or a built-in mutation loop
scripts/             one script per make target, all skip-if-missing
debian/              the packaging source; changelog is generated from VERSION
man/tmd.1            generated from --help; make lint fails if it is stale
docs/badges/         the coverage badge, regenerated by make coverage
```

The memory source in `source.c` is not a convenience for tests that happened to
be easier that way — it is what lets the whole reader run under the fuzzer
without touching the filesystem, and what lets a unit test build a deliberately
corrupt archive in a few lines.

## Design notes

A few decisions that are not obvious from the code, and that a reader would
otherwise have to rediscover.

**Both checksum conventions are accepted.** The header checksum is the sum of
all 512 bytes with the checksum field read as spaces. The original tar summed
`char`, which is signed on most platforms; later ones summed `unsigned char`.
The two agree until a header contains a byte over 127 — a non-ASCII name, a
base-256 numeric field — and then they differ by 256 per such byte. Rejecting
one of them means calling a perfectly good archive corrupt, so both are computed
and either matching is a pass.

**Bad checksums are counted at the archive level, not per entry.** A GNU
long-name block and a pax extended header are headers with checksums of their
own, and they belong to the member that follows rather than being members
themselves. Checking only the entry's own checksum missed damage to one of those
entirely — the corrupt block was an `L` header, and the entry it produced
carried the *next* header's perfectly good checksum.

**Numbers saturate rather than wrap.** A size field can say 2<sup>64</sup>−1.
Rounding that up to a block boundary the obvious way yields 0, which turns "skip
the payload" into "skip nothing" and reads the same header forever.

**A member that cannot carry data has its size ignored.** A directory with a
4096-byte size field is not a directory with a payload; believing it would
consume the next header. The size is discarded with a warning.

**One zero block is not an end marker.** Two are. GNU tar reads straight through
a single one, which is what makes archives concatenated with `cat` readable, so
`tmd` does too — loudly, because it is also what a corrupt block looks like.

**pax records are read by their length prefix, never split on newlines.** The
prefix is what lets a value contain a newline, and splitting on `\n` is the bug
every first implementation of pax has. There is a test whose whole purpose is to
fail if somebody reintroduces it.

**Allocation aborts rather than returning NULL.** This is a short-lived tool
reading one file; there is nothing useful to do about a failed 40-byte
allocation except die, and threading a NULL check through every parser would add
far more places to get the error handling wrong than it removes. Everything that
allocates a *caller-controlled* amount is bounded before it gets there.

## Roadmap

Ideas for future versions live in [ROADMAP.md](ROADMAP.md) — including finding
a member by name, archive diffing, and tar-bomb detection.

---

Copyright (c) 2026 Bryan C. Everly. Licensed under the [BSD 2-Clause
License](LICENSE).
