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
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJ_DIRS_STAMP)
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
$(OUTDIR)/tests/%: $(TESTDIR)/%.c $(LIB_STATIC) $(LIBRECLAW_STATIC) | $(OUTDIR)/tests
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
.PHONY: clean
clean:
	rm -rf $(BUILDDIR)/$(BUILD_TYPE)
	rm -f $(PROJECT_NAME)-1.0.pc

.PHONY: clean-deps
clean-deps:
	$(MAKE) -C $(LIBRECLAW_DIR) clean-all 2>/dev/null || true

# Clean everything including deps
.PHONY: clean-all
clean-all: clean-deps
	rm -rf $(BUILDDIR)
	rm -f $(PROJECT_NAME)-1.0.pc

.PHONY: distclean
distclean: clean-all

# Help
.PHONY: help
help:
	@echo "clawtilla build system"
	@echo ""
	@echo "Targets:"
	@echo "  all           - Build library, daemon, CLI, GTK client, web stub, plugins (default)"
	@echo "  shared        - Build shared library only"
	@echo "  static        - Build static library only"
	@echo "  daemon        - Build clawtillad only"
	@echo "  cli           - Build the clawtilla CLI only"
	@echo "  gtk           - Build the GTK4 client only"
	@echo "  web           - Build the web client stub only"
	@echo "  plugins       - Build bundled plugins"
	@echo "  config-files  - Regenerate data/{example,default}-config.yaml and the docs tables"
	@echo "  docs-check    - Fail on undocumented public API or stale doc references"
	@echo "  test          - Build and run the hermetic test suite"
	@echo "  test-verbose  - Same, with verbose output"
	@echo "  test-integration - Run tests needing podman/libvirt/gowl (CLAWT_TEST_INTEGRATION=1)"
	@echo "  install       - Install library, headers, binaries, plugins and data"
	@echo "  uninstall     - Remove installed files"
	@echo "  clean         - Remove current build type ($(BUILD_TYPE)), keeping deps built"
	@echo "  clean-deps    - Clean deps/libreclaw (slow to rebuild -- opt in)"
	@echo "  clean-all     - Remove all build artifacts including deps"
	@echo "  help          - Show this help"
	@echo ""
	@echo "Build options:"
	@echo "  DEBUG=1       - Debug symbols, no optimization"
	@echo "  ASAN=1        - Address sanitizer (turns GIR off; GIR=1 re-enables)"
	@echo "  UBSAN=1       - Undefined behavior sanitizer"
	@echo "  GIR=0|1       - Force GObject Introspection off/on (default: auto-detect)"
	@echo "  PREFIX=/path  - Installation prefix (default: /usr/local)"
	@echo ""
	@echo "Output directories:"
	@echo "  make          -> build/release/"
	@echo "  make DEBUG=1  -> build/debug/"

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
