#!/usr/bin/env bash
#
# Copyright (c) 2026 Bryan C. Everly
# SPDX-License-Identifier: BSD-2-Clause
#
# Install tmd into the filesystem, or take it back out again.
#
#   scripts/install.sh             install under $prefix (default /usr/local)
#   scripts/install.sh --uninstall remove what the above put there
#
# Reads the same variables the Makefile uses, so `make install prefix=...` and
# `make install DESTDIR=...` both behave the way every other package expects.
#
# The default prefix is /usr/local, not /usr, and that is the important part.
# /usr belongs to the distribution's package manager: a file written there by
# hand is a file dpkg does not know about, and the next `apt install tmd` from
# the PPA would either overwrite it or fight it. /usr/local is the place the
# Filesystem Hierarchy Standard sets aside for exactly this -- software the
# administrator installed themselves -- and it is on PATH ahead of /usr/bin on
# every distribution worth the name.
set -euo pipefail

UNINSTALL=0
if [ "${1:-}" = "--uninstall" ]; then
  UNINSTALL=1
fi

PREFIX="${prefix:-/usr/local}"
BINDIR="${bindir:-$PREFIX/bin}"
MANDIR="${mandir:-$PREFIX/share/man}"
DESTDIR="${DESTDIR:-}"
PROG="${PROG:-tmd}"
MAN_PAGE="${MAN_PAGE:-man/$PROG.1}"

BIN_TARGET="$DESTDIR$BINDIR/$PROG"
MAN_TARGET="$DESTDIR$MANDIR/man1/$PROG.1"

ok()   { printf '  \033[92m✓\033[0m %s\n' "$*"; }
note() { printf '    \033[2m%s\033[0m\n' "$*"; }
die()  { printf '  \033[91m✗\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Whether this needs to be root, and how it gets there.
#
# Asked rather than assumed. Running the whole thing under sudo when the target
# is already writable -- a prefix under $HOME, a staged DESTDIR, a container
# running as root -- would create files owned by root that the user then cannot
# remove, which is a worse outcome than the permission error it was avoiding.
# ---------------------------------------------------------------------------
writable() {
  # The nearest ancestor that exists is the one that has to be writable, since
  # everything below it is about to be created.
  local dir="$1"
  while [ ! -d "$dir" ] && [ "$dir" != "/" ]; do
    dir="$(dirname "$dir")"
  done
  [ -w "$dir" ]
}

SUDO=""
if [ "$(id -u)" != "0" ] \
   && { ! writable "$(dirname "$BIN_TARGET")" \
        || ! writable "$(dirname "$MAN_TARGET")"; }; then
  if command -v sudo > /dev/null 2>&1; then
    SUDO="sudo"
  else
    die "$BINDIR is not writable and sudo is not installed.
       Either run this as root, or choose somewhere you can write:
         make install prefix=\$HOME/.local"
  fi
fi

if [ -n "$SUDO" ]; then
  printf '\n\033[1mInstalling\033[0m \033[2m(needs root for %s)\033[0m\n\n' "$PREFIX"
  # Ask once, up front, so the password prompt does not appear in the middle of
  # the work with no explanation of what wants it.
  $SUDO -v || die "could not obtain the privileges to write to $PREFIX"
else
  printf '\n\033[1m%s\033[0m\n\n' "$([ "$UNINSTALL" = 1 ] && echo Uninstalling || echo Installing)"
fi

# ---------------------------------------------------------------------------
if [ "$UNINSTALL" = "1" ]; then
  $SUDO rm -f "$BIN_TARGET" "$MAN_TARGET" "$MAN_TARGET.gz"
  ok "removed $BIN_TARGET"
  ok "removed $MAN_TARGET"
else
  [ -x "bin/$PROG" ] || die "bin/$PROG is not built — run 'make build' first"
  [ -f "$MAN_PAGE" ] || die "$MAN_PAGE is missing — run 'make build' first"

  $SUDO install -d "$(dirname "$BIN_TARGET")"
  $SUDO install -m 0755 "bin/$PROG" "$BIN_TARGET"
  ok "installed $BIN_TARGET"

  $SUDO install -d "$(dirname "$MAN_TARGET")"
  $SUDO install -m 0644 "$MAN_PAGE" "$MAN_TARGET"
  ok "installed $MAN_TARGET"
fi

# ---------------------------------------------------------------------------
# The man database, so `man tmd` works now rather than after the nightly cron.
#
# Skipped for a staged install: DESTDIR is a directory being assembled for a
# package, and indexing it would describe a filesystem that does not exist yet.
if [ -z "$DESTDIR" ] && command -v mandb > /dev/null 2>&1; then
  # Claimed only when it actually happened. mandb fails for an unprivileged
  # install into a private prefix, and saying "refreshed" anyway would be a
  # small lie in the one place a reader is checking whether `man tmd` will work.
  if $SUDO mandb -q > /dev/null 2>&1; then
    note "man database refreshed"
  fi
fi

if [ "$UNINSTALL" = "0" ] && [ -z "$DESTDIR" ]; then
  printf '\n'
  # Report what the shell will actually run, which is not always what was just
  # installed: a copy from the PPA in /usr/bin wins over /usr/local/bin only if
  # PATH is unusual, and saying so beats a confusing version number later.
  FOUND="$(command -v "$PROG" 2>/dev/null || true)"
  if [ "$FOUND" = "$BIN_TARGET" ]; then
    ok "$($BIN_TARGET --version | head -1)"
  elif [ -n "$FOUND" ]; then
    ok "$("$BIN_TARGET" --version | head -1) installed"
    note "note: '$PROG' on your PATH is $FOUND, not the copy just installed"
    note "$BINDIR comes later in PATH, or another copy is installed"
  else
    ok "$("$BIN_TARGET" --version | head -1) installed"
    note "note: $BINDIR is not on your PATH"
  fi
  printf '\n'
fi
