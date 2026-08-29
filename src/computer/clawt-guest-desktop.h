/*
 * clawt-guest-desktop.h - A GNOME session inside the agent's own VM
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A cloud image has no desktop.  It has no X server, no Wayland
 * compositor, no display manager and no graphical target -- it is built to
 * boot to a serial console and run services.  So an agent given a VM and
 * told it may drive a desktop has nothing to drive until one is put there.
 *
 * This describes that desktop, and renders the cloud-config that installs
 * it: the packages, the account the session runs as, GDM's autologin, and
 * gnome-desktop-mcp -- a GNOME Shell extension and the stdio MCP server in
 * front of it, which is how the agent reaches the session at all.
 *
 * The extension is not an implementation detail that could be avoided.
 * GNOME on Wayland refuses screenshots and input injection to every
 * process outside the compositor, so the only way in is code running
 * *inside* GNOME Shell exposing a D-Bus interface.
 *
 * Everything here is a pure function of the configuration.  The rendering
 * can therefore be asserted on without a hypervisor, which matters because
 * the alternative way to find a mistake in it is a twenty-minute boot
 * ending in a black screen.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"

G_BEGIN_DECLS

/**
 * CLAWT_GUEST_DESKTOP_LAUNCHER:
 *
 * The launcher installed in the guest, and the whole of the command run
 * over SSH to reach the MCP server.
 *
 * A single unquoted word on purpose.  The environment the server needs --
 * the session bus of the account the desktop is logged in as -- can only
 * be worked out inside the guest, and threading `$(id -u)` through
 * clawtilla's argv quoting, ssh's own re-parsing and a remote shell is
 * three chances to get it wrong for no gain.
 */
#define CLAWT_GUEST_DESKTOP_LAUNCHER "clawtilla-desktop-mcp"

/*
 * Where the guest records whether its half of the desktop installed.
 *
 * The tools an agent drives live inside a GNOME Shell extension, and
 * everything that could stop that extension loading happens at first
 * boot, inside the guest, in a log nobody is reading.  What the agent
 * sees much later is "DBus object has no attribute", which names
 * nothing.  This file names it, in one line, in one place.
 */
#define CLAWT_GUEST_DESKTOP_STATUS_FILE \
    "/var/lib/clawtilla/desktop-install.status"

/*
 * The installer that writes it, which is safe to run again -- unlike the
 * seed that first ran it, since cloud-init acts at first boot only.
 */
#define CLAWT_GUEST_DESKTOP_INSTALL_SCRIPT \
    "/usr/local/bin/clawtilla-desktop-install"

/*
 * How an agent starts a graphical application inside the guest.
 *
 * `computer_exec` arrives over SSH, which carries no session
 * environment, so the obvious thing to reach for is `DISPLAY=:0 firefox`
 * -- and it appears to work.  It also puts the application on Xwayland
 * rather than in the Wayland session the desktop was built for, which
 * composites and receives synthetic input by a different path, and
 * nothing an agent can see says so.
 */
#define CLAWT_GUEST_DESKTOP_RUN_SCRIPT \
    "/usr/local/bin/clawtilla-desktop-run"

/*
 * Where gnome-desktop-mcp writes its screenshots inside the guest.
 *
 * Compiled into the extension, so it is a fact about that program
 * rather than a choice made here -- but it is a fact two places in
 * clawtilla need: the tmpfiles rule that links it into the workspace
 * share, and the frame reader that turns a path the guest named into
 * one this machine can open.  Two spellings of it would agree until
 * upstream moved the directory, and then the reader would go looking in
 * a place nothing writes to and report every frame as missing.
 */
#define CLAWT_GUEST_SCREENSHOT_DIR "/tmp/gnome-mcp"

#define CLAWT_TYPE_GUEST_DESKTOP (clawt_guest_desktop_get_type())

GType clawt_guest_desktop_get_type(void) G_GNUC_CONST;

/**
 * clawt_guest_desktop_new:
 * @session_user: the account the graphical session runs as
 *
 * Returns: (transfer full): a new #ClawtGuestDesktop
 */
ClawtGuestDesktop *clawt_guest_desktop_new(const gchar *session_user);

ClawtGuestDesktop *clawt_guest_desktop_ref(ClawtGuestDesktop *self);
void               clawt_guest_desktop_unref(ClawtGuestDesktop *self);

/**
 * clawt_guest_desktop_resolve_user:
 * @configured: (nullable): computer.vm.desktop.user, if it was set
 * @ssh_user: (nullable): computer.vm.ssh_user
 *
 * Works out which account the graphical session runs as.
 *
 * GDM refuses to log root in, and the default @ssh_user is root -- so a
 * VM left entirely at its defaults needs a second account or it has no
 * session at all.  Anything else the operator named is used as given,
 * including for the commands the agent runs.
 *
 * Returns: (transfer full): the account name
 */
gchar *clawt_guest_desktop_resolve_user(const gchar *configured,
                                        const gchar *ssh_user);

void clawt_guest_desktop_set_autologin(ClawtGuestDesktop *self,
                                       gboolean           autologin);
/**
 * clawt_guest_desktop_resolve_flavour:
 * @configured: (nullable): `computer.vm.desktop.flavour`
 * @image: (nullable): `computer.vm.image`
 *
 * Which family to install into.
 *
 * What somebody wrote down wins over anything derived from the image --
 * an image with an unhelpful name is exactly the case the key exists
 * for.  A plain function so the derivation can be asserted on without a
 * hypervisor, a download or a boot.
 *
 * Returns: the family, or %CLAWT_GUEST_FLAVOUR_AUTO when neither the
 *   configuration nor the image says
 */
ClawtGuestFlavour clawt_guest_desktop_resolve_flavour(const gchar *configured,
                                                      const gchar *image);

void clawt_guest_desktop_set_flavour(ClawtGuestDesktop *self,
                                     ClawtGuestFlavour  flavour);

ClawtGuestFlavour clawt_guest_desktop_get_flavour(ClawtGuestDesktop *self);

/**
 * clawt_guest_desktop_set_packages:
 * @self: a #ClawtGuestDesktop
 * @packages: (array zero-terminated=1) (nullable): what to install
 *
 * Overrides the family's desktop package list.
 *
 * An empty or %NULL list means the family's own, which is the ordinary
 * case: the names differ per distribution and nobody should have to
 * know that to switch image.  A list given here replaces the family's
 * rather than adding to it.
 */
void clawt_guest_desktop_set_packages(ClawtGuestDesktop  *self,
                                      const gchar *const *packages);
void clawt_guest_desktop_set_install_mcp(ClawtGuestDesktop *self,
                                         gboolean           install);
void clawt_guest_desktop_set_mcp_repo(ClawtGuestDesktop *self,
                                      const gchar       *repo);

const gchar *clawt_guest_desktop_get_session_user(ClawtGuestDesktop *self);
gboolean     clawt_guest_desktop_get_install_mcp(ClawtGuestDesktop *self);

/**
 * clawt_guest_desktop_render_account:
 * @self: a #ClawtGuestDesktop
 * @out: the `#cloud-config` being built
 * @authorized_key: (nullable): one OpenSSH public key line
 * @already_created: the account cloud-init is already creating
 *
 * Appends the session account to an open `users:` block, unless the login
 * already being created is the same one.
 *
 * The key is authorised here too: reaching the MCP server means an SSH
 * connection as this account, because the server needs that account's
 * session bus and nobody else's.
 */
void clawt_guest_desktop_render_account(ClawtGuestDesktop *self,
                                        GString           *out,
                                        const gchar       *authorized_key,
                                        const gchar       *already_created);

/**
 * clawt_guest_desktop_render_setup:
 * @self: a #ClawtGuestDesktop
 * @out: the `#cloud-config` being built
 * @extra: (nullable) (array zero-terminated=1): packages to install as
 *   well, from `computer.vm.packages`
 *
 * Appends everything that is not an account: the packages, the files, and
 * the commands that turn a headless cloud image into a machine sitting at
 * a logged-in GNOME session with automation switched on.
 *
 * @extra goes into the same `packages:` list rather than one of its own,
 * because cloud-config has a single top-level key for it -- two would be
 * a duplicate that YAML resolves by keeping the last, so one of the
 * lists would silently reach nothing.
 *
 * Emitted after the `users:` block, because cloud-config is a mapping and
 * reopening a key that is already there loses everything under the first
 * one.
 */
void clawt_guest_desktop_render_setup(ClawtGuestDesktop  *self,
                                      GString             *out,
                                      const gchar * const *extra);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtGuestDesktop, clawt_guest_desktop_unref)

G_END_DECLS
