#!/usr/bin/env bash
#
# Copyright (c) 2026 Bryan C. Everly
# SPDX-License-Identifier: BSD-2-Clause
#
# Report how Launchpad's builds of this version are going, and fail if any of
# them did not succeed.
#
#   scripts/ppa-builds.sh              look once, for the version in VERSION
#   scripts/ppa-builds.sh --wait 330   keep looking, up to 330 minutes
#   make ppa-builds [WAIT=330]
#
# A PPA upload is accepted long before anything is compiled, and Launchpad then
# builds it on its own machines, one per architecture per series. The release
# workflow's publish job can only say "accepted": a build that fails on s390x
# or riscv64 shows up on a web page and in an email, and nowhere else. This is
# the check that turns it into a red job.
#
# What it expects is worked out rather than configured: every processor the PPA
# has enabled that the series supports. Enabling one on Launchpad therefore
# needs no edit here, and a build Launchpad never created -- a processor
# enabled after the upload, say -- is reported as missing rather than passed
# over.
#
# Settings, from the environment (defaults shown):
#   LAUNCHPAD_PPA      ppa:bceverly/tmd
#   LAUNCHPAD_SERIES   jammy noble resolute
#
# Exit status: 0 every build succeeded, 1 a build failed or is missing, 2 still
# building when the wait ran out (or straight away, without --wait).
#
# Read-only: the API is public, so no credentials are involved, and nothing
# here touches git -- it can be run from a checkout or a release tarball.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

WAIT_MINUTES=0
case "${1:-}" in
  --wait)
    WAIT_MINUTES="${2:?--wait needs a number of minutes}"
    case "$WAIT_MINUTES" in
      ''|*[!0-9]*) echo "--wait takes whole minutes, not '$WAIT_MINUTES'" >&2; exit 2 ;;
    esac
    ;;
  "") ;;
  *) echo "Unknown option: $1" >&2; exit 2 ;;
esac

# How long to sleep between looks. A build of tmd takes about a minute; the
# wait is in the queue, so looking more often than this finds nothing new.
POLL_SECONDS="${POLL_SECONDS:-120}"

bold() { printf '\n\033[1m%s\033[0m\n' "$*"; }
info() { printf '  \033[96m→\033[0m %s\n' "$*"; }
ok()   { printf '  \033[92m✓\033[0m %s\n' "$*"; }
pend() { printf '  \033[93m…\033[0m %s\n' "$*"; }
bad()  { printf '  \033[91m✗\033[0m %s\n' "$*"; }
die()  { printf '  \033[91m✗\033[0m %s\n' "$*" >&2; exit 1; }

command -v curl > /dev/null 2>&1 || die "curl is not installed"
command -v python3 > /dev/null 2>&1 || die "python3 is not installed"

VERSION="$(cat VERSION)"
[ -n "$VERSION" ] || die "VERSION is empty"
PPA="${LAUNCHPAD_PPA:-ppa:bceverly/tmd}"
SERIES_LIST="${LAUNCHPAD_SERIES:-jammy noble resolute}"

OWNER="${PPA#ppa:}"; OWNER="${OWNER%%/*}"
NAME="${PPA##*/}"
# devel rather than 1.0: the PPA's processor list is only exposed there.
ROOT="https://api.launchpad.net/devel"
API="$ROOT/~$OWNER/+archive/ubuntu/$NAME"

# One GET, retried. Launchpad's API has bad minutes, and a release should not
# go red because one look out of a hundred timed out.
lp_get() {
  local attempt
  for attempt in 1 2 3 4 5; do
    if curl -fsS --max-time 30 -H 'Accept: application/json' "$1"; then
      return 0
    fi
    sleep $((attempt * 5))
  done
  die "Launchpad did not answer: $1"
}

# The JSON is read by python3 rather than scraped: a build state has spaces in
# it ("Failed to build"), and the log URL is null until there is a log.
entries() {
  python3 -c '
import json, sys
fields = sys.argv[1:]
for e in json.load(sys.stdin)["entries"]:
    print("\t".join(str(e.get(f) if e.get(f) is not None else "-") for f in fields))
' "$@"
}

bold "tmd — Launchpad builds"
info "Version: $VERSION"
info "PPA:     $PPA"
info "Series:  $SERIES_LIST"

PPA_ARCHES="$(lp_get "$API/processors" | entries name | sort)"
[ -n "$PPA_ARCHES" ] || die "$PPA has no processors enabled"
info "Enabled: $(tr '\n' ' ' <<< "$PPA_ARCHES")"

# What each series should get: the PPA's processors that the series builds
# for on the virtualized builders PPAs use. Worked out once, since neither
# changes during a release.
declare -A EXPECTED
for series in $SERIES_LIST; do
  have="$(lp_get "$ROOT/ubuntu/$series/architectures" \
          | entries architecture_tag enabled supports_virtualized \
          | awk -F'\t' '$2 == "True" && $3 == "True" { print $1 }' | sort)"
  EXPECTED[$series]="$(comm -12 <(printf '%s\n' "$PPA_ARCHES") <(printf '%s\n' "$have"))"
done

# One look at every series. Prints what it finds and sets FAILED and PENDING.
look() {
  FAILED=0
  PENDING=0
  local series version query pubs self builds arch state log web want
  for series in $SERIES_LIST; do
    version="${VERSION}~${series}1"
    query="ws.op=getPublishedSources&source_name=tmd&version=$version&exact_match=true"
    pubs="$(lp_get "$API?$query" | entries self_link)"
    if [ -z "$pubs" ]; then
      # Not accepted yet, or rejected: a rejection is only ever an email.
      pend "$version: not in the PPA yet (a rejected upload never appears)"
      PENDING=$((PENDING + 1))
      continue
    fi
    IFS=$'\t' read -r self <<< "$(printf '%s\n' "$pubs" | head -1)"
    builds="$(lp_get "$self?ws.op=getBuilds" | entries arch_tag buildstate build_log_url web_link)"

    for want in ${EXPECTED[$series]}; do
      if ! printf '%s\n' "$builds" | cut -f1 | grep -qxF "$want"; then
        bad "$version $want: Launchpad created no build"
        printf '      %s\n' "was the processor enabled after this was uploaded?"
        FAILED=$((FAILED + 1))
      fi
    done

    while IFS=$'\t' read -r arch state log web; do
      [ -n "$arch" ] || continue
      case "$state" in
        "Successfully built")
          ok "$version $arch: $state"
          ;;
        "Needs building"|"Currently building"|"Gathering build output"|"Uploading build"\
        |"Cancelling build")
          pend "$version $arch: $state"
          PENDING=$((PENDING + 1))
          ;;
        *)
          # Failed to build, Dependency wait, Chroot problem, Failed to upload,
          # Cancelled build, Build for superseded Source. A dependency wait can
          # in principle clear by itself, but tmd builds with debhelper and
          # nothing else, so here it means something is wrong with the series.
          bad "$version $arch: $state"
          printf '      %s\n' "$web"
          [ "$log" = "-" ] || printf '      log: %s\n' "$log"
          FAILED=$((FAILED + 1))
          ;;
      esac
    done <<< "$builds"
  done
}

DEADLINE=$(( $(date +%s) + WAIT_MINUTES * 60 ))
while :; do
  bold "$(date -u '+%Y-%m-%d %H:%M:%SZ')"
  look
  # A failure is final: there is no point waiting for the rest to say so.
  if [ "$FAILED" -gt 0 ]; then
    printf '\n'
    bad "$FAILED build(s) failed or are missing"
    exit 1
  fi
  if [ "$PENDING" -eq 0 ]; then
    printf '\n'
    ok "every build of $VERSION succeeded"
    exit 0
  fi
  if [ "$(date +%s)" -ge "$DEADLINE" ]; then
    printf '\n'
    pend "$PENDING still pending -- run this again later, or with --wait MINUTES"
    exit 2
  fi
  sleep "$POLL_SECONDS"
done
