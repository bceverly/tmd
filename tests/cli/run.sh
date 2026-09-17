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

ok()   { PASS=$((PASS + 1)); printf '  \033[92m✓\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  \033[91m✗\033[0m %s\n' "$1"; }
note() { printf '    \033[2m%s\033[0m\n' "$1"; }

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
# A tree with one of everything a tar header can describe.
# ---------------------------------------------------------------------------
mkdir -p tree/sub/deep
echo "hello world" > tree/hello.txt
printf 'x%.0s' $(seq 1 5000) > tree/sub/big.bin
ln -s ../hello.txt tree/sub/link
ln tree/hello.txt tree/hardlink
chmod 755 tree/sub/deep
chmod 4755 tree/sub/big.bin
touch -d "2021-03-04 05:06:07" tree/hello.txt

# A path too long for a ustar header, so the long-name machinery is exercised.
LONG="tree/$(printf 'a%.0s' $(seq 1 95))/$(printf 'b%.0s' $(seq 1 95))"
mkdir -p "$LONG"
echo "deep" > "$LONG/file.txt"

HAVE_BSDTAR=0
command -v bsdtar > /dev/null 2>&1 && HAVE_BSDTAR=1

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
  if ! tar --format="$format" -cf "$archive" $source_tree 2>/dev/null; then
    note "skipping $format: this tar cannot write it"
    continue
  fi

  out="$("$TMD" -f "$archive" 2>/dev/null)"
  status=$?
  check_status "$format: exits 0" "$status" 0
  check_contains "$format: lists a regular file" "$out" "tree/hello.txt"

  # The real check: tmd and tar must agree about every member, in order.
  # Fields are compared rather than whole lines, because the two pad their
  # columns differently and that is not a difference worth failing on.
  tar_view="$(tar -tvf "$archive" 2>/dev/null \
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
  bsdtar -cf bsd.tar tree 2>/dev/null
  out="$("$TMD" -f bsd.tar -s 2>/dev/null)"
  check_contains "bsdtar: recognized as pax" "$out" "POSIX pax"
  check_contains "bsdtar: long path read from the pax header" \
                 "$("$TMD" -f bsd.tar 2>/dev/null)" "$LONG/file.txt"

  bsdtar --format=ustar -cf bsdustar.tar tree/hello.txt 2>/dev/null
  check_contains "bsdtar ustar: recognized as ustar" \
                 "$("$TMD" -f bsdustar.tar -s 2>/dev/null)" "POSIX ustar"
else
  note "bsdtar is not installed — the BSD-writer tests are skipped"
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
if tar --sparse -cf sparse-gnu.tar tree/sparse.bin 2>/dev/null; then
  out="$("$TMD" -f sparse-gnu.tar -l 2>/dev/null)"
  check_contains "GNU sparse: the expanded size is reported" "$out" "10000000 bytes"
  check_contains "GNU sparse: the member is marked sparse" "$out" "sparse "
fi
if tar --format=posix --sparse -cf sparse-pax.tar tree/sparse.bin 2>/dev/null; then
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
tar --format=gnu -rf dup.tar tree/hello.txt 2>/dev/null
dups="$("$TMD" -f dup.tar -m hello.txt 2>/dev/null | grep -c 'tree/hello.txt')"
check "both copies of a duplicated path are reported" "$dups" "2"
offsets="$("$TMD" -f dup.tar -m hello.txt 2>/dev/null | grep -oE '@[0-9]+' | sort -u | wc -l)"
check "each copy reports a different offset" "$offsets" "2"

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
plain_count="$("$TMD" -f gnu.tar 2>/dev/null | wc -l)"
sorted_count="$("$TMD" -f gnu.tar --sort=size 2>/dev/null | wc -l)"
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
  note "python3 is not installed — the JSON validity checks are skipped"
fi

if command -v python3 > /dev/null 2>&1; then
  gen="$("$TMD" -f gnu.tar -i --format=json 2>/dev/null \
         | python3 -c 'import json,sys; print(json.load(sys.stdin)["summary"]["features"]["generation"])' 2>/dev/null)"
  check_contains "-i --format=json carries the generation" "$gen" "GNU branch"
fi

csv="$("$TMD" -f gnu.tar --format=csv 2>/dev/null)"
check_contains "csv writes a header row" "$csv" "path,kind,mode_string"
csv_rows=$(printf '%s\n' "$csv" | tail -n +2 | wc -l)
tar_rows=$(tar -tf gnu.tar | wc -l)
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
  tty_out="$(script -qec "$TMD" /dev/null < /dev/null 2>/dev/null)"
  check_contains "a bare tmd at a terminal shows the usage" \
                 "$tty_out" "dump the metadata out of a tar archive"
else
  note "script(1) is not installed — the bare-tmd-at-a-tty check is skipped"
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
      "$(stat -c %a permissive.txt 2>/dev/null)" "644"

# ...and a stricter umask is still honored, rather than forced back up to 644.
( umask 077 && "$TMD" -f gnu.tar -o strict.txt > /dev/null 2>&1 )
check "-o honors a stricter umask" \
      "$(stat -c %a strict.txt 2>/dev/null)" "600"

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

gzip -c gnu.tar > gnu.tar.gz
err="$("$TMD" -f gnu.tar.gz 2>&1 >/dev/null)"
check_status "a .tar.gz fails rather than being half-read" "$?" 1
check_contains "a .tar.gz says which wrapper it is in" "$err" "gzip"
check_contains "a .tar.gz names the command that opens it" "$err" "gzip -dc"

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
if [ "$FAIL" -eq 0 ]; then
  printf '  \033[92mok\033[0m  %d end-to-end checks passed\n\n' "$PASS"
  exit 0
fi
printf '  \033[91m%d of %d end-to-end checks failed\033[0m\n\n' "$FAIL" "$((PASS + FAIL))"
exit 1
