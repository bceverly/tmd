#!/usr/bin/env bash
#
# Copyright (c) 2026 Bryan C. Everly
# SPDX-License-Identifier: BSD-2-Clause
#
# MITRE's "Lucky 13", checked one by one.
#
#   make lucky13
#
# Steve Christey's "Unforgivable Vulnerabilities" (MITRE, Black Hat USA 2007)
# lists thirteen vulnerability classes that are so well documented, so obvious
# and so easy to find -- "found in five minutes" -- that shipping one is
# unforgivable. The slide that lists them is titled "The Lucky 13":
#
#    1. buffer overflow           CWE-120     8. authentication bypass   CWE-472
#    2. cross-site scripting      CWE-79      9. grow-your-own crypto    CWE-327
#    3. directory traversal       CWE-23     10. privilege escalation    CWE-271
#    4. remote file inclusion     CWE-98     11. symlink following       CWE-61
#    5. SQL injection             CWE-89     12. hard-coded password     CWE-259
#    6. world-writable files      CWE-276/279 13. integer overflow       CWE-190
#    7. direct request            CWE-425
#
# Every one is accounted for here, by number, and none is left out: a check
# that silently omitted the classes that do not apply would read as a check
# that never considered them. Where a class can occur in a program like this
# one, it is tested -- statically against the source, at runtime against the
# built binary, or both. Where it cannot (tmd has no web interface, no database
# and no login), the line says so and says why, and where a close analogue does
# apply, the analogue is tested instead.
#
# Static checks read the source with comments removed -- by the compiler's own
# preprocessor, so that a comment explaining why strcpy is not used does not
# count as a use of strcpy. Runtime checks need python3 to build hostile
# archives byte by byte; without it they are reported as skipped, never passed.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

CC="${CC:-cc}"
TMD="${TMD:-$REPO_ROOT/bin/tmd}"
FAILURES=()
SKIPPED=()

section() { printf '\n\033[1;94m▸ %s\033[0m\n' "$*"; }
ok()      { printf '  \033[92m✓\033[0m %s\n' "$*"; }
bad()     { printf '  \033[91m✗\033[0m %s\n' "$*"; FAILURES+=("$1"); }
skip()    { printf '  \033[93m-\033[0m %s\n' "$*"; SKIPPED+=("$1"); }
na()      { printf '  \033[96m○\033[0m %s\n' "$*"; }
note()    { printf '    \033[2m%s\033[0m\n' "$*"; }

printf '\n\033[1mMITRE "Lucky 13"\033[0m \033[2m(Christey, Unforgivable Vulnerabilities, 2007)\033[0m\n'

if [ ! -x "$TMD" ]; then
  printf '  \033[91m✗\033[0m %s is not built — run make build first\n\n' "$TMD"
  exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tmd-lucky13.XXXXXX")" || exit 1
trap 'rm -rf "$WORK"' EXIT

HAVE_PY=0
if command -v python3 > /dev/null 2>&1; then
  HAVE_PY=1
fi

# ---------------------------------------------------------------------------
# The source, as code.
#
# Each file once, through the preprocessor with -fpreprocessed: comments are
# removed and nothing else is -- no headers pulled in, no macros expanded -- so
# what is searched is exactly what was written, minus the prose. gcc and clang
# both accept it.
# ---------------------------------------------------------------------------
CODE="$WORK/code"
mkdir -p "$CODE"
for f in src/*.c src/*.h include/*.h; do
  out="$CODE/$(basename "$f")"
  if ! "$CC" -fpreprocessed -dD -E -P "$f" > "$out" 2> /dev/null; then
    printf '  \033[91m✗\033[0m %s could not be read through %s -fpreprocessed\n' "$f" "$CC"
    exit 1
  fi
done

# Lines of code (not comments) matching an extended regex, as "file: line".
code_grep() {
  grep -nE "$1" "$CODE"/* 2> /dev/null | sed "s#^$CODE/##" || true
}

# Run tmd with a time limit, stdin closed, output discarded; print the exit
# status. 124 means it ran out of time -- a hang is a finding, not a pass.
# `timeout` is not in macOS's base system, so this is the portable spelling.
run_bounded() {
  local limit="$1"
  shift
  "$@" < /dev/null > /dev/null 2>&1 &
  local pid=$!
  local waited=0
  while kill -0 "$pid" 2> /dev/null; do
    if [ "$waited" -ge $((limit * 10)) ]; then
      kill -9 "$pid" 2> /dev/null
      wait "$pid" 2> /dev/null
      echo 124
      return
    fi
    sleep 0.1
    waited=$((waited + 1))
  done
  wait "$pid"
  echo $?
}

# A status from run_bounded that means the program died or hung rather than
# answered: killed by a signal (128 and up), or out of time.
survived() {
  [ "$1" -lt 124 ]
}

# Build an archive with python3, from a script on stdin; $1 is the output.
craft() {
  python3 - "$1"
}

# ---------------------------------------------------------------------------
section "1. buffer overflow (CWE-120) — long strings of A"
# ---------------------------------------------------------------------------
hits="$(code_grep '\b(strcpy|strcat|sprintf|vsprintf|gets|stpcpy|wcscpy|wcscat|scanf|sscanf|fscanf|vscanf|vsscanf|vfscanf|realpath|getwd)[[:space:]]*\(')"
if [ -n "$hits" ]; then
  bad "unbounded string functions are called"
  printf '%s\n' "$hits" | sed 's/^/      /'
else
  ok "no unbounded copy, format or scan function is called anywhere in src/"
  note "strcpy, strcat, sprintf, gets, the scanf family and the rest: none"
fi

# The 2007 test, literally: a long run of A in every argument tmd accepts.
LONG_A="$(printf 'A%.0s' $(seq 1 65536))"
worst=0
crashed=""
for opt in -f -m --exclude -t --sort --hash --verify --mtime-before \
           --mtime-after --min-size --max-size --color; do
  if [ "$opt" = "-f" ]; then
    status="$(run_bounded 10 "$TMD" -f "$LONG_A")"
  else
    status="$(run_bounded 10 "$TMD" -f /dev/null "$opt" "$LONG_A")"
  fi
  if ! survived "$status"; then
    crashed="$crashed $opt($status)"
  fi
  if [ "$status" -gt "$worst" ]; then
    worst="$status"
  fi
done
if [ -n "$crashed" ]; then
  bad "64 KiB of A in an argument crashed or hung tmd:$crashed"
else
  ok "64 KiB of A in each of 12 arguments: refused cleanly every time"
fi

if [ "$HAVE_PY" -eq 1 ]; then
  craft "$WORK/long.tar" <<'PY'
import io, sys, tarfile
name = "d/" + "A" * (1 << 20)          # a one-megabyte path
with tarfile.open(sys.argv[1], "w", format=tarfile.GNU_FORMAT) as t:
    for fmt_name in (name, name + "-link"):
        info = tarfile.TarInfo(fmt_name)
        info.size = 1
        t.addfile(info, io.BytesIO(b"x"))
PY
  craft "$WORK/longpax.tar" <<'PY'
import io, sys, tarfile
with tarfile.open(sys.argv[1], "w", format=tarfile.PAX_FORMAT) as t:
    info = tarfile.TarInfo("p/" + "A" * (1 << 20))
    info.linkname = "B" * (1 << 20)
    info.type = tarfile.SYMTYPE
    info.uname = "U" * 100000
    t.addfile(info)
PY
  failed=""
  for a in long.tar longpax.tar; do
    for mode in "" -l -i -R "-t JSON" "-t CSV" "--hash=sha256" --manifest; do
      # shellcheck disable=SC2086  # the mode string is split on purpose
      status="$(run_bounded 20 "$TMD" -f "$WORK/$a" $mode)"
      if ! survived "$status"; then
        failed="$failed $a:${mode:-default}($status)"
      fi
    done
  done
  if [ -n "$failed" ]; then
    bad "a megabyte-long name crashed or hung tmd:$failed"
  else
    ok "megabyte-long GNU and pax names, link targets and owner names: read in every mode"
  fi
else
  skip "the long-name archives: python3 is needed to build them"
fi
note "make security also runs every test under ASan, UBSan and valgrind, and fuzzes the parser"

# ---------------------------------------------------------------------------
section "2. cross-site scripting (CWE-79)"
# ---------------------------------------------------------------------------
na "not applicable as written: tmd produces no HTML and serves nothing to a browser"
if [ "$HAVE_PY" -eq 1 ]; then
  # The analogue that does apply: a member name built to break out of the
  # machine-readable output somebody else will parse, the way <SCRIPT> breaks
  # out of a page.
  craft "$WORK/xss.tar" <<'PY'
import io, sys, tarfile
names = ['<SCRIPT>alert(1)</SCRIPT>', 'a"b\\c', 'line\nbreak', 'comma,"quote"',
         'tab\there', '\x1b[31mred', 'end"}, {"path": "forged']
with tarfile.open(sys.argv[1], "w", format=tarfile.PAX_FORMAT) as t:
    for n in names:
        info = tarfile.TarInfo(n)
        info.size = 1
        t.addfile(info, io.BytesIO(b"x"))
PY
  json_ok="$("$TMD" -f "$WORK/xss.tar" -t JSON 2> /dev/null | python3 -c '
import json, sys
doc = json.load(sys.stdin)
want = ["<SCRIPT>alert(1)</SCRIPT>", "a\"b\\c", "line\nbreak", "comma,\"quote\"",
        "tab\there", "\x1b[31mred", "end\"}, {\"path\": \"forged"]
print("yes" if [e["path"] for e in doc["entries"]] == want else "no")' 2> /dev/null)"
  csv_ok="$("$TMD" -f "$WORK/xss.tar" -t CSV 2> /dev/null | python3 -c '
import csv, sys
rows = list(csv.DictReader(sys.stdin))
print("yes" if len(rows) == 7 and rows[2]["path"] == "line\nbreak" and rows[6]["path"].endswith("forged") else "no")' 2> /dev/null)"
  if [ "$json_ok" = yes ] && [ "$csv_ok" = yes ]; then
    ok "the analogue: names built to break out of JSON and CSV come back intact, as data"
  else
    bad "a crafted member name escaped the JSON or CSV it was written into"
  fi
else
  skip "the output-injection analogue: python3 is needed to build the archive"
fi

# ---------------------------------------------------------------------------
section "3. directory traversal (CWE-23)"
# ---------------------------------------------------------------------------
# tmd never creates a file from a name in an archive. That is checked in the
# source: every call that opens or creates a file is accounted for, and only
# the -o report, named by the user, is opened for writing.
opens="$(code_grep '\b(fopen|freopen|open|openat|creat|mkdir|mkdirat|rename|renameat|unlink|unlinkat|symlink|symlinkat|link|linkat|remove|truncate)[[:space:]]*\(')"
unexpected="$(printf '%s\n' "$opens" | grep -vE \
  '^main\.c:[0-9]+:.*open\(cli\.output_path, O_WRONLY \| O_CREAT \| O_TRUNC, 0644\)|^source\.c:[0-9]+:.*fopen\(path, "rb"\)|^compare\.c:[0-9]+:.*fopen\(path, "r"\)|^$' || true)"
if [ -n "$unexpected" ]; then
  bad "a file-opening call outside the three that are accounted for"
  printf '%s\n' "$unexpected" | sed 's/^/      /'
  note "if it is deliberate, add it to the allowlist in scripts/lucky13.sh"
else
  ok "only three calls open a file: the archive and the manifest read-only, the -o report"
  note "no path from inside an archive is ever opened, created or written"
fi

if [ "$HAVE_PY" -eq 1 ]; then
  craft "$WORK/trav.tar" <<'PY'
import io, sys, tarfile
with tarfile.open(sys.argv[1], "w", format=tarfile.GNU_FORMAT) as t:
    for n in ("../../../tmp/lucky13-escape", "/tmp/lucky13-absolute", "a/../../b"):
        info = tarfile.TarInfo(n)
        info.size = 1
        t.addfile(info, io.BytesIO(b"x"))
PY
  mkdir -p "$WORK/cwd"
  before="$(find "$WORK/cwd" -mindepth 1 | wc -l)"
  err="$(cd "$WORK/cwd" && "$TMD" -f "$WORK/trav.tar" --hash=md5 -v 2>&1 > /dev/null < /dev/null)"
  after="$(find "$WORK/cwd" -mindepth 1 | wc -l)"
  if [ "$before" -eq "$after" ] && [ ! -e /tmp/lucky13-absolute ] &&
     grep -q 'climbs out' <<< "$err" && grep -q 'absolute path' <<< "$err"; then
    ok "a traversing archive writes nothing, and each escape is reported before anyone extracts"
  else
    bad "a traversing archive was not handled as expected"
    note "files before/after: $before/$after"
  fi
else
  skip "the traversal archive: python3 is needed to build it"
fi

# ---------------------------------------------------------------------------
section "4. remote file inclusion (CWE-98)"
# ---------------------------------------------------------------------------
# The C counterpart of include($_GET[...]): loading or running code chosen by
# the input. tmd runs exactly one other program -- a decompressor -- and which
# one comes from a compile-time table, never from a name in the input.
loads="$(code_grep '\b(system|popen|dlopen|execl|execle|execlp|execv|execve|execvpe|posix_spawn|posix_spawnp)[[:space:]]*\(')"
execs="$(code_grep '\bexecvp[[:space:]]*\(')"
if [ -n "$loads" ]; then
  bad "code is loaded or run by a route other than the decompressor table"
  printf '%s\n' "$loads" | sed 's/^/      /'
elif [ "$(printf '%s\n' "$execs" | grep -c .)" -ne 1 ] ||
     ! grep -q 'execvp(w->argv\[0\], args.writable)' <<< "$execs"; then
  bad "execvp is called somewhere other than the decompressor launch"
  printf '%s\n' "$execs" | sed 's/^/      /'
elif ! grep -q 'static const struct wrapper wrappers\[\]' "$CODE/source.c"; then
  bad "the decompressor table is no longer a static const array"
else
  ok "nothing is loaded or run from input: no system, popen, dlopen or shell"
  note "the one execvp runs a decompressor named in a static const table in source.c"
fi

# ---------------------------------------------------------------------------
section "5. SQL injection (CWE-89)"
# ---------------------------------------------------------------------------
na "not applicable: tmd has no database and builds no query in any language"
if code_grep '\b(sqlite3_|mysql_|PQexec|SQLExec)' | grep -q .; then
  bad "a database API appeared in src/; this line needs a real check now"
else
  ok "and none has appeared: no database API is called anywhere in src/"
fi

# ---------------------------------------------------------------------------
section "6. world-writable files (CWE-276, CWE-279)"
# ---------------------------------------------------------------------------
ww="$(find . \( -path ./.git -o -path ./.sanitize -o -path ./.coverage -o \
                -path ./.fuzz -o -path ./obj -o -path ./bin \) -prune -o \
           -type f -perm -0002 -print 2> /dev/null)"
if [ -n "$ww" ]; then
  bad "world-writable files in the source tree"
  printf '%s\n' "$ww" | sed 's/^/      /'
else
  ok "nothing in the source tree is world-writable"
fi

modes="$(grep -hoiE 'install[^#]*-m[[:space:]]*[0-7]{3,4}' Makefile scripts/install.sh 2> /dev/null \
         | grep -oE '[0-7]{3,4}$' | sort -u)"
loose=""
for m in $modes; do
  case "${m: -1}" in
    2 | 3 | 6 | 7) loose="$loose $m" ;;
  esac
done
if [ -z "$modes" ]; then
  bad "no install modes found to check in Makefile or scripts/install.sh"
elif [ -n "$loose" ]; then
  bad "an install recipe makes a file world-writable:$loose"
else
  ok "every install recipe sets an explicit mode, none world-writable: ${modes//$'\n'/ }"
fi

# The one file tmd writes, under the most permissive umask there is. The
# report is opened before the archive is read, so an empty "archive" -- which
# tmd rejects -- still creates it, and needs no fixture.
rm -f "$WORK/report.txt"
( umask 000 && "$TMD" -f /dev/null -o "$WORK/report.txt" > /dev/null 2>&1 < /dev/null )
if [ -f "$WORK/report.txt" ]; then
  # ls -l rather than stat: stat spells "show the mode" differently on GNU
  # and BSD, and the first ten columns of ls -l are the same everywhere.
  # shellcheck disable=SC2012  # one file, a name this script chose
  perm="$(ls -l "$WORK/report.txt" | cut -c1-10)"
  if [ "${perm:8:1}" = "w" ]; then
    bad "under umask 000, the -o report is world-writable ($perm)"
  else
    ok "under umask 000, the -o report is created $perm, not world-writable"
  fi
else
  bad "tmd -o did not create the report at all"
fi

# ---------------------------------------------------------------------------
section "7. direct request (CWE-425)"
# ---------------------------------------------------------------------------
na "not applicable: there is no server, no URL and no administrator function"
note "tmd's whole interface is its command line, and every feature is meant for every caller"

# ---------------------------------------------------------------------------
section "8. authentication bypass (CWE-472)"
# ---------------------------------------------------------------------------
na "not applicable: tmd authenticates nobody and grants access to nothing"
note "the analogue -- trusting a field the other side supplied -- is the archive"
note "header itself: every checksum is verified, every size it states is bounded"
note "before anything is allocated, and #1 and #13 test what happens when they lie"

# ---------------------------------------------------------------------------
section "9. grow-your-own crypto (CWE-327)"
# ---------------------------------------------------------------------------
# This one applies: --hash is tmd's own MD5 and SHA-256, written here rather
# than linked so that the binary still links nothing but libc. What makes that
# acceptable is that they are standard algorithms, checked against an
# independent implementation, and used to identify content -- never to decide
# whether anything is trusted.
if [ "$HAVE_PY" -eq 1 ]; then
  craft "$WORK/hash.tar" <<'PY'
import io, sys, tarfile, os
blobs = [b"", b"abc", b"A" * 1000003, os.urandom(70001), bytes(range(256)) * 300]
with tarfile.open(sys.argv[1], "w", format=tarfile.GNU_FORMAT) as t:
    for i, b in enumerate(blobs):
        info = tarfile.TarInfo("h%d" % i)
        info.size = len(b)
        t.addfile(info, io.BytesIO(b))
PY
  verdict="$(python3 - "$WORK/hash.tar" "$TMD" <<'PY'
import csv, hashlib, io, subprocess, sys, tarfile
archive, tmd = sys.argv[1], sys.argv[2]
want = {}
with tarfile.open(archive) as t:
    for m in t.getmembers():
        data = t.extractfile(m).read()
        want[m.name] = {"md5": hashlib.md5(data).hexdigest(),
                        "sha256": hashlib.sha256(data).hexdigest()}
bad = []
for algo in ("md5", "sha256"):
    out = subprocess.run([tmd, "-f", archive, "--hash=" + algo, "-t", "CSV"],
                         capture_output=True, text=True).stdout
    seen = set()
    for row in csv.DictReader(io.StringIO(out)):
        seen.add(row["path"])
        if row["content_hash"] != algo + ":" + want[row["path"]][algo]:
            bad.append(algo + " " + row["path"])
    # Every member, not just every row that came back: a tmd that printed
    # nothing would otherwise disagree with nothing, and pass.
    for name in want:
        if name not in seen:
            bad.append(algo + " " + name + " (missing)")
print("ok" if not bad else "BAD " + " ".join(bad))
PY
)"
  if [ "$verdict" = ok ]; then
    ok "tmd's MD5 and SHA-256 agree with python's hashlib on 5 inputs, 0 bytes to 1 MB"
  else
    bad "tmd's digests disagree with an independent implementation: $verdict"
  fi
else
  skip "the digest cross-check: python3 is needed as the independent implementation"
fi
crypto="$(code_grep '\b(rand|srand|random|srandom|drand48|lrand48|arc4random)[[:space:]]*\(|\b[a-z_]*(encrypt|decrypt|cipher)[a-z_]*[[:space:]]*\(')"
if [ -n "$crypto" ]; then
  bad "random numbers or encryption appeared in src/; neither belongs in tmd"
  printf '%s\n' "$crypto" | sed 's/^/      /'
else
  ok "no other cryptography: no encryption, no ciphers, no random numbers"
fi
note "the digests identify content somebody else already fingerprinted; MD5 is"
note "offered because load files record it, and is not collision-resistant"

# ---------------------------------------------------------------------------
section "10. privilege escalation (CWE-271)"
# ---------------------------------------------------------------------------
privs="$(code_grep '\b(setuid|seteuid|setreuid|setresuid|setgid|setegid|setregid|setresgid|setgroups|initgroups)[[:space:]]*\(')"
if [ -n "$privs" ]; then
  bad "tmd changes its own privileges"
  printf '%s\n' "$privs" | sed 's/^/      /'
else
  ok "tmd never changes its privileges: no set*uid or set*gid call"
fi
suid=""
for m in $modes; do
  if [ "${#m}" -eq 4 ] && [ "${m:0:1}" != 0 ]; then
    suid="$suid $m"
  fi
done
if [ -n "$suid" ] || grep -qE 'chmod[^#]*[ug]\+s|chmod[^#]*[2467][0-7]{3}' Makefile scripts/install.sh debian/rules 2> /dev/null; then
  bad "an install recipe sets setuid or setgid"
elif [ -u "$TMD" ] || [ -g "$TMD" ]; then
  bad "$TMD is setuid or setgid on disk"
else
  ok "it is never installed setuid or setgid, and the built binary is not"
fi
note "this matters because tmd finds its decompressor on PATH: harmless in an"
note "unprivileged program, and exactly CWE-271 if it were ever made setuid"

# ---------------------------------------------------------------------------
section "11. symlink following (CWE-61)"
# ---------------------------------------------------------------------------
temps="$(code_grep '\b(tmpnam|tempnam|mktemp|mkstemp|mkstemps|mkdtemp|tmpfile)[[:space:]]*\(|"/tmp|"/var/tmp')"
if [ -n "$temps" ]; then
  bad "src/ creates temporary files, which is where symlink races live"
  printf '%s\n' "$temps" | sed 's/^/      /'
else
  ok "tmd creates no temporary files and names no shared directory"
fi
note "the only file it writes is the -o report, at a path the user typed; it"
note "follows a symlink there deliberately, as shell > does (see main.c)"
if [ "$HAVE_PY" -eq 1 ]; then
  craft "$WORK/link.tar" <<'PY'
import io, sys, tarfile
with tarfile.open(sys.argv[1], "w", format=tarfile.GNU_FORMAT) as t:
    l = tarfile.TarInfo("conf")
    l.type = tarfile.SYMTYPE
    l.linkname = "/etc"
    t.addfile(l)
    f = tarfile.TarInfo("conf/cron.d/x")
    f.size = 1
    t.addfile(f, io.BytesIO(b"x"))
PY
  err="$("$TMD" -f "$WORK/link.tar" 2>&1 > /dev/null < /dev/null)"
  if grep -q 'link target leaves the extraction directory: /etc' <<< "$err"; then
    ok "an archive that plants a symlink out of the tree is reported before anyone extracts it"
  else
    bad "a symlink pointing out of the tree was not reported"
  fi
else
  skip "the planted-symlink archive: python3 is needed to build it"
fi

# ---------------------------------------------------------------------------
section "12. hard-coded password (CWE-259)"
# ---------------------------------------------------------------------------
# Comments included this time: a credential in a comment is still a credential.
creds="$(grep -nEi '(password|passwd|passphrase|secret|api[_-]?key|token|credential)[a-z_]*[[:space:]]*(=|:)[[:space:]]*"[^"]{3,}"' \
           src/*.c src/*.h include/*.h scripts/*.sh 2> /dev/null || true)"
if [ -n "$creds" ]; then
  bad "something that looks like a hard-coded credential"
  printf '%s\n' "$creds" | sed 's/^/      /'
else
  ok "no credential-shaped string literal in the source or the scripts"
fi
note "make security runs gitleaks over the whole history as well"

# ---------------------------------------------------------------------------
section "13. integer overflow (CWE-190) — length 0xffffffff"
# ---------------------------------------------------------------------------
if [ "$HAVE_PY" -eq 1 ]; then
  # Headers written by hand, because no tar will write these on purpose: every
  # numeric field that sizes something, at the largest value it can spell in
  # octal and in base-256, plus pax and sparse records claiming 2^64-1.
  craft "$WORK/ints.tar" <<'PY'
import sys

def header(name, size_field, typeflag=b"0", magic=b"ustar  \0", extra=None):
    h = bytearray(512)
    h[0:len(name)] = name
    h[100:108] = b"0000644\0"
    h[108:116] = b"0000000\0"
    h[116:124] = b"0000000\0"
    h[124:136] = size_field
    h[136:148] = b"00000000000\0"
    h[156:157] = typeflag
    h[257:265] = magic
    if extra:
        for off, val in extra:
            h[off:off + len(val)] = val
    h[148:156] = b"        "
    h[148:156] = b"%06o\0 " % (sum(h) & 0o777777)
    return bytes(h)

def pax(records, typeflag=b"x"):
    body = b""
    for k, v in records:
        rec = b" %s=%s\n" % (k, v)
        n = len(rec) + 1
        while len(b"%d" % n) + len(rec) != n:
            n += 1
        body += b"%d" % n + rec
    padded = body + bytes(-len(body) % 512)
    return header(b"PaxHeaders/x", b"%011o\0" % len(body), typeflag,
                  b"ustar\x0000") + padded

blocks = b""
blocks += header(b"octal-max", b"77777777777\0")                 # 8 GiB - 1
blocks += header(b"b256-max", b"\x80" + b"\xff" * 11)             # 2^88 - 1
blocks += header(b"b256-neg", b"\xff" * 12)                       # -1
blocks += pax([(b"size", b"18446744073709551615")]) + header(b"pax-size", b"00000000000\0")
blocks += pax([(b"path", b"x"), (b"99999999999999999999999", b"y")])
blocks += pax([(b"GNU.sparse.major", b"1"), (b"GNU.sparse.minor", b"0"),
               (b"GNU.sparse.realsize", b"18446744073709551615")]) + \
          header(b"sparse", b"00000001000\0") + b"18446744073709551615\n".ljust(512, b"\0")
blocks += header(b"gnu-sparse", b"00000000000\0", b"S", extra=[
    (386, b"77777777777\0"), (398, b"77777777777\0"),
    (483, b"\x80" + b"\xff" * 11), (482, b"\x01")])
blocks += header(b"L-huge", b"77777777777\0", b"L")
open(sys.argv[1], "wb").write(blocks)
PY
  failed=""
  for mode in "" -l -i -R --stat "-t JSON" "--hash=md5" "--sort=size" --manifest; do
    # shellcheck disable=SC2086  # the mode string is split on purpose
    status="$(run_bounded 20 "$TMD" -f "$WORK/ints.tar" $mode)"
    if ! survived "$status"; then
      failed="$failed ${mode:-default}($status)"
    fi
  done
  if [ -n "$failed" ]; then
    bad "sizes of 2^33, 2^64 and 2^88 crashed or hung tmd:$failed"
  else
    ok "octal, base-256 and pax sizes up to 2^88, negative sizes, a 2^64-1 sparse map and pax record length: all survived in 9 modes"
  fi
else
  skip "the integer-overflow archive: python3 is needed to build it"
fi
note "every size an archive states is bounded before it is allocated (MAX_* in"
note "tar.c), rounding saturates rather than wraps, and UBSan traps the rest"

# ---------------------------------------------------------------------------
printf '\n'
if [ ${#SKIPPED[@]} -gt 0 ]; then
  printf '  \033[93mSkipped:\033[0m %d check(s); install python3 to run them\n' "${#SKIPPED[@]}"
fi
if [ ${#FAILURES[@]} -eq 0 ]; then
  printf '  \033[92m✓ all 13 accounted for: none found\033[0m\n\n'
  exit 0
fi
printf '  \033[91m✗ %d check(s) failed\033[0m\n\n' "${#FAILURES[@]}"
exit 1
