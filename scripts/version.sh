#!/usr/bin/env bash
#
# Copyright (c) 2026 Bryan C. Everly
# SPDX-License-Identifier: BSD-2-Clause
#
# Print the version this build should carry.
#
#   1.0.0.6        the tree is clean and HEAD is exactly the v1.0.0.6 tag
#   1.0.0.6-dev    v1.0.0.6 is the newest tag, but this tree is not it
#
# The base number comes from the VERSION file, which is the single source of
# truth: the Makefile compiles it in with -DTMD_VERSION and
# scripts/build-deb.sh writes debian/changelog from it.
#
# The "-dev" says the binary in front of you is not the released one. That
# matters most in a bug report: "1.0.0.6" is a version somebody else can
# download, and "1.0.0.6-dev" is a build only the person running it has, which
# is the difference between reproducing a problem and chasing it.
#
# A tree is "not the release" if any of these is true:
#   * a tracked file has been modified, staged or not
#   * HEAD is not the commit the matching tag points at (commits since the tag)
#   * there is no tag for this version at all (the usual case mid-development)
#
# Untracked files are deliberately NOT counted. A scratch archive, an editor's
# swap file or a downloaded test fixture does not change what was built, and
# treating them as a change would mean every working tree is permanently "-dev"
# for reasons that have nothing to do with the code.
#
# Outside a git checkout — a release tarball, a Debian source package, anything
# unpacked rather than cloned — there is nothing to compare against, so the base
# version is printed as-is. That is the right answer there: a source package is
# built from exactly what was released.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

BASE="$(cat VERSION 2>/dev/null || echo "0.0.0.0")"
SUFFIX=""

is_release_tree() {
  command -v git > /dev/null 2>&1 || return 1
  git rev-parse --git-dir > /dev/null 2>&1 || return 1
  # A repository with no commits yet has no HEAD to compare against.
  git rev-parse --verify HEAD > /dev/null 2>&1 || return 1

  # Modified tracked files, staged or not.
  git diff --quiet HEAD -- > /dev/null 2>&1 || return 1

  # HEAD has to be the tagged commit itself, not a descendant of it.
  local tag="v$BASE"
  git rev-parse -q --verify "refs/tags/$tag" > /dev/null 2>&1 || return 1
  local head_sha tag_sha
  head_sha="$(git rev-parse HEAD 2>/dev/null)" || return 1
  # "^{commit}" resolves an annotated tag's own object to the commit it points
  # at; without it this compares a tag object's hash to a commit's and no tree
  # is ever a release.
  tag_sha="$(git rev-parse "$tag^{commit}" 2>/dev/null)" || return 1
  [ "$head_sha" = "$tag_sha" ]
}

# No git, or not a checkout at all: print the base version. Anything unpacked
# rather than cloned was built from exactly what was released.
if command -v git > /dev/null 2>&1 && git rev-parse --git-dir > /dev/null 2>&1; then
  is_release_tree || SUFFIX="-dev"
fi

printf '%s%s\n' "$BASE" "$SUFFIX"
