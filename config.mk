# config.mk - Build configuration for clawtilla
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Mirrors deps/libreclaw/config.mk deliberately: same three-file layout,
# same variable names, same DEBUG/ASAN/UBSAN handling.  Anyone who knows
# the libreclaw build knows this one.

# Project information
PROJECT_NAME = clawtilla
VERSION_MAJOR = 0
VERSION_MINOR = 14
VERSION_MICRO = 0
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_MICRO)

# Installation paths
PREFIX ?= /usr/local
INCLUDEDIR ?= $(PREFIX)/include
LIBDIR ?= $(PREFIX)/lib
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
PKGCONFIGDIR = $(LIBDIR)/pkgconfig

# Compiler settings
CC = gcc
CSTD = -std=gnu89
WARNINGS = -Wall -Wextra -Wno-unused-parameter -Wformat=2 -Wshadow
CFLAGS_BASE = $(CSTD) $(WARNINGS) -fPIC

# pkg-config dependencies.
#
# This is libreclaw's PKG_DEPS list reproduced in full, deliberately.
# libreclaw-1.0.pc declares an incomplete Requires: (it omits gio-unix-2.0,
# gmodule-2.0, libcmark, sqlite3, libetpan, libpq and libdex-1), which is
# harmless when linking its .so but breaks the static link we do here.  We
# fix the .pc upstream too, but we do not depend on that fix being present
# in whatever libreclaw checkout somebody has.
#
# A package missing from this list makes pkg-config fail for the WHOLE
# list at once, which blanks PKG_CFLAGS and surfaces as a misleading
# "glib.h: No such file or directory".
#
# libxml-2.0 is ours rather than libreclaw's: ai-glib's HTML-parsing search
# providers pull it in, and clawtilla is the first thing here to link
# AiToolExecutor, which drags them along.  Fedora package: libxml2-devel.
#
# gstreamer-1.0 and gstreamer-app-1.0 are ai-glib's too: its voice
# sessions (0.6) run on GStreamer, and the archive linked here names gst_*
# symbols.  Fedora package: gstreamer1-devel and gstreamer1-plugins-base-devel.
PKG_DEPS = glib-2.0 gobject-2.0 gio-2.0 gio-unix-2.0 gmodule-2.0 libsoup-3.0 \
           json-glib-1.0 libcmark sqlite3 libetpan libpq libdex-1 libxml-2.0 \
           gstreamer-1.0 gstreamer-app-1.0

PKG_CFLAGS := $(shell pkg-config --cflags $(PKG_DEPS))
PKG_LIBS := $(shell pkg-config --libs $(PKG_DEPS))

# ── libreclaw (bundled under deps/libreclaw) ──────────────────────────
#
# liblc-1.0.a holds ONLY libreclaw's own objects, so a static link has to
# pull in all five of its bundled dep archives as well.  Always built
# DEBUG=0 -- deps use release output regardless of our own DEBUG, exactly
# as libreclaw does with its own deps.
LIBRECLAW_DIR    = deps/libreclaw
LIBRECLAW_OUTDIR = $(LIBRECLAW_DIR)/build/release
LIBRECLAW_STATIC = $(LIBRECLAW_OUTDIR)/liblc-1.0.a

# libreclaw's own bundled deps, whose headers we also use directly:
#   ai-glib    - AiToolExecutor / AiTool, for the agent designer
#   yaml-glib  - config parse/emit
#   podomation - PodEngine + the container / vm_virtmanager modules
#   mcp-glib   - McpClient / McpServer, for the orchestration endpoint
#   htmx-glib  - the web client
LR_AI_GLIB_DIR    = $(LIBRECLAW_DIR)/deps/ai-glib
LR_YAML_GLIB_DIR  = $(LIBRECLAW_DIR)/deps/yaml-glib
LR_PODOMATION_DIR = $(LIBRECLAW_DIR)/deps/podomation
LR_MCP_GLIB_DIR   = $(LIBRECLAW_DIR)/deps/mcp-glib
LR_HTMX_GLIB_DIR  = $(LIBRECLAW_DIR)/deps/htmx-glib

LIBRECLAW_CFLAGS = -I$(LIBRECLAW_DIR)/src -I$(LIBRECLAW_OUTDIR) \
                   -I$(LR_AI_GLIB_DIR)/src -I$(LR_AI_GLIB_DIR)/build/release \
                   -I$(LR_YAML_GLIB_DIR)/src \
                   -I$(LR_PODOMATION_DIR)/src \
                   -I$(LR_PODOMATION_DIR)/src/core \
                   -I$(LR_PODOMATION_DIR)/src/module \
                   -I$(LR_PODOMATION_DIR)/src/interfaces \
                   -I$(LR_PODOMATION_DIR)/build/release \
                   -I$(LR_MCP_GLIB_DIR)/src \
                   -I$(LR_HTMX_GLIB_DIR)/src \
                   -I$(LR_HTMX_GLIB_DIR)/src/core \
                   -I$(LR_HTMX_GLIB_DIR)/src/element \
                   -I$(LR_HTMX_GLIB_DIR)/src/model \
                   -I$(LR_HTMX_GLIB_DIR)/src/extensions \
                   -I$(LR_HTMX_GLIB_DIR)/build/release

#
# Wrapped in a link group, because these six archives reference each
# other in both directions and a static archive is scanned exactly once,
# left to right.
#
# yaml-glib sat ahead of podomation here, so anything pulling
# podomation's pod-config.o *without* also pulling clawtilla's own config
# code -- which happens to drag yaml-glib in later -- failed to link with
# 557 undefined `yaml_mapping_*` references.  Every consumer that exists
# today is saved by that accident, so the order was wrong and nothing
# said so; the first test to reach podomation's config reader on its own
# found it.
#
# A group rather than a repeated archive: repeating one fixes the pair
# somebody noticed and leaves the next pair to be discovered the same
# way.  The cost is that ld re-scans the group until it settles, which
# for six archives is not measurable.
#
LIBRECLAW_LIBS = -Wl,--start-group \
                 $(LIBRECLAW_STATIC) \
                 $(LR_AI_GLIB_DIR)/build/release/libai-glib-1.0.a \
                 $(LR_YAML_GLIB_DIR)/build/release/libyaml-glib-1.0.a \
                 $(LR_PODOMATION_DIR)/build/release/libpodomation-1.0.a \
                 $(LR_MCP_GLIB_DIR)/build/libmcp-glib-1.0.a \
                 $(LR_HTMX_GLIB_DIR)/build/release/libhtmx-glib-1.0.a \
                 -Wl,--end-group \
                 $(shell pkg-config --libs yaml-0.1)

# Where libreclaw drops the podomation loadable modules.  The container and
# vm_virtmanager modules are how ClawtContainerComputer and ClawtLibvirtVm
# actually do their work, so the daemon needs to find them at runtime.
CLAWT_POD_MODULE_DIR = $(LIBRECLAW_OUTDIR)/modules

# ── GTK client (clients/gtk) ──────────────────────────────────────────
#
# Kept separate on purpose: libclawt and clawtillad must NOT link GTK, so
# the daemon runs headless on a server and cmacs can embed the library
# without dragging in a toolkit.
GTK_PKG_DEPS    = gtk4 libadwaita-1
GTK_PKG_CFLAGS := $(shell pkg-config --cflags $(GTK_PKG_DEPS) 2>/dev/null)
GTK_PKG_LIBS   := $(shell pkg-config --libs   $(GTK_PKG_DEPS) 2>/dev/null)
GTK_AVAILABLE  := $(shell pkg-config --exists $(GTK_PKG_DEPS) 2>/dev/null && echo 1 || echo 0)

# ── libregnum client (clients/lrg) ───────────────────────────────────
#
# Opt in with LRG_BACKEND=1.  libregnum is NOT a submodule: recursively
# it is enormous (graylib, raylib, crispy, optional ai-glib/cad-glib/
# Steam), so the client builds against a sibling checkout named by
# LRG_DIR.  `make LRG_BACKEND=1` builds that checkout's library in place
# when the archive is missing or older than its sources -- with
# libregnum's *own* default options, so an archive another project links
# (cmacs, gsurf) is rebuilt as itself rather than as a cut-down variant.
#
# One yaml-glib, not two.  liblibregnum.a has yaml-glib merged into it,
# and clawtilla links libreclaw's.  Two copies in one process register
# YamlParser and friends twice -- a GType clash, not merely duplicate
# symbols -- so the lrg binary drops libreclaw's archive and takes
# libregnum's.  That is only sound while libregnum's yaml-glib descends
# from clawtilla's pin; `lrg-yaml-check` refuses the build otherwise.
LRG_BACKEND ?= 0
LRG_DIR     ?= $(abspath $(CURDIR)/../libregnum)
LRG_LIB      = $(LRG_DIR)/build/release/lib/liblibregnum.a
LRG_YAML_DIR = $(LRG_DIR)/deps/yaml-glib

# What the client links from libregnum, checked with nm before a link: an
# archive older than the widget set links fine and fails nowhere useful.
LRG_REQUIRED_SYMBOLS = lrg_canvas_new lrg_island_new lrg_text_editor_new \
                       lrg_markdown_label_new lrg_table_new lrg_node_graph_new \
                       lrg_toast_queue_new lrg_header_bar_new lrg_switcher_new \
                       lrg_timeline_view_new lrg_property_grid_new \
                       lrg_label_set_ellipsize

ifeq ($(LRG_BACKEND),1)
LRG_AVAILABLE := $(shell test -f $(LRG_DIR)/src/libregnum.h && echo 1 || echo 0)
else
LRG_AVAILABLE := 0
endif

# The -DLRG_HAS_* set must match how the archive was built, or
# <libregnum.h> declares a different library than the one linked.  These
# mirror libregnum's own detection for its default options.
LRG_HAS_DEFS := -DLRG_PLATFORM_LINUX=1 -DLRG_BUILD_EDITOR=1 -DLRG_BUILD_EDITOR_UI=1 \
                $(if $(shell pkg-config --exists libdex-1 && echo y),-DLRG_HAS_LIBDEX=1) \
                $(if $(shell pkg-config --exists luajit && echo y),-DLRG_HAS_LUAJIT=1) \
                $(if $(shell pkg-config --exists fribidi && echo y),-DLRG_HAS_FRIBIDI=1) \
                $(if $(shell pkg-config --exists python3-embed && echo y),-DLRG_HAS_PYTHON=1) \
                $(if $(wildcard $(LRG_DIR)/deps/crispy/src/crispy.h),-DLRG_HAS_CRISPY=1)

# -include stdbool.h: under gnu89 <raylib.h> declares `bool` as an
# int-sized enum while raylib stores a one-byte _Bool, so mouse buttons
# read "held" for ever.  libregnum compiles itself the same way.
# -isystem: libregnum's and raylib's headers are not ours to warn about.
LRG_CFLAGS = -include stdbool.h $(LRG_HAS_DEFS) \
             -isystem $(LRG_DIR)/src \
             -isystem $(LRG_DIR)/deps/graylib/src \
             -isystem $(LRG_DIR)/deps/graylib/deps/raylib/src

LRG_PKG_DEPS = zlib libpq openssl sqlite3 fribidi \
               $(if $(shell pkg-config --exists luajit && echo y),luajit) \
               $(if $(shell pkg-config --exists python3-embed && echo y),python3-embed)
LRG_SYS_LIBS = $(shell pkg-config --libs $(LRG_PKG_DEPS) 2>/dev/null) \
               $(shell pkg-config --libs wayland-client wayland-cursor wayland-egl xkbcommon 2>/dev/null) \
               -lGL -lX11 -lm -lpthread -ldl -lrt -lreadline

# libreclaw's group without its yaml-glib (see above).
LRG_LIBRECLAW_LIBS = $(filter-out $(LR_YAML_GLIB_DIR)/build/release/libyaml-glib-1.0.a,$(LIBRECLAW_LIBS))

LRG_LDFLAGS = $(LDFLAGS_SAN) $(LRG_LIBRECLAW_LIBS) $(LRG_LIB) $(PKG_LIBS) $(LRG_SYS_LIBS) \
              $(shell pkg-config --libs yaml-0.1)

# Build directories
BUILDDIR := build
OBJDIR_DEBUG := $(BUILDDIR)/debug/obj
OBJDIR_RELEASE := $(BUILDDIR)/release/obj
BINDIR_DEBUG := $(BUILDDIR)/debug
BINDIR_RELEASE := $(BUILDDIR)/release

# Build options (0 or 1)
DEBUG ?= 0
ASAN ?= 0
UBSAN ?= 0

# Select build directories based on DEBUG
ifeq ($(DEBUG),1)
    OBJDIR := $(OBJDIR_DEBUG)
    OUTDIR := $(BINDIR_DEBUG)
    BUILD_TYPE := debug
    CFLAGS_OPT = -O0 -g3 -DDEBUG
else
    OBJDIR := $(OBJDIR_RELEASE)
    OUTDIR := $(BINDIR_RELEASE)

    BUILD_TYPE := release
    CFLAGS_OPT = -O2 -DNDEBUG
endif

# Both debug and release objects must notice compiler and sanitizer changes.
BUILD_FLAGS_STAMP := $(OUTDIR)/.build-flags

# Sanitizer flags
ifeq ($(ASAN),1)
    CFLAGS_SAN = -fsanitize=address -fno-omit-frame-pointer
    LDFLAGS_SAN = -fsanitize=address -lasan
endif

ifeq ($(UBSAN),1)
    CFLAGS_SAN += -fsanitize=undefined
    LDFLAGS_SAN += -fsanitize=undefined
endif

# Source and test directories
SRCDIR = src
TESTDIR = tests
TOOLSDIR = tools
DOCSDIR = docs
DATADIR_SRC = data

# Git version info (computed in Makefile, defined there)
GIT_SHA ?= unknown

# ── Browser sessions (GSURF=1) ────────────────────────────────────────
#
# Opt in with GSURF=1.  gsurf is NOT a submodule and clawtilla links
# nothing from it: a browser session is a gsurf *process* on a private
# Xvfb, driven over gsurf's control socket, because the daemon must never
# link GTK.  GSURF=1 only says this build may start one, and records the
# GSURF_DIR build as the last place to find the binary.  `make GSURF=1`
# builds that checkout when its binary is missing, and refuses a gsurf
# older than GSURF_MIN_VERSION, which added page.click, page.type,
# page.key and the inline screenshot the agent tools are made of.
GSURF       ?= 0
GSURF_DIR   ?= $(abspath $(CURDIR)/../gsurf)
GSURF_BIN    = $(GSURF_DIR)/build/release/gsurf
GSURF_MIN_VERSION = 0.5.10

ifeq ($(GSURF),1)
GSURF_CFLAGS = -DCLAWT_HAVE_GSURF=1 -DCLAWT_GSURF_BUILD_BIN=\"$(GSURF_BIN)\"
else
GSURF_CFLAGS =
endif

# Combined flags
#
# We include <libreclaw.h>, which defines LC_INSIDE itself, so we do NOT
# define LC_COMPILATION -- that macro is for building libreclaw, not for
# consuming it.
CFLAGS = $(CFLAGS_BASE) $(CFLAGS_OPT) $(CFLAGS_SAN) $(PKG_CFLAGS) \
         $(LIBRECLAW_CFLAGS) $(GSURF_CFLAGS) \
         -DCLAWT_COMPILATION \
         -DCLAWT_VERSION_MAJOR=$(VERSION_MAJOR) \
         -DCLAWT_VERSION_MINOR=$(VERSION_MINOR) \
         -DCLAWT_VERSION_MICRO=$(VERSION_MICRO) \
         -DCLAWT_GIT_SHA=\"$(GIT_SHA)\" \
         -DCLAWT_PLUGIN_DIR=\"$(PLUGIN_SYSTEM_DIR)\" \
         -DCLAWT_POD_MODULE_DIR=\"$(POD_MODULE_SYSTEM_DIR)\" \
         -DCLAWT_MCP_SERVER_DIR=\"$(MCP_SERVER_SYSTEM_DIR)\" \
         -DCLAWT_DATA_DIR=\"$(DATADIR)/$(PROJECT_NAME)\" \
         -DG_LOG_DOMAIN=\"Clawtilla\" \
         -I$(SRCDIR) -I$(OUTDIR)

LDFLAGS = $(LDFLAGS_SAN) $(LIBRECLAW_LIBS) $(PKG_LIBS) -lm

# Library names
LIB_NAME = libclawt-1.0
LIB_SHARED = $(OUTDIR)/$(LIB_NAME).so.$(VERSION)
LIB_SONAME = $(LIB_NAME).so.$(VERSION_MAJOR)
LIB_STATIC = $(OUTDIR)/$(LIB_NAME).a

# Binary names
DAEMON_BIN_NAME = clawtillad
DAEMON_BIN_TARGET = $(OUTDIR)/$(DAEMON_BIN_NAME)

CLI_BIN_NAME = clawtilla
CLI_BIN_TARGET = $(OUTDIR)/$(CLI_BIN_NAME)

GTK_BIN_NAME = clawtilla-gtk
GTK_BIN_TARGET = $(OUTDIR)/$(GTK_BIN_NAME)

WEB_BIN_NAME = clawtilla-web
WEB_BIN_TARGET = $(OUTDIR)/$(WEB_BIN_NAME)

LRG_BIN_NAME = clawtilla-lrg
LRG_BIN_TARGET = $(OUTDIR)/$(LRG_BIN_NAME)

# The agent-facing MCP server.  An agent's AI CLI starts this; it is not
# meant to be run by hand.
MCP_BIN_NAME = clawtilla-mcp-server
MCP_BIN_TARGET = $(OUTDIR)/$(MCP_BIN_NAME)

# Build-time config generator.  Runs on the build host, so it links only
# glib and yaml-glib -- never the full library.
GENCONFIG_BIN = $(OUTDIR)/clawt-genconfig

# GObject Introspection
#
# Same auto-detect policy as libreclaw: build .gir/.typelib when
# g-ir-scanner is on PATH, skip silently when it is not, and turn off for
# sanitizer builds (the scanner runs a dump binary linked against the
# instrumented library and dies with "ASan runtime does not come first").
# An explicit GIR=1 always wins.
GIR_SCANNER = g-ir-scanner
GIR_COMPILER = g-ir-compiler

ifeq ($(origin GIR),undefined)
    ifneq ($(filter 1,$(ASAN) $(UBSAN)),)
        GIR := 0
        GIR_SAN_DISABLED := 1
    else ifeq ($(shell command -v $(GIR_SCANNER) 2>/dev/null),)
        GIR := 0
        GIR_AUTO_DISABLED := 1
    else
        GIR := 1
    endif
endif

ifeq ($(GIR),1)
    ifneq ($(filter 1,$(ASAN) $(UBSAN)),)
        GIR_SAN_RT := $(shell $(CC) -print-file-name=libasan.so.8 2>/dev/null)
        ifneq ($(wildcard $(GIR_SAN_RT)),)
            GIR_SAN_PRELOAD := ASAN_OPTIONS=detect_leaks=0 LD_PRELOAD=$(GIR_SAN_RT)
        endif
    endif
endif

GIR_NAMESPACE = Clawt
GIR_VERSION = 1.0
GIR_FILE = $(OUTDIR)/$(GIR_NAMESPACE)-$(GIR_VERSION).gir
TYPELIB_FILE = $(OUTDIR)/$(GIR_NAMESPACE)-$(GIR_VERSION).typelib

# Plugin system
PLUGINS_SRCDIR = plugins
PLUGIN_OUTDIR = $(OUTDIR)/plugins
PLUGIN_SYSTEM_DIR = $(LIBDIR)/$(PROJECT_NAME)/plugins
PLUGIN_CFLAGS = $(CFLAGS_BASE) $(CFLAGS_OPT) $(PKG_CFLAGS) $(LIBRECLAW_CFLAGS) \
                -DCLAWT_COMPILATION -I$(SRCDIR) -I$(OUTDIR)
PLUGIN_LDFLAGS = -shared $(PKG_LIBS)

# Where an installed clawtilla looks for podomation's loadable modules.
# Uninstalled runs use CLAWT_POD_MODULE_DIR from the build tree instead.
POD_MODULE_SYSTEM_DIR = $(LIBDIR)/$(PROJECT_NAME)/pod-modules

# Where a connector's own third-party MCP server is looked for once
# neither "beside the running binary" nor PATH turns it up.  Nothing
# populates this directory automatically -- it exists so an operator who
# built or vendored a server has somewhere to drop it that clawtilla will
# actually find, the same role POD_MODULE_SYSTEM_DIR plays for podomation.
MCP_SERVER_SYSTEM_DIR = $(LIBDIR)/$(PROJECT_NAME)/mcp-servers
