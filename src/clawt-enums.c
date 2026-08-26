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
            { 0, NULL, NULL }
        };
        GType g_define_type_id = g_enum_register_static("ClawtTaskState", values);
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
