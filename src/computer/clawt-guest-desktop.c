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

/*
 * GNOME Shell looks for extensions under gnome-shell/extensions in each
 * XDG_DATA_DIRS entry.  Only Fedora's gnome-shell package ships the
 * directory, which is the whole of the bug this constant exists to keep
 * visible: it has to be created before anything is put in it.
 */
#define SYSTEM_EXTENSION_DIR "/usr/share/gnome-shell/extensions"

struct _ClawtGuestDesktop {
    gint      ref_count;

    gchar    *session_user;
    gchar    *mcp_repo;
    GStrv     packages;
    gboolean  autologin;
    gboolean  install_mcp;

    ClawtGuestFlavour flavour;
};

/* ── What each family calls things ───────────────────────────────── */

/*
 * cloud-init picks the package *manager* and nothing else, so a seed
 * still has to know the names -- and the names turned out to be the
 * smaller half of the problem.  Three other things differ, and all three
 * used to be written out as Fedora and nothing else:
 *
 *   - the display manager's unit is `gdm` here and `gdm3` on Debian, so
 *     `systemctl enable --now gdm.service` on a Debian guest fails and
 *     the machine sits at multi-user with a desktop installed;
 *
 *   - PyGObject is `python3-gobject` here and `python3-gi` there, and
 *     dasbus needs it, so the MCP server installs and dies on its first
 *     import;
 *
 *   - a Debian cloud image has neither the `dconf` binary nor
 *     `glib-compile-schemas`, so every default written into
 *     /etc/dconf/db stays inert and the extension's schema never
 *     compiles -- which reads as an extension that will not enable.
 *
 * None of that is reachable from `computer.vm.desktop.packages`, which
 * is why overriding the package list was never enough on its own.
 */
static const gchar *const fedora_desktop[] = {
    "gdm", "gnome-shell", "gnome-session", "gnome-console",
    "gnome-control-center", "nautilus", "gnome-text-editor",
    "xdg-user-dirs-gtk", "gnome-tweaks", "gnome-shell-extension-common",
    "firefox", "dconf", NULL
};

static const gchar *const fedora_mcp[] = {
    "git", "python3-pip", "python3-gobject",
    /*
     * OCR, which is what turns a screenshot into somewhere to click.
     *
     * An agent driving a desktop has the screen as an image and needs a
     * coordinate; without this it estimates one from an assumed layout
     * and misses controls by a few dozen pixels, which reads as the
     * pointer tools being unreliable. `tesseract <file> - tsv` prints a
     * bounding box per word, and the centre of the right box is the
     * answer. The language data is a separate package everywhere and
     * tesseract does nothing useful without it.
     */
    "tesseract", "tesseract-langpack-eng", NULL
};

/*
 * Enterprise Linux is Fedora's names minus the parts that are Fedora's
 * alone.  gnome-console, gnome-text-editor and gnome-tweaks are not in
 * the EL10 repositories, and cloud-init treats a package it cannot find
 * as a failure of the whole install -- so asking for one takes the
 * desktop with it.
 */
static const gchar *const enterprise_desktop[] = {
    "gdm", "gnome-shell", "gnome-session", "gnome-terminal",
    "gnome-control-center", "nautilus", "xdg-user-dirs-gtk",
    "firefox", "dconf", NULL
};

/*
 * No tesseract here, and it is not an oversight.
 *
 * On Enterprise Linux it lives in EPEL, which a cloud image does not
 * have enabled -- and cloud-init treats a package it cannot find as a
 * failure of the whole install, so naming it would take the desktop down
 * with it. An EL guest can drive a desktop; it just cannot read one
 * until somebody adds EPEL.
 */
static const gchar *const enterprise_mcp[] = {
    "git", "python3-pip", "python3-gobject", NULL
};

static const gchar *const debian_desktop[] = {
    "gdm3", "gnome-shell", "gnome-session", "gnome-terminal",
    "gnome-control-center", "nautilus", "gnome-text-editor",
    "xdg-user-dirs-gtk", "gnome-tweaks",
    /*
     * Debian stable ships Firefox as the extended-support release and
     * has no `firefox` package at all, so asking for that name fails
     * the whole package install rather than merely missing a browser.
     */
    "firefox-esr",
    /* The `dconf` binary is here, not in the gsettings backend. */
    "dconf-cli", NULL
};

/*
 * Ubuntu is Debian's list with one name changed, and the change is the
 * reason it is a family of its own: there is no `firefox-esr` in
 * Ubuntu's archive, and `firefox` there is a transitional package that
 * installs the snap.  Either name is wrong on the other distribution.
 */
static const gchar *const ubuntu_desktop[] = {
    "gdm3", "gnome-shell", "gnome-session", "gnome-terminal",
    "gnome-control-center", "nautilus", "gnome-text-editor",
    "xdg-user-dirs-gtk", "gnome-tweaks",
    "firefox",
    "dconf-cli", NULL
};

/*
 * python3-venv is a separate package on Debian and `python3 -m venv`
 * fails without it; libglib2.0-bin carries glib-compile-schemas, which
 * a cloud image does not otherwise have.
 */
static const gchar *const debian_mcp[] = {
    "git", "python3-pip", "python3-gi", "python3-venv", "libglib2.0-bin",
    /* Debian splits the binary from the language data; both are needed. */
    "tesseract-ocr", "tesseract-ocr-eng",
    NULL
};

/*
 * Arch drops the `3` that Debian and Fedora both put in their Python
 * package names, and `glib-compile-schemas` lives in glib2-devel since
 * the development tools were split out of glib2 -- a cloud image has the
 * library and not the compiler.  `python -m venv` needs nothing extra:
 * venv is in the core python package.
 */
static const gchar *const arch_desktop[] = {
    "gdm", "gnome-shell", "gnome-session", "gnome-terminal",
    "gnome-control-center", "nautilus", "gnome-text-editor",
    "xdg-user-dirs-gtk", "gnome-tweaks", "firefox", "dconf", NULL
};

static const gchar *const arch_mcp[] = {
    "git", "python-pip", "python-gobject", "glib2-devel",
    "tesseract", "tesseract-data-eng", NULL
};

typedef struct {
    ClawtGuestFlavour   flavour;
    const gchar *const *desktop;
    const gchar *const *mcp;
    const gchar        *display_manager;
    /*
     * Where that display manager reads its configuration.
     *
     * Three spellings across five families, and no two of them agree by
     * accident: Fedora, Enterprise Linux and Arch ship
     * /etc/gdm/custom.conf, Debian renamed it to /etc/gdm3/daemon.conf,
     * and Ubuntu kept the upstream name under Debian's directory --
     * /etc/gdm3/custom.conf. Verified against each distribution's own
     * package file list rather than recalled.
     *
     * This used to be handled by writing two of them and letting the
     * unread one lie there, which reads as thorough and is why the third
     * was never noticed missing: an Ubuntu guest installed a desktop
     * perfectly and stopped at a login prompt whose password is locked,
     * with nobody there to answer it.
     */
    const gchar        *autologin_conf;
    /*
     * Whether to upgrade everything before installing anything.
     *
     * Only Arch, and not as a nicety.  cloud-init's package_update runs
     * `pacman -Sy`, which refreshes the index without upgrading what is
     * already there -- and installing against that is the partial
     * upgrade Arch warns about, where a new package is linked against
     * libraries the image still has the old versions of.  It breaks in
     * the guest, after the desktop appears to have installed.
     *
     * package_upgrade adds the `-Su`, and cloud-init runs it between the
     * refresh and the install, which is exactly the order wanted.  A
     * point release does not need this, which is why it is per family
     * rather than always on: it is a long download on a distribution
     * that would not benefit.
     */
    gboolean            full_upgrade;
} FlavourSpec;

static const FlavourSpec flavours[] = {
    { CLAWT_GUEST_FLAVOUR_FEDORA, fedora_desktop, fedora_mcp,
      "gdm.service", "/etc/gdm/custom.conf", FALSE },
    { CLAWT_GUEST_FLAVOUR_ENTERPRISE, enterprise_desktop, enterprise_mcp,
      "gdm.service", "/etc/gdm/custom.conf", FALSE },
    { CLAWT_GUEST_FLAVOUR_DEBIAN, debian_desktop, debian_mcp,
      "gdm3.service", "/etc/gdm3/daemon.conf", FALSE },
    /*
     * Everything but the browser is Debian's, so the rest is shared --
     * except this, which is the second thing Ubuntu does its own way and
     * the reason the family exists at all.
     */
    { CLAWT_GUEST_FLAVOUR_UBUNTU, ubuntu_desktop, debian_mcp,
      "gdm3.service", "/etc/gdm3/custom.conf", FALSE },
    { CLAWT_GUEST_FLAVOUR_ARCH, arch_desktop, arch_mcp,
      "gdm.service", "/etc/gdm/custom.conf", TRUE }
};

/*
 * Falls back to Fedora, which is what clawtilla is developed against.
 * A caller that could not place its image has already said so; this is
 * not the place to warn again, once per rendered field.
 */
static const FlavourSpec *
spec_for(ClawtGuestFlavour flavour)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(flavours); i++) {
        if (flavours[i].flavour == flavour)
            return &flavours[i];
    }

    return &flavours[0];
}

ClawtGuestFlavour
clawt_guest_desktop_resolve_flavour(const gchar *configured,
                                    const gchar *image)
{
    gint value = 0;

    /*
     * What somebody wrote down wins over anything derived.  An image
     * with an unhelpful name is exactly the case this key exists for.
     */
    if (configured != NULL && *configured != '\0' &&
        clawt_enum_from_nick(CLAWT_TYPE_GUEST_FLAVOUR, configured, &value) &&
        (ClawtGuestFlavour)value != CLAWT_GUEST_FLAVOUR_AUTO)
        return (ClawtGuestFlavour)value;

    return clawt_vm_image_flavour(image);
}

void
clawt_guest_desktop_set_flavour(ClawtGuestDesktop *self,
                                ClawtGuestFlavour  flavour)
{
    g_return_if_fail(self != NULL);

    self->flavour = flavour;
}

ClawtGuestFlavour
clawt_guest_desktop_get_flavour(ClawtGuestDesktop *self)
{
    g_return_val_if_fail(self != NULL, CLAWT_GUEST_FLAVOUR_FEDORA);

    return self->flavour;
}

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
 * GDM's autologin, in the one file this family's GDM actually reads.
 *
 * It used to write two -- Fedora's and Debian's -- on the reasoning that
 * an unread file costs nothing.  It does not, but it hides something:
 * two paths look like every path, and Ubuntu's is a third.  Its gdm3
 * ships /etc/gdm3/custom.conf and no daemon.conf, so both files were
 * inert, autologin never happened, and the guest stopped at a login
 * prompt for an account whose password is deliberately locked -- which
 * nobody, and nothing, can get past.
 *
 * The path is in the flavour table beside the unit name, because they
 * are the same fact about the same program, and a family added later
 * cannot leave one of them out without leaving a hole in the row.
 */
static void
render_autologin(ClawtGuestDesktop *self, GString *out)
{
    const FlavourSpec *spec = spec_for(self->flavour);
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

    append_file(out, spec->autologin_conf, "0644", body);
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
 * Starting a graphical application inside the session, rather than
 * beside it.
 *
 * An SSH connection has no session environment -- no WAYLAND_DISPLAY, no
 * XDG_RUNTIME_DIR, no session bus -- so an agent handed `computer_exec`
 * and told to open a browser reaches for `DISPLAY=:0 firefox`.  That is
 * the worst kind of workaround: it succeeds.  A window appears, the
 * process runs, and the application has quietly been put on Xwayland
 * instead of in the Wayland session the desktop was built for, where it
 * composites and receives synthetic input by a different path.  Nothing
 * the agent can query says which of the two it got.
 *
 * So the environment is taken from the session instead of guessed at.
 * `systemd-run --user` starts the application under the user's own
 * service manager, which gnome-session has already told about the
 * session -- the same route a desktop launcher takes.
 */
static void
render_run_script(ClawtGuestDesktop *self, GString *out)
{
    g_autoptr(GString) body = g_string_new(NULL);
    g_autofree gchar *user = g_shell_quote(self->session_user);

    g_string_append(body,
        "#!/bin/bash\n"
        "# Written by clawtilla.\n"
        "#\n"
        "# Starts a graphical application inside the guest's session.\n"
        "#\n"
        "# Use this rather than `DISPLAY=:0 <app>` from a plain shell.\n"
        "# That form works, and puts the application on Xwayland instead\n"
        "# of in the session -- a different compositing and input path,\n"
        "# with nothing to show which one you got.\n"
        "set -uo pipefail\n"
        "\n"
        "user=");
    g_string_append(body, user);
    g_string_append(body,
        "\n"
        "\n"
        "if [ $# -eq 0 ]\n"
        "then\n"
        "    echo \"usage: $(basename \"$0\") <command> [args...]\" >&2\n"
        "    exit 2\n"
        "fi\n"
        "\n"
        "if ! uid=\"$(id -u \"$user\")\"\n"
        "then\n"
        "    echo \"clawtilla: no such user: $user\" >&2\n"
        "    exit 1\n"
        "fi\n"
        "\n"
        "# Root cannot use the session's Wayland socket and the session's\n"
        "# own account can, so become it -- re-execing this script rather\n"
        "# than wrapping the application, so what runs is this\n"
        "# environment and not a login shell's.\n"
        "if [ \"$(id -u)\" -ne \"$uid\" ]\n"
        "then\n"
        "    exec runuser -u \"$user\" -- \"$0\" \"$@\"\n"
        "fi\n"
        "\n"
        "export XDG_RUNTIME_DIR=\"/run/user/${uid}\"\n"
        "export DBUS_SESSION_BUS_ADDRESS=\"unix:path=${XDG_RUNTIME_DIR}/bus\"\n"
        "\n"
        "# Said plainly, because the alternative is an error from the\n"
        "# application about a display, which points at the application.\n"
        "# A guest still booting, or one whose session died, looks\n"
        "# exactly like a browser that will not start.\n"
        "if ! systemctl --user show-environment 2>/dev/null \\\n"
        "        | grep -q '^WAYLAND_DISPLAY='\n"
        "then\n"
        "    echo \"clawtilla: $user has no graphical session yet.\" >&2\n"
        "    echo \"clawtilla: the desktop may still be starting; there is\" \\\n"
        "         \"nothing to open a window in until it has.\" >&2\n"
        "    exit 1\n"
        "fi\n"
        "\n"
        "# Returns as soon as the application is started, not when it\n"
        "# exits: an agent waiting on a browser would wait for ever.\n"
        "exec systemd-run --user --collect --quiet -- \"$@\"\n");

    append_file(out, CLAWT_GUEST_DESKTOP_RUN_SCRIPT, "0755", body->str);
}

/*
 * Everything the guest has to build for the desktop to be drivable.
 *
 * A script rather than the list of runcmd entries this used to be, and
 * the difference cost a working feature on two distributions out of
 * five.  cloud-init runs runcmd without `set -e`: a step that fails is
 * one line in a log inside the guest and every later step runs anyway.
 * So the extension failed to install, the venv and the MCP server were
 * built regardless, dconf enabled an extension that was not on disk, and
 * the first anybody heard of it was an agent -- days later -- being told
 * "DBus object has no attribute" by every desktop tool it owns.
 *
 * A script can check each step, name the one that went wrong, and write
 * the answer down.  It can also be run a second time, which a seed
 * cannot: cloud-init acts at first boot only.
 */
static void
render_install_script(ClawtGuestDesktop *self, GString *out)
{
    g_autoptr(GString) body = g_string_new(NULL);
    g_autofree gchar *repo = NULL;

    /*
     * Shell-quoted rather than interpolated. It is a URL out of a config
     * file somebody edits, and it is about to be a word in a command
     * line -- the same rule the credential format follows a directory
     * along, for the same reason.
     */
    repo = g_shell_quote(self->mcp_repo != NULL ? self->mcp_repo : "");

    g_string_append(body,
        "#!/bin/bash\n"
        "# Written by clawtilla.\n"
        "#\n"
        "# Installs the half of the desktop that lives in the guest: the\n"
        "# GNOME Shell extension the agent's tools actually are, and the\n"
        "# MCP server that speaks to it.\n"
        "#\n"
        "# Safe to run again. Every step checks for what it was going to\n"
        "# create, which matters because cloud-init will not run twice.\n"
        "set -uo pipefail\n"
        "\n"
        "uuid='" EXTENSION_UUID "'\n"
        "checkout='" CHECKOUT_DIR "'\n"
        "sysext='" SYSTEM_EXTENSION_DIR "'\n"
        "status='" CLAWT_GUEST_DESKTOP_STATUS_FILE "'\n"
        "\n"
        "mkdir -p \"$(dirname \"$status\")\"\n"
        "\n"
        "fail () {\n"
        "    printf 'failed: %s\\n' \"$1\" > \"$status\"\n"
        "    printf 'clawtilla: desktop install failed: %s\\n' \"$1\" >&2\n"
        "    exit 1\n"
        "}\n"
        "\n"
        "printf 'installing\\n' > \"$status\"\n"
        "\n"
        "if [ ! -d \"$checkout\" ]\n"
        "then\n"
        "    git clone --depth 1 ");
    g_string_append(body, repo);
    g_string_append(body,
        " \"$checkout\" \\\n"
        "        || fail 'could not clone the desktop MCP repository'\n"
        "fi\n"
        "\n"
        "[ -d \"$checkout/extension/$uuid\" ] \\\n"
        "    || fail \"the checkout has no extension at extension/$uuid\"\n"
        "\n"
        "# The step that only ever worked on Fedora.\n"
        "#\n"
        "# GNOME Shell looks for extensions under gnome-shell/extensions\n"
        "# in each XDG_DATA_DIRS entry, but only Fedora's gnome-shell\n"
        "# package ships that directory -- and `ln` will not create a\n"
        "# parent. On Debian and Arch the link failed with ENOENT while\n"
        "# everything around it succeeded.\n"
        "mkdir -p \"$sysext\" || fail \"could not create $sysext\"\n"
        "\n"
        "ln -sfn \"$checkout/extension/$uuid\" \"$sysext/$uuid\" \\\n"
        "    || fail \"could not link the extension into $sysext\"\n"
        "\n"
        "# Through the link rather than at it. A dangling symlink\n"
        "# enumerates as a symlink and not as a directory, which GNOME\n"
        "# Shell skips without saying anything.\n"
        "[ -f \"$sysext/$uuid/metadata.json\" ] \\\n"
        "    || fail 'the extension link does not resolve to an extension'\n"
        "\n"
        "glib-compile-schemas \"$checkout/extension/$uuid/schemas\" \\\n"
        "    || fail \"could not compile the extension's schemas\"\n"
        "\n"
        "# --system-site-packages so PyGObject is visible; the virtualenv\n"
        "# is otherwise there to keep pip away from an\n"
        "# externally-managed /usr, which on Fedora refuses outright.\n"
        "python3 -m venv --system-site-packages \"$checkout/venv\" \\\n"
        "    || fail 'could not create the virtualenv'\n"
        "\n"
        "# No version constraints, deliberately: they belong to the\n"
        "# repository being cloned, where they can be raised in step with\n"
        "# the code that needs them.\n"
        "\"$checkout/venv/bin/pip\" install --quiet \"$checkout/mcp-server\" \\\n"
        "    || fail 'could not install the desktop MCP server'\n"
        "\n"
        "[ -x \"$checkout/venv/bin/gnome-desktop-mcp\" ] \\\n"
        "    || fail 'the server installed but left no gnome-desktop-mcp'\n"
        "\n"
        "printf 'ok\\n' > \"$status\"\n");

    append_file(out, CLAWT_GUEST_DESKTOP_INSTALL_SCRIPT, "0755", body->str);
}

/*
 * Where the extension's screenshots land.
 *
 * gnome-desktop-mcp writes them to /tmp/gnome-mcp inside the guest and
 * returns the path.  An agent's own `read` runs on the *host*, so that
 * path names a file it cannot open -- and the agent has no way to tell
 * that from a screenshot that failed.  One spent a session reasoning
 * from window titles instead of looking at the screen, and reported the
 * capture as broken; the capture was perfect and unreachable.
 *
 * A symlink into the workspace share rather than a change upstream: the
 * directory is compiled into the extension, and this is the same file
 * either way.  tmpfiles rather than the installer, because /tmp is
 * usually a tmpfs and the link has to be there again after a reboot.
 */
static void
render_screenshot_share(ClawtGuestDesktop *self, GString *out)
{
    if (!self->install_mcp)
        return;

    append_file(out, "/etc/tmpfiles.d/clawtilla-desktop.conf", "0644",
        "# Written by clawtilla.\n"
        "#\n"
        "# The agent reads these with tools that run on the host, so they\n"
        "# have to land in something the host shares with this guest.\n"
        "# 0777 because the uid the session runs as and the uid that owns\n"
        "# the workspace on the host are not the same question.\n"
        "d " CLAWT_WORKSPACE_MOUNT_POINT "/screenshots 0777 - - -\n"
        "L+ /tmp/gnome-mcp - - - - "
        CLAWT_WORKSPACE_MOUNT_POINT "/screenshots\n");
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

    render_run_script(self, out);
    render_screenshot_share(self, out);

    if (!self->install_mcp)
        return;

    render_install_script(self, out);

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
    const FlavourSpec *spec;
    gsize i;

    g_return_if_fail(self != NULL);
    g_return_if_fail(out != NULL);

    spec = spec_for(self->flavour);

    /*
     * A cloud image's package metadata is however old the image is, and
     * an install against a stale index fails on a package that has since
     * been rebuilt.
     */
    g_string_append(out, "package_update: true\n");

    if (spec->full_upgrade)
        g_string_append(out, "package_upgrade: true\n");

    g_string_append(out, "packages:\n");

    /*
     * A configured list wins outright and is not merged with the
     * family's.  Somebody who wrote a list meant that list -- merging
     * would quietly reinstate a package they had removed on purpose,
     * and there would be no way to express "not that one".
     */
    if (self->packages != NULL && self->packages[0] != NULL) {
        for (i = 0; self->packages[i] != NULL; i++) {
            g_string_append(out, "  - ");
            append_quoted(out, self->packages[i]);
            g_string_append_c(out, '\n');
        }
    } else {
        for (i = 0; spec->desktop[i] != NULL; i++) {
            g_string_append(out, "  - ");
            append_quoted(out, spec->desktop[i]);
            g_string_append_c(out, '\n');
        }
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
        for (i = 0; spec->mcp[i] != NULL; i++) {
            g_string_append(out, "  - ");
            append_quoted(out, spec->mcp[i]);
            g_string_append_c(out, '\n');
        }
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

    /*
     * One entry, not six.  What it does and why it is a script rather
     * than a list of commands is in render_install_script().
     */
    if (self->install_mcp)
        g_string_append(out, "  - [" CLAWT_GUEST_DESKTOP_INSTALL_SCRIPT "]\n");

    /*
     * dconf reads the text files above once and compiles them; until it
     * has, every default written here is inert.
     */
    g_string_append(out, "  - [dconf, update]\n");

    if (self->autologin) {
        /*
         * By its real unit name.  Debian's package is gdm3 and so is its
         * unit; asking for gdm.service there fails, cloud-init logs one
         * line about it, and the guest sits at a text console with a
         * complete desktop installed on it.
         */
        g_string_append_printf(out, "  - [systemctl, enable, --now, %s]\n",
                               spec->display_manager);
    }
}
