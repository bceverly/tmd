#!/usr/bin/env bash
#
# Copyright (c) 2026 Bryan C. Everly
# SPDX-License-Identifier: BSD-2-Clause
#
# Static analysis and the house rules.
#
#   make lint
#
# Six checks, in order of how likely they are to be the thing that is wrong:
#
#   1. the compiler, with every warning this project is clean under and -Werror
#   2. gcc -fanalyzer, which does path-sensitive analysis and finds leaks and
#      null dereferences the warning flags do not
#   3. cppcheck, a second opinion from a different engine
#   4. clang-tidy, a third
#   5. the copyright audit — every source file carries the same notice
#   6. shellcheck on every script, and the manpage freshness check
#
# A tool that is not installed is reported as skipped rather than failing the
# run. `make install-dev` installs all of them, and CI installs all of them, so
# nothing skips in the place where skipping would matter.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

CC="${CC:-cc}"
VERSION="$(cat VERSION)"
WORK=".lint"
FAILURES=()
SKIPPED=()

section() { printf '\n\033[1;94m▸ %s\033[0m\n' "$*"; }
ok()      { printf '  \033[92m✓\033[0m %s\n' "$*"; }
bad()     { printf '  \033[91m✗\033[0m %s\n' "$*"; FAILURES+=("$1"); }
skip()    { printf '  \033[93m-\033[0m %s\n' "$*"; SKIPPED+=("$1"); }
note()    { printf '    \033[2m%s\033[0m\n' "$*"; }

mkdir -p "$WORK"
printf '\n\033[1mLint\033[0m\n'

WARNINGS=(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
          -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wformat-nonliteral -Wcast-qual
          -Wwrite-strings -Wpointer-arith -Wundef -Wredundant-decls
          -Wmissing-declarations -Wdouble-promotion
          -Wold-style-definition -Wnested-externs)
# -Wswitch-enum is deliberately NOT in that list. It demands every enumerator
# be named even in a switch that already has a default, which would turn each
# of the "how do I render this kind" switches into a wall of cases that all do
# the same thing — and would then have to be edited in five places to add one
# enumerator. -Wswitch, which -Wall already includes, catches the case that
# actually matters: a switch with no default that forgot a value.
CPPFLAGS_ALL=(-Iinclude -Isrc -Itests -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64
              -D_FORTIFY_SOURCE=3 -DTMD_VERSION="\"$VERSION\"")

# ---------------------------------------------------------------------------
section "the compiler, with -Werror"
if "$CC" -std=c11 -O2 "${CPPFLAGS_ALL[@]}" "${WARNINGS[@]}" -Werror \
     -fsyntax-only src/*.c 2> "$WORK/compiler.log"; then
  ok "no warnings from $("$CC" --version | head -1 | cut -d' ' -f1-3)"
else
  bad "the compiler found something"
  sed 's/^/      /' "$WORK/compiler.log" | head -40
fi

# ---------------------------------------------------------------------------
section "gcc -fanalyzer — path-sensitive analysis"
# Separate from the warning pass because it is slow and because its findings
# are of a different kind: leaks, double frees and null dereferences along a
# specific path, rather than a suspicious line.
# Probed rather than matched on the compiler's name: this machine's `cc` is
# gcc and says "cc (Ubuntu 15.2.0) 15.2.0" when asked, with the word "gcc"
# nowhere in it — so a name match reported "not gcc" about gcc and skipped the
# analyzer silently, which is the one thing this script promises not to do.
if "$CC" -fanalyzer -fsyntax-only -x c /dev/null > /dev/null 2>&1; then
  if "$CC" -std=c11 -O2 "${CPPFLAGS_ALL[@]}" -fanalyzer -Wall \
       -fsyntax-only src/*.c 2> "$WORK/analyzer.log"; then
    ok "no findings"
  else
    bad "gcc -fanalyzer found something"
    sed 's/^/      /' "$WORK/analyzer.log" | head -40
  fi
else
  skip "gcc -fanalyzer needs gcc (this is $("$CC" --version | head -1))"
fi

# ---------------------------------------------------------------------------
section "cppcheck"
if command -v cppcheck > /dev/null 2>&1; then
  # --error-exitcode makes a finding a failure. The suppressions are for checks
  # that do not apply rather than for findings being ignored:
  #   missingIncludeSystem  cppcheck does not have the system headers
  #   unusedFunction        every exported function is used by another unit or
  #                         by the tests, which cppcheck does not analyze here
  #   checkersReport        an informational note, not a finding
  if cppcheck --quiet --error-exitcode=1 --std=c11 --enable=warning,style,performance,portability \
       --inline-suppr \
       --suppress=missingIncludeSystem --suppress=unusedFunction \
       --suppress=checkersReport \
       -I include -I src -I tests \
       src tests 2> "$WORK/cppcheck.log"; then
    ok "no findings"
  else
    bad "cppcheck found something"
    sed 's/^/      /' "$WORK/cppcheck.log" | head -40
  fi
else
  skip "cppcheck is not installed"
  note "sudo apt install cppcheck   (or: make install-dev)"
fi

# ---------------------------------------------------------------------------
section "clang-tidy"
if command -v clang-tidy > /dev/null 2>&1; then
  # bugprone, cert and the clang analyzer. readability-* and the naming checks
  # are deliberately off: they encode a house style that is not this one, and a
  # linter that reports 400 things nobody intends to change is a linter that
  # gets switched off entirely.
  #
  # Four checks are disabled by name. Each is off because it does not apply to
  # C as written here, not because its findings were inconvenient — everything
  # else these checks reported was fixed rather than silenced:
  #
  #   security.insecureAPI.DeprecatedOrUnsafeBufferHandling
  #       Wants fprintf_s and snprintf_s, the bounds-checked functions of ISO
  #       C11 Annex K. Annex K is OPTIONAL, glibc does not implement any of it,
  #       and no Linux libc does — so there is literally nothing to call. It
  #       reported 160 findings, one per printf in the program.
  #
  #   bugprone-multi-level-implicit-pointer-conversion
  #       Flags `char **p = malloc(...)` and `free(p)`. Conversion to and from
  #       `void *` is defined by C itself (C11 6.3.2.3), and casting the result
  #       of an allocator is a C++ habit that C style guides advise against.
  #       All eight findings were malloc/realloc/free calls.
  #
  #   misc-include-cleaner
  #       Wants every transitively-used declaration included directly, which
  #       means naming <stddef.h> beside every <stdio.h>.
  #
  #   bugprone-easily-swappable-parameters
  #       Flags (const char *a, const char *b) signatures. True of most string
  #       functions ever written.
  #
  #   clang-analyzer-valist.Uninitialized
  #       A false positive in the analyzer shipped with clang 18 (Ubuntu
  #       24.04), which does not model va_copy: it reports the vsnprintf in
  #       tmd_xvasprintf as taking an uninitialized va_list when the argument
  #       is the freshly va_copy'd one, and reports bad_usage's vfprintf the
  #       same way with va_start three lines above it. Both are correct C and
  #       a newer clang says nothing. The sibling checks valist.Unterminated
  #       and valist.CopyToSelf stay on — those catch real bugs.
  if clang-tidy --quiet \
       '-checks=-*,bugprone-*,cert-*,clang-analyzer-*,misc-*,performance-*,-misc-include-cleaner,-bugprone-easily-swappable-parameters,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-bugprone-multi-level-implicit-pointer-conversion,-clang-analyzer-valist.Uninitialized' \
       --warnings-as-errors='*' \
       src/*.c -- -std=c11 "${CPPFLAGS_ALL[@]}" > "$WORK/clang-tidy.log" 2>&1; then
    ok "no findings"
  else
    bad "clang-tidy found something"
    grep -E "warning:|error:" "$WORK/clang-tidy.log" | head -30 | sed 's/^/      /'
  fi
else
  skip "clang-tidy is not installed"
  note "sudo apt install clang-tidy   (or: make install-dev)"
fi

# ---------------------------------------------------------------------------
section "copyright audit"
#
# Every source file carries the same notice, and this is what keeps that true.
# It is not bureaucracy: the license grant on a BSD-2 project is the file
# header, and a file that reaches somebody without one has no license at all.
# The owner and the SPDX identifier are matched exactly; the year is allowed to
# be a single year or a range, so bumping it does not mean touching every file.
EXPECTED_OWNER="Bryan C. Everly"
EXPECTED_SPDX="SPDX-License-Identifier: BSD-2-Clause"
COPYRIGHT_RE="Copyright \(c\) 20[0-9][0-9](-20[0-9][0-9])? $EXPECTED_OWNER"

missing=0
checked=0
while IFS= read -r file; do
  checked=$((checked + 1))
  head_text="$(head -12 "$file")"
  problem=""
  if ! printf '%s' "$head_text" | grep -Eq "$COPYRIGHT_RE"; then
    problem="no \"Copyright (c) <year> $EXPECTED_OWNER\" in the first 12 lines"
  elif ! printf '%s' "$head_text" | grep -qF "$EXPECTED_SPDX"; then
    problem="no \"$EXPECTED_SPDX\" in the first 12 lines"
  fi
  if [ -n "$problem" ]; then
    if [ "$missing" -eq 0 ]; then
      bad "files without the project copyright header"
    fi
    missing=$((missing + 1))
    note "$file: $problem"
  fi
done < <(
  {
    find src include tests scripts -type f \
         \( -name '*.c' -o -name '*.h' -o -name '*.def' -o -name '*.sh' \)
    # The git hooks carry no extension, so the pattern above misses them --
    # and they are shipped source like everything else.
    find .githooks -type f 2>/dev/null
  } | sort
)

if [ "$missing" -eq 0 ]; then
  ok "$checked source files carry the copyright and SPDX header"
fi

# The notice in the code, the one the program prints, and the one in the
# packaging have to name the same person and the same license.
for file in LICENSE debian/copyright man/tmd.1 include/tmd.h; do
  [ -f "$file" ] || continue
  if ! grep -qF "$EXPECTED_OWNER" "$file"; then
    bad "$file does not name $EXPECTED_OWNER"
  fi
done

# And the same address.
#
# debian/control is the single definition: scripts/build-deb.sh reads the
# maintainer out of it to write debian/changelog, and that trailer is what
# Launchpad puts in its ACCEPTED/REJECTED mail and what anyone reading the
# package sees. A second copy of the address that has drifted means the docs
# tell one story and the package another, so the other places that state it are
# checked against this one rather than against a constant written here.
MAINTAINER_EMAIL="$(sed -n 's/^Maintainer: .*<\(.*\)>.*/\1/p' debian/control | head -1)"
if [ -z "$MAINTAINER_EMAIL" ]; then
  bad "debian/control has no parseable Maintainer line"
else
  #
  # Only the files that genuinely restate the address.
  #
  # The README and the release workflow used to be on this list, back when the
  # changelog identity came from repository variables that named it. It comes
  # from debian/control now, so neither states it any more, and requiring them
  # to was checking a rule that had stopped existing. What is left is the two
  # places where a stale address does real damage: one misdirects a bug report,
  # the other misdirects a vulnerability report.
  mismatch=0
  for file in debian/copyright SECURITY.md; do
    [ -f "$file" ] || continue
    if ! grep -qF "$MAINTAINER_EMAIL" "$file"; then
      bad "$file does not carry the maintainer address $MAINTAINER_EMAIL"
      mismatch=1
    fi
  done
  [ "$mismatch" -eq 0 ] && ok "every file agrees the maintainer is <$MAINTAINER_EMAIL>"
fi

# The pinned signing key.
#
# Format only: whether the key is in *this* keyring says nothing useful (CI has
# no keyring, and a contributor's has a different one), but a truncated or
# short-id fingerprint is a release that fails after the tag exists.
if [ -f debian/signing-fingerprint ]; then
  PINNED_KEY="$(grep -vE '^[[:space:]]*(#|$)' debian/signing-fingerprint | head -1 | tr -d '[:space:]')"
  if [ -z "$PINNED_KEY" ]; then
    bad "debian/signing-fingerprint has no fingerprint in it"
  elif ! printf '%s' "$PINNED_KEY" | grep -qE '^[0-9A-Fa-f]{40}$'; then
    bad "debian/signing-fingerprint is not a 40-character fingerprint: $PINNED_KEY"
    note "a short key id is not enough — several of yours may share an address"
  else
    ok "debian/signing-fingerprint pins a full fingerprint"
  fi
else
  bad "debian/signing-fingerprint is missing — source packages would build unsigned"
fi
if ! ./bin/tmd --version 2>/dev/null | grep -qF "$EXPECTED_OWNER"; then
  if [ -x ./bin/tmd ]; then
    bad "the binary's --version output does not carry the copyright"
  else
    note "./bin/tmd is not built, so its --version was not checked"
  fi
else
  ok "LICENSE, debian/copyright, the manpage and --version all agree"
fi

# ---------------------------------------------------------------------------
section "shellcheck"
if command -v shellcheck > /dev/null 2>&1; then
  if shellcheck --severity=style scripts/*.sh tests/cli/*.sh .githooks/* \
       > "$WORK/shellcheck.log" 2>&1; then
    ok "every script is clean"
  else
    bad "shellcheck found something"
    head -40 "$WORK/shellcheck.log" | sed 's/^/      /'
  fi
else
  skip "shellcheck is not installed"
  note "sudo apt install shellcheck   (or: make install-dev)"
fi

# ---------------------------------------------------------------------------
section "generated files are current"
if [ -x ./bin/tmd ]; then
  if scripts/gen-man.sh --check ./bin/tmd man/tmd.1 > "$WORK/man.log" 2>&1; then
    ok "man/tmd.1 matches the program's --help"
  else
    bad "man/tmd.1 is out of date — run 'make man' and commit it"
    head -20 "$WORK/man.log" | sed 's/^/      /'
  fi
else
  skip "./bin/tmd is not built — run 'make build' before 'make lint'"
fi

# The version the binary reports has to be the one this tree should produce, or
# a release tag will name something that is not what was built. Compared against
# scripts/version.sh rather than against the VERSION file, because a working
# tree correctly reports "1.0.0.6-dev" and a released one reports "1.0.0.6".
if [ -x ./bin/tmd ]; then
  expected="$(scripts/version.sh)"
  built="$(./bin/tmd --version | head -1 | awk '{print $2}')"
  if [ "$built" = "$expected" ]; then
    case "$built" in
      *-dev) ok "the built binary reports $built (this tree is not the release)" ;;
      *)     ok "the built binary reports version $built" ;;
    esac
  else
    bad "./bin/tmd reports $built but this tree should build $expected — run 'make build'"
  fi
fi

# ---------------------------------------------------------------------------
printf '\n'
if [ ${#SKIPPED[@]} -gt 0 ]; then
  printf '  \033[93mSkipped:\033[0m %d tool(s) not installed\n' "${#SKIPPED[@]}"
fi
if [ ${#FAILURES[@]} -eq 0 ]; then
  printf '  \033[92m✓ lint is clean\033[0m\n\n'
  exit 0
fi
printf '  \033[91m✗ %d lint check(s) failed\033[0m\n\n' "${#FAILURES[@]}"
exit 1
