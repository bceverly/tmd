#!/usr/bin/env bash
#
# Copyright (c) 2026 Bryan C. Everly
# SPDX-License-Identifier: BSD-2-Clause
#
# Throw mutated archives at the parser.
#
#   make fuzz                         30 seconds with the built-in mutator
#   FUZZ_SECONDS=600 make fuzz        longer
#   FUZZ_SEED=12345 make fuzz         reproduce a specific run
#
# A tar reader parses a format that arrives from other people, which is exactly
# the code a fuzzer is for. Two engines:
#
#   the built-in mutation loop  runs everywhere, needs only the compiler already
#                               in use, and is what this runs by default
#   libFuzzer                   coverage-guided and far better at finding the
#                               deep paths; used automatically when clang is
#                               installed
#
# Either way the target is built with AddressSanitizer and UndefinedBehavior-
# Sanitizer, because the fuzzer does not decide what a bug is — they do.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

FUZZ_SECONDS="${FUZZ_SECONDS:-30}"
FUZZ_SEED="${FUZZ_SEED:-0}"
CC="${CC:-cc}"
VERSION="$(cat VERSION)"
WORK=".fuzz"

ok()   { printf '  \033[92m✓\033[0m %s\n' "$*"; }
bad()  { printf '  \033[91m✗\033[0m %s\n' "$*"; }
note() { printf '    \033[2m%s\033[0m\n' "$*"; }

printf '\n\033[1mFuzzing\033[0m \033[2m(%ss)\033[0m\n' "$FUZZ_SECONDS"

mkdir -p "$WORK/corpus"

# ---------------------------------------------------------------------------
# The seed corpus: one archive of every dialect this understands. A fuzzer with
# a poor corpus spends its whole run rediscovering what a tar header is.
# ---------------------------------------------------------------------------
if [ -z "$(ls -A "$WORK/corpus" 2>/dev/null)" ]; then
  SEED_TREE="$(mktemp -d)"
  trap 'rm -rf "$SEED_TREE"' EXIT
  mkdir -p "$SEED_TREE/tree/sub"
  echo hello > "$SEED_TREE/tree/hello.txt"
  ln -sf hello.txt "$SEED_TREE/tree/link"
  mkdir -p "$SEED_TREE/tree/$(printf 'x%.0s' $(seq 1 99))"
  echo deep > "$SEED_TREE/tree/$(printf 'x%.0s' $(seq 1 99))/f.txt"
  dd if=/dev/zero of="$SEED_TREE/tree/sparse.bin" bs=1 count=0 seek=2000000 2>/dev/null

  ( cd "$SEED_TREE" || exit 1
    for format in gnu ustar posix v7; do
      tar --format="$format" -cf "$format.tar" tree 2>/dev/null || true
    done
    tar --sparse -cf sparse-gnu.tar tree/sparse.bin 2>/dev/null || true
    tar --format=posix --sparse -cf sparse-pax.tar tree/sparse.bin 2>/dev/null || true
    command -v bsdtar > /dev/null 2>&1 && bsdtar -cf bsd.tar tree 2>/dev/null
  )
  cp "$SEED_TREE"/*.tar "$WORK/corpus/" 2>/dev/null || true
  ok "built a seed corpus of $(find "$WORK/corpus" -name '*.tar' | wc -l) archives"
else
  ok "reusing the seed corpus in $WORK/corpus"
fi

CPPFLAGS_ALL=(-Iinclude -Isrc -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64
              -DTMD_VERSION="\"$VERSION\"")
SAN=("-fsanitize=address,undefined" -fno-sanitize-recover=all
     -fno-omit-frame-pointer -O1 -g)
# main.c is excluded: the fuzz target brings its own entry point.
SOURCES=(src/util.c src/source.c src/tar.c src/render.c tests/fuzz/fuzz_tar.c)

# ---------------------------------------------------------------------------
# libFuzzer, when clang is here
# ---------------------------------------------------------------------------
if command -v clang > /dev/null 2>&1 && [ "${FUZZ_ENGINE:-auto}" != "builtin" ]; then
  if clang -std=c11 "${CPPFLAGS_ALL[@]}" "${SAN[@]}" -DTMD_LIBFUZZER \
        -fsanitize=fuzzer -o "$WORK/fuzz-libfuzzer" "${SOURCES[@]}" \
        2> "$WORK/build-libfuzzer.log"; then
    ok "built the libFuzzer target"
    mkdir -p "$WORK/findings"
    if "$WORK/fuzz-libfuzzer" "$WORK/corpus" \
         -max_total_time="$FUZZ_SECONDS" -artifact_prefix="$WORK/findings/" \
         -print_final_stats=1 -rss_limit_mb=2048 \
         > "$WORK/libfuzzer.log" 2>&1; then
      ok "libFuzzer found nothing in ${FUZZ_SECONDS}s"
      grep -E "^stat::|cov:" "$WORK/libfuzzer.log" | tail -3 | sed 's/^/    /' || true
      exit 0
    else
      bad "libFuzzer found a crash"
      note "the failing input is under $WORK/findings/"
      tail -30 "$WORK/libfuzzer.log" | sed 's/^/      /'
      exit 1
    fi
  else
    note "clang is installed but the libFuzzer build failed; using the built-in loop"
    if grep -q 'libclang_rt' "$WORK/build-libfuzzer.log" 2>/dev/null; then
      note "clang's runtime is missing: sudo apt install libclang-rt-dev"
      note "(or: make install-dev)"
    fi
    note "see $WORK/build-libfuzzer.log"
  fi
fi

# ---------------------------------------------------------------------------
# The built-in mutation loop
# ---------------------------------------------------------------------------
if ! "$CC" -std=c11 "${CPPFLAGS_ALL[@]}" "${SAN[@]}" -Wall -Wextra \
      -o "$WORK/fuzz-builtin" "${SOURCES[@]}" 2> "$WORK/build-builtin.log"; then
  bad "could not build the fuzz target"
  note "see $WORK/build-builtin.log"
  exit 1
fi
ok "built the built-in mutation target"

export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:symbolize=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

rm -f "$WORK/crash.tar"
if "$WORK/fuzz-builtin" --seconds="$FUZZ_SECONDS" --seed="$FUZZ_SEED" \
     --crash-file="$WORK/crash.tar" "$WORK"/corpus/*.tar; then
  ok "no crashes"
  exit 0
fi

bad "the fuzzer found a crash"
if [ -f "$WORK/crash.tar" ]; then
  note "the failing archive is $WORK/crash.tar"
  note "reproduce it with: $WORK/fuzz-builtin --replay=$WORK/crash.tar"
  note "or look at it with: ./bin/tmd -f $WORK/crash.tar -l"
fi
exit 1
