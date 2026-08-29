# rules.mk - Common build rules for clawtilla
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Mirrors deps/libreclaw/rules.mk, including the hard-won bits: the single
# up-front mkdir stamp and the dep archives as test prerequisites.

# Object subdirectories (derived from every object list)
OBJ_DIRS := $(sort $(dir $(LIB_OBJECTS) $(DAEMON_OBJECTS) $(CLI_OBJECTS) \
                         $(GTK_OBJECTS) $(WEB_OBJECTS)))

# All object directories are created up front in a single mkdir via one
# stamp target.  Concurrent per-object "mkdir -p" calls under a parallel
# build (make -j) race on shared parent components and intermittently fail
# with "can't create ...o: No such file or directory"; one mkdir in one
# process does not.  The pattern rules depend on the stamp order-only so
# it is made exactly once before any compilation.
OBJ_DIRS_STAMP := $(OBJDIR)/.dirs-stamp

# The stamp depends on the Makefile because OBJ_DIRS is derived from the
# source lists there.  Without it, adding a source in a new subdirectory
# compiles against a stamp made before that directory existed, and gcc fails
# with "can't create ...o: No such file or directory" -- which reads like a
# permissions problem rather than a missing mkdir.
$(OBJ_DIRS_STAMP): Makefile config.mk
	@mkdir -p $(OBJ_DIRS)
	@touch $@

# Generic pattern rule for library object files
#
# -MMD -MP writes a .d beside each .o listing the headers it included,
# and those are included at the bottom of this file.  Without them a
# header edit rebuilt nothing at all: the sources were untouched, so make
# had no reason to act, and `make test` went on exercising objects
# compiled against the previous declarations.  That is the same class of
# failure as a test binary that is not relinked, except it reaches every
# source in the tree rather than one target -- and it is silent, because
# a stale build and an unchanged one look identical from the outside.
#
# -MP emits a phony target for each header so that *deleting* one does
# not leave make refusing to build anything with "No rule to make target".

# Objects rebuild when the build *configuration* changes, not only when a
# source does.
#
# Without this, `make DEBUG=1 ASAN=1 all` followed by `make DEBUG=1 all`
# leaves instrumented objects in place and the link fails on unresolved
# __asan_* symbols -- pointing at whichever file happened to be linked
# first rather than at the flag change that caused it. That cost a bad
# push here: the build failed, the code was fine, and the two look
# identical from the exit status.
#
# FORCE runs the comparison every time; the stamp's timestamp moves only
# when the flags actually differ, so repeated identical builds stay
# incremental. Copied from ai-glib, which had already paid for it.
.PHONY: FORCE
FORCE:

$(BUILD_FLAGS_STAMP): FORCE | $(OUTDIR)
	@{ \
		printf '%s\n' "CC=$(CC)" "CFLAGS=$(CFLAGS)" \
			"LDFLAGS=$(LDFLAGS)" > $@.tmp; \
		if test -r $@ && cmp -s $@.tmp $@; then \
			rm -f $@.tmp; \
		else \
			mv -f $@.tmp $@; \
		fi; \
	}

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(BUILD_FLAGS_STAMP) | $(OBJ_DIRS_STAMP)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Create build directories
$(OUTDIR):
	mkdir -p $(OUTDIR)

$(OBJDIR):
	mkdir -p $(OBJDIR)

# Test compilation rule
#
# The bundled static libs are prerequisites, not just link arguments:
# without them a fix in a dep rebuilt its .a but never relinked the tests,
# so the suite kept exercising the old code and the failure looked like the
# fix not working.
#
# BUILD_OUTDIR lets a test that spawns one of our binaries find the one
# matching its own build type.  Without it those tests fall back to
# build/release, so `make DEBUG=1 test` would exercise release binaries --
# or skip silently if only the debug tree exists.
#
# CLAWT_TEST_FIXTURES points at tests/fixtures/ so a test can find its
# golden files and fake modules without guessing at the cwd it was run in.
#
# CLAWT_TEST_POD_MODULE_DIR is the built podomation modules, which a test
# binary cannot otherwise find: the daemon resolves them beside the
# running binary, and for a test that is build/<type>/tests.  A test that
# needs a real module names this and skips when it is absent -- passing
# because the module could not be loaded is passing for the wrong reason,
# and that is precisely what the first draft of the mute-socket test did.
$(OUTDIR)/tests/%: $(TESTDIR)/%.c $(LIB_STATIC) $(LIBRECLAW_STATIC) \
                   $(BUILD_FLAGS_STAMP) | $(OUTDIR)/tests
	$(CC) $(CFLAGS) -MMD -MP -I$(SRCDIR) -I$(TESTDIR) \
		-DBUILD_OUTDIR='"$(OUTDIR)"' \
		-DCLAWT_TEST_FIXTURES='"$(CURDIR)/$(TESTDIR)/fixtures"' \
		-DCLAWT_TEST_SRCDIR='"$(CURDIR)"' \
		-DCLAWT_TEST_POD_MODULE_DIR='"$(CURDIR)/$(CLAWT_POD_MODULE_DIR)"' \
		$< -o $@ $(LIB_STATIC) $(LDFLAGS)

$(OUTDIR)/tests: | $(OUTDIR)
	mkdir -p $@

# Clean current build type.  We deliberately do NOT clean deps/libreclaw
# here: it owns a large dependency tree of its own and rebuilding it takes
# minutes, so `make clean` staying inside clawtilla is the useful default.
# `make clean-deps` is there when you actually mean it.
.PHONY: clean  ## Remove this build type's output, leaving deps/libreclaw built
clean:
	rm -rf $(BUILDDIR)/$(BUILD_TYPE)
	rm -f $(PROJECT_NAME)-1.0.pc

# Every vendored project, at any depth, in both build types.
#
# The list is *found* rather than written down.  The tree is fifteen
# submodules three levels deep -- libreclaw brings five, podomation
# brings six of its own, and ai-glib brings one -- and the hand-written
# version reached the first level only, so `make clean-all` left nine
# dep build trees standing and the next `make all` reused them.
#
# A project root is a directory holding a Makefile whose parent is named
# `deps`.  That is what separates a vendored project from podomation's
# per-module Makefiles under `modules/`, which are not standalone and
# have no clean target of their own.
#
# Found by walking the filesystem rather than by asking git, so this
# works in an exported tree with no .git in it.
DEP_ROOTS = $(shell find deps -name Makefile -type f 2>/dev/null \
	| while read -r mk; do \
		dir=$$(dirname "$$mk"); \
		[ "$$(basename "$$(dirname "$$dir")")" = deps ] && echo "$$dir"; \
	done | sort)

# Both build types, because a dep cleaned with only one leaves the other
# standing.  libreclaw's own clean-all pins DEBUG=0 for podomation and
# htmx-glib, which is why a debug build of podomation -- 166 object
# files, measured -- survived every `make clean-all` there has been.
#
# A missing Makefile is an uninitialised submodule and is skipped in
# silence; a clean that *runs* and fails is reported and fails the
# target, because "clean-all succeeded" has to mean the next build
# starts from nothing.
.PHONY: clean-deps  ## Clean every vendored dep, recursively, both build types
clean-deps:
	@failed=""; \
	for dir in $(DEP_ROOTS); do \
		echo "Cleaning $$dir..."; \
		for build_type in "DEBUG=0" "DEBUG=1"; do \
			$(MAKE) --no-print-directory -C "$$dir" clean $$build_type \
				>/dev/null 2>&1 || failed="$$failed $$dir($$build_type)"; \
		done; \
	done; \
	if [ -n "$$failed" ]; then \
		echo "clean-deps: these did not clean:$$failed" >&2; \
		exit 1; \
	fi

# Clean everything including deps
.PHONY: clean-all  ## Remove every build artifact, deps included
clean-all: clean-deps
	rm -rf $(BUILDDIR)
	rm -f $(PROJECT_NAME)-1.0.pc

.PHONY: distclean  ## Alias for clean-all
distclean: clean-all

# Help
#
# The target list is *derived* rather than typed out.  The typed one had
# drifted badly: it named nineteen targets out of thirty, called the web
# client a stub long after it reached parity with the GTK one, and left
# `tests`, `parity`, `web-smoke`, `gir`, `mcp-server` and `version`
# undiscoverable -- the same failure this codebase keeps finding in
# hand-maintained lists of things the build already knows.
#
# So the description lives beside the declaration: `.PHONY: <target>  ##
# <what it does>`, and awk reads both makefiles for them.  A target added
# without a `##` still appears, marked "(undocumented)", because a
# missing description is worth seeing and a missing *target* is not.
# `## @internal` is how a target says it is plumbing rather than
# something to run -- a decision, spelled out, rather than silence.
#
# Targets a name cannot survive as a shell word ($(MAKECMDGOALS)) and the
# serialization helper (__serialize) are skipped on their spelling.
.PHONY: help  ## Show this help
help:
	@echo "clawtilla build system"
	@echo ""
	@echo "Targets:"
	@awk '/^\.PHONY:/ { \
		line = $$0; desc = ""; \
		i = index(line, "##"); \
		if (i > 0) { \
			desc = substr(line, i + 2); \
			line = substr(line, 1, i - 1); \
			sub(/^[ \t]+/, "", desc); \
			sub(/[ \t]+$$/, "", desc); \
		} \
		sub(/^\.PHONY:/, "", line); \
		n = split(line, t, "[ \t]+"); \
		for (j = 1; j <= n; j++) { \
			if (t[j] == "" || t[j] ~ /\$$/ || t[j] ~ /^__/) continue; \
			if (desc == "@internal") continue; \
			if (seen[t[j]]++) continue; \
			printf "  %-17s %s\n", t[j], \
				(desc == "" ? "(undocumented)" : desc); \
		} }' Makefile rules.mk
	@echo ""
	@echo "Build options:"
	@echo "  DEBUG=1       - Debug symbols, no optimization"
	@echo "  ASAN=1        - Address sanitizer (turns GIR off; GIR=1 re-enables)"
	@echo "  UBSAN=1       - Undefined behavior sanitizer"
	@echo "  GIR=0|1       - Force GObject Introspection off/on (default: auto-detect)"
	@echo "  PREFIX=/path  - Installation prefix (default: $(PREFIX))"
	@echo ""
	@echo "  WEB_SMOKE_URL / WEB_SMOKE_AGENT - what \`make web-smoke\` asks, and about which agent"
	@echo ""
	@echo "Output directories:"
	@echo "  make          -> build/release/"
	@echo "  make DEBUG=1  -> build/debug/"
	@echo "  this run      -> $(OUTDIR)/  (BUILD_TYPE=$(BUILD_TYPE))"

# ─────────────────────────────────────────────────────────────────────
# Header dependencies
#
# Written by -MMD above, one .d beside each object and each test binary.
# Included with a leading dash so that a first build, where none of them
# exist yet, is not an error.
#
# This is what makes a header edit rebuild the things that include it.
# Without it `make test` after changing a declaration compiles nothing
# and passes, which is indistinguishable from a change that worked --
# and it silently defeats the habit of proving a test fails without its
# fix, because the sabotage never reaches the binary.
# ─────────────────────────────────────────────────────────────────────
-include $(shell find $(BUILDDIR)/$(BUILD_TYPE) -name '*.d' 2>/dev/null)
