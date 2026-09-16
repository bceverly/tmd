#!/usr/bin/env bash
#
# Copyright (c) 2026 Bryan C. Everly
# SPDX-License-Identifier: BSD-2-Clause
#
# Cut a release version.
#
#   make release                      bump the last digit of the highest tag
#   make release VERSION=1.2.3.4      set an explicit version
#   make release VERSION=v1.2.3.4     the same thing; the "v" is optional
#
# Versions are four-part and tags carry a lowercase "v", e.g. v1.2.3.4. With no
# version tags in the repository at all, the first release is v1.0.0.0 rather
# than a bump of nothing.
#
# What it does, in order, after a single confirmation:
#   1. Works out the next version and asks you to confirm it.
#   2. Writes it into VERSION, rebuilds, and regenerates the manpage.
#   3. Commits and pushes that change.
#   4. Creates an annotated tag and pushes the tag.
#
# Pushing the tag is what triggers the release build, so the confirmation in
# step 1 is the point of no return — answer "n" and nothing at all happens.
#
# Commit and tag signing follow your git config (commit.gpgsign / tag.gpgsign);
# this script does not override them.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

#: Used when the repository has no version tags yet.
FIRST_VERSION="1.0.0.0"
VERSION_PATTERN='^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$'

GREEN=$'\033[1;92m'
RESET=$'\033[0m'

bold() { printf '\n\033[1m%s\033[0m\n' "$*"; }
info() { printf '  \033[96m→\033[0m %s\n' "$*"; }
ok()   { printf '  \033[92m✓\033[0m %s\n' "$*"; }
warn() { printf '  \033[93m!\033[0m %s\n' "$*"; }
die()  { printf '  \033[91m✗\033[0m %s\n' "$*" >&2; exit 1; }

command -v git > /dev/null 2>&1 || die "git is not installed."
[ -d .git ] || die "Not a git repository."

# ---------------------------------------------------------------------------
# Work out the version
# ---------------------------------------------------------------------------
bold "Release"

# `sort -V` orders version strings numerically, so v1.10.0.0 correctly sorts
# above v1.9.0.0 — a plain lexical sort gets that backwards.
LATEST="$(git tag -l 'v*' 2>/dev/null \
          | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' \
          | sed 's/^v//' \
          | sort -V \
          | tail -1)"

EXPLICIT="${VERSION:-}"

if [ -n "$EXPLICIT" ]; then
  # Both "1.2.3.4" and "v1.2.3.4" are accepted; the tag always gets the "v".
  EXPLICIT="${EXPLICIT#v}"
  echo "$EXPLICIT" | grep -qE "$VERSION_PATTERN" \
    || die "VERSION must look like 1.2.3.4 or v1.2.3.4 (four numbers), got '${VERSION}'."
  NEXT="$EXPLICIT"
  if [ -n "$LATEST" ]; then
    info "Current version:  v$LATEST"
  else
    info "Current version:  (no tags yet)"
  fi
  info "Setting version:  v$NEXT  (explicit)"
elif [ -z "$LATEST" ]; then
  NEXT="$FIRST_VERSION"
  info "No version tags found in this repository."
  info "First release:    v$NEXT"
else
  # Bump the last of the four components.
  IFS='.' read -r MAJOR MINOR PATCH BUILD <<< "$LATEST"
  NEXT="${MAJOR}.${MINOR}.${PATCH}.$((BUILD + 1))"
  info "Current version:  v$LATEST"
  info "Next version:     v$NEXT"
fi

TAG="v$NEXT"

# Re-using a tag would move a release that may already have been built and
# published, so say so plainly rather than silently overwriting.
if git rev-parse -q --verify "refs/tags/$TAG" > /dev/null 2>&1; then
  warn "Tag $TAG already exists."
  warn "Delete it first if you really mean to move it:"
  warn "    git tag -d $TAG && git push --delete origin $TAG"
fi

# A release built from a dirty tree is a release nobody can reproduce.
if [ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ]; then
  warn "The working tree has uncommitted changes:"
  git status --short --untracked-files=no | sed 's/^/      /'
  warn "They will NOT be part of the release unless you commit them first."
fi

# ---------------------------------------------------------------------------
# Confirm
# ---------------------------------------------------------------------------
printf '\n'
printf '  Release as \033[1;96m%s\033[0m? [y/N] ' "$TAG"
read -r REPLY
case "$REPLY" in
  y | Y | yes | YES | Yes) ;;
  *)
    printf '\n'
    info "Stopped. Nothing was changed."
    exit 0
    ;;
esac

# ---------------------------------------------------------------------------
# Write the version into the project
# ---------------------------------------------------------------------------
bold "Updating the version"

# One file. The Makefile reads it, compiles it into the binary with
# -DTMD_VERSION, and scripts/build-deb.sh writes debian/changelog from it —
# so there is exactly one place a version can be wrong.
CURRENT="$(cat VERSION 2>/dev/null || echo)"
if [ "$CURRENT" = "$NEXT" ]; then
  info "VERSION already says $NEXT"
else
  echo "$NEXT" > VERSION || die "Could not write VERSION."
  ok "VERSION -> $NEXT"
fi

# Rebuild so the manpage's version line and the binary's --version agree with
# the tag. `make lint` checks exactly this, and finding out at that point that
# the release is wrong is finding out too late.
info "Rebuilding so the binary and the manpage carry $NEXT…"
make --no-print-directory build > /dev/null || die "The build failed; nothing was committed."
BUILT="$(./bin/tmd --version | head -1 | awk '{print $2}')"
# The "-dev" suffix is expected here and is not a problem: at this point the
# version bump is not committed and the tag does not exist, so scripts/version.sh
# is correctly saying this tree is not the release. What matters is the number
# in front of it. Once the commit and the tag below land, a rebuild drops the
# suffix on its own.
[ "${BUILT%-dev}" = "$NEXT" ] \
  || die "The rebuilt binary reports $BUILT, not $NEXT."
ok "./bin/tmd reports $BUILT"

# ---------------------------------------------------------------------------
# Commit, push, tag, push the tag
# ---------------------------------------------------------------------------
bold "Publishing"

BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
[ -n "$BRANCH" ] && [ "$BRANCH" != "HEAD" ] \
  || die "Not on a branch (detached HEAD?); cannot push."

git remote get-url origin > /dev/null 2>&1 \
  || die "No 'origin' remote configured; nothing to push to."

git add VERSION man/tmd.1 || die "Could not stage the version files."

# An empty diff means the files already carried this version, which is fine on
# a re-run; skip the commit rather than failing on "nothing to commit".
if git diff --cached --quiet; then
  warn "VERSION and the manpage already say $NEXT — nothing to commit."
else
  git commit -m "Release $TAG" || die "Commit failed."
  ok "Committed the version bump."
fi

info "Pushing $BRANCH to origin…"
git push origin "$BRANCH" || die "Push failed; the tag was not created."
ok "Pushed $BRANCH."

info "Tagging $TAG…"
git tag -a "$TAG" -m "Release $TAG" || die "Could not create tag $TAG."
ok "Created tag $TAG."

info "Pushing tag $TAG…"
if ! git push origin "$TAG"; then
  # Leave the local tag in place so it can be retried or inspected.
  die "Could not push the tag. The local tag $TAG still exists; delete it with
       'git tag -d $TAG' if you want to start over."
fi
ok "Pushed tag $TAG."

cat <<NEXT_STEPS

  ────────────────────────────────────────────────────────────────
   ${GREEN}Released ${TAG}${RESET}

   Pushing the tag triggers the Release workflow, which waits for CI and
   the security scan to pass on this commit, builds a source package for
   every supported Ubuntu series, proves the binary package installs and
   runs in a clean container, and only then uploads to the PPA. Watch it
   with:

       gh run watch

   Launchpad builds asynchronously after the upload, so a green workflow
   means "accepted", not "published". The PPA's own page is the last word:

       https://launchpad.net/~bceverly/+archive/ubuntu/tmd

   To undo, before anything consumes the tag:

       git push --delete origin ${TAG}
       git tag -d ${TAG}
  ────────────────────────────────────────────────────────────────

NEXT_STEPS
