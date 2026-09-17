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

out="$("$TMD" -f gnu.tar -u 2>/dev/null)"
check_contains "the stored mtime is reported, not today's date" "$out" "2021-03-04"
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
printf '\n\033[1;94m▸ output formats\033[0m\n'

json="$("$TMD" -f gnu.tar --format=json 2>/dev/null)"
if command -v python3 > /dev/null 2>&1; then
  if printf '%s' "$json" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
    ok "json output parses"
  else
    bad "json output does not parse"
  fi
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

"$TMD" > help.txt 2>&1
check_status "a bare tmd shows the usage and succeeds" "$?" 0
check_contains "the usage names the required -f" "$(cat help.txt)" "-f, --file=FILE"
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

# Reading a pipe.
#
# The cat is deliberate and must not become a `< gnu.tar` redirect, however
# much the linter would prefer one. A redirect hands tmd a REGULAR FILE on
# stdin, which is seekable, so it would exercise the same fseeko path the -f
# case already covers. Piping gives it a pipe, which cannot seek — and the
# read-and-discard fallback in tmd_source_skip is the whole point of this check.
# (Any comment line starting with the linter's own name is read as a directive,
# hence the circumlocution above.)
# shellcheck disable=SC2002
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
