# Makefile - Main build file for clawtilla
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage:
#   make                 - Release build to build/release/
#   make DEBUG=1         - Debug build to build/debug/
#   make test            - Run the hermetic test suite
#   make install         - Install to PREFIX
#   make help            - Show all targets
#
# Deliberately mirrors deps/libreclaw/Makefile: same phase serialization,
# same goal-mixing guard, same generated-header handling.

# When clean and build targets appear together (e.g. make clean all -j),
# serialize them via sub-make so clean finishes before the build starts.
ifneq ($(filter clean clean-all clean-deps distclean,$(MAKECMDGOALS)),)
ifneq ($(filter-out clean clean-all clean-deps distclean,$(MAKECMDGOALS)),)

.PHONY: $(MAKECMDGOALS) __serialize
$(MAKECMDGOALS): __serialize ;
__serialize:
	$(MAKE) --no-print-directory $(filter clean clean-all clean-deps distclean,$(MAKECMDGOALS))
	$(MAKE) --no-print-directory $(filter-out clean clean-all clean-deps distclean,$(MAKECMDGOALS))

__MIXED := 1
endif
endif

ifndef __MIXED

# Capture git commit SHA for version info; append -UNSTAGED if the tree is dirty
GIT_SHA_BASE := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo "unknown")
GIT_DIRTY    := $(shell git diff --quiet && git diff --cached --quiet || echo "-UNSTAGED")
GIT_SHA      := $(GIT_SHA_BASE)$(GIT_DIRTY)

include config.mk

# rules.mk is included further down and its first rule would otherwise
# become the default goal, so a bare `make` would build a directory stamp
# and nothing else.  Pin the default explicitly.
.DEFAULT_GOAL := all

# ============================================================
# Public headers (installed to $(INCLUDEDIR)/clawtilla-1.0/)
# ============================================================
PUBLIC_HEADERS = \
	$(SRCDIR)/clawtilla.h \
	$(SRCDIR)/clawt-types.h \
	$(SRCDIR)/clawt-enums.h \
	$(SRCDIR)/clawt-error.h \
	$(SRCDIR)/clawt-util.h \
	$(SRCDIR)/config/clawt-config-schema.h \
	$(SRCDIR)/config/clawt-secret-ref.h \
	$(SRCDIR)/config/clawt-config.h \
	$(SRCDIR)/computer/clawt-mount.h \
	$(SRCDIR)/link/clawt-link.h \
	$(SRCDIR)/link/clawt-link-server.h

# ============================================================
# Library sources
# ============================================================
LIB_SOURCES = \
	$(SRCDIR)/clawt-enums.c \
	$(SRCDIR)/clawt-error.c \
	$(SRCDIR)/clawt-util.c \
	$(SRCDIR)/config/clawt-config-schema.c \
	$(SRCDIR)/config/clawt-schema-render.c \
	$(SRCDIR)/config/clawt-secret-ref.c \
	$(SRCDIR)/config/clawt-config.c \
	$(SRCDIR)/computer/clawt-mount.c \
	$(SRCDIR)/link/clawt-link.c \
	$(SRCDIR)/link/clawt-link-server.c

# Object files
LIB_OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(LIB_SOURCES))

# Test files
TEST_SOURCES = $(wildcard $(TESTDIR)/test-*.c)
TEST_BINARIES = $(patsubst $(TESTDIR)/%.c,$(OUTDIR)/tests/%,$(TEST_SOURCES))

# Include common rules
include rules.mk

# ============================================================
# Phases
#
# Serialized on purpose.  Recursive dep sub-makes race the jobserver, and
# libreclaw's own build already documents why its module step has to run
# alone; we inherit that by calling into it rather than reimplementing it.
# ============================================================
.PHONY: all
all:
	@$(MAKE) --no-print-directory check-submodules
	@$(MAKE) --no-print-directory deps
	@$(MAKE) --no-print-directory all-impl

.PHONY: all-impl
all-impl: $(OUTDIR)/clawt-version.h shared static daemon cli gtk web \
          $(PROJECT_NAME)-1.0.pc plugins gir

# ── Submodule freshness ───────────────────────────────────────────────
#
# Warns when deps/libreclaw is checked out behind the commit this tree
# records.  Being ahead is fine and stays silent: that is what a dep under
# active development looks like, and we are actively developing libreclaw.
.PHONY: check-submodules
check-submodules:
	@if [ ! -f $(LIBRECLAW_DIR)/Makefile ]; then \
		echo "error: deps/libreclaw is not checked out."; \
		echo "       run: git submodule update --init --recursive"; \
		exit 1; \
	fi
	@recorded=$$(git ls-tree HEAD $(LIBRECLAW_DIR) 2>/dev/null | awk '{print $$3}'); \
	actual=$$(git -C $(LIBRECLAW_DIR) rev-parse HEAD 2>/dev/null); \
	if [ -n "$$recorded" ] && [ -n "$$actual" ] && [ "$$recorded" != "$$actual" ]; then \
		if git -C $(LIBRECLAW_DIR) merge-base --is-ancestor "$$actual" "$$recorded" 2>/dev/null; then \
			echo "warning: deps/libreclaw is BEHIND the recorded commit."; \
			echo "         run: git submodule update --init --recursive"; \
		fi; \
	fi

# ── Bundled dependency: libreclaw ─────────────────────────────────────
#
# One recursive make builds libreclaw's library, its five bundled dep
# archives and the podomation loadable modules.  Its `static` target alone
# builds none of those, so we ask for `all`.  Always DEBUG=0: deps use
# release output regardless of our own build type, as libreclaw does.
.PHONY: deps
deps:
	@echo "Building libreclaw and its dependencies..."
	$(MAKE) -C $(LIBRECLAW_DIR) all DEBUG=0

$(LIBRECLAW_STATIC):
	@$(MAKE) --no-print-directory deps

# ── Generated headers ─────────────────────────────────────────────────
#
# config.mk is a prerequisite, not just the template: the version numbers
# live there.  Without it a version bump relinks the library under the new
# soname while the header keeps reporting the old numbers, so
# CLAWT_CHECK_VERSION returns FALSE for a version the library genuinely is
# -- and keeps doing so across rebuilds.  libreclaw had exactly this bug;
# it is fixed there too.
$(OUTDIR)/clawt-version.h: $(SRCDIR)/clawt-version.h.in config.mk | $(OUTDIR)
	@echo "Generating clawt-version.h..."
	@sed -e 's/@VERSION_MAJOR@/$(VERSION_MAJOR)/g' \
	     -e 's/@VERSION_MINOR@/$(VERSION_MINOR)/g' \
	     -e 's/@VERSION_MICRO@/$(VERSION_MICRO)/g' \
	     $< > $@

# Embed data/default-config.yaml into a generated C header at build time.
# The awk script escapes backslashes and double-quotes, then wraps each
# line in a C string literal so the CLI can #include it directly and serve
# it from --generate-config without reading a file at runtime.
DEFAULT_CONFIG_SRC = $(DATADIR_SRC)/default-config.yaml
DEFAULT_CONFIG_H   = $(OUTDIR)/clawt-default-config.h

$(DEFAULT_CONFIG_H): $(DEFAULT_CONFIG_SRC) | $(OUTDIR)
	@echo "Generating $(notdir $@) from $(DEFAULT_CONFIG_SRC)..."
	@printf 'static const gchar default_config_yaml[] =\n' > $@
	@awk '{ gsub(/\\/, "\\\\"); gsub(/"/, "\\\""); printf "    \"%s\\n\"\n", $$0 }' $< >> $@
	@printf '    ;\n' >> $@

# Every library object needs the version header and the dep archives first.
$(LIB_OBJECTS): $(OUTDIR)/clawt-version.h $(LIBRECLAW_STATIC)

# ============================================================
# Library
# ============================================================
.PHONY: shared static
shared: $(LIB_SHARED)
static: $(LIB_STATIC)

$(LIB_STATIC): $(LIB_OBJECTS) | $(OUTDIR)
	@echo "Creating static library..."
	$(AR) rcs $@ $(LIB_OBJECTS)

$(LIB_SHARED): $(LIB_OBJECTS) | $(OUTDIR)
	@echo "Creating shared library..."
	$(CC) -shared -Wl,-soname,$(LIB_SONAME) -o $@ $(LIB_OBJECTS) $(LDFLAGS)
	@ln -sf $(notdir $(LIB_SHARED)) $(OUTDIR)/$(LIB_SONAME)
	@ln -sf $(LIB_SONAME) $(OUTDIR)/$(LIB_NAME).so

# ============================================================
# Clients
# ============================================================
DAEMON_SRCDIR = clients/daemon
CLI_SRCDIR    = clients/cli
GTK_SRCDIR    = clients/gtk
WEB_SRCDIR    = clients/web

DAEMON_SOURCES = $(wildcard $(DAEMON_SRCDIR)/*.c)
CLI_SOURCES    = $(wildcard $(CLI_SRCDIR)/*.c)
GTK_SOURCES    = $(wildcard $(GTK_SRCDIR)/*.c)
WEB_SOURCES    = $(wildcard $(WEB_SRCDIR)/*.c)

.PHONY: daemon cli gtk web
daemon: $(DAEMON_BIN_TARGET)
cli:    $(CLI_BIN_TARGET)
web:    $(WEB_BIN_TARGET)

$(DAEMON_BIN_TARGET): $(DAEMON_SOURCES) $(LIB_STATIC) | $(OUTDIR) $(OUTDIR)/clawt-version.h
	@echo "Building $(DAEMON_BIN_NAME)..."
	$(CC) $(CFLAGS) -I$(DAEMON_SRCDIR) $(DAEMON_SOURCES) -o $@ $(LIB_STATIC) $(LDFLAGS)

$(CLI_BIN_TARGET): $(CLI_SOURCES) $(LIB_STATIC) $(DEFAULT_CONFIG_H) | $(OUTDIR) $(OUTDIR)/clawt-version.h
	@echo "Building $(CLI_BIN_NAME)..."
	$(CC) $(CFLAGS) -I$(CLI_SRCDIR) $(CLI_SOURCES) -o $@ $(LIB_STATIC) $(LDFLAGS)

$(WEB_BIN_TARGET): $(WEB_SOURCES) $(LIB_STATIC) | $(OUTDIR) $(OUTDIR)/clawt-version.h
	@echo "Building $(WEB_BIN_NAME)..."
	$(CC) $(CFLAGS) -I$(WEB_SRCDIR) $(WEB_SOURCES) -o $@ $(LIB_STATIC) $(LDFLAGS)

# The GTK client is skipped -- loudly, once -- when libadwaita is absent,
# rather than failing the whole build.  Everything else here is headless
# and still useful on a machine with no desktop toolkit installed.
ifeq ($(GTK_AVAILABLE),1)
gtk: $(GTK_BIN_TARGET)

$(GTK_BIN_TARGET): $(GTK_SOURCES) $(LIB_STATIC) | $(OUTDIR) $(OUTDIR)/clawt-version.h
	@echo "Building $(GTK_BIN_NAME)..."
	$(CC) $(CFLAGS) $(GTK_PKG_CFLAGS) -I$(GTK_SRCDIR) $(GTK_SOURCES) \
		-o $@ $(LIB_STATIC) $(GTK_PKG_LIBS) $(LDFLAGS)
else
gtk:
	@echo "Skipping $(GTK_BIN_NAME): $(GTK_PKG_DEPS) not found by pkg-config."
	@echo "  Fedora: layer gtk4-devel and libadwaita-devel into the image."
endif

# ============================================================
# Plugins
# ============================================================
.PHONY: plugins
plugins: $(LIB_STATIC)
	@mkdir -p $(PLUGIN_OUTDIR)
	@for dir in $(PLUGINS_SRCDIR)/*/; do \
		[ -d "$$dir" ] || continue; \
		name=$$(basename $$dir); \
		srcs=$$(ls $$dir*.c 2>/dev/null); \
		if [ -n "$$srcs" ]; then \
			echo "Building plugin: $$name"; \
			$(CC) $(PLUGIN_CFLAGS) -fPIC $$srcs \
				-o $(PLUGIN_OUTDIR)/libclawt-plugin-$$name.so \
				$(PLUGIN_LDFLAGS) || exit 1; \
		fi; \
	done

# ============================================================
# Generated config files and docs tables
#
# src/config/clawt-config-schema.c is the single source of truth for every
# configuration option.  These files are generated from it and checked in,
# so a test can assert they have not drifted.
# ============================================================
$(GENCONFIG_BIN): $(TOOLSDIR)/clawt-genconfig.c $(LIB_STATIC) | $(OUTDIR)
	@echo "Building $(notdir $@)..."
	$(CC) $(CFLAGS) -I$(SRCDIR) $< -o $@ $(LIB_STATIC) $(LDFLAGS)

.PHONY: config-files
config-files: $(GENCONFIG_BIN)
	@echo "Regenerating config files and docs tables from the schema..."
	@$(GENCONFIG_BIN) --example > $(DATADIR_SRC)/example-config.yaml
	@$(GENCONFIG_BIN) --default > $(DATADIR_SRC)/default-config.yaml
	@$(GENCONFIG_BIN) --org     > $(DOCSDIR)/configuration-options.org
	@echo "  $(DATADIR_SRC)/example-config.yaml"
	@echo "  $(DATADIR_SRC)/default-config.yaml"
	@echo "  $(DOCSDIR)/configuration-options.org"

.PHONY: docs-check
docs-check: $(GENCONFIG_BIN)
	@sh $(TOOLSDIR)/clawt-docs-check.sh

# ============================================================
# pkg-config
# ============================================================
$(PROJECT_NAME)-1.0.pc: $(PROJECT_NAME)-1.0.pc.in
	@echo "Generating pkg-config file..."
	@sed -e 's|@PREFIX@|$(PREFIX)|g' \
	     -e 's|@LIBDIR@|$(LIBDIR)|g' \
	     -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' \
	     -e 's|@VERSION@|$(VERSION)|g' \
	     $< > $@

# ============================================================
# GObject Introspection
# ============================================================
.PHONY: gir
ifeq ($(GIR),1)
gir: $(TYPELIB_FILE)

$(GIR_FILE): $(LIB_SHARED) $(PUBLIC_HEADERS) | $(OUTDIR)
	@echo "Generating GIR file..."
	@# Only -I and --pkg flags go to the scanner.  Handing it $(CFLAGS)
	@# instead looks harmless and is not: it parses -std=gnu89 as its own
	@# -s option and dies with "no such option: -s", which names nothing
	@# that appears anywhere in this rule.  LIBRECLAW_CFLAGS is safe here
	@# precisely because it is nothing but include paths.
	$(GIR_SAN_PRELOAD) $(GIR_SCANNER) \
		--namespace=$(GIR_NAMESPACE) \
		--nsversion=$(GIR_VERSION) \
		--identifier-prefix=Clawt \
		--symbol-prefix=clawt \
		--library=clawt-1.0 \
		--library-path=$(OUTDIR) \
		--include=GLib-2.0 \
		--include=GObject-2.0 \
		--include=Gio-2.0 \
		--pkg glib-2.0 --pkg gobject-2.0 --pkg gio-2.0 \
		--pkg json-glib-1.0 --pkg libsoup-3.0 \
		-I$(SRCDIR) -I$(OUTDIR) \
		$(LIBRECLAW_CFLAGS) \
		-DCLAWT_COMPILATION \
		--warn-all \
		--output=$@ \
		$(PUBLIC_HEADERS) $(LIB_SOURCES)

$(TYPELIB_FILE): $(GIR_FILE)
	@if [ -f $< ]; then \
		echo "Generating typelib..."; \
		$(GIR_COMPILER) --output=$@ $<; \
	fi
else
gir:
ifdef GIR_AUTO_DISABLED
	@echo "Skipping GObject Introspection: $(GIR_SCANNER) not found (GIR=1 to force)."
endif
ifdef GIR_SAN_DISABLED
	@echo "Skipping GObject Introspection: sanitizer build (GIR=1 to force)."
endif
endif

# ============================================================
# Tests
# ============================================================
.PHONY: test
test: $(TEST_BINARIES)
	@echo "Running tests..."
	@fail=0; ran=0; \
	for t in $(TEST_BINARIES); do \
		[ -x "$$t" ] || continue; \
		ran=$$((ran+1)); \
		echo "  --> $$(basename $$t)"; \
		"$$t" || fail=1; \
	done; \
	if [ $$ran -eq 0 ]; then echo "No tests found in $(TESTDIR)/"; fi; \
	for s in $(wildcard $(TESTDIR)/test-*.sh); do \
		echo "  --> $$(basename $$s)"; \
		sh "$$s" || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	sh $(TOOLSDIR)/clawt-test-floor.sh $$ran || exit 1; \
	echo "All tests passed."

.PHONY: test-verbose
test-verbose: $(TEST_BINARIES)
	@for t in $(TEST_BINARIES); do \
		[ -x "$$t" ] || continue; \
		echo "=== $$(basename $$t) ==="; \
		"$$t" --verbose || exit 1; \
	done

# Tests that need real podman / libvirt / gowl.  Kept out of `make test`
# so the default suite stays hermetic and runs anywhere.
.PHONY: test-integration
test-integration: $(TEST_BINARIES)
	@echo "Running integration tests (CLAWT_TEST_INTEGRATION=1)..."
	@fail=0; \
	for t in $(TEST_BINARIES); do \
		[ -x "$$t" ] || continue; \
		CLAWT_TEST_INTEGRATION=1 "$$t" || fail=1; \
	done; \
	[ $$fail -eq 0 ] || { echo "INTEGRATION TESTS FAILED"; exit 1; }; \
	echo "All integration tests passed."

# ============================================================
# Install / uninstall
# ============================================================
.PHONY: install
install: all
	@echo "Installing to $(DESTDIR)$(PREFIX)..."
	install -d $(DESTDIR)$(LIBDIR)
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0
	install -d $(DESTDIR)$(PKGCONFIGDIR)
	install -d $(DESTDIR)$(PLUGIN_SYSTEM_DIR)
	install -d $(DESTDIR)$(POD_MODULE_SYSTEM_DIR)
	install -d $(DESTDIR)$(DATADIR)/$(PROJECT_NAME)
	install -m 755 $(LIB_SHARED) $(DESTDIR)$(LIBDIR)/
	ln -sf $(notdir $(LIB_SHARED)) $(DESTDIR)$(LIBDIR)/$(LIB_SONAME)
	ln -sf $(LIB_SONAME) $(DESTDIR)$(LIBDIR)/$(LIB_NAME).so
	install -m 644 $(LIB_STATIC) $(DESTDIR)$(LIBDIR)/
	install -m 755 $(DAEMON_BIN_TARGET) $(DESTDIR)$(BINDIR)/
	install -m 755 $(CLI_BIN_TARGET) $(DESTDIR)$(BINDIR)/
	@[ -x $(GTK_BIN_TARGET) ] && install -m 755 $(GTK_BIN_TARGET) $(DESTDIR)$(BINDIR)/ || true
	@[ -x $(WEB_BIN_TARGET) ] && install -m 755 $(WEB_BIN_TARGET) $(DESTDIR)$(BINDIR)/ || true
	@for h in $(PUBLIC_HEADERS); do \
		sub=$$(dirname $${h#$(SRCDIR)/}); \
		install -d $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/$$sub; \
		install -m 644 $$h $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/$$sub/; \
	done
	install -m 644 $(OUTDIR)/clawt-version.h $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/
	install -m 644 $(PROJECT_NAME)-1.0.pc $(DESTDIR)$(PKGCONFIGDIR)/
	install -m 644 $(DATADIR_SRC)/example-config.yaml $(DESTDIR)$(DATADIR)/$(PROJECT_NAME)/
	install -m 644 $(DATADIR_SRC)/default-config.yaml $(DESTDIR)$(DATADIR)/$(PROJECT_NAME)/
	@for p in $(PLUGIN_OUTDIR)/*.so; do \
		[ -f "$$p" ] && install -m 755 "$$p" $(DESTDIR)$(PLUGIN_SYSTEM_DIR)/ || true; \
	done
	@# The container and vm_virtmanager modules are how the computer
	@# backends do their work, so an installed clawtilla needs them too.
	@for m in $(CLAWT_POD_MODULE_DIR)/*.so; do \
		[ -f "$$m" ] && install -m 755 "$$m" $(DESTDIR)$(POD_MODULE_SYSTEM_DIR)/ || true; \
	done
	@if [ -f $(GIR_FILE) ]; then \
		install -d $(DESTDIR)$(DATADIR)/gir-1.0; \
		install -m 644 $(GIR_FILE) $(DESTDIR)$(DATADIR)/gir-1.0/; \
	fi
	@if [ -f $(TYPELIB_FILE) ]; then \
		install -d $(DESTDIR)$(LIBDIR)/girepository-1.0; \
		install -m 644 $(TYPELIB_FILE) $(DESTDIR)$(LIBDIR)/girepository-1.0/; \
	fi
	@echo "Install complete."

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_NAME).so*
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_NAME).a
	rm -f $(DESTDIR)$(BINDIR)/$(DAEMON_BIN_NAME)
	rm -f $(DESTDIR)$(BINDIR)/$(CLI_BIN_NAME)
	rm -f $(DESTDIR)$(BINDIR)/$(GTK_BIN_NAME)
	rm -f $(DESTDIR)$(BINDIR)/$(WEB_BIN_NAME)
	rm -rf $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0
	rm -f $(DESTDIR)$(PKGCONFIGDIR)/$(PROJECT_NAME)-1.0.pc
	rm -rf $(DESTDIR)$(LIBDIR)/$(PROJECT_NAME)
	rm -rf $(DESTDIR)$(DATADIR)/$(PROJECT_NAME)
	rm -f $(DESTDIR)$(DATADIR)/gir-1.0/$(GIR_NAMESPACE)-$(GIR_VERSION).gir
	rm -f $(DESTDIR)$(LIBDIR)/girepository-1.0/$(GIR_NAMESPACE)-$(GIR_VERSION).typelib

.PHONY: version
version:
	@echo "$(PROJECT_NAME) $(VERSION) ($(GIT_SHA))"

endif # __MIXED
