#!/usr/bin/env sh
#
# Copyright (c) 2026 Bryan C. Everly
# SPDX-License-Identifier: BSD-2-Clause
#
# The feature-test macros this program is compiled with, for this platform.
#
#   scripts/features.sh
#
# One place, because there are seven callers -- the Makefile and every analysis
# script -- and the answer is not the same everywhere. When it lived in each of
# them, changing it changed one and left six saying something else; the macOS
# work found exactly that, with the Makefile correct and five scripts still
# asserting the Linux answer.
#
#   Linux/glibc   _XOPEN_SOURCE=700 asks for POSIX.1-2008 and exposes
#                 everything used here.
#
#   macOS         the same request also switches off __DARWIN_C_LEVEL, and
#                 getopt_long lives outside POSIX. _DARWIN_C_SOURCE puts it
#                 back. Without it the build fails on an undeclared function.
#
#   the BSDs      __BSD_VISIBLE is on by default and _XOPEN_SOURCE turns it
#                 off, hiding getopt_long there for the same reason. They are
#                 better served by asking for nothing at all.
#
# _FILE_OFFSET_BITS=64 is a glibc question. The BSDs have had a 64-bit off_t
# since before the macro existed, and on macOS it is accepted and ignored --
# harmless, but left off where it means nothing.
#
# sh rather than bash: the Makefile runs this for every target, so it is on the
# path of every build, and there is nothing here that needs more than sh.
case "$(uname -s)" in
Darwin)
    echo "-D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64 -D_DARWIN_C_SOURCE"
    ;;
FreeBSD | NetBSD | OpenBSD | DragonFly)
    echo "-D_FILE_OFFSET_BITS=64"
    ;;
*)
    echo "-D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64"
    ;;
esac
