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
| [Read compressed archives directly](#read-compressed-archives-directly-v1600) | **Shipped** in v1.6.0.0 |
| [`--diff` between two archives](#--diff-between-two-archives-v1500) | **Shipped** in v1.5.0.0 |
| [`--verify` against a manifest](#--verify-against-a-manifest-v1500) | **Shipped** in v1.5.0.0 |
| [Promote `LIBARCHIVE.creationtime` to a `created` line](#promote-libarchivecreationtime-to-a-created-line-v1500) | **Shipped** in v1.5.0.0 |
| [Report the *order* members appear in](#report-the-order-members-appear-in-v1500) | **Shipped** in v1.5.0.0 |
| [Reproduce the extension blocks, not just the member header](#reproduce-the-extension-blocks-not-just-the-member-header-v1500) | **Shipped** in v1.5.0.0 |
| [Detect tar bombs](#detect-tar-bombs-v1400) | **Shipped** in v1.4.0.0 |
| [A `--stat` mode](#a---stat-mode-v1400) | **Shipped** in v1.4.0.0 |
| [Find a member by name: `-m` / `--match`](#find-a-member-by-name--m----match-pattern-v1300) | **Shipped** in v1.3.0.0 |
| [`--sort` for the listing](#--sort-for-the-listing-v1300) | **Shipped** in v1.3.0.0 |
| [Exhaustive JSON, and a `-t` spelling for it](#exhaustive-json-output-and-a--t-spelling-for-it-v1200) | **Shipped** in v1.2.0.0 |
| [More architectures in CI](#more-architectures-in-ci--the-aarch64-half-v1600) | **aarch64 shipped** in v1.6.0.0; [big-endian declined](#a-big-endian-ci-leg) |
| [Build and test on macOS and the BSDs](#build-and-test-on-macos-and-the-bsds) | **Shipped**: macOS natively, the three BSDs in QEMU |

Nothing open. Fourteen shipped, one declined.

The roadmap has been worked through. New ideas go under
[Other ideas](#other-ideas); anything decided against goes to
[Considered and declined](#considered-and-declined) with the reason, so it does
not come back as a suggestion a year from now.

The minor moves each time the command line grows — the rule v1.2.0.0 set when
`-t` was added. v1.5.0.0 took `--diff`, `--verify` and exit status 5 together
with three items that added no switch but did add JSON keys, because a minor
version is the unit being spent either way. v1.6.0.0 adds no switch at all:
compressed archives are recognized by content, so nothing new had to be typed —
but it changes what `tmd -f a.tar.gz` *does*, and adds a runtime dependency the
package declares, which is more than a patch should carry. The macOS and BSD
work stays on the patch digit by the same rule read the other way: new CI legs
and a build that works out its own link flags change nothing a user types and
nothing a user gets — the Linux binary is byte for byte what it was.

---

## Other ideas

*(nothing yet)*

Sketches go here first — unordered, and not thought through as far as a worked
item. Everything that was here has shipped.

---

## Shipped

### Read compressed archives directly (v1.6.0.0)

**What shipped:** `.tar.gz`, `.tar.xz`, `.tar.bz2`, `.tar.zst`, `.tar.lz4`,
`.tar.lz` and `.tar.Z`, recognized by content rather than extension and unpacked
on the way in — from a file, from a redirect, or from a pipe with nothing
seekable behind it.

**The sketch's premise was wrong about the cost.** It assumed "doing it
internally means linking zlib", and concluded the feature had to be
`dlopen`-on-demand or a compile-time option off by default. There is a third
way, and it is what GNU tar has always done: `tar -z` forks gzip. Running the
system's decompressor as a separate process costs no linked library at all, so
the "libc only" property survives intact — `make security` still checks it with
ldd and would fail if anything else appeared — and it covers every format in the
table at once instead of one library per format.

It is also the better trade on the merits, not just the cheaper one: the code
that parses hostile *compressed* bytes then runs in a different process from the
code that parses hostile *tar* headers.

Three things the sketch did not raise, each of which turned out to matter:

- **Standard input cannot be rewound.** tmd has to read the first bytes to learn
  what the file is, and a pipe will not give them back. So there are three
  processes: a feeder that writes the bytes already read and then copies the
  rest, the decompressor, and tmd. A seekable file could be rewound instead, but
  one path that always works beats two that each work sometimes.
- **A truncated stream must not look like a short archive.** A corrupt `.tar.gz`
  decompresses part way and stops, and what reaches the reader is a
  well-formed *prefix*. Reporting that as a complete listing with exit 0 was the
  behavior on the first attempt, and it is a silent wrong answer; tmd now checks
  the decompressor's exit status.
- **A missing tool must say so.** execvp failing looks exactly like an empty
  file, so the first bytes of the decompressed stream are read at open time and
  a 127 exit is reported as "gzip is not installed".

The Debian package declares the tools as runtime dependencies: `xz-utils` as a
Depends, `bzip2` and `zstd` as Recommends, `lz4` and `lzip` as Suggests. `gzip`
is deliberately absent — it is Essential on every Debian system, and depending
on an essential package is a Policy violation lintian rejects.

### More architectures in CI — the aarch64 half (v1.6.0.0)

**What shipped:** a `Tests (aarch64)` job on GitHub's native arm runners,
building and running the unit and end-to-end suites there.

**Half the item, and the half worth having first.** Everything else runs on
x86-64, where three things are true that are not true everywhere: little-endian,
64-bit `time_t`, and *signed* plain `char`. aarch64 moves the last of those —
`char` is unsigned there — and exercises a different ABI, on native hardware, so
it costs no more than the x86 legs.

It is still little-endian, so a genuine byte-order bug would survive it. The
qemu-based big-endian leg the sketch asked for is slower by a factor of ten or
more and belongs in a scheduled job rather than on every push, so it stays open.

The job asserts `uname -m` is aarch64 before doing anything else: a runner label
that silently fell back to x86 would leave this reporting success while testing
nothing.

### Build and test on macOS and the BSDs

**What shipped:** four jobs. `Tests (macOS)` on GitHub's native macOS runners
— build, `-Werror` build, both suites, a staged `make install`, and reading a
real `.tar.gz` from a file and from a pipe — then `Tests (FreeBSD)`,
`Tests (NetBSD)` and `Tests (OpenBSD)` doing the same inside QEMU virtual
machines, because GitHub has no BSD runners. Plus the portability work all four
needed, and from-source sections in the README for each.

**Two things about the build were true only on Linux, and nothing said so.**

- The link hardening. `-Wl,-z,relro`, `-z now` and `-z noexecstack` are ELF
  concepts; Apple's linker rejects `-z` outright. They are now *probed* by
  linking a one-line program, so they are passed where they mean something and
  dropped where they do not.
- `-D_XOPEN_SOURCE=700`. Asking libc for strict POSIX also switches off the
  namespace Darwin declares `getopt_long` in — the one function here POSIX does
  not define. `-D_DARWIN_C_SOURCE` restores it there, and the BSDs, which put
  `getopt_long` outside POSIX too, do not get `_XOPEN_SOURCE` at all.

Neither could be checked without a Mac, which is why the job is the deliverable
rather than the flags.

**It also found a real bug, which is the argument for the whole item.** tmd
does not judge a compressed stream; it runs the system's decompressor and
reports what that said. It reads the first bytes while opening, so that a
decompressor which is not installed can be named rather than looking like an
empty file -- and when that read came back empty, the exit status was checked
for exactly one value and then dropped. Any other thing the decompressor was
saying went with it, so a corrupt archive was reported as "not a tar archive"
with nothing to say why. Which of the two paths an input takes belongs to the
decompressor, not to tmd: GNU gzip flushes the bytes it managed before failing,
so a half-written stream went the ordinary way and the bug stayed hidden;
Apple's buffers and emits nothing, so the same bytes went the other way and a
test that had passed since it was written went red. The bug was never
macOS-specific -- a gzip header with nothing after it reproduces it on Linux,
and that is the regression test.

**And one answer, not seven.** The feature macros moved into
`scripts/features.sh`, which the Makefile and all five analysis scripts now
ask. They had each carried their own copy of the Linux answer, so teaching the
Makefile about Darwin left lint, coverage, memcheck, security and fuzz still
asserting that `_XOPEN_SOURCE=700` alone was right -- every one of which would
have failed on a Mac, on a line about `getopt_long`, for a reason already
fixed somewhere else in the tree.

**The end-to-end suite had Linux assumptions of its own,** and they were the
larger share of the work: `tar` meaning GNU tar, `stat -c`, `date -d @SECONDS`,
and `script -qec`. Each is now detected once at the top of the suite and used
through a small function, so the same script runs on GNU and BSD userlands
without a fork. GNU tar arrives from Homebrew as `gtar`; bsdtar needs no
installing on macOS, because there it *is* the system `tar`, so the BSD-writer
half of the suite runs natively rather than being skipped.

**Then the three BSDs, in `vmactions/*-vm` virtual machines** — the repository
rsynced in, the build run inside, the actions pinned by commit rather than by
tag. Two packages are installed rather than worked around: `gmake`, because the
Makefile is GNU make thirty-five constructs deep and BSD make fails on its
syntax rather than degrading; and `bash`, because seventeen scripts here start
with it. Rewriting either to avoid a package that is one command away on all
three systems would cost more than it could return.

OpenBSD earned its place as the strictest of the three. It is the only system
where a tool this suite uses is *absent* rather than spelled differently — its
`head(1)` takes a line count and has no `-c` — and the only one with no `sudo`
at all, so `make install` learned `doas`, and `makewhatis` for the man index
where Linux has `mandb`.

### `--diff` between two archives (v1.5.0.0)

**What shipped:** `tmd -f old.tar -f new.tar --diff`, reporting added, removed
and changed members with the fields that differ — size, mode, mtime, kind and a
link's target — always ordered by path so a comparison can be diffed against
itself. Exit 5 when they differ.

**The sketch's premise turned out not to be the right build.** It said "the
schema 2 JSON is the enabler: a diff is a comparison of two documents rather
than a second parser". Comparing two rendered documents would have meant
building both in full, in memory, and then parsing them back — for an operation
whose second side only ever needs to look its own path up. What shipped indexes
the first archive and *streams* the second past it, keeping a digest of the
compared fields rather than whole entries. The JSON did enable this, but as the
thing that settled which fields are worth comparing, not as an intermediate
format.

`--verify` shares the machine underneath, which is what made it cheap.

### `--verify` against a manifest (v1.5.0.0)

**What shipped:** `--verify=FILE`, checking an archive against a list of
expected `SIZE PATH` lines, where `SIZE` may be `-` for "this path should be
here and I am not saying how big it is". Comments and blank lines are ignored.
Exit 5 on any disagreement.

tmd can produce a manifest from an archive it has read, so the round trip is a
one-liner with awk, and the format is simple enough that anything else can
produce one too.

Two decisions the sketch did not raise:

- **Its own wording.** A diff runs left to right in time, so "size 6 -> 26"
  reads correctly; a verify checks reality against a claim, where "size 999
  expected, 3 found" is what a person means. The JSON keys follow: `from`/`to`
  for a diff, `expected`/`found` for a verify.
- **An over-long line is refused, not split.** fgets hands the remainder back as
  the next line, which would turn one absurd path into two plausible-looking
  ones — a silently wrong answer from a file the caller may not have written.
  The clang analyzer's taint warning on the manifest path is what sent me
  looking for it.

### Promote `LIBARCHIVE.creationtime` to a `created` line (v1.5.0.0)

**What shipped:** a `created` line under `-l`, a mention on the `-i` timestamps
line, and `created` / `created_epoch` / `created_source` in the JSON. The value
was already parsed and in the pax list, so this was presentation, as predicted.

Only `LIBARCHIVE.creationtime` is accepted. star's and GNU's keyword tables were
checked and neither has a birth-time key, so a second spelling would mean
rendering a field no writer produces. The `-i` line says where a creation time
comes from — "written by libarchive on a BSD or a Mac" — because that is the
part a reader cannot guess: the platform gate means bsdtar on Linux never writes
one.

### Report the *order* members appear in (v1.5.0.0)

**What shipped:** a `member order` line (lexicographic, or not in path order)
and a `top level` line naming what extracting would create, both in the summary
and the `-i` report, plus an `order` object in the JSON.

**The sketch asked for a fingerprint identifying the writer, and that turned out
not to be sound.** The first implementation claimed "lexicographic, so it came
from a sorted list" and "unsorted with directories, so it was a directory walk".
Both were caught being wrong while being tested: `tar -c DIR` over a small tree
came out in exact lexicographic order because readdir returned it that way, and
a reverse-sorted file list came out unsorted with directory members present. So
the ordering is reported as an observation and the reader draws their own
conclusion.

One inference does follow and is stated: no directory members means a list of
files, because a directory walk emits the directories it walks through.

The `top level` line was not in the sketch and is the more useful half. It
answers the *older* sense of "tar bomb" — an archive that unpacks two hundred
entries into the current directory rather than one tidy one — which is a
different question from the path escapes shipped in v1.4.0.0 and sits naturally
beside them.

### Reproduce the extension blocks, not just the member header (v1.5.0.0)

**What shipped:** `raw.extension_blocks` under `-R`, each entry carrying the
block's offset, the typeflag of the header that introduced it (`L`, `K`, `x`,
`g`, or null for one of its payload blocks), and its 512 bytes as base64 —
payload padding included, because that is the one place in an archive where
bytes can sit that no field accounts for.

The JSON now accounts for every byte a member occupies, and the claim is
enforced rather than asserted: a test decodes every block, compares it against
the archive at its own offset, and checks that the captured count plus the
member's own header equals `blocks.header_blocks`. It runs over GNU and pax
archives, and passed on bsdtar's too.

The open questions, answered:

- *Where to put a variable number of blocks?* A fixed array of 32 per member,
  with `extension_blocks_truncated` when it was cut. A pax header is as large as
  its writer chose.
- *Is a 200-block pax header something to emit in full?* No. 32 blocks is 16KB
  and covers every long name and pax header a real writer produces.
- The reader does not keep the blocks at all unless `-R` asked for them, which
  also decided a thing the sketch did not raise: capturing means *reading* the
  payload padding where the normal path seeks past it, so the default path is
  left exactly as it was.

### Detect tar bombs (v1.4.0.0)

**What shipped:** the three classes the roadmap named — absolute paths, `..`
traversals, and links whose target leaves the tree — each counted separately,
because they are three different mistakes to make when extracting and a reader
wants to know which one this archive makes.

Reported three ways: a per-member warning (so it is loud by default and appears
in `-l` and the machine formats), a class line in the summary and the `-i`
report, and an `extraction` object in the JSON carrying a single `escapes`
boolean so a script reads one field instead of inferring safety from absent
keys.

Two decisions the sketch did not cover:

- **Traversals are counted by depth, not by searching for `..`.** `a/../b`
  extracts to `b`, inside the current directory, and a substring match would
  call it a bomb. A path is reported only when its depth actually falls below
  where it started. The same walk resolves a link target against the directory
  the link sits in, so `a/b/link -> ../../c` is fine and `../../../c` is not.
- **The clean case is stated, not left silent.** Every other line in the `-i`
  report appears only when there is something to say. This one always appears,
  because a reader asking "is this safe to unpack" gets no answer from silence —
  it reads as "tmd did not look".

It stays pure reporting, as the roadmap said: no refusal, no new exit status.

### A `--stat` mode (v1.4.0.0)

**What shipped:** `--stat`, reporting sizes (extracted, stored, padding and its
share of the archive), the largest few members, and the timestamp distribution —
distinct count, earliest, latest, span, and the modal timestamp with how many
members share it.

The roadmap called the timestamp half "the more useful", and it is the half that
needed the work. `--stat` states the inference the roadmap's own table
describes: one timestamp everywhere reads as normalized, one timestamp that is
the epoch reads as *discarded* rather than normalized, a handful within an hour
reads as an export followed by a release script, and a spread over months reads
as real per-file times. Phrased as an inference, because that is what it is.

It also reports what a one-member-per-line listing cannot show: members with no
mtime, members dated in the future, members older than tar itself (1979), and
members dated before 1970.

Three decisions the sketch did not cover:

- **The distinct count is capped at 4096**, as suggested, and the cap is
  enforced with a hash table rather than a scan: a linear search through up to
  4096 values for each of 200,000 members is 800 million comparisons, which
  turns a report into a wait.
- **`-m` narrows what `--stat` describes**, unlike the summary, which keeps
  describing the whole archive. A summary answers "what is this file", which a
  filter cannot change; a distribution answers "what is in this set", which is
  exactly what a filter selects. The `matched N of M` line says which is being
  read.
- **The epoch is worth separating from any other single timestamp.** Every
  member dated `0` is not a reproducible build being tidy, it is timestamps
  being thrown away, and for somebody trying to date a tarball that is the
  opposite of reassuring.

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

### A big-endian CI leg

**The idea:** every job runs on x86-64, which is little-endian. tmd claims to be
endian-clean; a qemu-based `s390x` or `ppc64` leg would prove it.

**Declined, after shipping the aarch64 half of the same item** (see
[More architectures in CI](#more-architectures-in-ci--the-aarch64-half-v1600)),
because the return does not justify what it costs to run.

The property it would test is true *by construction*, and checkably so: there is
not a single multi-byte load in the program. Every numeric field in a tar header
goes through `parse_octal` or `parse_base256`, both of which walk bytes one at a
time and shift them into place. No `memcpy` into an integer, no cast of a
`char *` to a wider pointer, no `htonl`. A byte-order bug has nowhere to live.

Against that: qemu emulation runs ten to fifty times slower, so the leg could
not run on every push — it would be a weekly job running a subset, which is the
kind of check that goes red on a Tuesday and gets looked at on a Friday.

**What was kept instead.** The aarch64 leg, which is native, costs no more than
the x86 legs, and moves a variable that x86 cannot: plain `char` is unsigned
there. That is a real difference with real bugs attached, and it runs on every
push.

**If this is ever revisited, pick 32-bit over big-endian.** An `i386` or `armhf`
leg costs the same emulation and exercises live code rather than proving
something already true: `render.c` guards a `time_t` narrower than 64 bits with

```c
seconds = (time_t)t->sec;
if ((int64_t)seconds != t->sec)     /* print the raw seconds instead */
```

and on x86-64 and aarch64 alike that branch is unreachable. A 32-bit leg would
be the first thing ever to run it — and it sits in the same function where a
timestamp bug has already been found once.
