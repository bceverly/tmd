<!--
Copyright (c) 2026 Bryan C. Everly
SPDX-License-Identifier: BSD-2-Clause
-->

# Security Policy

## The threat model

`tmd` parses a file that somebody else produced. Every byte of a tar header is
attacker-controlled, and the program is routinely pointed at archives of unknown
provenance — that is what it is *for*. So the vulnerabilities that matter here
are memory-safety bugs in the parser and the ways a hostile archive can make the
program allocate without bound, loop without end, or index outside what it
allocated.

Three properties are load-bearing:

- **It never writes to the archive.** The file is opened read-only, no member is
  extracted, and nothing is created but the report.
- **It never executes anything.** No subprocess, no decompressor, no plugin.
- **It links nothing but libc.** There is no dependency to have a CVE.

## What is done about it

| | |
|---|---|
| Every allocation whose size comes from the file is bounded | a long name or pax header is read to 16 MiB and then truncated with a warning; a sparse map is capped at a million entries; a member gives up after a thousand extension headers |
| Arithmetic saturates rather than wrapping | a size field of 2<sup>64</sup>−1 rounds to the largest whole block, not to zero |
| The binary is hardened | PIE, stack protector, `_FORTIFY_SOURCE=3`, stack-clash protection, CET, full RELRO, BIND_NOW — and `make security` checks the *binary*, not the flags |
| Every push runs the sanitizers | AddressSanitizer, UndefinedBehaviorSanitizer, LeakSanitizer and valgrind, over both test suites and a corpus of deliberately damaged archives |
| Every push runs a fuzzer | libFuzzer where clang is available, a built-in mutation loop otherwise; 15 minutes on the weekly schedule |
| Every push runs the static analyzers | `gcc -fanalyzer`, cppcheck with the CERT C rules, the clang static analyzer, flawfinder, semgrep and CodeQL with `security-extended` |

`make security` runs all of that locally except CodeQL. A clean local run and a
clean CI run mean the same thing, on purpose.

## Reporting a vulnerability

**Please do not open a public issue for a security problem.**

Use GitHub's private reporting — **Security → Report a vulnerability** on
https://github.com/bceverly/tmd — or email **bryan@theeverlys.com**.

Helpful, in rough order of how much it helps:

1. The archive that triggers it. A crash in a tar parser is a file; attaching it
   is worth more than any description of it.
2. The command line and the `tmd --version` output.
3. What you saw — a sanitizer report, a core dump, a hang, unexpected memory
   growth.

If you found it with a fuzzer, the raw input is exactly what is wanted, however
malformed. `.fuzz/crash.tar` from a failed `make fuzz` run is the right thing to
send.

### What to expect

- An acknowledgement within a few days.
- An assessment of whether it is exploitable and how, shared with you.
- A fix and a released version, with credit in the release notes unless you
  would rather not have it.

This is a small tool maintained by one person; there is no bounty and no formal
SLA. What there is, is a reply.

## Supported versions

The most recent release. Given how small this is, a fix means a new version
rather than a backport.

## Scope

In scope: anything that makes `tmd` crash, hang, allocate without bound, read or
write outside its allocations, or report something as fact that the archive does
not say.

Out of scope: the *contents* of an archive being malicious. `tmd` reports what a
header claims and never acts on it — a member named `../../etc/passwd` is listed
exactly as it is stored, because hiding it would defeat the reason somebody is
looking.
