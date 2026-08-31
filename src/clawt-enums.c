/*
 * clawt-enums.c - Enumeration type registration for clawtilla
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Written out longhand, one registration per type, exactly as libreclaw
 * does it in lc-enums.c.  A macro looks tempting here and does not work:
 * the value tables are brace-enclosed initializer lists, and the C
 * preprocessor does not treat braces as protecting commas the way it does
 * parentheses, so every table lands as a dozen separate macro arguments.
 */

#include "clawtilla.h"

#include <string.h>

/*
 * Suppress -Wdiscarded-qualifiers for this file.  The g_once_init_enter
 * pattern used by GLib's enum registration triggers a volatile qualifier
 * warning in gatomic.h -- a known GLib issue, not a bug here.  libreclaw
 * carries the same pragma in lc-enums.c for the same reason.
 */
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"

/* Register ClawtAgentState as a GLib enum type */
GType
clawt_agent_state_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_AGENT_STATE_STOPPED, "CLAWT_AGENT_STATE_STOPPED", "stopped" },
            { CLAWT_AGENT_STATE_STARTING, "CLAWT_AGENT_STATE_STARTING", "starting" },
            { CLAWT_AGENT_STATE_RUNNING, "CLAWT_AGENT_STATE_RUNNING", "running" },
            { CLAWT_AGENT_STATE_DEGRADED, "CLAWT_AGENT_STATE_DEGRADED", "degraded" },
            { CLAWT_AGENT_STATE_STOPPING, "CLAWT_AGENT_STATE_STOPPING", "stopping" },
            { CLAWT_AGENT_STATE_ERROR, "CLAWT_AGENT_STATE_ERROR", "error" },
            { CLAWT_AGENT_STATE_SHADOW, "CLAWT_AGENT_STATE_SHADOW", "shadow" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtAgentState", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtAgentCaps as a GLib flags type */
GType
clawt_agent_caps_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GFlagsValue values[] = {
            { CLAWT_AGENT_CAPS_TOOLS_MCP, "CLAWT_AGENT_CAPS_TOOLS_MCP", "tools-mcp" },
            { CLAWT_AGENT_CAPS_COMPUTER, "CLAWT_AGENT_CAPS_COMPUTER", "computer" },
            { CLAWT_AGENT_CAPS_HOST_CONTROL, "CLAWT_AGENT_CAPS_HOST_CONTROL", "host-control" },
            { CLAWT_AGENT_CAPS_DESKTOP, "CLAWT_AGENT_CAPS_DESKTOP", "desktop" },
            { CLAWT_AGENT_CAPS_DESKTOP_INPUT, "CLAWT_AGENT_CAPS_DESKTOP_INPUT", "desktop-input" },
            { CLAWT_AGENT_CAPS_MOUNTS, "CLAWT_AGENT_CAPS_MOUNTS", "mounts" },
            /*
             * "images" and "attachments" used to sit here.  Both were
             * registered, neither was ever set by recompute_caps(), and
             * a flag that is always false makes every control bound to
             * it permanently insensitive -- so they advertised a set of
             * capabilities the agent could not have.  Bits 6 and 7 are
             * left unused rather than reassigned: renumbering the flags
             * below them would change what an older client reads out of
             * a caps string.
             */
            { CLAWT_AGENT_CAPS_STREAMING, "CLAWT_AGENT_CAPS_STREAMING", "streaming" },
            { CLAWT_AGENT_CAPS_EFFORT_LEVELS, "CLAWT_AGENT_CAPS_EFFORT_LEVELS", "effort-levels" },
            { CLAWT_AGENT_CAPS_PEER_COMMS, "CLAWT_AGENT_CAPS_PEER_COMMS", "peer-comms" },
            { CLAWT_AGENT_CAPS_INTERRUPT, "CLAWT_AGENT_CAPS_INTERRUPT", "interrupt" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_flags_register_static("ClawtAgentCaps", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtRuntimeType as a GLib enum type */
GType
clawt_runtime_type_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_RUNTIME_PROCESS, "CLAWT_RUNTIME_PROCESS", "process" },
            { CLAWT_RUNTIME_EMBEDDED, "CLAWT_RUNTIME_EMBEDDED", "embedded" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtRuntimeType", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtTeamRole as a GLib enum type */
GType
clawt_team_role_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_TEAM_MEMBER, "CLAWT_TEAM_MEMBER", "member" },
            { CLAWT_TEAM_LEAD, "CLAWT_TEAM_LEAD", "lead" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtTeamRole", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtRestartPolicy as a GLib enum type */
GType
clawt_restart_policy_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_RESTART_NEVER, "CLAWT_RESTART_NEVER", "never" },
            { CLAWT_RESTART_ON_FAILURE, "CLAWT_RESTART_ON_FAILURE", "on-failure" },
            { CLAWT_RESTART_ALWAYS, "CLAWT_RESTART_ALWAYS", "always" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtRestartPolicy", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/*
 * The order a client offers them in, with what each one gives away.
 *
 * Least capable first, so the list reads as an escalation rather than
 * as an arbitrary set -- somebody scrolling it should be able to stop
 * at the first entry that is enough.
 *
 * distrobox sits between the container and the VM by that reading and
 * *above* the container by blast radius, which is why its label says so
 * rather than calling it a container and leaving somebody to find out.
 */
static const struct {
    ClawtComputerType type;
    const gchar      *label;
} clawt_computer_type_order[] = {
    { CLAWT_COMPUTER_NONE, "None \xe2\x80\x94 chat only" },
    { CLAWT_COMPUTER_CONTAINER, "Container \xe2\x80\x94 isolated from the host" },
    { CLAWT_COMPUTER_DISTROBOX,
      "Distrobox \xe2\x80\x94 a container wired into your session" },
    { CLAWT_COMPUTER_VM, "Virtual machine \xe2\x80\x94 its own kernel" },
    { CLAWT_COMPUTER_SSH, "SSH \xe2\x80\x94 another machine you already run" },
    { CLAWT_COMPUTER_HOST, "Host \xe2\x80\x94 this machine itself" }
};

guint
clawt_computer_type_count(void)
{
    return G_N_ELEMENTS(clawt_computer_type_order);
}

ClawtComputerType
clawt_computer_type_nth(guint n)
{
    if (n >= G_N_ELEMENTS(clawt_computer_type_order))
        return CLAWT_COMPUTER_NONE;

    return clawt_computer_type_order[n].type;
}

const gchar *
clawt_computer_type_nth_nick(guint n)
{
    return clawt_enum_to_nick(CLAWT_TYPE_COMPUTER_TYPE,
                              clawt_computer_type_nth(n));
}

const gchar *
clawt_computer_type_nth_label(guint n)
{
    if (n >= G_N_ELEMENTS(clawt_computer_type_order))
        return clawt_computer_type_order[0].label;

    return clawt_computer_type_order[n].label;
}

gboolean
clawt_computer_type_takes_image(ClawtComputerType type)
{
    return type == CLAWT_COMPUTER_CONTAINER ||
           type == CLAWT_COMPUTER_DISTROBOX;
}

gboolean
clawt_computer_type_takes_mounts(ClawtComputerType type)
{
    return type == CLAWT_COMPUTER_CONTAINER ||
           type == CLAWT_COMPUTER_DISTROBOX ||
           type == CLAWT_COMPUTER_VM;
}

gboolean
clawt_computer_type_has_machine(ClawtComputerType type)
{
    /*
     * The same three as takes_mounts today, and a separate question on
     * purpose: a backend could perfectly well take shared folders
     * without being something you power on -- an ssh host would be
     * exactly that -- and one predicate answering both would be right
     * until the day it silently was not.
     *
     * ssh arrived and settled it from the other side: it is not something
     * to power on and it does not take shared folders either, so the two
     * still agree -- but they now disagree with
     * clawt_computer_type_shares_host_paths(), which is the third
     * question and the one that decides whether the workspace and the
     * exchange are placed inside.
     */
    return type == CLAWT_COMPUTER_CONTAINER ||
           type == CLAWT_COMPUTER_DISTROBOX ||
           type == CLAWT_COMPUTER_VM;
}

gboolean
clawt_computer_type_shares_host_paths(ClawtComputerType type)
{
    /*
     * A switch naming every value rather than an || chain, so -Wswitch
     * stops the next backend being added without somebody answering
     * this. Getting it wrong in the permissive direction promises an
     * agent a workspace that is not there, which it discovers a turn
     * later by reading an empty directory and drawing a conclusion.
     */
    switch (type) {
    case CLAWT_COMPUTER_HOST:
    case CLAWT_COMPUTER_CONTAINER:
    case CLAWT_COMPUTER_DISTROBOX:
    case CLAWT_COMPUTER_VM:
        return TRUE;

    case CLAWT_COMPUTER_NONE:
    case CLAWT_COMPUTER_SSH:
        return FALSE;
    }

    return FALSE;
}

gboolean
clawt_computer_type_has_screen(ClawtComputerType type)
{
    /*
     * The fourth of these predicates, and a switch for the same reason
     * the third is one: -Wswitch is what stops a backend being added
     * without somebody deciding whether it can be watched.
     *
     * `host` and `vm` are the two with a compositor clawtilla can ask
     * for a picture -- gowl or GNOME Shell on the machine the daemon
     * runs on, and the guest desktop `computer.desktop.enabled` builds
     * inside a VM.
     *
     * A container is %FALSE and that is the interesting one. It is
     * perfectly possible to install a compositor in a container, and
     * saying %TRUE here would mean every container agent offering a
     * Screen tab that never produces a frame -- which reads as clawtilla
     * being broken rather than as there being no desktop in there.
     * Whoever wants that should give the agent a VM, which is the type
     * that comes with a screen built.
     *
     * `ssh` is %FALSE because `computer.ssh.desktop` was never
     * implemented; the day it is, this is the line that changes.
     */
    switch (type) {
    case CLAWT_COMPUTER_HOST:
    case CLAWT_COMPUTER_VM:
        return TRUE;

    case CLAWT_COMPUTER_NONE:
    case CLAWT_COMPUTER_CONTAINER:
    case CLAWT_COMPUTER_DISTROBOX:
    case CLAWT_COMPUTER_SSH:
        return FALSE;
    }

    return FALSE;
}

/* Register ClawtComputerType as a GLib enum type */
GType
clawt_computer_type_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_COMPUTER_NONE, "CLAWT_COMPUTER_NONE", "none" },
            { CLAWT_COMPUTER_HOST, "CLAWT_COMPUTER_HOST", "host" },
            { CLAWT_COMPUTER_CONTAINER, "CLAWT_COMPUTER_CONTAINER", "container" },
            { CLAWT_COMPUTER_VM, "CLAWT_COMPUTER_VM", "vm" },
            { CLAWT_COMPUTER_DISTROBOX, "CLAWT_COMPUTER_DISTROBOX", "distrobox" },
            { CLAWT_COMPUTER_SSH, "CLAWT_COMPUTER_SSH", "ssh" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtComputerType", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtComputerState as a GLib enum type */
GType
clawt_computer_state_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_COMPUTER_STATE_ABSENT, "CLAWT_COMPUTER_STATE_ABSENT", "absent" },
            { CLAWT_COMPUTER_STATE_PROVISIONING, "CLAWT_COMPUTER_STATE_PROVISIONING", "provisioning" },
            { CLAWT_COMPUTER_STATE_STOPPED, "CLAWT_COMPUTER_STATE_STOPPED", "stopped" },
            { CLAWT_COMPUTER_STATE_STARTING, "CLAWT_COMPUTER_STATE_STARTING", "starting" },
            { CLAWT_COMPUTER_STATE_RUNNING, "CLAWT_COMPUTER_STATE_RUNNING", "running" },
            { CLAWT_COMPUTER_STATE_STOPPING, "CLAWT_COMPUTER_STATE_STOPPING", "stopping" },
            { CLAWT_COMPUTER_STATE_ERROR, "CLAWT_COMPUTER_STATE_ERROR", "error" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtComputerState", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtVmBackend as a GLib enum type */
GType
clawt_vm_backend_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_VM_BACKEND_LIBVIRT, "CLAWT_VM_BACKEND_LIBVIRT", "libvirt" },
            { CLAWT_VM_BACKEND_QEMU, "CLAWT_VM_BACKEND_QEMU", "qemu" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtVmBackend", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtConfineMode as a GLib enum type */
GType
clawt_confine_mode_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_CONFINE_NONE, "CLAWT_CONFINE_NONE", "none" },
            { CLAWT_CONFINE_WORKSPACE, "CLAWT_CONFINE_WORKSPACE", "workspace" },
            { CLAWT_CONFINE_ALLOWLIST, "CLAWT_CONFINE_ALLOWLIST", "allowlist" },
            { CLAWT_CONFINE_BWRAP, "CLAWT_CONFINE_BWRAP", "bwrap" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtConfineMode", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtDesktopBackend as a GLib enum type */
GType
clawt_desktop_backend_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_DESKTOP_BACKEND_AUTO, "CLAWT_DESKTOP_BACKEND_AUTO", "auto" },
            { CLAWT_DESKTOP_BACKEND_GOWL, "CLAWT_DESKTOP_BACKEND_GOWL", "gowl" },
            { CLAWT_DESKTOP_BACKEND_GNOME, "CLAWT_DESKTOP_BACKEND_GNOME", "gnome" },
            { CLAWT_DESKTOP_BACKEND_GUEST, "CLAWT_DESKTOP_BACKEND_GUEST", "guest" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtDesktopBackend", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/*
 * The Computer page's sub-views, in the order both clients show them.
 *
 * Shell first because it is the one every computer has -- a container
 * with no desktop still runs commands -- and Screen second because it is
 * the reason somebody opened the page when there is one.
 */
static const struct {
    ClawtComputerView  view;
    const gchar       *nick;
    const gchar       *label;
} clawt_computer_view_order[] = {
    { CLAWT_COMPUTER_VIEW_SHELL, "shell", "Shell" },
    { CLAWT_COMPUTER_VIEW_SCREEN, "screen", "Screen" },
    { CLAWT_COMPUTER_VIEW_MOUNTS, "mounts", "Mounts" },
    { CLAWT_COMPUTER_VIEW_EXCHANGE, "exchange", "Exchange" }
};

guint
clawt_computer_view_count(void)
{
    return G_N_ELEMENTS(clawt_computer_view_order);
}

ClawtComputerView
clawt_computer_view_nth(guint n)
{
    if (n >= G_N_ELEMENTS(clawt_computer_view_order))
        return CLAWT_COMPUTER_VIEW_SHELL;

    return clawt_computer_view_order[n].view;
}

const gchar *
clawt_computer_view_nth_nick(guint n)
{
    if (n >= G_N_ELEMENTS(clawt_computer_view_order))
        return clawt_computer_view_order[0].nick;

    return clawt_computer_view_order[n].nick;
}

const gchar *
clawt_computer_view_nth_label(guint n)
{
    if (n >= G_N_ELEMENTS(clawt_computer_view_order))
        return clawt_computer_view_order[0].label;

    return clawt_computer_view_order[n].label;
}

ClawtComputerView
clawt_computer_view_from_nick(const gchar *nick)
{
    gsize i;

    if (nick == NULL)
        return CLAWT_COMPUTER_VIEW_SHELL;

    for (i = 0; i < G_N_ELEMENTS(clawt_computer_view_order); i++) {
        if (g_strcmp0(nick, clawt_computer_view_order[i].nick) == 0)
            return clawt_computer_view_order[i].view;
    }

    return CLAWT_COMPUTER_VIEW_SHELL;
}

/* Register ClawtInputKind as a GLib enum type */
GType
clawt_input_kind_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_INPUT_KEY, "CLAWT_INPUT_KEY", "key" },
            { CLAWT_INPUT_TEXT, "CLAWT_INPUT_TEXT", "text" },
            { CLAWT_INPUT_CLICK, "CLAWT_INPUT_CLICK", "click" },
            { CLAWT_INPUT_MOVE, "CLAWT_INPUT_MOVE", "move" },
            { CLAWT_INPUT_SCROLL, "CLAWT_INPUT_SCROLL", "scroll" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtInputKind", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtMountType as a GLib enum type */
GType
clawt_mount_type_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_MOUNT_BIND, "CLAWT_MOUNT_BIND", "bind" },
            { CLAWT_MOUNT_VOLUME, "CLAWT_MOUNT_VOLUME", "volume" },
            { CLAWT_MOUNT_VIRTIOFS, "CLAWT_MOUNT_VIRTIOFS", "virtiofs" },
            { CLAWT_MOUNT_9P, "CLAWT_MOUNT_9P", "9p" },
            { CLAWT_MOUNT_TMPFS, "CLAWT_MOUNT_TMPFS", "tmpfs" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtMountType", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtMountMode as a GLib enum type */
GType
clawt_mount_mode_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_MOUNT_MODE_RO, "CLAWT_MOUNT_MODE_RO", "ro" },
            { CLAWT_MOUNT_MODE_RW, "CLAWT_MOUNT_MODE_RW", "rw" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtMountMode", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtRelabel as a GLib enum type */
GType
clawt_relabel_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_RELABEL_NONE, "CLAWT_RELABEL_NONE", "none" },
            { CLAWT_RELABEL_SHARED, "CLAWT_RELABEL_SHARED", "shared" },
            { CLAWT_RELABEL_PRIVATE, "CLAWT_RELABEL_PRIVATE", "private" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtRelabel", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/*
 * The relabel settings, in the order a person is offered them.
 *
 * Walked by both clients rather than spelled out in each.  The web
 * client had its own copy of these three nicks and the GTK client had
 * no control at all, so the one setting that decides whether podman
 * will start a container was reachable from one client, from the YAML,
 * and from nowhere else.
 *
 * Least invasive first: `private` rewrites the host directory's labels
 * and breaks whatever else was using it, so it is not the one to land
 * on by accident.
 */
static const struct {
    ClawtRelabel  relabel;
    const gchar  *nick;
    const gchar  *label;
} relabels[] = {
    { CLAWT_RELABEL_NONE,    "none",    "Leave labels alone" },
    { CLAWT_RELABEL_SHARED,  "shared",  "Relabel, shareable (:z)" },
    { CLAWT_RELABEL_PRIVATE, "private", "Relabel, this container only (:Z)" }
};

guint
clawt_relabel_count(void)
{
    return G_N_ELEMENTS(relabels);
}

ClawtRelabel
clawt_relabel_nth(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(relabels), CLAWT_RELABEL_NONE);

    return relabels[n].relabel;
}

const gchar *
clawt_relabel_nth_nick(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(relabels), "none");

    return relabels[n].nick;
}

const gchar *
clawt_relabel_nth_label(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(relabels), "Leave labels alone");

    return relabels[n].label;
}

/* Register ClawtMailboxState as a GLib enum type */
GType
clawt_mailbox_state_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_MAILBOX_PENDING, "CLAWT_MAILBOX_PENDING", "pending" },
            { CLAWT_MAILBOX_LEASED, "CLAWT_MAILBOX_LEASED", "leased" },
            { CLAWT_MAILBOX_DELIVERED, "CLAWT_MAILBOX_DELIVERED", "delivered" },
            { CLAWT_MAILBOX_ACKED, "CLAWT_MAILBOX_ACKED", "acked" },
            { CLAWT_MAILBOX_FAILED, "CLAWT_MAILBOX_FAILED", "failed" },
            { CLAWT_MAILBOX_DEAD, "CLAWT_MAILBOX_DEAD", "dead" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtMailboxState", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtPriority as a GLib enum type */
GType
clawt_priority_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_PRIORITY_LOW, "CLAWT_PRIORITY_LOW", "low" },
            { CLAWT_PRIORITY_NORMAL, "CLAWT_PRIORITY_NORMAL", "normal" },
            { CLAWT_PRIORITY_HIGH, "CLAWT_PRIORITY_HIGH", "high" },
            { CLAWT_PRIORITY_URGENT, "CLAWT_PRIORITY_URGENT", "urgent" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtPriority", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtOverflowPolicy as a GLib enum type */
GType
clawt_overflow_policy_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_OVERFLOW_REJECT, "CLAWT_OVERFLOW_REJECT", "reject" },
            { CLAWT_OVERFLOW_DROP_OLDEST, "CLAWT_OVERFLOW_DROP_OLDEST", "drop-oldest" },
            { CLAWT_OVERFLOW_BLOCK_SENDER, "CLAWT_OVERFLOW_BLOCK_SENDER", "block-sender" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtOverflowPolicy", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtTaskState as a GLib enum type */
GType
clawt_task_state_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_TASK_PENDING, "CLAWT_TASK_PENDING", "pending" },
            { CLAWT_TASK_RUNNING, "CLAWT_TASK_RUNNING", "running" },
            { CLAWT_TASK_COMPLETED, "CLAWT_TASK_COMPLETED", "completed" },
            { CLAWT_TASK_FAILED, "CLAWT_TASK_FAILED", "failed" },
            { CLAWT_TASK_CANCELLED, "CLAWT_TASK_CANCELLED", "cancelled" },
            { CLAWT_TASK_STALLED, "CLAWT_TASK_STALLED", "stalled" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtTaskState", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtStallReason as a GLib enum type */
GType
clawt_stall_reason_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_STALL_NONE, "CLAWT_STALL_NONE", "none" },
            { CLAWT_STALL_REPEATED_MESSAGE, "CLAWT_STALL_REPEATED_MESSAGE",
              "repeated-message" },
            { CLAWT_STALL_TURN_TIMEOUT, "CLAWT_STALL_TURN_TIMEOUT",
              "turn-timeout" },
            { CLAWT_STALL_ROOM_TIMEOUT, "CLAWT_STALL_ROOM_TIMEOUT",
              "room-timeout" },
            { CLAWT_STALL_REPEATED_TOOL_CALL,
              "CLAWT_STALL_REPEATED_TOOL_CALL", "repeated-tool-call" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtStallReason", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/*
 * Register ClawtHandoffState as a GLib enum type.
 *
 * The nicks are what a receipt reads as in a tool's answer, so they are
 * the words an agent sees: `busy-gave-up` says nobody was free, which is
 * a different instruction from `failed`.
 */
GType
clawt_handoff_state_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_HANDOFF_QUEUED, "CLAWT_HANDOFF_QUEUED", "queued" },
            { CLAWT_HANDOFF_DONE, "CLAWT_HANDOFF_DONE", "done" },
            { CLAWT_HANDOFF_FAILED, "CLAWT_HANDOFF_FAILED", "failed" },
            { CLAWT_HANDOFF_DENIED, "CLAWT_HANDOFF_DENIED", "denied" },
            { CLAWT_HANDOFF_BUSY_GAVE_UP, "CLAWT_HANDOFF_BUSY_GAVE_UP",
              "busy-gave-up" },
            { CLAWT_HANDOFF_DROPPED, "CLAWT_HANDOFF_DROPPED", "dropped" },
            { CLAWT_HANDOFF_ERROR, "CLAWT_HANDOFF_ERROR", "error" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtHandoffState", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtSecretBackend as a GLib enum type */
GType
clawt_secret_backend_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_SECRET_BACKEND_FILE, "CLAWT_SECRET_BACKEND_FILE", "file" },
            { CLAWT_SECRET_BACKEND_ENV, "CLAWT_SECRET_BACKEND_ENV", "env" },
            { CLAWT_SECRET_BACKEND_COMMAND, "CLAWT_SECRET_BACKEND_COMMAND", "command" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtSecretBackend", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtLogLevel as a GLib enum type */
GType
clawt_log_level_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_LOG_ERROR, "CLAWT_LOG_ERROR", "error" },
            { CLAWT_LOG_WARNING, "CLAWT_LOG_WARNING", "warning" },
            { CLAWT_LOG_INFO, "CLAWT_LOG_INFO", "info" },
            { CLAWT_LOG_DEBUG, "CLAWT_LOG_DEBUG", "debug" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtLogLevel", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtScope as a GLib enum type */
GType
clawt_scope_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_SCOPE_NONE, "CLAWT_SCOPE_NONE", "none" },
            { CLAWT_SCOPE_SELECTED, "CLAWT_SCOPE_SELECTED", "selected" },
            { CLAWT_SCOPE_ALL, "CLAWT_SCOPE_ALL", "all" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtScope", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtIntegrationKind as a GLib enum type */
GType
clawt_integration_kind_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_INTEGRATION_KIND_CHANNEL, "CLAWT_INTEGRATION_KIND_CHANNEL", "channel" },
            { CLAWT_INTEGRATION_KIND_TOOLS, "CLAWT_INTEGRATION_KIND_TOOLS", "tools" },
            { CLAWT_INTEGRATION_KIND_NOTIFY, "CLAWT_INTEGRATION_KIND_NOTIFY", "notify" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtIntegrationKind", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtNotifyBackend as a GLib enum type */
GType
clawt_notify_backend_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_NOTIFY_BACKEND_DESKTOP, "CLAWT_NOTIFY_BACKEND_DESKTOP", "desktop" },
            { CLAWT_NOTIFY_BACKEND_NTFY, "CLAWT_NOTIFY_BACKEND_NTFY", "ntfy" },
            { CLAWT_NOTIFY_BACKEND_GOTIFY, "CLAWT_NOTIFY_BACKEND_GOTIFY", "gotify" },
            { CLAWT_NOTIFY_BACKEND_MATRIX, "CLAWT_NOTIFY_BACKEND_MATRIX", "matrix" },
            { CLAWT_NOTIFY_BACKEND_COMMAND, "CLAWT_NOTIFY_BACKEND_COMMAND", "command" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtNotifyBackend", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtNotifyEvents as a GLib flags type */
GType
clawt_notify_events_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GFlagsValue values[] = {
            { CLAWT_NOTIFY_EVENTS_QUESTION, "CLAWT_NOTIFY_EVENTS_QUESTION", "question" },
            { CLAWT_NOTIFY_EVENTS_DONE, "CLAWT_NOTIFY_EVENTS_DONE", "done" },
            { CLAWT_NOTIFY_EVENTS_ERROR, "CLAWT_NOTIFY_EVENTS_ERROR", "error" },
            { CLAWT_NOTIFY_EVENTS_ROUTINE, "CLAWT_NOTIFY_EVENTS_ROUTINE", "routine" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_flags_register_static("ClawtNotifyEvents", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtSchedule as a GLib enum type */
GType
clawt_schedule_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_SCHEDULE_MANUAL, "CLAWT_SCHEDULE_MANUAL", "manual" },
            { CLAWT_SCHEDULE_HOURLY, "CLAWT_SCHEDULE_HOURLY", "hourly" },
            { CLAWT_SCHEDULE_DAILY, "CLAWT_SCHEDULE_DAILY", "daily" },
            { CLAWT_SCHEDULE_WEEKDAYS, "CLAWT_SCHEDULE_WEEKDAYS", "weekdays" },
            { CLAWT_SCHEDULE_WEEKLY, "CLAWT_SCHEDULE_WEEKLY", "weekly" },
            { CLAWT_SCHEDULE_CUSTOM, "CLAWT_SCHEDULE_CUSTOM", "custom" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtSchedule", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtRunState as a GLib enum type */
GType
clawt_run_state_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_RUN_NEVER, "CLAWT_RUN_NEVER", "never" },
            { CLAWT_RUN_OK, "CLAWT_RUN_OK", "ok" },
            { CLAWT_RUN_FAILED, "CLAWT_RUN_FAILED", "failed" },
            { CLAWT_RUN_MISSED, "CLAWT_RUN_MISSED", "missed" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtRunState", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}


/* Register ClawtGuestFlavour as a GLib enum type */
GType
clawt_guest_flavour_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_GUEST_FLAVOUR_AUTO, "CLAWT_GUEST_FLAVOUR_AUTO", "auto" },
            { CLAWT_GUEST_FLAVOUR_FEDORA, "CLAWT_GUEST_FLAVOUR_FEDORA",
              "fedora" },
            { CLAWT_GUEST_FLAVOUR_ENTERPRISE,
              "CLAWT_GUEST_FLAVOUR_ENTERPRISE", "enterprise" },
            { CLAWT_GUEST_FLAVOUR_DEBIAN, "CLAWT_GUEST_FLAVOUR_DEBIAN",
              "debian" },
            { CLAWT_GUEST_FLAVOUR_UBUNTU, "CLAWT_GUEST_FLAVOUR_UBUNTU",
              "ubuntu" },
            { CLAWT_GUEST_FLAVOUR_ARCH, "CLAWT_GUEST_FLAVOUR_ARCH",
              "arch" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id =
            g_enum_register_static("ClawtGuestFlavour", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtConnectorAuth as a GLib enum type */
GType
clawt_connector_auth_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_CONNECTOR_AUTH_NONE, "CLAWT_CONNECTOR_AUTH_NONE", "none" },
            { CLAWT_CONNECTOR_AUTH_DEVICE, "CLAWT_CONNECTOR_AUTH_DEVICE", "device" },
            { CLAWT_CONNECTOR_AUTH_PKCE, "CLAWT_CONNECTOR_AUTH_PKCE", "pkce" },
            { CLAWT_CONNECTOR_AUTH_API_KEY, "CLAWT_CONNECTOR_AUTH_API_KEY", "api_key" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id =
            g_enum_register_static("ClawtConnectorAuth", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtCredentialPlacement as a GLib enum type */
GType
clawt_credential_placement_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_CREDENTIAL_PLACEMENT_ENV,
              "CLAWT_CREDENTIAL_PLACEMENT_ENV", "env" },
            { CLAWT_CREDENTIAL_PLACEMENT_HEADER,
              "CLAWT_CREDENTIAL_PLACEMENT_HEADER", "header" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id =
            g_enum_register_static("ClawtCredentialPlacement", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* ── Nickname helpers ────────────────────────────────────────────── */

/*
 * Config files and wire frames spell enum values as nicknames, never as
 * integers: an integer in a YAML file is unreadable, and worse, it silently
 * keeps meaning something after the enum is reordered.
 */
const gchar *
clawt_enum_to_nick(GType enum_type, gint value)
{
    g_autoptr(GEnumClass) klass = NULL;
    GEnumValue *ev;

    g_return_val_if_fail(G_TYPE_IS_ENUM(enum_type), NULL);

    klass = g_type_class_ref(enum_type);
    ev = g_enum_get_value(klass, value);

    return (ev != NULL) ? ev->value_nick : NULL;
}

gboolean
clawt_enum_from_nick(GType enum_type, const gchar *nick, gint *out_value)
{
    g_autoptr(GEnumClass) klass = NULL;
    guint i;

    g_return_val_if_fail(G_TYPE_IS_ENUM(enum_type), FALSE);
    g_return_val_if_fail(out_value != NULL, FALSE);

    if (nick == NULL || *nick == '\0')
        return FALSE;

    klass = g_type_class_ref(enum_type);

    /*
     * The comparison is ours rather than g_enum_get_value_by_nick()'s on
     * purpose.  That function is case-insensitive in GLib 2.88 and was not
     * always, so leaning on it would make whether `confine: BWRAP` parses
     * depend on which GLib the user happens to be running -- a config file
     * that works on one machine and is rejected on another.
     *
     * The policy, matching libreclaw's bridge codec: lenient on input,
     * strict on output.  Any case is accepted here because these values are
     * typed by hand into YAML; clawt_enum_to_nick() always emits the
     * canonical lowercase spelling, so anything we write back is exact.
     *
     * Leniency stops at case.  An unrecognised nickname still fails, which
     * is the part that matters: a value that silently fell through to zero
     * would turn `confine: bwarp` into `confine: none` -- a typo quietly
     * removing a sandbox.
     */
    for (i = 0; i < klass->n_values; i++) {
        const GEnumValue *ev = &klass->values[i];

        if (ev->value_nick != NULL &&
            g_ascii_strcasecmp(nick, ev->value_nick) == 0) {
            *out_value = ev->value;
            return TRUE;
        }
    }

    /*
     * Fall back to the full C identifier, so a value pasted out of a header
     * or a log line still parses instead of being rejected as unknown.
     */
    for (i = 0; i < klass->n_values; i++) {
        const GEnumValue *ev = &klass->values[i];

        if (ev->value_name != NULL &&
            g_ascii_strcasecmp(nick, ev->value_name) == 0) {
            *out_value = ev->value;
            return TRUE;
        }
    }

    return FALSE;
}

gchar *
clawt_enum_nick_list(GType enum_type)
{
    g_autoptr(GEnumClass) klass = NULL;
    GString *out;
    guint named = 0;
    guint written = 0;
    guint i;

    g_return_val_if_fail(G_TYPE_IS_ENUM(enum_type), NULL);

    klass = g_type_class_ref(enum_type);

    /*
     * Counted first rather than joined as we go, so "and" lands before
     * the last nickname that is actually written.  Deciding it from the
     * loop index instead would leave a trailing " and " on any enum
     * whose last value has no nickname -- the kind of thing nobody
     * notices until one grows an odd member.
     */
    for (i = 0; i < klass->n_values; i++) {
        if (klass->values[i].value_nick != NULL)
            named++;
    }

    out = g_string_new(NULL);

    for (i = 0; i < klass->n_values; i++) {
        if (klass->values[i].value_nick == NULL)
            continue;

        /*
         * "a, b and c" rather than a comma-separated list, because this
         * goes into a sentence somebody reads when their config has
         * just been refused, not into a log line.
         */
        if (written > 0)
            g_string_append(out, (written + 1 == named) ? " and " : ", ");

        g_string_append(out, klass->values[i].value_nick);
        written++;
    }

    return g_string_free(out, FALSE);
}

gboolean
clawt_flags_from_nick(GType flags_type, const gchar *nick, guint *out_value)
{
    g_autoptr(GFlagsClass) klass = NULL;
    guint i;

    g_return_val_if_fail(G_TYPE_IS_FLAGS(flags_type), FALSE);
    g_return_val_if_fail(out_value != NULL, FALSE);

    if (nick == NULL)
        return FALSE;

    klass = g_type_class_ref(flags_type);

    /*
     * Compared here rather than through g_flags_get_value_by_nick(),
     * for the same reason clawt_enum_from_nick() does its own: whether
     * a config file parses must not depend on which GLib is installed.
     */
    for (i = 0; i < klass->n_values; i++) {
        if (g_ascii_strcasecmp(klass->values[i].value_nick, nick) != 0)
            continue;

        *out_value = klass->values[i].value;
        return TRUE;
    }

    return FALSE;
}

gchar *
clawt_flags_to_string(GType flags_type, guint value)
{
    g_autoptr(GFlagsClass) klass = NULL;
    GString *out;
    guint i;

    g_return_val_if_fail(G_TYPE_IS_FLAGS(flags_type), NULL);

    klass = g_type_class_ref(flags_type);
    out = g_string_new(NULL);

    for (i = 0; i < klass->n_values; i++) {
        const GFlagsValue *fv = &klass->values[i];

        if (fv->value == 0)
            continue;
        if ((value & fv->value) != fv->value)
            continue;

        if (out->len > 0)
            g_string_append_c(out, '|');
        g_string_append(out, fv->value_nick);
    }

    /*
     * "none" rather than "": an empty capability set is a real answer and
     * should read like one in a log line, not like a formatting bug.
     */
    if (out->len == 0)
        g_string_append(out, "none");

    return g_string_free(out, FALSE);
}

gboolean
clawt_scope_covers(ClawtScope           scope,
                   const gchar * const *agents,
                   const gchar * const *teams,
                   const gchar         *agent_id,
                   const gchar         *team)
{
    guint i;

    if (agent_id == NULL)
        return FALSE;

    switch (scope) {
    case CLAWT_SCOPE_ALL:
        return TRUE;

    case CLAWT_SCOPE_NONE:
        return FALSE;

    case CLAWT_SCOPE_SELECTED:
    default:
        break;
    }

    for (i = 0; agents != NULL && agents[i] != NULL; i++) {
        if (g_strcmp0(agents[i], agent_id) == 0)
            return TRUE;
    }

    /*
     * A teamless agent matches no team rather than matching an entry
     * spelled "". An agent taken off a team has `team: ""` while one
     * that never had a team has no key at all, and both must miss --
     * the two spellings of absent are already recorded in this tree as
     * having cost a wrong sidebar in both clients.
     */
    if (team == NULL || *team == '\0')
        return FALSE;

    for (i = 0; teams != NULL && teams[i] != NULL; i++) {
        if (g_strcmp0(teams[i], team) == 0)
            return TRUE;
    }

    return FALSE;
}

/* Register ClawtMemoryScope as a GLib enum type */
GType
clawt_memory_scope_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_MEMORY_SCOPE_AGENT, "CLAWT_MEMORY_SCOPE_AGENT", "agent" },
            { CLAWT_MEMORY_SCOPE_TEAM, "CLAWT_MEMORY_SCOPE_TEAM", "team" },
            { CLAWT_MEMORY_SCOPE_FLEET, "CLAWT_MEMORY_SCOPE_FLEET", "fleet" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtMemoryScope", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/*
 * The scopes, narrowest first, so a list of them reads as a widening.
 *
 * Walked rather than written down: a client that spelled these itself
 * would be a second copy of the set, and every hand-written copy in this
 * tree has drifted from the one the library keeps.
 */
static const struct {
    ClawtMemoryScope  scope;
    const gchar      *nick;
    const gchar      *label;
} memory_scopes[] = {
    { CLAWT_MEMORY_SCOPE_AGENT, "agent", "This agent only" },
    { CLAWT_MEMORY_SCOPE_TEAM,  "team",  "Its team" },
    { CLAWT_MEMORY_SCOPE_FLEET, "fleet", "The whole fleet" }
};

guint
clawt_memory_scope_count(void)
{
    return G_N_ELEMENTS(memory_scopes);
}

ClawtMemoryScope
clawt_memory_scope_nth(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(memory_scopes),
                         CLAWT_MEMORY_SCOPE_AGENT);

    return memory_scopes[n].scope;
}

const gchar *
clawt_memory_scope_nth_nick(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(memory_scopes), "agent");

    return memory_scopes[n].nick;
}

const gchar *
clawt_memory_scope_nth_label(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(memory_scopes), "This agent only");

    return memory_scopes[n].label;
}

/* Register ClawtSkillSource as a GLib enum type */
GType
clawt_skill_source_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_SKILL_SOURCE_USER, "CLAWT_SKILL_SOURCE_USER", "user" },
            { CLAWT_SKILL_SOURCE_IMPORTED, "CLAWT_SKILL_SOURCE_IMPORTED",
              "imported" },
            { CLAWT_SKILL_SOURCE_TAUGHT, "CLAWT_SKILL_SOURCE_TAUGHT",
              "taught" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id =
            g_enum_register_static("ClawtSkillSource", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtTeachSource as a GLib enum type */
GType
clawt_teach_source_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_TEACH_SOURCE_AGENT, "CLAWT_TEACH_SOURCE_AGENT",
              "agent" },
            { CLAWT_TEACH_SOURCE_HOST_DEMO, "CLAWT_TEACH_SOURCE_HOST_DEMO",
              "host-demo" },
            { CLAWT_TEACH_SOURCE_GUEST_DEMO, "CLAWT_TEACH_SOURCE_GUEST_DEMO",
              "guest-demo" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id =
            g_enum_register_static("ClawtTeachSource", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/*
 * The recorders, least invasive first.
 *
 * Order is the order a client offers them in, and it is deliberate: an
 * agent trace watches a program, and the two demonstrations watch a
 * person.  Listing the keylogger first would be offering it first.
 */
static const struct {
    ClawtTeachSource  source;
    const gchar      *nick;
    const gchar      *label;
} teach_sources[] = {
    { CLAWT_TEACH_SOURCE_AGENT,      "agent",      "Watch the agent work" },
    { CLAWT_TEACH_SOURCE_HOST_DEMO,  "host-demo",  "Demonstrate on this "
                                                   "desktop" },
    { CLAWT_TEACH_SOURCE_GUEST_DEMO, "guest-demo", "Demonstrate in the "
                                                   "agent's VM" }
};

guint
clawt_teach_source_count(void)
{
    return G_N_ELEMENTS(teach_sources);
}

ClawtTeachSource
clawt_teach_source_nth(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(teach_sources),
                         CLAWT_TEACH_SOURCE_AGENT);

    return teach_sources[n].source;
}

const gchar *
clawt_teach_source_nth_nick(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(teach_sources), "agent");

    return teach_sources[n].nick;
}

const gchar *
clawt_teach_source_nth_label(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(teach_sources),
                         "Watch the agent work");

    return teach_sources[n].label;
}

/* Register ClawtTeachStepKind as a GLib enum type */
GType
clawt_teach_step_kind_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_TEACH_STEP_TOOL, "CLAWT_TEACH_STEP_TOOL", "tool" },
            { CLAWT_TEACH_STEP_EXEC, "CLAWT_TEACH_STEP_EXEC", "exec" },
            { CLAWT_TEACH_STEP_DESKTOP, "CLAWT_TEACH_STEP_DESKTOP",
              "desktop" },
            { CLAWT_TEACH_STEP_KEY, "CLAWT_TEACH_STEP_KEY", "key" },
            { CLAWT_TEACH_STEP_POINTER, "CLAWT_TEACH_STEP_POINTER",
              "pointer" },
            { CLAWT_TEACH_STEP_SCROLL, "CLAWT_TEACH_STEP_SCROLL", "scroll" },
            { CLAWT_TEACH_STEP_MARKER, "CLAWT_TEACH_STEP_MARKER", "marker" },
            { CLAWT_TEACH_STEP_NOTE, "CLAWT_TEACH_STEP_NOTE", "note" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id =
            g_enum_register_static("ClawtTeachStepKind", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/* Register ClawtTriggerProvider as a GLib enum type */
GType
clawt_trigger_provider_get_type(void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter(&g_define_type_id__volatile)) {
        static const GEnumValue values[] = {
            { CLAWT_TRIGGER_PROVIDER_GENERIC, "CLAWT_TRIGGER_PROVIDER_GENERIC", "generic" },
            { CLAWT_TRIGGER_PROVIDER_FORGEJO, "CLAWT_TRIGGER_PROVIDER_FORGEJO", "forgejo" },
            { CLAWT_TRIGGER_PROVIDER_GITEA, "CLAWT_TRIGGER_PROVIDER_GITEA", "gitea" },
            { CLAWT_TRIGGER_PROVIDER_GITHUB, "CLAWT_TRIGGER_PROVIDER_GITHUB", "github" },
            { CLAWT_TRIGGER_PROVIDER_GITLAB, "CLAWT_TRIGGER_PROVIDER_GITLAB", "gitlab" },
            { 0, NULL, NULL }
        };
        GType g_define_type_id =
            g_enum_register_static("ClawtTriggerProvider", values);
        g_once_init_leave(&g_define_type_id__volatile, g_define_type_id);
    }
    return g_define_type_id__volatile;
}

/*
 * Generic first, because it is the one that needs no forge at all and is
 * the right answer for anything that can set a header.
 */
static const struct {
    ClawtTriggerProvider  provider;
    const gchar          *nick;
    const gchar          *label;
} trigger_providers[] = {
    { CLAWT_TRIGGER_PROVIDER_GENERIC, "generic", "Anything (bearer token)" },
    { CLAWT_TRIGGER_PROVIDER_FORGEJO, "forgejo", "Forgejo" },
    { CLAWT_TRIGGER_PROVIDER_GITEA,   "gitea",   "Gitea" },
    { CLAWT_TRIGGER_PROVIDER_GITHUB,  "github",  "GitHub" },
    { CLAWT_TRIGGER_PROVIDER_GITLAB,  "gitlab",  "GitLab" }
};

guint
clawt_trigger_provider_count(void)
{
    return G_N_ELEMENTS(trigger_providers);
}

ClawtTriggerProvider
clawt_trigger_provider_nth(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(trigger_providers),
                         CLAWT_TRIGGER_PROVIDER_GENERIC);

    return trigger_providers[n].provider;
}

const gchar *
clawt_trigger_provider_nth_nick(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(trigger_providers), "generic");

    return trigger_providers[n].nick;
}

const gchar *
clawt_trigger_provider_nth_label(guint n)
{
    g_return_val_if_fail(n < G_N_ELEMENTS(trigger_providers),
                         "Anything (bearer token)");

    return trigger_providers[n].label;
}

/* ── The pages, and the groups they are drawn in ─────────────────── */

/*
 * The six groups, in the order the switcher shows them.
 *
 * Chat first because it is what the window opens on.  Agent and Computer
 * next, being about the one agent selected in the sidebar; then
 * Automation and Work, which are about what it is doing; then Library,
 * which is about the fleet and is the one somebody reaches for least
 * often.
 */
static const struct {
    ClawtSection  section;
    const gchar  *nick;
    const gchar  *label;
} clawt_section_order[] = {
    { CLAWT_SECTION_CHAT,       "chat",       "Chat" },
    { CLAWT_SECTION_AGENT,      "agent",      "Agent" },
    { CLAWT_SECTION_COMPUTER,   "computer",   "Computer" },
    { CLAWT_SECTION_AUTOMATION, "automation", "Automation" },
    { CLAWT_SECTION_WORK,       "work",       "Work" },
    { CLAWT_SECTION_LIBRARY,    "library",    "Library" }
};

/*
 * Every page, grouped.
 *
 * Ordered by section and then within it, which is what lets
 * clawt_section_page_nth() walk a run of this table rather than filter
 * it -- and what makes a page that has been moved to another section a
 * visible edit here rather than a silent reordering somewhere else.
 *
 * The nickname is the URL and the widget name at once.  Both clients
 * already had these spellings; they are unchanged so that a link
 * somebody bookmarked still lands where it did.
 *
 * There is deliberately no icon column.  The web client draws none and
 * the GTK client picks one from a `switch` with no `default:`, so a
 * section added here fails to compile there until somebody chooses --
 * which is better than a GNOME icon name sitting in a library that must
 * never link GTK.
 */
static const struct {
    ClawtPage     page;
    ClawtSection  section;
    const gchar  *nick;
    const gchar  *label;
} clawt_page_order[] = {
    { CLAWT_PAGE_CHAT,      CLAWT_SECTION_CHAT,       "chat",      "Chat" },
    { CLAWT_PAGE_AGENT,     CLAWT_SECTION_AGENT,      "agent",     "Overview" },
    { CLAWT_PAGE_MAILBOX,   CLAWT_SECTION_AGENT,      "mailbox",   "Mailbox" },
    { CLAWT_PAGE_COMPUTER,  CLAWT_SECTION_COMPUTER,   "computer",  "Computer" },
    { CLAWT_PAGE_ROUTINES,  CLAWT_SECTION_AUTOMATION, "routines",  "Routines" },
    { CLAWT_PAGE_TRIGGERS,  CLAWT_SECTION_AUTOMATION, "triggers",  "Triggers" },
    { CLAWT_PAGE_TASKS,     CLAWT_SECTION_WORK,       "tasks",     "Tasks" },
    { CLAWT_PAGE_DECISIONS, CLAWT_SECTION_WORK,       "decisions", "Decisions" },
    { CLAWT_PAGE_FLOW,      CLAWT_SECTION_WORK,       "flow",      "Flow" },
    { CLAWT_PAGE_SKILLS,    CLAWT_SECTION_LIBRARY,    "skills",    "Skills" },
    { CLAWT_PAGE_MEMORY,    CLAWT_SECTION_LIBRARY,    "memory",    "Memory" }
};

guint
clawt_section_count(void)
{
    return G_N_ELEMENTS(clawt_section_order);
}

ClawtSection
clawt_section_nth(guint n)
{
    if (n >= G_N_ELEMENTS(clawt_section_order))
        return CLAWT_SECTION_CHAT;

    return clawt_section_order[n].section;
}

const gchar *
clawt_section_nth_nick(guint n)
{
    if (n >= G_N_ELEMENTS(clawt_section_order))
        return clawt_section_order[0].nick;

    return clawt_section_order[n].nick;
}

const gchar *
clawt_section_nth_label(guint n)
{
    if (n >= G_N_ELEMENTS(clawt_section_order))
        return clawt_section_order[0].label;

    return clawt_section_order[n].label;
}

static gssize
clawt_section_index(ClawtSection section)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(clawt_section_order); i++) {
        if (clawt_section_order[i].section == section)
            return (gssize)i;
    }

    return -1;
}

const gchar *
clawt_section_nick(ClawtSection section)
{
    gssize i = clawt_section_index(section);

    return (i < 0) ? clawt_section_order[0].nick
                   : clawt_section_order[i].nick;
}

const gchar *
clawt_section_label(ClawtSection section)
{
    gssize i = clawt_section_index(section);

    return (i < 0) ? clawt_section_order[0].label
                   : clawt_section_order[i].label;
}

ClawtSection
clawt_section_from_nick(const gchar *nick)
{
    gsize i;

    if (nick == NULL)
        return CLAWT_SECTION_CHAT;

    for (i = 0; i < G_N_ELEMENTS(clawt_section_order); i++) {
        if (g_strcmp0(nick, clawt_section_order[i].nick) == 0)
            return clawt_section_order[i].section;
    }

    return CLAWT_SECTION_CHAT;
}

guint
clawt_section_page_count(ClawtSection section)
{
    guint count = 0;
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(clawt_page_order); i++) {
        if (clawt_page_order[i].section == section)
            count++;
    }

    return count;
}

ClawtPage
clawt_section_page_nth(ClawtSection section, guint n)
{
    guint seen = 0;
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(clawt_page_order); i++) {
        if (clawt_page_order[i].section != section)
            continue;

        if (seen == n)
            return clawt_page_order[i].page;

        seen++;
    }

    return CLAWT_PAGE_CHAT;
}

ClawtPage
clawt_section_default_page(ClawtSection section)
{
    return clawt_section_page_nth(section, 0);
}

guint
clawt_page_count(void)
{
    return G_N_ELEMENTS(clawt_page_order);
}

/*
 * Found by walking rather than by indexing with the enumeration value.
 *
 * The two agree -- tests/test-sections.c asserts it, which is what lets
 * a client size an array by clawt_page_count() and index it by the value
 * it is holding -- but a lookup that assumes it would answer for the
 * wrong page the first time somebody reorders the table, and answer
 * confidently.
 */
static gssize
clawt_page_index(ClawtPage page)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(clawt_page_order); i++) {
        if (clawt_page_order[i].page == page)
            return (gssize)i;
    }

    return -1;
}

const gchar *
clawt_page_nick(ClawtPage page)
{
    gssize i = clawt_page_index(page);

    return (i < 0) ? clawt_page_order[0].nick : clawt_page_order[i].nick;
}

const gchar *
clawt_page_label(ClawtPage page)
{
    gssize i = clawt_page_index(page);

    return (i < 0) ? clawt_page_order[0].label : clawt_page_order[i].label;
}

ClawtSection
clawt_page_section(ClawtPage page)
{
    gssize i = clawt_page_index(page);

    return (i < 0) ? CLAWT_SECTION_CHAT : clawt_page_order[i].section;
}

ClawtPage
clawt_page_from_nick(const gchar *nick)
{
    gsize i;

    if (nick == NULL)
        return CLAWT_PAGE_CHAT;

    for (i = 0; i < G_N_ELEMENTS(clawt_page_order); i++) {
        if (g_strcmp0(nick, clawt_page_order[i].nick) == 0)
            return clawt_page_order[i].page;
    }

    return CLAWT_PAGE_CHAT;
}
