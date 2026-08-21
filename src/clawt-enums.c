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
            { CLAWT_AGENT_CAPS_IMAGES, "CLAWT_AGENT_CAPS_IMAGES", "images" },
            { CLAWT_AGENT_CAPS_ATTACHMENTS, "CLAWT_AGENT_CAPS_ATTACHMENTS", "attachments" },
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
