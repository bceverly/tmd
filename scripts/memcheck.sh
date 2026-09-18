#!/usr/bin/env bash
#
# Copyright (c) 2026 Bryan C. Everly
# SPDX-License-Identifier: BSD-2-Clause
#
# Memory safety: the same tests again, under instrumentation that notices what
# a passing test cannot.
#
#   make test-memory
#
# Three passes, because they find different things:
#
#   AddressSanitizer   buffer overflows, use-after-free, double free, and
#                      (LeakSanitizer, which it includes) leaks
#   UndefinedBehavior  signed overflow, bad shifts, misaligned loads, and the
#      Sanitizer       integer conversions a tar parser is full of chances for
#   valgrind           the same ground as ASan from a different angle, and it
#                      catches uninitialised reads, which ASan does not
#
# A tool that is not installed is reported as skipped rather than failing the
# run, so this is useful before all of them are set up. `make install-dev`
# installs them and CI installs them, so in both of those places nothing skips.
#
# The sanitizers are pointed at a log file rather than stderr: the end-to-end
# tests assert on what the program writes to stderr, and a sanitizer summary
# printed there would fail those tests for the wrong reason.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

CC="${CC:-cc}"
VERSION="$(cat VERSION)"
WORK=".sanitize"
FAILURES=()
SKIPPED=()

section() { printf '\n\033[1;94m▸ %s\033[0m\n' "$*"; }
ok()      { printf '  \033[92m✓\033[0m %s\n' "$*"; }
bad()     { printf '  \033[91m✗\033[0m %s\n' "$*"; FAILURES+=("$1"); }
skip()    { printf '  \033[93m-\033[0m %s\n' "$*"; SKIPPED+=("$1"); }
note()    { printf '    \033[2m%s\033[0m\n' "$*"; }

# ---------------------------------------------------------------------------
# What a failing step says.
#
# Pointing at a log is not reporting a failure, for one reason: the next run
# starts by deleting $WORK, so by the time anybody looks, the evidence is a
# file from a run that passed. That is not a hypothetical -- it is how two
# separate afternoons were spent, once here and once on a macOS runner where
# the log could not be reached at all.
#
# So a failure prints its own evidence, and keeps a copy the next run will not
# touch. The copy is named for the step rather than the time, because a second
# failure of the same step supersedes the first and a directory of timestamped
# logs is its own kind of unreadable.
# ---------------------------------------------------------------------------
show_failure() {
  local log="$1"
  local kept

  [ -f "$log" ] || { note "no log at $log"; return; }
  kept="$WORK.failed/$(basename "$log")"

  # Beside $WORK, not inside it: inside is the one place guaranteed to be
  # deleted by the next run, which is the whole problem being fixed. Not
  # cleared at startup either -- a kept log stops being interesting when the
  # same step fails again and overwrites it, and not a moment earlier.
  mkdir -p "$WORK.failed" 2> /dev/null
  cp "$log" "$kept" 2> /dev/null || kept="$log"
  # The lines that say what went wrong, not the hundreds that say what did not.
  if grep -qE '✗|FAIL|ERROR|runtime error' "$log"; then
    grep -E '✗|FAIL|ERROR|runtime error' "$log" | head -12 | sed 's/^/      /'
  else
    tail -12 "$log" | sed 's/^/      /'
  fi
  note "full output: $kept"
}

printf '\n\033[1mMemory safety\033[0m\n'

# ---------------------------------------------------------------------------
# One run at a time in this working tree.
#
# $WORK is a fixed directory rather than a mktemp one, deliberately: the logs
# and the instrumented binaries are what a failure is diagnosed from, and a
# path that changes every run is a path nobody can find afterwards. The price
# is that two runs in the same checkout share all of it -- one rebuilds
# tmd-asan while the other is executing it, and both write the same log.
#
# What that produces is worse than an error. Every check still prints, so the
# run looks normal; the suite just exits non-zero with nothing in the log to
# say why, because the log that survived belongs to whichever run finished
# last -- typically the one that passed. Diagnosed exactly once, the hard way.
#
# mkdir is the primitive rather than flock, which is not on macOS or the BSDs.
# It is atomic everywhere: of two racing processes exactly one creates the
# directory and the other is told no. The lock sits beside $WORK rather than
# inside it, because the first thing below is to delete $WORK.
# ---------------------------------------------------------------------------
LOCK="$WORK.lock"
LOCK_HELD=0

# Released on the way out, but only if it is ours: a lock we failed to take
# belongs to the run that has it, and removing that on our way out would hand
# its directory to the next caller while it is still working.
trap 'if [ "$LOCK_HELD" = "1" ]; then rm -rf "$LOCK"; fi' EXIT

if ! mkdir "$LOCK" 2> /dev/null; then
  holder="$(cat "$LOCK/pid" 2> /dev/null || true)"
  if [ -n "$holder" ] && kill -0 "$holder" 2> /dev/null; then
    bad "another memcheck is already running in this tree (pid $holder)"
    note "Both would share $WORK -- the instrumented binaries and the logs --"
    note "so this one would overwrite what that one is still using. Wait for it"
    note "to finish, or stop it, and run again."
    printf '\n'
    exit 1
  fi
  # Nobody alive is holding it: a previous run was killed before it could clean
  # up. Taking it over is the right answer, and racing to take it over is safe
  # -- the mkdir below is what decides, and the loser is told no.
  note "clearing a stale lock left by pid ${holder:-unknown}"
  rm -rf "$LOCK"
  if ! mkdir "$LOCK" 2> /dev/null; then
    bad "could not take $LOCK"
    printf '\n'
    exit 1
  fi
fi
LOCK_HELD=1
echo "$$" > "$LOCK/pid"

rm -rf "$WORK"
mkdir -p "$WORK"

# Platform feature macros in one place; see scripts/features.sh.
read -r -a TMD_FEATURES <<< "$(scripts/features.sh)"
CPPFLAGS_ALL=(-Iinclude -Isrc -Itests "${TMD_FEATURES[@]}"
              -DTMD_VERSION="\"$VERSION\"")

# ---------------------------------------------------------------------------
# A corpus of archives to point the instrumented binary at. The end-to-end
# suite builds its own; these are the hand-made nasty ones, because the parser
# paths worth instrumenting are the ones a real tar never produces.
# ---------------------------------------------------------------------------
build_corpus() {
  local dir="$1"
  mkdir -p "$dir/tree/sub"
  echo hello > "$dir/tree/hello.txt"
  ln -sf hello.txt "$dir/tree/link"
  mkdir -p "$dir/tree/$(printf 'x%.0s' $(seq 1 99))"
  echo deep > "$dir/tree/$(printf 'x%.0s' $(seq 1 99))/file.txt"
  dd if=/dev/zero of="$dir/tree/sparse.bin" bs=1 count=0 seek=4000000 2>/dev/null

  ( cd "$dir" || exit 1
    for format in gnu ustar posix v7; do
      tar --format="$format" -cf "$format.tar" tree 2>/dev/null || true
    done
    tar --sparse -cf sparse.tar tree/sparse.bin 2>/dev/null || true
    tar --format=posix --sparse -cf sparse-pax.tar tree/sparse.bin 2>/dev/null || true
    command -v bsdtar > /dev/null 2>&1 && bsdtar -cf bsd.tar tree 2>/dev/null

    # Damaged copies: truncated mid-header, truncated mid-data, a flipped
    # checksum byte, and an archive with garbage glued onto the end.
    [ -f gnu.tar ] || return 0
    head -c 700 gnu.tar > truncated-header.tar
    head -c 1536 gnu.tar > truncated-data.tar
    cp gnu.tar corrupt.tar
    printf 'ZZZZZZZZ' | dd of=corrupt.tar bs=1 seek=660 conv=notrunc 2>/dev/null
    cat gnu.tar > appended.tar
    head -c 4000 /dev/urandom >> appended.tar
    head -c 200 /dev/urandom > tiny-random.bin
    : > empty.tar
    head -c 1024 /dev/zero > zeros.tar
  )
}

run_over_corpus() {
  local binary="$1"
  local dir="$2"
  local archive

  for archive in "$dir"/*.tar "$dir"/*.bin; do
    [ -f "$archive" ] || continue
    for mode in "" "-l" "-i" "-R" "-S" "-s" "--format=json" "--format=csv" \
                "-c" "-n -H -u -T"; do
      # shellcheck disable=SC2086  # the mode string is deliberately split
      "$binary" -f "$archive" $mode > /dev/null 2>&1
    done
    "$binary" -f "$archive" -f "$archive" --format=json > /dev/null 2>&1
    "$binary" -f - < "$archive" > /dev/null 2>&1
  done
  # The argument-handling paths too.
  "$binary" > /dev/null 2>&1
  "$binary" --help > /dev/null 2>&1
  "$binary" --version > /dev/null 2>&1
  "$binary" --format=nonsense -f "$dir/gnu.tar" > /dev/null 2>&1
  "$binary" -f /nonexistent.tar > /dev/null 2>&1
  return 0
}

build_corpus "$WORK/corpus"

# ---------------------------------------------------------------------------
section "AddressSanitizer + UndefinedBehaviorSanitizer"

SAN_FLAGS=(-std=c11 -O1 -g -fno-omit-frame-pointer
           "-fsanitize=address,undefined"
           -fno-sanitize-recover=all
           -Wall -Wextra)

# Everything except main.c, which the unit tests replace with their own.
LIB_SOURCES=()
for source in src/*.c; do
  [ "$source" = "src/main.c" ] || LIB_SOURCES+=("$source")
done

if "$CC" "${CPPFLAGS_ALL[@]}" "${SAN_FLAGS[@]}" -o "$WORK/tmd-asan" src/*.c \
     2> "$WORK/build-asan.log"; then
  "$CC" "${CPPFLAGS_ALL[@]}" "${SAN_FLAGS[@]}" -o "$WORK/unittests-asan" \
     "${LIB_SOURCES[@]}" tests/*.c 2>> "$WORK/build-asan.log"

  export ASAN_OPTIONS="detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1:detect_invalid_pointer_pairs=2:log_path=$PWD/$WORK/asan"
  export UBSAN_OPTIONS="print_stacktrace=1:log_path=$PWD/$WORK/ubsan"
  export LSAN_OPTIONS="log_path=$PWD/$WORK/lsan"

  if "$WORK/unittests-asan" > "$WORK/unittests-asan.log" 2>&1; then
    ok "unit tests are clean under ASan/UBSan (leak detection on)"
  else
    bad "unit tests failed under the sanitizers"
    show_failure "$WORK/unittests-asan.log"
    note "and $WORK/asan.*"
  fi

  if tests/cli/run.sh "$WORK/tmd-asan" > "$WORK/cli-asan.log" 2>&1; then
    ok "end-to-end tests are clean under ASan/UBSan"
  else
    bad "end-to-end tests failed under the sanitizers"
    show_failure "$WORK/cli-asan.log"
  fi

  run_over_corpus "$WORK/tmd-asan" "$WORK/corpus"

  # The sanitizers write to <log_path>.<pid>, so any such file at all is a
  # finding — the exit status alone would miss a leak in a run whose exit
  # status the test harness deliberately ignores.
  if compgen -G "$WORK/asan.*" > /dev/null || \
     compgen -G "$WORK/ubsan.*" > /dev/null || \
     compgen -G "$WORK/lsan.*" > /dev/null; then
    bad "the sanitizers reported something"
    for log in "$WORK"/asan.* "$WORK"/ubsan.* "$WORK"/lsan.*; do
      [ -f "$log" ] || continue
      note "$log:"
      head -15 "$log" | sed 's/^/      /'
    done
  else
    ok "damaged and malformed archives are clean under the sanitizers"
  fi
  unset ASAN_OPTIONS UBSAN_OPTIONS LSAN_OPTIONS
else
  skip "this compiler does not support -fsanitize=address,undefined"
  note "see $WORK/build-asan.log"
fi

# ---------------------------------------------------------------------------
section "valgrind — uninitialised reads and leaks"

if command -v valgrind > /dev/null 2>&1; then
  # An uninstrumented build: valgrind and ASan must not be combined, and the
  # optimizer's view is the one a released binary has.
  "$CC" "${CPPFLAGS_ALL[@]}" -std=c11 -O1 -g -Wall -Wextra \
        -o "$WORK/tmd-plain" src/*.c 2> "$WORK/build-plain.log"
  "$CC" "${CPPFLAGS_ALL[@]}" -std=c11 -O1 -g -Wall -Wextra \
        -o "$WORK/unittests-plain" "${LIB_SOURCES[@]}" tests/*.c \
        2>> "$WORK/build-plain.log"

  #
  # --child-silent-after-fork is not a way of ignoring inconvenient findings.
  #
  # tmd forks to read a compressed archive: one child execs the system's
  # decompressor, the other copies bytes into it and leaves through _exit. A
  # forked child inherits the whole of the parent's heap and frees none of it,
  # because there is nothing there it owns -- so valgrind, correctly by its own
  # rules, reports every inherited allocation as definitely lost, once per
  # child, for every compressed archive the suite reads.
  #
  # None of that is tmd's memory behavior; it is what fork means. The parent is
  # still fully checked, which is the process with the parsing in it.
  VG=(valgrind --quiet --error-exitcode=42
      --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all
      --child-silent-after-fork=yes
      --track-origins=yes --num-callers=25)

  "${VG[@]}" --log-file="$WORK/valgrind-unit.log" \
    "$WORK/unittests-plain" > /dev/null 2>&1
  unit_status=$?
  if [ "$unit_status" -eq 0 ]; then
    ok "unit tests are clean under valgrind"
  elif [ "$unit_status" -eq 42 ]; then
    bad "valgrind found something in the unit tests"
    note "see $WORK/valgrind-unit.log"
    head -25 "$WORK/valgrind-unit.log" | sed 's/^/      /'
  else
    bad "the unit tests themselves failed under valgrind (exit $unit_status)"
  fi

  #
  # The test is `exit status == 42`, not `!= 0`.
  #
  # --error-exitcode=42 is what valgrind returns when VALGRIND finds something;
  # with a clean run it passes the program's own status through untouched. Half
  # this corpus is deliberately broken archives, on which tmd correctly exits 1
  # or 3 — so `if ! ...` counted every one of those as a memory error and
  # reported four failures against four empty log files.
  VG_ERROR_EXIT=42
  vg_failures=0
  index=0
  for archive in "$WORK"/corpus/*.tar "$WORK"/corpus/*.bin; do
    [ -f "$archive" ] || continue
    for mode in "-l" "-i" "--format=json"; do
      index=$((index + 1))
      "${VG[@]}" --log-file="$WORK/valgrind-$index.log" \
        "$WORK/tmd-plain" -f "$archive" "$mode" > /dev/null 2>&1
      if [ $? -eq "$VG_ERROR_EXIT" ]; then
        vg_failures=$((vg_failures + 1))
        note "$(basename "$archive") $mode — see $WORK/valgrind-$index.log"
      fi
    done
  done
  if [ "$vg_failures" -eq 0 ]; then
    ok "every archive in the corpus is clean under valgrind"
  else
    bad "valgrind found something on $vg_failures of the corpus runs"
  fi
else
  skip "valgrind is not installed"
  note "sudo apt install valgrind   (or: make install-dev)"
fi

# ---------------------------------------------------------------------------
printf '\n'
if [ ${#SKIPPED[@]} -gt 0 ]; then
  printf '  \033[93mSkipped:\033[0m %s\n' "${SKIPPED[*]}"
fi
if [ ${#FAILURES[@]} -eq 0 ]; then
  printf '  \033[92m✓ no memory errors, no leaks, no undefined behavior\033[0m\n\n'
  exit 0
fi
printf '  \033[91m✗ %d memory check(s) failed\033[0m\n\n' "${#FAILURES[@]}"
exit 1
