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
VERSION_MINOR = 1
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
PKG_DEPS = glib-2.0 gobject-2.0 gio-2.0 gio-unix-2.0 gmodule-2.0 libsoup-3.0 \
           json-glib-1.0 libcmark sqlite3 libetpan libpq libdex-1 libxml-2.0

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

LIBRECLAW_LIBS = $(LIBRECLAW_STATIC) \
                 $(LR_AI_GLIB_DIR)/build/release/libai-glib-1.0.a \
                 $(LR_YAML_GLIB_DIR)/build/release/libyaml-glib-1.0.a \
                 $(LR_PODOMATION_DIR)/build/release/libpodomation-1.0.a \
                 $(LR_MCP_GLIB_DIR)/build/libmcp-glib-1.0.a \
                 $(LR_HTMX_GLIB_DIR)/build/release/libhtmx-glib-1.0.a \
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

# The signature of the build configuration itself.
BUILD_FLAGS_STAMP := $(OUTDIR)/.build-flags
    BUILD_TYPE := release
    CFLAGS_OPT = -O2 -DNDEBUG
endif

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

# Combined flags
#
# We include <libreclaw.h>, which defines LC_INSIDE itself, so we do NOT
# define LC_COMPILATION -- that macro is for building libreclaw, not for
# consuming it.
CFLAGS = $(CFLAGS_BASE) $(CFLAGS_OPT) $(CFLAGS_SAN) $(PKG_CFLAGS) \
         $(LIBRECLAW_CFLAGS) \
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
