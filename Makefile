# =============================================================================
# tmd — tar metadata dump
#
# Run `make` with no arguments for the list of targets.
# =============================================================================

SHELL       := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c

# The released number, and the one this build actually carries.
#
# BASE_VERSION is the VERSION file: what the packaging, the manpage and the
# release tag all use. VERSION is what gets compiled into the binary, and it
# gains a "-dev" suffix whenever this tree is not the tagged release — see
# scripts/version.sh. A bug report saying "1.0.0.6" names a build somebody else
# can download; one saying "1.0.0.6-dev" names a build only its author has.
#
# Note that VERSION is overloaded: `make release VERSION=1.2.3.4` sets it as a
# command-line override, which wins over the assignment below and propagates to
# every sub-make through MAKEFLAGS. That is deliberate for `make deb`, where
# debian/rules passes the released number on purpose — but it silently stamped
# release builds with a bare number instead of the "-dev" the tree really was,
# so scripts/release.sh clears the override once it has read the argument.
BASE_VERSION := $(shell cat VERSION)
VERSION     := $(shell scripts/version.sh)
PROG        := tmd

BIN_DIR     := bin
OBJ_DIR     := obj
COV_DIR     := .coverage
MAN_PAGE    := man/$(PROG).1

SRC         := $(wildcard src/*.c)
TEST_SRC    := $(wildcard tests/*.c)
# Everything except main.c: the unit tests link the program's own objects and
# bring their own entry point.
LIB_SRC     := $(filter-out src/main.c,$(SRC))

# -----------------------------------------------------------------------------
# Flags
#
# CC, CFLAGS, CPPFLAGS and LDFLAGS are left to the caller, because a distribution
# build passes its own (dpkg-buildflags does, and a package that ignores them
# ships without the hardening the distribution promises). Everything this
# project insists on goes in TMD_* and is appended, so both survive.
# -----------------------------------------------------------------------------
CC          ?= cc
CFLAGS      ?= -O2 -g

TMD_STD     := -std=c11
TMD_CPPFLAGS := -Iinclude -Isrc -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64 \
                -DTMD_VERSION='"$(VERSION)"'

# The warnings the code is actually clean under. -Wconversion and -Wcast-qual
# are here rather than in a "strict" mode nobody runs: they caught real sign
# and const problems while this was being written, and a warning set that is
# only switched on in CI is a warning set that fails in CI.
TMD_WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
                -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wformat-nonliteral \
                -Wcast-qual -Wwrite-strings -Wpointer-arith -Wundef

# The tests cast string literals to char* to build entries by hand, which is
# exactly what -Wwrite-strings and -Wcast-qual exist to stop in real code.
TEST_WARNINGS := $(filter-out -Wcast-qual -Wwrite-strings -Wconversion -Wsign-conversion,$(TMD_WARNINGS))

# -Werror is deliberately NOT on by default: a newer compiler inventing a new
# warning must not stop somebody building the package from source. `make lint`
# and CI set it, which is where a new warning should fail.
WERROR      ?=

# Hardening.
#
# Each optional flag is probed rather than assumed: -fstack-clash-protection
# and -fcf-protection exist on x86 and not everywhere, and a hardening flag
# that stops the build on somebody's architecture is worse than one that is
# absent there. `make security` checks the *binary* for the result, which is
# what catches a flag that was accepted and did nothing.
cc_supports = $(shell $(CC) $(1) -fsyntax-only -x c /dev/null > /dev/null 2>&1 && echo yes)

TMD_HARDEN  := -fstack-protector-strong -fno-common
TMD_HARDEN  += $(if $(call cc_supports,-fstack-clash-protection),-fstack-clash-protection)
TMD_HARDEN  += $(if $(call cc_supports,-fcf-protection=full),-fcf-protection=full)

# _FORTIFY_SOURCE: 3 where it works, 2 where it does not.
#
# Level 3 needs GCC 12 or clang 9. On anything older — 22.04 LTS ships GCC 11,
# and the PPA builds for it — glibc's features.h answers a request for 3 with
#
#     #warning _FORTIFY_SOURCE > 2 is treated like 2 on this platform
#
# which is harmless until -Werror is on, and -Werror is on in CI and in
# `make lint`. So the level is probed rather than assumed, by actually
# compiling something that includes a header: the flag-probe above would not
# catch this, because an empty translation unit never reaches features.h.
#
# -U first, because a distribution's own build flags already define it (22.04's
# dpkg-buildflags sets 2) and redefining a macro to a different value is itself
# a warning.
FORTIFY_LEVEL := $(if $(shell printf '\043include <string.h>\nint main(void){return 0;}\n' \
                        | $(CC) -O2 -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 \
                                -x c - -o /dev/null > /dev/null 2>&1 && echo yes),3,2)

# _FORTIFY_SOURCE needs optimization to do anything at all, and defining it
# under -O0 only produces a warning about that.
ifeq (,$(findstring -O0,$(CFLAGS)))
TMD_HARDEN  += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=$(FORTIFY_LEVEL)
endif
TMD_LDHARDEN := -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack

ALL_CFLAGS   = $(TMD_STD) $(TMD_WARNINGS) $(WERROR) $(TMD_HARDEN) $(CFLAGS)
ALL_TESTFLAGS = $(TMD_STD) $(TEST_WARNINGS) $(WERROR) $(TMD_HARDEN) $(CFLAGS)
ALL_CPPFLAGS = $(TMD_CPPFLAGS) $(CPPFLAGS) -Itests
ALL_LDFLAGS  = $(TMD_LDHARDEN) $(LDFLAGS)

# Installation paths, overridable the way every package build expects.
#
# /usr/local, not /usr: this default is what `make install` uses, and /usr
# belongs to the distribution's package manager. A file written there by hand is
# one dpkg does not know about, and the PPA package would then be fighting it.
# debian/rules passes prefix=/usr explicitly, so the package still lands where a
# package should.
prefix      ?= /usr/local
exec_prefix ?= $(prefix)
bindir      ?= $(exec_prefix)/bin
datarootdir ?= $(prefix)/share
mandir      ?= $(datarootdir)/man
DESTDIR     ?=
INSTALL     ?= install

.DEFAULT_GOAL := help

# -----------------------------------------------------------------------------
# Help — `make` with no target prints this.
#
# Targets are documented with a `## description` comment on the target line and
# grouped by `##@ Section` headers, so this list cannot drift out of sync with
# the targets that actually exist.
# -----------------------------------------------------------------------------
.PHONY: help
help:
	@printf '\n  \033[1;97mtmd\033[0m \033[2m%s\033[0m — dump the metadata out of a tar archive\n' '$(VERSION)'
	@printf '  \033[2mUsage: make <target>\033[0m\n'
	@awk 'BEGIN {FS = ":.*##"} \
		/^##@/ { printf "\n  \033[1;94m%s\033[0m\n", substr($$0, 5); next } \
		/^[a-zA-Z0-9_-]+:.*?##/ { printf "    \033[96m%-16s\033[0m %s\n", $$1, $$2 } \
	' $(MAKEFILE_LIST)
	@printf '\n  \033[2mFirst time? Run: make install-dev && make build && make test\033[0m\n\n'

##@ Build

.PHONY: build
build: $(BIN_DIR)/$(PROG) $(MAN_PAGE) ## Compile the program into ./bin, regenerating the manpage

$(BIN_DIR)/$(PROG): $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRC)) | $(BIN_DIR)
	@$(CC) $(ALL_CFLAGS) -o $@ $^ $(ALL_LDFLAGS)
	@printf '  \033[92m✓\033[0m %s (%s)\n' "$@" "$(VERSION)"

#
# A stamp that changes only when the version string does.
#
# The version is compiled in with -DTMD_VERSION, so a binary built with a
# different one is stale — but nothing in the file timestamps says so, and make
# correctly reports "nothing to be done". That bites in a specific and
# confusing way: `make deb` builds with the released number (debian/rules
# passes VERSION explicitly), leaving bin/tmd reporting 1.0.0.0 in a working
# tree whose version is 1.0.0.0-dev. `make lint` then objects and tells you to
# run `make build` — which does nothing, because from make's point of view
# everything is up to date.
#
# The FORCE prerequisite is what makes the recipe run at all. A target whose
# only prerequisite is order-only is considered up to date by make as soon as
# the file exists — so without it the recipe never ran, the stamp kept its first
# value forever, and nothing was ever rebuilt for a version change, which is
# precisely the failure it was added to prevent. `make release` is what caught
# it: it wrote VERSION, rebuilt, and found the binary still reporting the old
# number.
#
# With FORCE the recipe runs every time and rewrites the file only when the
# content differs, so the mtime moves exactly when a rebuild is needed and not
# otherwise.
.PHONY: FORCE
FORCE:

$(OBJ_DIR)/version.stamp: FORCE | $(OBJ_DIR)
	@printf '%s' '$(VERSION)' | cmp -s - $@ 2>/dev/null \
	  || printf '%s' '$(VERSION)' > $@

$(OBJ_DIR)/%.o: src/%.c $(OBJ_DIR)/version.stamp | $(OBJ_DIR)
	@$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR) $(OBJ_DIR) $(COV_DIR):
	@mkdir -p $@

# The manpage is generated from the program's own --help, so it depends on the
# binary that prints it and on the option table that binary was built from.
# Not circular: the page depends on the program, never the other way round.
$(MAN_PAGE): $(BIN_DIR)/$(PROG) src/options.def scripts/gen-man.sh VERSION
	@scripts/gen-man.sh $(BIN_DIR)/$(PROG) $@

-include $(wildcard $(OBJ_DIR)/*.d)

.PHONY: debug
debug: ## Rebuild unoptimized with symbols, for a debugger
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory build CFLAGS="-O0 -g3 -fno-omit-frame-pointer"

.PHONY: run
run: build ## Build, then list this project's own source tree as a tar archive
	@tar -cf /tmp/tmd-demo.tar src include tests
	@./$(BIN_DIR)/$(PROG) -f /tmp/tmd-demo.tar -S

##@ Test

.PHONY: test
test: test-unit test-cli test-memory coverage ## Unit + CLI tests, sanitizers, and the 80% coverage gate

.PHONY: test-unit
test-unit: $(BIN_DIR)/unittests ## Run the C unit tests
	@./$(BIN_DIR)/unittests

# $(filter %.c,$^) rather than $^: the stamp is a real prerequisite, so that a
# change of version rebuilds this too, and $^ would hand it to the linker as an
# input file — which fails with "file format not recognized; treating as linker
# script". It cannot be an order-only prerequisite instead, because those do not
# trigger a rebuild when they change, which is the whole point of it.
$(BIN_DIR)/unittests: $(LIB_SRC) $(TEST_SRC) $(OBJ_DIR)/version.stamp | $(BIN_DIR)
	@$(CC) $(ALL_CPPFLAGS) $(ALL_TESTFLAGS) -o $@ $(filter %.c,$^) $(ALL_LDFLAGS)

.PHONY: test-cli
test-cli: build ## Run the end-to-end tests against real GNU and BSD archives
	@tests/cli/run.sh ./$(BIN_DIR)/$(PROG)

.PHONY: test-memory
test-memory: ## Run everything again under the sanitizers and valgrind
	@scripts/memcheck.sh

.PHONY: coverage
coverage: ## Measure line coverage, refresh the README badge, fail below 80%
	@scripts/coverage.sh

.PHONY: fuzz
fuzz: ## Throw mutated archives at the parser (FUZZ_SECONDS=30 by default)
	@scripts/fuzz.sh

##@ Quality

.PHONY: lint
lint: build ## Static analysis, the copyright audit and the manpage freshness check
	@scripts/lint.sh

.PHONY: security
security: build ## Run the same security scanners CI runs, locally
	@scripts/security.sh

# The manpage and the README's Usage block both restate `tmd --help`. Both are
# generated, and `make lint` checks both, so adding an option updates all three
# from the one definition in src/options.def.
.PHONY: man
man: docs ## Force the manpage to be regenerated from --help

.PHONY: docs
docs: $(BIN_DIR)/$(PROG) ## Regenerate the manpage and the README's Usage block
	@scripts/gen-man.sh ./$(BIN_DIR)/$(PROG) $(MAN_PAGE)
	@scripts/gen-readme-usage.sh ./$(BIN_DIR)/$(PROG) README.md
	@printf '  \033[92m✓\033[0m %s and the README Usage block are current\n' "$(MAN_PAGE)"

.PHONY: man-check
man-check: build ## Fail if the committed manpage or README block is out of date
	@scripts/gen-man.sh --check ./$(BIN_DIR)/$(PROG) $(MAN_PAGE)
	@scripts/gen-readme-usage.sh --check ./$(BIN_DIR)/$(PROG) README.md

.PHONY: install-hooks
install-hooks: ## Install the git pre-commit (lint) and pre-push (test) hooks
	@scripts/install-hooks.sh

##@ Installing

.PHONY: install
install: build ## Install into $(prefix) (default /usr/local), asking for sudo if needed
	@scripts/install.sh

.PHONY: uninstall
uninstall: ## Remove what `make install` installed
	@scripts/install.sh --uninstall

##@ Packaging

.PHONY: deb
deb: build ## Build the binary .deb into the parent directory
	@scripts/build-deb.sh

.PHONY: deb-source
deb-source: build ## Build the signed source package for a Launchpad upload
	@scripts/build-deb.sh --source

.PHONY: deb-sbuild
deb-sbuild: build ## Build in a clean chroot, the way a Launchpad builder does
	@scripts/build-deb.sh --sbuild

# The staged install debian/rules calls.
#
# Separate from `make install`, because the two jobs want different answers.
# This one assembles a directory that is about to become a package: it writes
# wherever DESTDIR says, uses prefix=/usr as debian/rules passes it, never asks
# for privileges, and never touches the man database -- indexing a staging tree
# would describe a filesystem that does not exist yet. `make install` puts files
# on a live system, defaults to /usr/local so it cannot collide with what dpkg
# owns, and escalates only if it has to.
.PHONY: install-tree
install-tree: build ## Install into DESTDIR (used by the package build)
	@$(INSTALL) -d $(DESTDIR)$(bindir)
	@$(INSTALL) -m 0755 $(BIN_DIR)/$(PROG) $(DESTDIR)$(bindir)/$(PROG)
	@$(INSTALL) -d $(DESTDIR)$(mandir)/man1
	@$(INSTALL) -m 0644 $(MAN_PAGE) $(DESTDIR)$(mandir)/man1/$(PROG).1
	@printf '  \033[92m✓\033[0m installed into %s\n' "$(DESTDIR)$(prefix)"

.PHONY: deb-clean
deb-clean: ## Remove packaging output
	@rm -rf debian/$(PROG) debian/.debhelper debian/files debian/changelog
	@rm -f debian/*.substvars debian/*.debhelper debian/*.debhelper.log
	@rm -f debian/debhelper-build-stamp
	@rm -f ../$(PROG)_*.deb ../$(PROG)_*.changes ../$(PROG)_*.buildinfo \
	       ../$(PROG)_*.dsc ../$(PROG)_*.tar.* 2>/dev/null || true

##@ Release

.PHONY: release
release: ## Bump the version, tag it and push (make release VERSION=1.2.3.4)
	@scripts/release.sh

##@ Setup

.PHONY: install-dev
install-dev: ## Install the compilers, analyzers and packaging tools this needs
	@scripts/install-dev.sh

##@ Housekeeping

# Everything below is regenerated by some `make` target, so removing it leaves
# the tree exactly as a fresh checkout. The generated manpage is the one
# deliberate exception: it is committed, because a release tarball has to carry
# a manpage and building one requires building the program first.
CLEAN_DIRS := $(OBJ_DIR) $(BIN_DIR) $(COV_DIR) .lint .sanitize .fuzz               .security-reports

.PHONY: clean
clean: ## Remove every intermediate file: objects, binaries, coverage, logs
	@rm -rf $(CLEAN_DIRS)
	@# gcov scatters these next to whatever was compiled, which is not always
	@# where the build put its objects.
	@find . -path ./.git -prune -o \
	        \( -name '*.o' -o -name '*.d' -o -name '*.gcda' -o -name '*.gcno' \
	           -o -name '*.gcov' -o -name '*.core' -o -name 'core.[0-9]*' \
	           -o -name 'vgcore.*' \) -type f -print0 2>/dev/null \
	  | xargs -0 -r rm -f
	@echo "Cleaned. (The generated manpage and coverage badge are kept; both are committed.)"

.PHONY: distclean
distclean: clean deb-clean ## clean, plus the Debian packaging output
	@echo "Distribution clean."
