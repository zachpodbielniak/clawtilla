/*
 * clawt-guest-desktop.c - A GNOME session inside the agent's own VM
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-guest-desktop.h"

/*
 * The extension's own identifier.  GNOME Shell finds an extension by the
 * directory it sits in matching the uuid in its metadata, so this is not a
 * name clawtilla is free to choose.
 */
#define EXTENSION_UUID "desktop-automation@gnomemcp.github.io"

#define CHECKOUT_DIR "/opt/gnome-desktop-mcp"

/*
 * Where the account with no shell prompt of its own turns automation on.
 * The extension starts with it off and the only other way to flip it is
 * the top bar indicator, which needs somebody sitting at the VM.
 */
#define ENABLE_SCRIPT "/usr/local/bin/clawtilla-desktop-automation-enable"

struct _ClawtGuestDesktop {
    gint      ref_count;

    gchar    *session_user;
    gchar    *mcp_repo;
    GStrv     packages;
    gboolean  autologin;
    gboolean  install_mcp;
};

G_DEFINE_BOXED_TYPE(ClawtGuestDesktop, clawt_guest_desktop,
                    clawt_guest_desktop_ref, clawt_guest_desktop_unref)

/*
 * Emits a YAML double-quoted scalar.
 *
 * A repository URL and an account name both reach here from a config file
 * somebody wrote, so neither is guaranteed to be a plain scalar.  Quoting
 * unconditionally is cheaper than deciding each time.
 */
static void
append_quoted(GString *out, const gchar *value)
{
    const gchar *p;

    g_string_append_c(out, '"');

    for (p = value; *p != '\0'; p++) {
        if (*p == '\\' || *p == '"')
            g_string_append_c(out, '\\');
        g_string_append_c(out, *p);
    }

    g_string_append_c(out, '"');
}

/*
 * A file, written through cloud-init's write_files.
 *
 * The body goes in as a block scalar rather than a quoted string: these
 * are shell scripts and ini files, and reading a diff of them matters
 * more than the two lines of code a quoted form would save.  Every line
 * is indented by the same amount, which is what makes it a block.
 */
static void
append_file(GString     *out,
            const gchar *path,
            const gchar *permissions,
            const gchar *body)
{
    g_auto(GStrv) lines = g_strsplit(body, "\n", -1);
    gsize i;

    g_string_append(out, "  - path: ");
    append_quoted(out, path);
    g_string_append_c(out, '\n');
    g_string_append_printf(out, "    permissions: \"%s\"\n", permissions);
    g_string_append(out, "    content: |\n");

    for (i = 0; lines[i] != NULL; i++) {
        /*
         * A trailing empty line would emit trailing whitespace, which is
         * legal YAML and an eyesore in a file people will read.
         */
        if (lines[i][0] == '\0' && lines[i + 1] == NULL)
            break;

        if (lines[i][0] == '\0')
            g_string_append_c(out, '\n');
        else
            g_string_append_printf(out, "      %s\n", lines[i]);
    }
}

ClawtGuestDesktop *
clawt_guest_desktop_new(const gchar *session_user)
{
    ClawtGuestDesktop *self;

    g_return_val_if_fail(session_user != NULL, NULL);

    self = g_new0(ClawtGuestDesktop, 1);
    self->ref_count = 1;
    self->session_user = g_strdup(session_user);
    self->autologin = TRUE;
    self->install_mcp = TRUE;

    return self;
}

ClawtGuestDesktop *
clawt_guest_desktop_ref(ClawtGuestDesktop *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);

    return self;
}

void
clawt_guest_desktop_unref(ClawtGuestDesktop *self)
{
    g_return_if_fail(self != NULL);

    if (!g_atomic_int_dec_and_test(&self->ref_count))
        return;

    g_free(self->session_user);
    g_free(self->mcp_repo);
    g_strfreev(self->packages);
    g_free(self);
}

gchar *
clawt_guest_desktop_resolve_user(const gchar *configured,
                                 const gchar *ssh_user)
{
    if (configured != NULL && *configured != '\0')
        return g_strdup(configured);

    /*
     * GDM refuses to log root in, and it is right to: a GNOME session as
     * root is a long-standing way to end up with root-owned files all
     * through a home directory.  The default ssh_user *is* root, so the
     * commonest configuration there is needs a second account -- the
     * agent keeps running its commands as root and the screen belongs to
     * somebody else.
     */
    if (ssh_user == NULL || *ssh_user == '\0' ||
        g_strcmp0(ssh_user, "root") == 0)
        return g_strdup("clawt");

    return g_strdup(ssh_user);
}

void
clawt_guest_desktop_set_autologin(ClawtGuestDesktop *self, gboolean autologin)
{
    g_return_if_fail(self != NULL);

    self->autologin = autologin;
}

void
clawt_guest_desktop_set_packages(ClawtGuestDesktop  *self,
                                 const gchar *const *packages)
{
    g_return_if_fail(self != NULL);

    g_strfreev(self->packages);
    self->packages = (packages != NULL) ? g_strdupv((GStrv)packages) : NULL;
}

void
clawt_guest_desktop_set_install_mcp(ClawtGuestDesktop *self, gboolean install)
{
    g_return_if_fail(self != NULL);

    self->install_mcp = install;
}

void
clawt_guest_desktop_set_mcp_repo(ClawtGuestDesktop *self, const gchar *repo)
{
    g_return_if_fail(self != NULL);

    if (repo == NULL)
        return;

    g_free(self->mcp_repo);
    self->mcp_repo = g_strdup(repo);
}

const gchar *
clawt_guest_desktop_get_session_user(ClawtGuestDesktop *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->session_user;
}

gboolean
clawt_guest_desktop_get_install_mcp(ClawtGuestDesktop *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->install_mcp;
}

void
clawt_guest_desktop_render_account(ClawtGuestDesktop *self,
                                   GString           *out,
                                   const gchar       *authorized_key,
                                   const gchar       *already_created)
{
    g_return_if_fail(self != NULL);
    g_return_if_fail(out != NULL);

    /*
     * One account, not two, when the session and the shell are the same
     * login.  cloud-init names a duplicate as an error and creates
     * neither.
     */
    if (g_strcmp0(self->session_user, already_created) == 0)
        return;

    g_string_append(out, "  - name: ");
    append_quoted(out, self->session_user);
    g_string_append(out, "\n    lock_passwd: true\n");
    g_string_append(out, "    sudo: \"ALL=(ALL) NOPASSWD:ALL\"\n");

    /*
     * The key reaches this account as well, and has to.  The MCP server
     * talks to the extension over the session bus of whoever is logged
     * in, so it has to run as that account -- which means an SSH
     * connection as that account, not as root.
     */
    if (authorized_key != NULL) {
        g_string_append(out, "    ssh_authorized_keys:\n      - ");
        append_quoted(out, authorized_key);
        g_string_append_c(out, '\n');
    }
}

/*
 * GDM's autologin, spelled for both families that ship it.
 *
 * Fedora reads /etc/gdm/custom.conf and Debian reads
 * /etc/gdm3/daemon.conf.  Writing both costs one unread file on each and
 * saves the failure where the desktop installs perfectly and stops at a
 * login prompt nobody is there to answer.
 */
static void
render_autologin(ClawtGuestDesktop *self, GString *out)
{
    g_autofree gchar *body = NULL;

    body = g_strdup_printf(
        "# Written by clawtilla.\n"
        "#\n"
        "# There is nobody at this machine's console, and until a session\n"
        "# exists there is no desktop to drive: the extension the agent\n"
        "# talks to runs inside GNOME Shell.\n"
        "[daemon]\n"
        "WaylandEnable=true\n"
        "AutomaticLoginEnable=true\n"
        "AutomaticLogin=%s\n",
        self->session_user);

    append_file(out, "/etc/gdm/custom.conf", "0644", body);
    append_file(out, "/etc/gdm3/daemon.conf", "0644", body);
}

/*
 * Machine-wide dconf defaults.
 *
 * Set here rather than with `gsettings set` because there is no session
 * to set them in yet: cloud-init runs long before anybody logs in, and
 * gsettings without a bus writes nothing and reports success.
 */
static void
render_dconf(ClawtGuestDesktop *self, GString *out)
{
    g_autoptr(GString) body = g_string_new(NULL);

    append_file(out, "/etc/dconf/profile/user", "0644",
                "user-db:user\n"
                "system-db:local\n");

    g_string_append(body,
        "# Written by clawtilla.\n");

    if (self->install_mcp) {
        g_string_append(body,
            "\n"
            "# The extension is the only way into a Wayland session from\n"
            "# outside it, so it is enabled before anyone logs in.\n"
            "[org/gnome/shell]\n"
            "enabled-extensions=['" EXTENSION_UUID "']\n"
            "disable-user-extensions=false\n"
            "welcome-dialog-last-shown-version='99.0'\n"
            "\n"
            "# The extension asks for consent in a modal dialog on first\n"
            "# enable.  Nobody is here to dismiss it, and it would sit in\n"
            "# front of everything the agent tried to look at.\n"
            "[org/gnome/shell/extensions/desktop-automation]\n"
            "consent-acknowledged=true\n");
    }

    /*
     * A locked screen is the quiet way this whole feature stops working:
     * screenshots come back showing a lock screen, clicks land nowhere,
     * and the password that would fix it has nobody to type it.
     */
    g_string_append(body,
        "\n"
        "# Nothing here can answer a lock screen.\n"
        "[org/gnome/desktop/screensaver]\n"
        "lock-enabled=false\n"
        "idle-activation-enabled=false\n"
        "\n"
        "[org/gnome/desktop/session]\n"
        "idle-delay=uint32 0\n"
        "\n"
        "[org/gnome/settings-daemon/plugins/power]\n"
        "sleep-inactive-ac-type='nothing'\n"
        "idle-dim=false\n");

    append_file(out, "/etc/dconf/db/local.d/00-clawtilla-desktop", "0644",
                body->str);
}

/*
 * The two scripts the guest ends up running: one to reach the MCP server
 * with the right bus in the environment, one to switch automation on once
 * the session exists.
 */
static void
render_scripts(ClawtGuestDesktop *self, GString *out)
{
    append_file(out, "/usr/local/bin/" CLAWT_GUEST_DESKTOP_LAUNCHER, "0755",
        "#!/bin/bash\n"
        "# Written by clawtilla.\n"
        "#\n"
        "# The MCP server reaches GNOME Shell over the session bus of\n"
        "# whoever is logged in.  An SSH connection arrives with no such\n"
        "# variable set, so it is worked out here -- inside the guest,\n"
        "# where the uid is knowable -- rather than being threaded\n"
        "# through argv quoting on the host.\n"
        "set -euo pipefail\n"
        "\n"
        "uid=\"$(id -u)\"\n"
        "export DBUS_SESSION_BUS_ADDRESS=\"unix:path=/run/user/${uid}/bus\"\n"
        "\n"
        "exec " CHECKOUT_DIR "/venv/bin/gnome-desktop-mcp \"$@\"\n");

    if (!self->install_mcp)
        return;

    append_file(out, ENABLE_SCRIPT, "0755",
        "#!/bin/bash\n"
        "# Written by clawtilla.\n"
        "#\n"
        "# The extension starts with automation off and stays that way\n"
        "# until somebody clicks the top bar indicator.  Nobody is going\n"
        "# to, so this does it -- retrying, because the session starts\n"
        "# before the extension has exported anything to call.\n"
        "set -uo pipefail\n"
        "\n"
        "for _ in $(seq 1 60)\n"
        "do\n"
        "    if gdbus call --session \\\n"
        "        --dest org.gnome.Shell \\\n"
        "        --object-path /io/github/gnomemcp/DesktopAutomation \\\n"
        "        --method io.github.gnomemcp.DesktopAutomation.SetEnabled \\\n"
        "        true >/dev/null 2>&1\n"
        "    then\n"
        "        exit 0\n"
        "    fi\n"
        "\n"
        "    sleep 2\n"
        "done\n"
        "\n"
        "echo 'desktop automation never answered' >&2\n"
        "exit 1\n");

    append_file(out,
                "/etc/xdg/autostart/clawtilla-desktop-automation.desktop",
                "0644",
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=clawtilla desktop automation\n"
        "Comment=Switches on the automation interface the agent drives\n"
        "Exec=" ENABLE_SCRIPT "\n"
        "NoDisplay=true\n"
        /*
         * No X-GNOME-Autostart-Phase.  gnome-session stopped managing
         * session services, and an entry that sets it is now *skipped*
         * with a line in the journal saying so -- so the script never
         * ran, automation stayed off, and every tool answered
         * "Automation disabled by user. Enable from top bar indicator."
         * on a machine with no top bar to click.
         */
        "X-GNOME-Autostart-enabled=true\n");
}

void
clawt_guest_desktop_render_setup(ClawtGuestDesktop *self, GString *out)
{
    gsize i;

    g_return_if_fail(self != NULL);
    g_return_if_fail(out != NULL);

    /*
     * A cloud image's package metadata is however old the image is, and
     * an install against a stale index fails on a package that has since
     * been rebuilt.
     */
    g_string_append(out, "package_update: true\npackages:\n");

    for (i = 0; self->packages != NULL && self->packages[i] != NULL; i++) {
        g_string_append(out, "  - ");
        append_quoted(out, self->packages[i]);
        g_string_append_c(out, '\n');
    }

    if (self->install_mcp) {
        /*
         * Not in the configured list, because they are not the desktop:
         * they are what it takes to build the thing that drives it, and
         * an operator trimming the package list should not silently
         * disable automation by leaving one out.
         *
         * PyGObject comes from the distribution rather than from pip --
         * dasbus needs it, and building it in a virtualenv means a
         * compiler and a set of -devel packages for a library that is
         * already installed.
         */
        g_string_append(out,
            "  - \"git\"\n"
            "  - \"python3-pip\"\n"
            "  - \"python3-gobject\"\n");
    }

    g_string_append(out, "write_files:\n");

    if (self->autologin)
        render_autologin(self, out);

    render_dconf(self, out);
    render_scripts(self, out);

    g_string_append(out, "runcmd:\n");

    /*
     * A cloud image boots to multi-user.target, so GDM would be
     * installed, enabled and never started.
     */
    g_string_append(out, "  - [systemctl, set-default, graphical.target]\n");

    if (self->install_mcp) {
        g_autofree gchar *clone = NULL;

        /*
         * Cloned rather than taken from PyPI: the extension only exists
         * in the repository, and the server calls D-Bus methods that
         * exact extension has to export.  A version skew between the two
         * halves surfaces as a tool that exists and always fails.
         */
        clone = g_strdup_printf(
            "test -d %s || git clone --depth 1 %s %s",
            CHECKOUT_DIR,
            self->mcp_repo != NULL ? self->mcp_repo : "", CHECKOUT_DIR);

        g_string_append(out, "  - ");
        append_quoted(out, clone);
        g_string_append_c(out, '\n');

        g_string_append(out,
            "  - [ln, -sfn, \"" CHECKOUT_DIR "/extension/" EXTENSION_UUID
            "\", \"/usr/share/gnome-shell/extensions/" EXTENSION_UUID "\"]\n"
            "  - [glib-compile-schemas, \"" CHECKOUT_DIR "/extension/"
            EXTENSION_UUID "/schemas\"]\n");

        /*
         * --system-site-packages so PyGObject is visible; the virtualenv
         * is otherwise there to keep pip away from an
         * externally-managed /usr, which on Fedora refuses outright.
         */
        g_string_append(out,
            "  - [python3, -m, venv, --system-site-packages, \""
            CHECKOUT_DIR "/venv\"]\n");

        /*
         * mcp is pinned below 2.
         *
         * gnome-desktop-mcp asks for `mcp>=1.0.0` and imports
         * `mcp.server.fastmcp`, which the 2.x SDK removed -- so pip
         * resolves the newest, installs cleanly, and the server dies on
         * its first import with ModuleNotFoundError. Nothing before that
         * point fails: the clone works, the venv works, the install
         * reports success, and the only symptom is an MCP server that
         * exits the moment the agent's client speaks to it.
         *
         * The constraint belongs in the guest's own pyproject and this
         * is a bandage over somebody else's floor being too low. It
         * stays until that is raised, because the alternative is a
         * feature that silently does not work.
         */
        g_string_append(out,
            "  - [\"" CHECKOUT_DIR "/venv/bin/pip\", install, --quiet, "
            "\"mcp<2\", \"" CHECKOUT_DIR "/mcp-server\"]\n");
    }

    /*
     * dconf reads the text files above once and compiles them; until it
     * has, every default written here is inert.
     */
    g_string_append(out, "  - [dconf, update]\n");

    if (self->autologin)
        g_string_append(out,
            "  - [systemctl, enable, --now, gdm.service]\n");
}
