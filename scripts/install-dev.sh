#!/usr/bin/env bash
#
# Copyright (c) 2026 Bryan C. Everly
# SPDX-License-Identifier: BSD-2-Clause
#
# Install everything `make test`, `make lint`, `make security` and `make deb`
# want, so that none of them has to skip a check.
#
#   make install-dev
#
# Every script in this project skips a tool it cannot find rather than failing,
# which is what makes them usable on a fresh checkout — and also what makes it
# possible to have a green local run that CI then fails, because CI has all of
# them. This closes that gap.
#
# Nothing here is required to *build* tmd. A C compiler and make are enough for
# that, deliberately: the list below is for developing it.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

# Pinned, because an unpinned scanner is a build that starts failing on a day
# nobody changed anything. CI installs the same version.
GITLEAKS_VERSION="8.21.2"

bold()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
info()  { printf '  \033[96m→\033[0m %s\n' "$*"; }
ok()    { printf '  \033[92m✓\033[0m %s\n' "$*"; }
warn()  { printf '  \033[93m!\033[0m %s\n' "$*"; }
die()   { printf '  \033[91m✗\033[0m %s\n' "$*" >&2; exit 1; }

bold "tmd — development environment"

# ---------------------------------------------------------------------------
# System packages
# ---------------------------------------------------------------------------
if ! command -v apt-get > /dev/null 2>&1; then
  warn "This script installs with apt. On another system, the equivalents are:"
  printf '\n'
  printf '    build       gcc make\n'
  printf '    tests       valgrind, and tar/bsdtar for the end-to-end fixtures\n'
  printf '    coverage    gcov (ships with gcc), gcovr for the XML report\n'
  printf '    lint        cppcheck clang-tidy shellcheck\n'
  printf '    security    clang-tools (scan-build) flawfinder semgrep gitleaks\n'
  printf '    packaging   debhelper devscripts dput lintian\n\n'
  exit 0
fi

PACKAGES=(
  # --- build ---------------------------------------------------------------
  build-essential          # gcc, make, libc headers
  # --- tests ---------------------------------------------------------------
  valgrind                 # the uninitialised-read pass in make test-memory
  libarchive-tools         # bsdtar, so the BSD-writer tests do not skip
  # --- coverage ------------------------------------------------------------
  gcovr                    # the XML and HTML reports CI keeps
  # --- lint ----------------------------------------------------------------
  cppcheck
  clang-tidy
  clang-tools              # scan-build, for the clang static analyzer
  shellcheck
  # --- security ------------------------------------------------------------
  flawfinder
  # --- packaging -----------------------------------------------------------
  debhelper
  devscripts               # debsign, debuild, hardening-check
  dput                     # the Launchpad upload
  lintian
  # --- the fuzzer's better engine ------------------------------------------
  clang                    # libFuzzer; the built-in mutator works without it
  # clang alone is not enough: -fsanitize=fuzzer links against compiler-rt,
  # and without this the build fails with "cannot find libclang_rt.fuzzer.a"
  # and scripts/fuzz.sh quietly falls back to the weaker built-in mutator.
  # Quietly is the problem -- it looks like a passing fuzz run either way.
  libclang-rt-dev
  # --- what this script itself needs ---------------------------------------
  curl                     # fetching the pinned gitleaks release
  pipx                     # semgrep is Python, and apt has no current package
)

bold "System packages"
MISSING=()
for package in "${PACKAGES[@]}"; do
  if dpkg -s "$package" > /dev/null 2>&1; then
    continue
  fi
  MISSING+=("$package")
done

if [ ${#MISSING[@]} -eq 0 ]; then
  ok "every system package is already installed"
else
  info "installing: ${MISSING[*]}"
  sudo apt-get update -qq || die "apt-get update failed"
  sudo apt-get install -y -qq --no-install-recommends "${MISSING[@]}" \
    || die "apt-get install failed"
  ok "installed ${#MISSING[@]} package(s)"
fi

# ---------------------------------------------------------------------------
# gitleaks — a release binary, because the apt package lags well behind
# ---------------------------------------------------------------------------
bold "gitleaks"
if command -v gitleaks > /dev/null 2>&1; then
  ok "already installed ($(gitleaks version 2>/dev/null || echo 'version unknown'))"
else
  ARCH="$(uname -m)"
  case "$ARCH" in
    x86_64)  GL_ARCH=x64 ;;
    aarch64) GL_ARCH=arm64 ;;
    *)       GL_ARCH="" ;;
  esac
  if [ -z "$GL_ARCH" ]; then
    warn "no gitleaks release for $ARCH — the secret scan will skip"
  else
    URL="https://github.com/gitleaks/gitleaks/releases/download/v${GITLEAKS_VERSION}/gitleaks_${GITLEAKS_VERSION}_linux_${GL_ARCH}.tar.gz"
    info "downloading gitleaks $GITLEAKS_VERSION"
    if curl -sSfL "$URL" -o /tmp/gitleaks.tar.gz \
       && tar -xzf /tmp/gitleaks.tar.gz -C /tmp gitleaks \
       && sudo install -m 0755 /tmp/gitleaks /usr/local/bin/gitleaks; then
      rm -f /tmp/gitleaks.tar.gz /tmp/gitleaks
      ok "installed gitleaks $GITLEAKS_VERSION"
    else
      warn "could not install gitleaks — the secret scan will skip"
    fi
  fi
fi

# ---------------------------------------------------------------------------
# semgrep — Python, so pipx rather than apt
# ---------------------------------------------------------------------------
bold "semgrep"
if command -v semgrep > /dev/null 2>&1; then
  ok "already installed"
elif command -v pipx > /dev/null 2>&1; then
  if pipx install semgrep > /dev/null 2>&1; then
    ok "installed with pipx"
  else
    warn "pipx install semgrep failed — that scan will skip"
  fi
else
  warn "pipx is not installed, so semgrep was not installed"
  info "sudo apt install pipx && pipx install semgrep"
fi

# ---------------------------------------------------------------------------
# The hooks
# ---------------------------------------------------------------------------
bold "Git hooks"
scripts/install-hooks.sh

# ---------------------------------------------------------------------------
bold "Ready"
printf '  \033[2mmake build     compile into ./bin\033[0m\n'
printf '  \033[2mmake test      unit + end-to-end tests, sanitizers, 80%% coverage gate\033[0m\n'
printf '  \033[2mmake lint      the compiler, the analyzers and the copyright audit\033[0m\n'
printf '  \033[2mmake security  the security scanners, the sanitizers and a fuzz run\033[0m\n'
printf '  \033[2mmake install   build the .deb\033[0m\n\n'
