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
 * rather than a choice made here.  Two spellings of it would agree until
 * upstream moved the directory, and then the reader would go looking in
 * a place nothing writes to and report every frame as missing.
 *
 * It stays inside the guest, and the bytes come out over SSH.  This was
 * once a symlink into the workspace share, so that a frame would already
 * be on the host and cost nothing to read; that cannot work, and the
 * reason is the one `docs/computers.org#vm-share-ownership` already
 * gives.  An unprivileged libvirt session maps the guest's *root* to the
 * host user and every other guest id into that user's subuid range, so
 * the graphical session -- which GDM will not let be root -- is on the
 * wrong side of a share in both directions: it cannot enter the
 * workspace directory to write a frame, and a frame it did write is
 * owned by a subuid this machine cannot read.  Do not link it back.
 */
#define CLAWT_GUEST_SCREENSHOT_DIR "/tmp/gnome-mcp"

/**
 * CLAWT_GUEST_DESKTOP_TMPFILES_CONF:
 *
 * A tmpfiles rule clawtilla used to write, and now removes.
 *
 * It linked %CLAWT_GUEST_SCREENSHOT_DIR into the workspace share, which
 * put every frame somewhere the graphical session could not write.
 * cloud-init reads its seed once, so dropping the rule from the seed
 * reaches new guests only; every guest already built keeps recreating
 * the link at each boot until this file goes.
 */
#define CLAWT_GUEST_DESKTOP_TMPFILES_CONF \
    "/etc/tmpfiles.d/clawtilla-desktop.conf"

/**
 * CLAWT_GUEST_DESKTOP_UPDATE_SCRIPT:
 *
 * Brings the guest's checkout of gnome-desktop-mcp forward to the
 * repository's head.
 *
 * The checkout is cloned at first boot and cloud-init acts at first
 * boot only, so without this a guest ran whatever the repository held
 * the day its overlay was built, for ever.  The fix that found it was
 * to mouse input: press and release events were being lost by an
 * extension bug fixed upstream days earlier, and no built guest could
 * ever receive the fix short of a full rebuild.
 */
#define CLAWT_GUEST_DESKTOP_UPDATE_SCRIPT \
    "/usr/local/bin/clawtilla-desktop-update"

/**
 * CLAWT_GUEST_DESKTOP_UPDATE_STATUS_FILE:
 *
 * One line saying what the last update run did: `updated`, `current`,
 * `held`, or why it could not move.  Separate from
 * %CLAWT_GUEST_DESKTOP_STATUS_FILE because "the desktop installed" and
 * "the desktop is current" are different claims with different
 * remedies.
 */
#define CLAWT_GUEST_DESKTOP_UPDATE_STATUS_FILE \
    "/var/lib/clawtilla/desktop-update.status"

/**
 * CLAWT_GUEST_DESKTOP_UPDATE_UNIT:
 *
 * The systemd unit that runs the update before the display manager, so
 * a session never starts on desktop code older than the repository it
 * was installed from.
 */
#define CLAWT_GUEST_DESKTOP_UPDATE_UNIT \
    "/etc/systemd/system/clawtilla-desktop-update.service"

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

/**
 * clawt_guest_desktop_frame_dir_script:
 * @session_user: the account the graphical session runs as
 *
 * The shell that makes %CLAWT_GUEST_SCREENSHOT_DIR belong to
 * @session_user, run in the guest as the login commands use.
 *
 * Three things, and each is needed on its own: the stale tmpfiles rule
 * goes, so the link is not recreated at the next boot; the link itself
 * goes, so this boot is fixed too; and the directory is created owned by
 * the session, so the compositor -- which makes it 0700 and its own if
 * it gets there first -- can publish into it whoever got there first.
 *
 * A pure function so that what would run can be asserted on: it is one
 * line of shell reached only by a guest nobody is looking at, and a typo
 * in it fails silently, since removing a file that is not there succeeds.
 *
 * Returns: (transfer full) (nullable): the script
 */
gchar *clawt_guest_desktop_frame_dir_script(const gchar *session_user);

/**
 * clawt_guest_desktop_update_script:
 *
 * The script at %CLAWT_GUEST_DESKTOP_UPDATE_SCRIPT: fast-forwards the
 * guest's gnome-desktop-mcp checkout to the repository's head and
 * redoes the parts of the install that depend on its contents.  It
 * never fails the boot -- a forge that is down costs seconds and a
 * line in %CLAWT_GUEST_DESKTOP_UPDATE_STATUS_FILE, not a login screen.
 * A `.clawtilla-hold` file in the checkout pins it.
 *
 * Returns: (transfer full): the script body
 */
gchar *clawt_guest_desktop_update_script(void);

/**
 * clawt_guest_desktop_update_unit:
 *
 * The unit at %CLAWT_GUEST_DESKTOP_UPDATE_UNIT.  It orders before
 * `display-manager.service` -- the alias every display manager
 * provides, which is what keeps this file identical across the guest
 * families -- so the session that starts is the one the update just
 * refreshed.
 *
 * Returns: (transfer full): the unit body
 */
gchar *clawt_guest_desktop_update_unit(void);

/**
 * clawt_guest_desktop_maintain_script:
 *
 * A script run over SSH that installs the update script and its unit
 * into a guest that predates them.  cloud-init reads its seed once, so
 * a guest built before the unit existed has no other way to receive it
 * -- the same route the frame directory rule was taken back by.  Built
 * from clawt_guest_desktop_update_script() and
 * clawt_guest_desktop_update_unit() so the pushed copy and the seeded
 * copy cannot drift.
 *
 * Returns: (transfer full): the script body
 */
gchar *clawt_guest_desktop_maintain_script(void);
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
