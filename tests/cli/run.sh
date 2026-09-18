#!/usr/bin/env bash
#
# Copyright (c) 2026 Bryan C. Everly
# SPDX-License-Identifier: BSD-2-Clause
#
# End-to-end tests: the real binary, against archives written by the real tools.
#
#   tests/cli/run.sh ./bin/tmd
#
# The unit tests build archives byte by byte, which is the only way to test a
# header that no tool would ever write. These do the opposite and check the
# cases that matter most in practice: that what GNU tar and bsdtar actually
# produce is read correctly, and that tmd's own listing agrees with `tar -tvf`
# on the same archive — line for line, on every format both can write.
#
# Archives are built in a temporary directory that is removed on exit, so a
# failed run leaves nothing behind and no fixture can go stale.
#
# SC2002 ("useless cat") is disabled for this whole file, deliberately.
#
# Every `cat archive.tar | tmd` here exists to hand tmd a PIPE. The suggested
# `tmd < archive.tar` would hand it a regular file, which is seekable — so it
# would exercise the fseeko path that the -f tests already cover, and the
# read-and-discard fallback in tmd_source_skip would go untested. The cat is
# the test.
#
# Stated once rather than four times because there is no case in this file
# where a genuinely useless cat would matter, and because the rule keeps being
# rediscovered: shellcheck 0.11 does not raise it at all, 0.9 on the CI runner
# does, and a green local run has twice been followed by a red CI one for
# exactly this.
# shellcheck disable=SC2002

set -uo pipefail

TMD="${1:?usage: run.sh /path/to/tmd}"
[ -x "$TMD" ] || { echo "run.sh: $TMD is not executable" >&2; exit 1; }
TMD="$(cd "$(dirname "$TMD")" && pwd)/$(basename "$TMD")"

PASS=0
FAIL=0
SKIP=0
SKIPS=()

ok()   { PASS=$((PASS + 1)); printf '  \033[92m✓\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  \033[91m✗\033[0m %s\n' "$1"; }
note() { printf '    \033[2m%s\033[0m\n' "$1"; }

# A group of checks that did not run, and why.
#
# Counted and repeated at the end rather than only mentioned in passing. A run
# that skips something still says "ok" and still exits 0, so the skip is the one
# result a reader can miss -- and the number of checks is not a constant anyone
# can eyeball: this suite reported 185 on Linux and 184 on macOS for a week
# before anyone asked which one was missing, and the answer turned out to be a
# tool that was simply not installed there.
skip() { SKIP=$((SKIP + 1)); SKIPS+=("$1"); printf '  \033[93m-\033[0m %s\n' "$1"; }

# check NAME EXPECTED ACTUAL
check() {
  if [ "$2" = "$3" ]; then
    ok "$1"
  else
    bad "$1"
    note "expected: $2"
    note "actual:   $3"
  fi
}

check_contains() {
  if printf '%s' "$2" | grep -qF -- "$3"; then
    ok "$1"
  else
    bad "$1"
    note "output does not contain: $3"
    note "output was: $(printf '%s' "$2" | head -3)"
  fi
}

check_status() {
  if [ "$2" -eq "$3" ]; then
    ok "$1"
  else
    bad "$1"
    note "expected exit $3, got $2"
  fi
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

printf '\ntmd end-to-end tests\n'

# ---------------------------------------------------------------------------
# Portability
#
# The fixtures are built with the real tar and inspected with the system's own
# tools, and every one of those differs between GNU userland and BSD userland
# (macOS included). Named once here rather than at each of the thirty-odd
# places that would otherwise have to care.
# ---------------------------------------------------------------------------

# GNU tar, which the fixtures genuinely need: --format=gnu/v7/ustar/posix and
# --no-recursion are GNU spellings, and the archives they produce are the
# reference this suite checks tmd against. On macOS and the BSDs `tar` IS
# bsdtar, and GNU tar is a package installed as `gtar`.
if command -v gtar > /dev/null 2>&1 && gtar --version 2>&1 | grep -q 'GNU tar'; then
  TAR=gtar
elif tar --version 2>&1 | grep -q 'GNU tar'; then
  TAR=tar
else
  printf '\033[91mrun.sh: GNU tar is required to build the fixtures\033[0m\n' >&2
  printf '  macOS:   brew install gnu-tar\n' >&2
  printf '  FreeBSD: pkg install gtar\n' >&2
  printf '  NetBSD:  pkgin install gtar\n' >&2
  printf '  OpenBSD: pkg_add gtar\n' >&2
  exit 1
fi

# bsdtar, which half the format tests exist to read. Present under its own name
# on Linux (libarchive-tools); on macOS and the BSDs it is the system tar, so
# looking only for a command called "bsdtar" skipped those tests on the very
# systems where bsdtar is native.
if command -v bsdtar > /dev/null 2>&1; then
  BSDTAR=bsdtar
  HAVE_BSDTAR=1
elif tar --version 2>&1 | grep -qi 'bsdtar\|libarchive'; then
  BSDTAR=tar
  HAVE_BSDTAR=1
else
  BSDTAR=
  HAVE_BSDTAR=0
fi

# The permission bits of a file, as octal. GNU stat spells it -c %a, BSD -f %Lp.
if stat -c %a . > /dev/null 2>&1; then
  file_mode() { stat -c %a "$1"; }
else
  file_mode() { stat -f %Lp "$1"; }
fi

# Set a file's mtime from a Unix epoch.
#
# GNU touch takes -d @EPOCH and BSD touch does not, but both take
# -t CCYYMMDDhhmm.SS -- so the epoch is converted first, with a date(1) that is
# itself spelled differently: GNU wants -d @N, BSD wants -r N. The probe uses
# the fact that GNU's -r takes a FILE, so `date -r 0` fails there and succeeds
# on BSD.
# Deliberately NOT date -u: `touch -t` reads its stamp as LOCAL time, so the
# conversion has to be local too or the mtime lands an offset away. Written with
# -u first, which round-tripped perfectly on a UTC machine and would have been
# five hours out for anyone else.
if date -r 0 '+%Y' > /dev/null 2>&1; then
  epoch_stamp() { date -r "$1" '+%Y%m%d%H%M.%S'; }
else
  epoch_stamp() { date -d "@$1" '+%Y%m%d%H%M.%S'; }
fi
touch_epoch() {
  _stamp="$(epoch_stamp "$1")"
  shift
  touch -t "$_stamp" "$@"
}

# Counting lines.
#
# BSD wc pads its number into a column -- "       2" -- and GNU wc does not, so
# a `wc -l` compared against a literal passes on Linux and fails on macOS with
# the two numbers looking identical in the failure message. awk counts without
# formatting, and is in every userland this runs on.
count_lines() { awk 'END { print NR }'; }

# script(1), for the one check that needs a real terminal. util-linux spells it
# `script -qec CMD FILE`; the BSD and macOS one is `script -q FILE CMD`.
if script --version > /dev/null 2>&1; then
  tty_run() { script -qec "$1" /dev/null < /dev/null 2>/dev/null; }
else
  tty_run() { script -q /dev/null "$1" < /dev/null 2>/dev/null; }
fi

# ---------------------------------------------------------------------------
# A tree with one of everything a tar header can describe.
#
# umask is set first, so that the permission bits the fixtures end up with are
# the same everywhere. Without it a file is 0664 on a machine with umask 002 and
# 0644 on one with umask 022, and any check that names a mode passes on one and
# fails on the other. The tests that care about umask set their own, in a
# subshell.
# ---------------------------------------------------------------------------
umask 022
mkdir -p tree/sub/deep
echo "hello world" > tree/hello.txt
printf 'x%.0s' $(seq 1 5000) > tree/sub/big.bin
ln -s ../hello.txt tree/sub/link
ln tree/hello.txt tree/hardlink
chmod 755 tree/sub/deep
chmod 4755 tree/sub/big.bin
touch -t 202103040506.07 tree/hello.txt

# A path too long for a ustar header, so the long-name machinery is exercised.
LONG="tree/$(printf 'a%.0s' $(seq 1 95))/$(printf 'b%.0s' $(seq 1 95))"
mkdir -p "$LONG"
echo "deep" > "$LONG/file.txt"


# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ reading what the real tools write\033[0m\n'

for format in gnu ustar posix v7; do
  archive="$format.tar"
  # v7 and ustar cannot hold the long path, so they get the short tree.
  case "$format" in
    v7 | ustar) source_tree="tree/hello.txt tree/sub" ;;
    *)          source_tree="tree" ;;
  esac
  # shellcheck disable=SC2086  # the tree list is deliberately word-split
  if ! "$TAR" --format="$format" -cf "$archive" $source_tree 2>/dev/null; then
    skip "$format: this tar cannot write that format"
    continue
  fi

  out="$("$TMD" -f "$archive" 2>/dev/null)"
  status=$?
  check_status "$format: exits 0" "$status" 0
  check_contains "$format: lists a regular file" "$out" "tree/hello.txt"

  # The real check: tmd and tar must agree about every member, in order.
  # Fields are compared rather than whole lines, because the two pad their
  # columns differently and that is not a difference worth failing on.
  tar_view="$("$TAR" -tvf "$archive" 2>/dev/null \
              | awk '{ printf "%s %s %s %s\n", $1, $2, $3, $NF }')"
  tmd_view="$("$TMD" -f "$archive" 2>/dev/null \
              | awk '{ printf "%s %s %s %s\n", $1, $2, $3, $NF }')"
  if [ "$tar_view" = "$tmd_view" ]; then
    ok "$format: every member agrees with tar -tvf"
  else
    bad "$format: listing differs from tar -tvf"
    note "$(diff <(echo "$tar_view") <(echo "$tmd_view") | head -6 | tr '\n' ' ')"
  fi
done

if [ "$HAVE_BSDTAR" -eq 1 ]; then
  "$BSDTAR" -cf bsd.tar tree 2>/dev/null
  out="$("$TMD" -f bsd.tar -s 2>/dev/null)"
  check_contains "bsdtar: recognized as pax" "$out" "POSIX pax"
  check_contains "bsdtar: long path read from the pax header" \
                 "$("$TMD" -f bsd.tar 2>/dev/null)" "$LONG/file.txt"

  "$BSDTAR" --format=ustar -cf bsdustar.tar tree/hello.txt 2>/dev/null
  check_contains "bsdtar ustar: recognized as ustar" \
                 "$("$TMD" -f bsdustar.tar -s 2>/dev/null)" "POSIX ustar"
else
  skip "bsdtar format checks: no bsdtar on this system"
fi

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ the metadata itself\033[0m\n'

out="$("$TMD" -f gnu.tar 2>/dev/null)"
check_contains "the stored mtime is reported, not today's date" "$out" "2021-03-04"
check_contains "timestamps carry a zone marker by default" "$out" "Z"

# UTC by default, and --local opts out. Checked by comparing the two renderings
# of the same archive: they must differ unless the machine happens to be on UTC.
utc_out="$("$TMD" -f gnu.tar 2>/dev/null | head -3)"
loc_out="$("$TMD" -f gnu.tar --local 2>/dev/null | head -3)"
check_contains "the default rendering is marked UTC" "$utc_out" "Z"
if [ "$(date +%z)" = "+0000" ]; then
  note "this machine is on UTC, so --local cannot be told apart by value"
else
  check "--local renders differently from the UTC default" \
        "$([ "$utc_out" != "$loc_out" ] && echo differs)" "differs"
fi
check_contains "a symlink shows its target" "$out" "tree/sub/link -> ../hello.txt"
check_contains "a hard link is marked as one" "$out" "link to"
check_contains "setuid shows in the mode string" "$out" "rws"
check_contains "a directory is marked with d" "$out" "drwx"

out="$("$TMD" -f gnu.tar -n 2>/dev/null)"
check_contains "-n prints numeric ids" "$out" "$(id -u)/$(id -g)"

out="$("$TMD" -f gnu.tar -H 2>/dev/null)"
check_contains "-H prints human sizes" "$out" "4.9K"

out="$("$TMD" -f gnu.tar -l 2>/dev/null)"
check_contains "-l names the format" "$out" "format      gnu"
check_contains "-l reports the offset in the archive" "$out" "at offset"

out="$("$TMD" -f gnu.tar -R 2>/dev/null)"
check_contains "-R shows the raw magic field" "$out" 'magic     "ustar "'

out="$("$TMD" -f gnu.tar -S 2>/dev/null)"
check_contains "-S reports the end-of-archive marker" "$out" "end marker    present"
check_contains "-S counts the members" "$out" "members"

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ sparse files\033[0m\n'

# 10 MB of nothing: the archive should be a few kilobytes, and the listing
# should still report the size the file expands to.
dd if=/dev/zero of=tree/sparse.bin bs=1 count=0 seek=10000000 2>/dev/null
if "$TAR" --sparse -cf sparse-gnu.tar tree/sparse.bin 2>/dev/null; then
  out="$("$TMD" -f sparse-gnu.tar -l 2>/dev/null)"
  check_contains "GNU sparse: the expanded size is reported" "$out" "10000000 bytes"
  check_contains "GNU sparse: the member is marked sparse" "$out" "sparse "
fi
if "$TAR" --format=posix --sparse -cf sparse-pax.tar tree/sparse.bin 2>/dev/null; then
  out="$("$TMD" -f sparse-pax.tar -l 2>/dev/null)"
  check_contains "pax sparse 1.0: the expanded size is reported" "$out" "10000000 bytes"
  check_contains "pax sparse 1.0: the real name is recovered" "$out" "tree/sparse.bin"
fi
rm -f tree/sparse.bin

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ --info: what the archive is\033[0m\n'

out="$("$TMD" -f gnu.tar -i 2>/dev/null)"
check_contains "-i names the format generation" "$out" "2nd, GNU branch"
check_contains "-i identifies the writer" "$out" "written by       GNU tar"
check_contains "-i lists the extensions in use" "$out" "GNU long name blocks"
check_contains "-i says what is needed to read it" "$out" "needs a reader"
check_contains "-i reports the end marker" "$out" "end marker       present"
check_contains "-i reports integrity" "$out" "every header checksum matched"
check "-i prints no member listing" \
      "$(printf '%s' "$out" | grep -c 'hello.txt' || true)" "0"

out="$("$TMD" -f v7.tar -i 2>/dev/null)"
check_contains "a v7 archive is named as the first generation" "$out" "1979"
check_contains "a v7 archive needs nothing special to read" "$out" "any tar at all"

if [ "$HAVE_BSDTAR" -eq 1 ]; then
  out="$("$TMD" -f bsd.tar -i 2>/dev/null)"
  check_contains "bsdtar is told apart from GNU tar by its pax header names" \
                 "$out" "libarchive (bsdtar)"
  check_contains "a pax archive is named as the third generation" "$out" "1003.1-2001"
else
  skip "bsdtar writer identification: no bsdtar on this system"
fi

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ -m, the member filter\033[0m\n'

out="$("$TMD" -f gnu.tar -m hello.txt 2>/dev/null)"
check_contains "a bare name matches the basename at any depth" "$out" "tree/hello.txt"
if printf '%s' "$out" | grep -q 'big.bin'; then
  bad "-m let through a member that does not match"
else
  ok "-m keeps out what does not match"
fi

# The offset is the point of the feature: it is what tells two members with the
# same path apart.
check_contains "a matched line carries the member's offset" "$out" "@"

out="$("$TMD" -f gnu.tar -m '*.bin' 2>/dev/null)"
check_contains "a glob matches the basename" "$out" "big.bin"

out="$("$TMD" -f gnu.tar -m 'tree/sub/*' 2>/dev/null)"
check_contains "a pattern with a slash matches the whole path" "$out" "tree/sub/"

out="$("$TMD" -f gnu.tar -m hello.txt -m '*.bin' 2>/dev/null)"
check_contains "several -m patterns are an either/or (1)" "$out" "hello.txt"
check_contains "several -m patterns are an either/or (2)" "$out" "big.bin"

# It is a filter, so it must not need to seek: the pipe form has to work too.
out="$(cat gnu.tar | "$TMD" -m hello.txt 2>/dev/null)"
check_contains "-m works on a pipe, without buffering" "$out" "tree/hello.txt"

"$TMD" -f gnu.tar -m hello.txt > /dev/null 2>&1
check_status "a match exits 0" "$?" 0
"$TMD" -f gnu.tar -m 'no-such-member.xyz' > /dev/null 2>&1
check_status "no match exits 4" "$?" 4
# An unreadable archive is the more important answer than "no match".
"$TMD" -f /nonexistent.tar -m 'anything' > /dev/null 2>&1
check_status "an unreadable archive still exits 1, not 4" "$?" 1

out="$("$TMD" -f gnu.tar -m '*.bin' -S 2>/dev/null)"
check_contains "the summary reports what matched" "$out" "matched"
# ...and still describes the whole archive, not the filtered subset.
check_contains "the summary still counts every member" "$out" "members"

# ---------------------------------------------------------------------------
# The reason this belongs in tmd rather than in `tar -t | grep`: an archive can
# hold the same path twice (tar -r appends, an incremental backup re-adds a
# changed file), extraction silently keeps the last, and nothing else will show
# you both.
cp gnu.tar dup.tar
echo "a replacement, longer than the original" > tree/hello.txt
"$TAR" --format=gnu -rf dup.tar tree/hello.txt 2>/dev/null
dups="$("$TMD" -f dup.tar -m hello.txt 2>/dev/null | grep -c 'tree/hello.txt')"
check "both copies of a duplicated path are reported" "$dups" "2"
offsets="$("$TMD" -f dup.tar -m hello.txt 2>/dev/null | grep -oE '@[0-9]+' | sort -u | count_lines)"
check "each copy reports a different offset" "$offsets" "2"

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ compressed archives\033[0m\n'

# Read directly, by running the system's decompressor. Each format is skipped
# rather than failed when its tool is not installed: tmd's behavior there is a
# clear message, which is checked separately below.
# The control is an UNCOMPRESSED archive of the same tree, written by the same
# tar. Comparing against the gnu.tar fixture instead compared two different sets
# of members, which is what the first version of this did.
"$TAR" -cf plainctl.tar tree 2>/dev/null
plain="$("$TMD" -f plainctl.tar 2>/dev/null)"

for spec in "gz:gzip:-z" "xz:xz:-J" "bz2:bzip2:-j" "zst:zstd:--zstd"; do
  ext="${spec%%:*}"
  rest="${spec#*:}"
  tool="${rest%%:*}"
  flag="${rest#*:}"

  if ! command -v "$tool" > /dev/null 2>&1; then
    skip ".tar.$ext: $tool is not installed"
    continue
  fi
  if ! "$TAR" "$flag" -cf "c.tar.$ext" tree 2>/dev/null; then
    note "this tar cannot write .tar.$ext — skipping"
    continue
  fi

  # The listing must be identical to the uncompressed one: the compression is
  # a wrapper, not a difference.
  got="$("$TMD" -f "c.tar.$ext" 2>/dev/null)"
  if [ "$got" = "$plain" ]; then
    ok ".tar.$ext reads exactly as the uncompressed archive does"
  else
    bad ".tar.$ext did not match the uncompressed listing"
  fi

  named="$("$TMD" -f "c.tar.$ext" -i 2>/dev/null | grep -c "compression")"
  check ".tar.$ext is named as $tool-compressed in the report" "$named" "1"

  # And through standard input, all three ways of getting it there. This is the
  # case the design turns on: the first bytes have already been read to identify
  # the file, a pipe cannot be rewound to give them back, so they have to be
  # handed to the decompressor by the process that feeds it.
  #
  #   cat FILE | tmd   a real pipe, nothing seekable behind it
  #   tmd < FILE       stdin redirected from a file
  #   tmd -f -         the same, named explicitly
  route_ok=1
  for piped in "$(cat "c.tar.$ext" | "$TMD" 2>/dev/null)" \
               "$("$TMD" < "c.tar.$ext" 2>/dev/null)" \
               "$("$TMD" -f - < "c.tar.$ext" 2>/dev/null)"; do
    if [ "$piped" != "$plain" ]; then
      route_ok=0
    fi
  done
  if [ "$route_ok" = "1" ]; then
    ok ".tar.$ext reads the same piped, redirected and as -f -"
  else
    bad ".tar.$ext differs when it arrives on standard input"
  fi
done

# An uncompressed archive must not claim to be compressed.
plain_codec="$("$TMD" -f gnu.tar -i 2>/dev/null | grep -c 'compression' || true)"
check "a plain archive reports no compression" "$plain_codec" "0"

# A decompressor that is not installed is a clear message, not an empty archive.
if command -v gzip > /dev/null 2>&1; then
  "$TAR" -z -cf missing.tar.gz tree 2>/dev/null
  err="$(PATH=/nonexistent "$TMD" -f missing.tar.gz 2>&1 >/dev/null || true)"
  check_contains "a missing decompressor says which tool is missing" "$err" \
                 "gzip is not installed"
else
  skip "the missing-decompressor check: gzip is not installed"
fi

# Corrupt compressed data must not be reported as a short but valid archive:
# what came out is a prefix, and saying so is the whole point.
if command -v gzip > /dev/null 2>&1; then
  "$TAR" -z -cf trunc.tar.gz tree 2>/dev/null
  head -c 120 trunc.tar.gz > cut.tar.gz
  "$TMD" -f cut.tar.gz > /dev/null 2>&1
  check_status "a truncated .tar.gz is an error, not a partial success" "$?" 1
  err="$("$TMD" -f cut.tar.gz 2>&1 >/dev/null || true)"
  check_contains "and says the stream ended badly" "$err" "ended badly"
else
  skip "the truncated-.tar.gz checks: gzip is not installed"
fi

# A container that is not tar in a wrapper still says what it is.
printf 'PK\003\004rest of it' > notatar.zip
err="$("$TMD" -f notatar.zip 2>&1 >/dev/null || true)"
check_contains "a zip is named, not guessed at" "$err" "zip data, not a tar archive"

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ --diff\033[0m\n'

mkdir -p dtree
echo "original"      > dtree/keep.txt
echo "small"         > dtree/changes.txt
echo "gone"          > dtree/removed.txt
# The starting mode is set, not inherited. A new file is 0664 under umask 002
# and 0644 under umask 022, so asserting on whichever this machine happens to
# use is a test that passes at home and fails on somebody else's runner --
# which is exactly what it did.
chmod 644 dtree/keep.txt
touch_epoch 1700000000 dtree/keep.txt dtree/changes.txt dtree/removed.txt dtree
"$TAR" --format=gnu -cf old.tar dtree 2>/dev/null

echo "a much longer replacement" > dtree/changes.txt
rm dtree/removed.txt
echo "new" > dtree/added.txt
chmod 600 dtree/keep.txt
touch_epoch 1700000000 dtree/changes.txt dtree/added.txt dtree/keep.txt dtree
"$TAR" --format=gnu -cf new.tar dtree 2>/dev/null

out="$("$TMD" -f old.tar -f new.tar --diff 2>/dev/null)"
check_contains "--diff reports an added member" "$out" "+ dtree/added.txt"
check_contains "--diff reports a removed member" "$out" "- dtree/removed.txt"
check_contains "--diff names the field that changed" "$out" "size 6 -> 26"
check_contains "--diff notices a mode change" "$out" "mode 0644 -> 0600"

# Sorted by path, so a comparison can be diffed against itself.
first="$("$TMD" -f old.tar -f new.tar --diff 2>/dev/null | sed -n '3p')"
check_contains "--diff output is ordered by path" "$first" "dtree/added.txt"

"$TMD" -f old.tar -f new.tar --diff > /dev/null 2>&1
check_status "archives that differ exit 5" "$?" 5
"$TMD" -f old.tar -f old.tar --diff > /dev/null 2>&1
check_status "identical archives exit 0" "$?" 0

# The comparison must be symmetric: what is added one way is removed the other.
a="$("$TMD" -f old.tar -f new.tar --diff 2>/dev/null | grep -c '^+')"
b="$("$TMD" -f new.tar -f old.tar --diff 2>/dev/null | grep -c '^-')"
check "added one way is removed the other" "$a" "$b"

"$TMD" -f old.tar --diff > /dev/null 2>&1
check_status "--diff with one archive is a usage error" "$?" 2

if command -v python3 > /dev/null 2>&1; then
  matched="$("$TMD" -f old.tar -f new.tar --diff -t JSON 2>/dev/null | python3 -c '
import json, sys
print(json.load(sys.stdin)["diff"]["matches"])' 2>/dev/null)"
  check "--diff reports a verdict in JSON" "$matched" "False"
else
  skip "the --diff JSON verdict: python3 is not installed"
fi

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ --verify\033[0m\n'

# A manifest tmd can produce itself, which is the round trip that matters.
"$TMD" -f old.tar -t CSV 2>/dev/null | tail -n +2 | awk -F, '{print $10, $1}' > man.txt
"$TMD" -f old.tar --verify=man.txt > /dev/null 2>&1
check_status "an archive verifies against its own manifest" "$?" 0

{ echo "# a comment"; echo ""; echo "999 dtree/keep.txt"; echo "- dtree/changes.txt";
  echo "0 dtree/never-was.txt"; } > bad.txt
out="$("$TMD" -f old.tar --verify=bad.txt 2>/dev/null)"
check_contains "--verify reports a wrong size in its own words" "$out" \
               "size 999 expected"
check_contains "--verify reports a missing member" "$out" "dtree/never-was.txt"
check_contains "--verify reports an unexpected member" "$out" "not expected"
"$TMD" -f old.tar --verify=bad.txt > /dev/null 2>&1
check_status "a manifest that disagrees exits 5" "$?" 5

# "-" means the path is expected and its size is not being checked.
#
# Asserted on the member's own line rather than by grepping the whole report
# for "size": the first version did that, and matched the report's own header,
# because the fixture was called sizeless.txt.
printf -- '- dtree/keep.txt\n' > nosize.txt
out="$("$TMD" -f old.tar --verify=nosize.txt 2>/dev/null)"
if printf '%s' "$out" | grep -q '^~ dtree/keep.txt'; then
  bad "a sizeless manifest line still checked the size"
else
  ok "a sizeless manifest line checks only that the path is there"
fi

# A line too long to fit must not be split into two plausible-looking paths.
{ printf '0 dtree/keep.txt\n'; printf '5 '; printf 'x%.0s' $(seq 1 9000);
  printf '\n'; } > longline.txt
err="$("$TMD" -f old.tar --verify=longline.txt 2>&1 >/dev/null)"
check_contains "an over-long manifest line is refused, not split" "$err" \
               "longer than"

"$TMD" -f old.tar --verify=/nonexistent/manifest > /dev/null 2>&1
check_status "a missing manifest is a read error, not a mismatch" "$?" 1
"$TMD" -f old.tar -f new.tar --verify=man.txt > /dev/null 2>&1
check_status "--verify with two archives is a usage error" "$?" 2

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ member order and top-level entries\033[0m\n'

out="$("$TMD" -f gnu.tar -i 2>/dev/null)"
check_contains "the info report names the member order" "$out" "member order"
check_contains "and what extracting would create" "$out" "top level"

# An archive that unpacks many entries into the current directory — the older
# meaning of "tar bomb", and a different question from a path that escapes.
for i in 1 2 3 4 5 6 7 8 9 10; do echo x > "scatter$i.txt"; done
"$TAR" --format=gnu -cf scatter.tar scatter*.txt 2>/dev/null
out="$("$TMD" -f scatter.tar -i 2>/dev/null)"
check_contains "many top-level entries are called out" "$out" \
               "scatters them into the current directory"

# A file list carries no directory members, which a directory walk always does.
find tree -type f | LC_ALL=C sort > flist.txt
"$TAR" --format=gnu --no-recursion -cf flist.tar -T flist.txt 2>/dev/null
out="$("$TMD" -f flist.tar -i 2>/dev/null)"
check_contains "no directory members reads as a file list" "$out" \
               "written from a list of files"

# Sorted and reversed inputs, with recursion off so the order is exactly the
# list's. This is the check that the ordering test is measuring the archive and
# not tar's traversal.
find tree | LC_ALL=C sort > asc.txt
find tree | LC_ALL=C sort -r > desc.txt
"$TAR" --format=gnu --no-recursion -cf asc.tar -T asc.txt 2>/dev/null
"$TAR" --format=gnu --no-recursion -cf desc.tar -T desc.txt 2>/dev/null
out="$("$TMD" -f asc.tar -i 2>/dev/null)"
check_contains "a sorted archive is reported sorted" "$out" "lexicographic by path"
out="$("$TMD" -f desc.tar -i 2>/dev/null)"
check_contains "a reversed archive is reported unsorted" "$out" "not in path order"

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ raw extension blocks\033[0m\n'

# The claim -R makes is that the JSON accounts for every byte a member occupies.
# That is checkable: each captured block must equal the archive at its offset,
# and the captured count plus the member's own header must equal the member's
# header_blocks.
if command -v python3 > /dev/null 2>&1; then
  for archive in gnu.tar posix.tar; do
    verdict="$("$TMD" -f "$archive" -R -t JSON 2>/dev/null | python3 -c "
import json, sys, base64, pathlib
d = json.load(sys.stdin)
blob = pathlib.Path('$archive').read_bytes()
for e in d['entries']:
    raw = e.get('raw', {})
    ext = raw.get('extension_blocks', [])
    if base64.b64decode(raw['block_base64']) != blob[raw['block_offset']:raw['block_offset'] + 512]:
        print('member header not byte-exact'); sys.exit()
    for x in ext:
        if base64.b64decode(x['base64']) != blob[x['offset']:x['offset'] + 512]:
            print('extension block not byte-exact'); sys.exit()
    if len(ext) + 1 != e['blocks']['header_blocks']:
        print('blocks unaccounted for'); sys.exit()
print('accounted')" 2>/dev/null)"
    check "$archive: every header block is accounted for byte for byte" \
          "$verdict" "accounted"
  done

  # A GNU long name occupies three header blocks: the 'L' header, the name it
  # carries, and the member's own header. Two of those are extension blocks.
  kinds="$("$TMD" -f gnu.tar -R -t JSON 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
for e in d["entries"]:
    ext = e.get("raw", {}).get("extension_blocks", [])
    if len(ext) == 2:
        print("".join(x["kind"] or "-" for x in ext)); break' 2>/dev/null)"
  check "a long-name member carries an 'L' header and its payload" "$kinds" "L-"
else
  skip "the raw-block byte-for-byte checks: python3 is not installed"
fi

# Without -R there is nothing to report and nothing is paid for it.
if "$TMD" -f gnu.tar -t JSON 2>/dev/null | grep -q 'extension_blocks'; then
  bad "extension blocks were emitted without -R"
else
  ok "extension blocks are only emitted under -R"
fi

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ extraction safety\033[0m\n'

# An archive that would write outside the directory it is unpacked in. Built
# here rather than described, because the point is that tmd answers the question
# from the bytes, before anything has been extracted.
mkdir -p bomb/safe
echo ok > bomb/safe/a.txt
ln -sf /etc/passwd bomb/out-abs
ln -sf ../../../outside bomb/out-rel
"$TAR" --format=gnu -cf bomb.tar -C bomb safe out-abs out-rel 2>/dev/null

err="$("$TMD" -f bomb.tar 2>&1 >/dev/null)"
check_contains "a link out of the tree is reported by default" "$err" \
               "link target leaves the extraction directory"

out="$("$TMD" -f bomb.tar -i 2>/dev/null)"
check_contains "the info report names the escapes as a class" "$out" \
               "would extract OUTSIDE the current directory"
check_contains "and says how many links point out" "$out" "pointing outside"

# A clean archive has to say so. Silence would read as "tmd did not look".
out="$("$TMD" -f gnu.tar -i 2>/dev/null)"
check_contains "a clean archive is stated to be clean" "$out" \
               "every member stays inside the extraction directory"

# An absolute path is a different class from a traversal.
#
# The archived file is one this suite just made, not a system file. The first
# version used /etc/hostname, which exists on Linux and does not on macOS --
# the hostname there lives in scutil, not in a file. tar wrote nothing, `|| true`
# swallowed the error, and the guard below asked only whether abs.tar EXISTED,
# which it did: tar creates the file before it discovers it has nothing to put
# in it. So tmd was handed an empty archive and the check failed reporting an
# empty string, which says nothing about what went wrong.
#
# Hence both halves of the fix: archive a file that is certainly there, and
# believe tar's exit status rather than the presence of a file it may have
# abandoned. $PWD is the suite's own temporary directory, so this is an
# absolute path that exists on every system this runs on.
echo "archived by its absolute path" > absfile.txt
if "$TAR" -cPf abs.tar "$PWD/absfile.txt" 2>/dev/null && [ -s abs.tar ]; then
  err="$("$TMD" -f abs.tar 2>&1 >/dev/null)"
  check_contains "an absolute path is reported" "$err" "absolute path"
else
  skip "absolute paths: this tar would not write one"
fi

if command -v python3 > /dev/null 2>&1; then
  verdict="$("$TMD" -f bomb.tar -S -t JSON 2>/dev/null | python3 -c '
import json, sys
print(json.load(sys.stdin)["summary"]["extraction"]["escapes"])' 2>/dev/null)"
  check "the JSON answers the whole question with one boolean" "$verdict" "True"
  verdict="$("$TMD" -f gnu.tar -S -t JSON 2>/dev/null | python3 -c '
import json, sys
print(json.load(sys.stdin)["summary"]["extraction"]["escapes"])' 2>/dev/null)"
  check "and says False for an archive that is fine" "$verdict" "False"
else
  skip "the extraction-safety JSON verdict: python3 is not installed"
fi

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ --stat\033[0m\n'

# Three members sharing one second and one two minutes later: the shape a
# release script leaves, and the case --stat exists to name.
mkdir -p rel
echo a > rel/one
echo bb > rel/two
echo ccc > rel/three
touch_epoch 1700000000 rel/one rel/two rel/three rel
touch_epoch 1700000120 rel/one
"$TAR" --format=gnu -cf rel.tar rel 2>/dev/null

out="$("$TMD" -f rel.tar --stat 2>/dev/null)"
check_contains "--stat counts distinct timestamps, not members" "$out" "2 distinct"
check_contains "--stat reports the span" "$out" "2 minutes"
check_contains "--stat names the timestamp most members share" "$out" "most common"
check_contains "--stat infers how the archive was produced" "$out" \
               "exported into a fresh directory"
check_contains "--stat reports the padding" "$out" "padding"
check_contains "--stat lists the largest members" "$out" "largest"

# It replaces the listing rather than adding to it.
if printf '%s' "$out" | grep -q 'rel/one'; then
  ok "--stat lists the largest members by path"
else
  bad "--stat did not name any member"
fi

# One timestamp everywhere is a different finding from a spread.
touch_epoch 1700000000 rel/one rel/two rel/three rel
"$TAR" --format=gnu -cf norm.tar rel 2>/dev/null
out="$("$TMD" -f norm.tar --stat 2>/dev/null)"
check_contains "one timestamp for everything reads as normalized" "$out" "normalized"
check_contains "and the span says so plainly" "$out" "every member shares one second"

if command -v python3 > /dev/null 2>&1; then
  distinct="$("$TMD" -f rel.tar --stat -t JSON 2>/dev/null | python3 -c '
import json, sys
print(json.load(sys.stdin)["summary"]["stat"]["timestamps"]["distinct"])' 2>/dev/null)"
  check "--stat reaches the JSON" "$distinct" "2"
else
  skip "the --stat JSON check: python3 is not installed"
fi

# A filter narrows what the distribution describes.
out="$("$TMD" -f rel.tar --stat -m 'one' 2>/dev/null)"
check_contains "-m narrows the set --stat describes" "$out" "matched       1 of 4 members"

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ --sort\033[0m\n'

# Read the path from CSV rather than from the listing: the last field of a
# listing line is a symlink's TARGET, not its path, which is what the first
# version of this check compared and why it failed on an archive containing a
# symlink.
paths="$("$TMD" -f gnu.tar --sort=path -t CSV 2>/dev/null | tail -n +2 | cut -d, -f1)"
if [ "$paths" = "$(printf '%s\n' "$paths" | LC_ALL=C sort)" ]; then
  ok "--sort=path puts the paths in order"
else
  bad "--sort=path did not order the paths"
  note "got: $(printf '%s' "$paths" | tr '\n' ' ')"
fi

biggest="$("$TMD" -f gnu.tar --sort=size --reverse 2>/dev/null | head -1)"
check_contains "--sort=size --reverse puts the largest first" "$biggest" "big.bin"

# Sorting must not lose or invent members.
plain_count="$("$TMD" -f gnu.tar 2>/dev/null | count_lines)"
sorted_count="$("$TMD" -f gnu.tar --sort=size 2>/dev/null | count_lines)"
check "sorting changes the order, not the membership" "$plain_count" "$sorted_count"

# The same members, whatever the order: sorting both listings must make them
# identical.
a="$("$TMD" -f gnu.tar 2>/dev/null | sort)"
b="$("$TMD" -f gnu.tar --sort=mtime 2>/dev/null | sort)"
check "a sorted listing holds exactly the same lines" "$a" "$b"

"$TMD" -f gnu.tar --sort=nonsense > /dev/null 2>&1
check_status "an unknown sort key is a usage error" "$?" 2

# It has to work in the machine formats too, where the entries array is simply
# emitted in the sorted order.
if command -v python3 > /dev/null 2>&1; then
  ordered="$("$TMD" -f gnu.tar --sort=path -t JSON 2>/dev/null | python3 -c '
import json, sys
paths = [e["path"] for e in json.load(sys.stdin)["entries"]]
print("yes" if paths == sorted(paths) else "no")' 2>/dev/null)"
  check "--sort orders the JSON entries too" "$ordered" "yes"
else
  skip "the --sort JSON ordering check: python3 is not installed"
fi

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ output formats\033[0m\n'

# -t and --format are the same switch. --format is in a released manpage and
# keeps working; -t is the documented spelling from 1.2 on. Both cases of the
# type name are accepted, so all four spellings below must agree exactly.
for spelling in "-t JSON" "-t json" "--format=json"; do
  # shellcheck disable=SC2086
  if [ "$("$TMD" -f gnu.tar $spelling 2>/dev/null | head -c 200)" \
     = "$("$TMD" -f gnu.tar --format=json 2>/dev/null | head -c 200)" ]; then
    ok "\"$spelling\" selects JSON"
  else
    bad "\"$spelling\" does not match --format=json"
  fi
done
for spelling in "-t TXT" "-t txt" "--format=text"; do
  # shellcheck disable=SC2086
  if [ "$("$TMD" -f gnu.tar $spelling 2>/dev/null | head -1)" \
     = "$("$TMD" -f gnu.tar 2>/dev/null | head -1)" ]; then
    ok "\"$spelling\" selects the text listing"
  else
    bad "\"$spelling\" does not match the default listing"
  fi
done

"$TMD" -f gnu.tar -t XML > /dev/null 2>&1
check_status "an unknown output type is a usage error" "$?" 2

json="$("$TMD" -f gnu.tar --format=json 2>/dev/null)"
if command -v python3 > /dev/null 2>&1; then
  if printf '%s' "$json" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
    ok "json output parses"
  else
    bad "json output does not parse"
  fi
  schema="$(printf '%s' "$json" \
            | python3 -c 'import json,sys; print(json.load(sys.stdin)["schema"])' 2>/dev/null)"
  check "the document declares schema 2" "$schema" "2"

  # The point of the exhaustive output: the base64 header has to be the header,
  # byte for byte, or "a faithful description of the bytes" is just a slogan.
  exact="$("$TMD" -f gnu.tar -R --format=json 2>/dev/null | python3 -c '
import json, sys, base64, pathlib
d = json.load(sys.stdin)
blob = pathlib.Path("gnu.tar").read_bytes()
for e in d["entries"]:
    off = e["raw"]["block_offset"]
    if base64.b64decode(e["raw"]["block_base64"]) != blob[off:off + 512]:
        print("no"); sys.exit()
print("yes")' 2>/dev/null)"
  check "every raw header block round-trips byte for byte" "$exact" "yes"

  members="$(printf '%s' "$json" \
             | python3 -c 'import json,sys; print(json.load(sys.stdin)["summary"]["members"])' 2>/dev/null)"
  entries="$(printf '%s' "$json" \
             | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["entries"]))' 2>/dev/null)"
  check "the summary count matches the entry count" "$members" "$entries"

  # Two archives must produce a top-level array, not two objects glued together.
  multi="$("$TMD" -f gnu.tar -f ustar.tar --format=json 2>/dev/null \
           | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))' 2>/dev/null)"
  check "two archives produce a two-element array" "$multi" "2"
else
  skip "the JSON document checks: python3 is not installed"
fi

if command -v python3 > /dev/null 2>&1; then
  gen="$("$TMD" -f gnu.tar -i --format=json 2>/dev/null \
         | python3 -c 'import json,sys; print(json.load(sys.stdin)["summary"]["features"]["generation"])' 2>/dev/null)"
  check_contains "-i --format=json carries the generation" "$gen" "GNU branch"
else
  skip "the -i JSON check: python3 is not installed"
fi

csv="$("$TMD" -f gnu.tar --format=csv 2>/dev/null)"
check_contains "csv writes a header row" "$csv" "path,kind,mode_string"
csv_rows=$(printf '%s\n' "$csv" | tail -n +2 | count_lines)
tar_rows=$("$TAR" -tf gnu.tar | count_lines)
check "csv has one row per member" "$csv_rows" "$tar_rows"

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ the command line\033[0m\n'

# The usage text itself, via the explicit flag.
#
# Deliberately NOT a bare `tmd` here. Now that no -f means "read standard
# input", a bare invocation only prints the usage when stdin is a TERMINAL —
# and stdin in a test harness is a pipe or a file, so a bare `tmd` would sit
# there waiting for a tar archive to arrive. It did, for two minutes, which is
# how this test found out.
"$TMD" --help > help.txt 2>&1
check_status "--help succeeds" "$?" 0
check_contains "the usage names -f" "$(cat help.txt)" "-f, --file=FILE"
check_contains "the usage shows the stdin form" "$(cat help.txt)" "| tmd"

# And the bare-invocation-at-a-terminal behavior, which needs an actual tty.
# script(1) allocates one; where it is missing the check is skipped rather than
# silently dropped.
if command -v script > /dev/null 2>&1; then
  # `< /dev/null` and a grep over the WHOLE output, not `head -1`.
  #
  # script(1) copies its own stdin into the pty it allocates, and the pty
  # echoes what arrives — so whatever this suite inherited on stdin comes back
  # as the first line of output. Run from a git pre-push hook, that is git's
  # list of refs being pushed, and this check compared the banner against
  # "refs/heads/main 1c44b07 ...". Feeding script an empty stdin stops the
  # echo; grepping the whole output means an echo could not fool it anyway.
  tty_out="$(tty_run "$TMD")"
  check_contains "a bare tmd at a terminal shows the usage" \
                 "$tty_out" "dump the metadata out of a tar archive"
else
  skip "the terminal-detection check: script(1) is not installed"
fi
check_contains "the usage shows the copyright" "$(cat help.txt)" "Bryan C. Everly"

check "--help matches the no-argument output" \
      "$("$TMD" --help)" "$(cat help.txt)"
check "-h matches --help" "$("$TMD" -h)" "$("$TMD" --help)"

version="$("$TMD" --version)"
check_contains "--version prints the program name" "$version" "tmd "
check "-V matches --version" "$("$TMD" -V)" "$version"

"$TMD" --nonsense > /dev/null 2>&1
check_status "an unknown option is a usage error" "$?" 2
"$TMD" -f gnu.tar stray-argument > /dev/null 2>&1
check_status "a stray positional argument is a usage error" "$?" 2
"$TMD" --format=bogus -f gnu.tar > /dev/null 2>&1
check_status "an unknown --format is a usage error" "$?" 2

# -o writes the report and leaves stderr alone.
"$TMD" -f gnu.tar -o report.txt 2> stderr.txt
check_status "-o exits 0" "$?" 0
check "-o writes nothing to stdout" "$("$TMD" -f gnu.tar -o report2.txt)" ""
check "-o produces the same report as stdout" \
      "$(cat report.txt)" "$("$TMD" -f gnu.tar)"
check "-o leaves standard error empty for a clean archive" "$(cat stderr.txt)" ""
"$TMD" -f gnu.tar -o /nonexistent/dir/report.txt > /dev/null 2>&1
check_status "-o to an unwritable path fails" "$?" 1

# The report must not be group- or world-writable, whatever the umask is.
#
# Run under umask 000 on purpose: that is the only setting where the bug this
# guards against is visible. fopen(path, "w") creates with 0666 & ~umask, so
# with a normal 022 it lands on 0644 and looks correct, and with 000 it lands
# on 0666 — a world-writable integrity report, which is a report that proves
# nothing. CodeQL caught it; this keeps it caught.
( umask 000 && "$TMD" -f gnu.tar -o permissive.txt > /dev/null 2>&1 )
check "-o creates the report 0644 even under umask 000" \
      "$(file_mode permissive.txt 2>/dev/null)" "644"

# ...and a stricter umask is still honored, rather than forced back up to 644.
( umask 077 && "$TMD" -f gnu.tar -o strict.txt > /dev/null 2>&1 )
check "-o honors a stricter umask" \
      "$(file_mode strict.txt 2>/dev/null)" "600"

# No -f at all: standard input is read when it is not a terminal.
#
# This is the `gzip -dc a.tar.gz | tmd` form. It has to coexist with a bare
# `tmd` printing the usage, and the thing that separates them is whether stdin
# is a tty — which is why these tests pipe and redirect rather than just
# running the binary.
check "no -f, piped: reads standard input" \
      "$(cat gnu.tar | "$TMD" 2>/dev/null)" "$("$TMD" -f gnu.tar 2>/dev/null)"
check "no -f, redirected: reads standard input" \
      "$("$TMD" < gnu.tar 2>/dev/null)" "$("$TMD" -f gnu.tar 2>/dev/null)"
# The summary too, with the archive's own name dropped: that line is the one
# thing that SHOULD differ, since a pipe has no name to report. Everything else
# must match, including the byte count -- for a pipe there is no length until
# the stream ends, so it is measured while reading rather than stat'd, and the
# two have to agree.
check "no -f, piped, with options" \
      "$(cat gnu.tar | "$TMD" -s 2>/dev/null | tail -n +3)" \
      "$("$TMD" -f gnu.tar -s 2>/dev/null | tail -n +3)"
check "a piped archive reports the same size as the same file" \
      "$(cat gnu.tar | "$TMD" -s 2>/dev/null | grep '^  archive')" \
      "$("$TMD" -f gnu.tar -s 2>/dev/null | grep '^  archive')"

# With stdin closed off and no tty, an empty stream is an empty archive rather
# than a silent success — a pipeline that produced nothing must not look as
# though it worked.
"$TMD" < /dev/null > /dev/null 2>&1
check_status "no -f with an empty stdin is an error, not a silent pass" "$?" 1

# Reading a pipe.
#
# The cat is deliberate: see the SC2002 note at the top of this file.
check "reading from standard input matches reading the file" \
      "$(cat gnu.tar | "$TMD" -f - 2>/dev/null)" "$("$TMD" -f gnu.tar 2>/dev/null)"

# Two -f flags.
both="$("$TMD" -f gnu.tar -f ustar.tar 2>/dev/null | grep -c '==>' || true)"
check "two archives get a banner each" "$both" "2"

# ---------------------------------------------------------------------------
printf '\n\033[1;94m▸ damaged and mistaken input\033[0m\n'

# A .tar.gz used to be refused here, with a message naming the pipe to type.
# Since v1.5.0.0 it is read directly, so what this checks is the opposite: the
# wrapper is opened, and the listing is the same one the uncompressed archive
# gives. The refusal tests moved to the "compressed archives" section, where
# they now cover the cases that genuinely still fail -- a missing tool and a
# truncated stream.
gzip -c gnu.tar > gnu.tar.gz
check "a .tar.gz reads as the archive inside it" \
      "$("$TMD" -f gnu.tar.gz 2>/dev/null)" "$("$TMD" -f gnu.tar 2>/dev/null)"
"$TMD" -f gnu.tar.gz > /dev/null 2>&1
check_status "and succeeds" "$?" 0

# A .tar.gz the decompressor cannot get a single byte out of: a complete gzip
# header with nothing after it. The interesting part is that this reaches the
# code by a different route than a half-written stream does -- tmd reads the
# first bytes while opening, so a stream that yields nothing is finished before
# the reader ever runs -- and that route used to drop the decompressor's exit
# status on the floor. A corrupt archive then read as an empty one.
printf '\037\213\010\000\000\000\000\000\002\377' > headeronly.tar.gz
"$TMD" -f headeronly.tar.gz > /dev/null 2>&1
check_status "a .tar.gz that yields nothing is an error" "$?" 1
err="$("$TMD" -f headeronly.tar.gz 2>&1 > /dev/null)"
check_contains "and says nothing could be decompressed" \
               "$err" "nothing could be decompressed"

"$TMD" -f /dev/null > /dev/null 2>&1
check_status "an empty file is an error" "$?" 1
"$TMD" -f no-such-file.tar > /dev/null 2>&1
check_status "a missing file is an error" "$?" 1

echo "this is just text, and more than one block of it" > text.txt
for i in $(seq 1 40); do echo "padding line $i to get past 512 bytes" >> text.txt; done
"$TMD" -f text.txt > /dev/null 2>&1
check_status "a text file is not a tar archive" "$?" 1

head -c 3000 gnu.tar > truncated.tar
"$TMD" -f truncated.tar > /dev/null 2>&1
check_status "a truncated archive still lists what survived" "$?" 0
"$TMD" -f truncated.tar -c > /dev/null 2>&1
check_status "-c fails on a truncated archive" "$?" 3
err="$("$TMD" -f truncated.tar 2>&1 >/dev/null)"
check_contains "a truncated archive warns on stderr" "$err" "end-of-archive"
err="$("$TMD" -f truncated.tar -q 2>&1 >/dev/null)"
check "-q silences the warnings" "$err" ""

# A flipped byte inside a header: the checksum must catch it.
cp gnu.tar corrupt.tar
printf 'ZZZZZZZZ' | dd of=corrupt.tar bs=1 seek=$((512 + 148)) conv=notrunc 2>/dev/null
"$TMD" -f corrupt.tar -c > /dev/null 2>&1
check_status "-c fails on a bad header checksum" "$?" 3
"$TMD" -f corrupt.tar > /dev/null 2>&1
check_status "a bad checksum alone is not a failure without -c" "$?" 0

# A clean archive must pass the check it is given.
"$TMD" -f gnu.tar -c > /dev/null 2>&1
check_status "-c passes on an undamaged archive" "$?" 0

# ---------------------------------------------------------------------------
printf '\n'
if [ "$SKIP" -gt 0 ]; then
  printf '  \033[93m%d group(s) skipped:\033[0m\n' "$SKIP"
  for entry in "${SKIPS[@]}"; do
    printf '    \033[2m%s\033[0m\n' "$entry"
  done
  printf '\n'
fi
if [ "$FAIL" -eq 0 ]; then
  printf '  \033[92mok\033[0m  %d end-to-end checks passed\n\n' "$PASS"
  exit 0
fi
printf '  \033[91m%d of %d end-to-end checks failed\033[0m\n\n' "$FAIL" "$((PASS + FAIL))"
exit 1
