/*
 * clawt-enums.h - Enumerations and flags for clawtilla
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every enum here is registered as a GType so it can be a GObject property,
 * appear in introspection, and round-trip through YAML by nickname rather
 * than by integer.  The nicknames are the spelling used in config files and
 * on the wire, so renaming one is a breaking change.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * ClawtAgentState:
 * @CLAWT_AGENT_STATE_STOPPED: configured but not running
 * @CLAWT_AGENT_STATE_STARTING: runtime is coming up, link not yet established
 * @CLAWT_AGENT_STATE_RUNNING: running and linked
 * @CLAWT_AGENT_STATE_DEGRADED: running but its link or computer is unhealthy
 * @CLAWT_AGENT_STATE_STOPPING: shutting down
 * @CLAWT_AGENT_STATE_ERROR: failed to start, or died and will not be restarted
 * @CLAWT_AGENT_STATE_SHADOW: its configuration could not be understood
 *
 * Lifecycle state of a #ClawtAgent.
 *
 * %CLAWT_AGENT_STATE_SHADOW is the forward-compatibility state: an agent
 * whose block names an unknown computer type, integration or mount type --
 * or which fails validation for any other reason -- becomes a shadow rather
 * than aborting daemon startup.  It is listed, it explains itself, and it
 * refuses to run.  This is what lets a config written by a newer build
 * round-trip through an older one without data loss.
 */
typedef enum {
    CLAWT_AGENT_STATE_STOPPED = 0,
    CLAWT_AGENT_STATE_STARTING,
    CLAWT_AGENT_STATE_RUNNING,
    CLAWT_AGENT_STATE_DEGRADED,
    CLAWT_AGENT_STATE_STOPPING,
    CLAWT_AGENT_STATE_ERROR,
    CLAWT_AGENT_STATE_SHADOW
} ClawtAgentState;

/**
 * ClawtAgentCaps:
 * @CLAWT_AGENT_CAPS_NONE: no capabilities
 * @CLAWT_AGENT_CAPS_TOOLS_MCP: can be served the clawtilla_* orchestration tools
 * @CLAWT_AGENT_CAPS_COMPUTER: has a computer it can run commands on
 * @CLAWT_AGENT_CAPS_HOST_CONTROL: its computer is the real host
 * @CLAWT_AGENT_CAPS_DESKTOP: can drive a desktop session
 * @CLAWT_AGENT_CAPS_DESKTOP_INPUT: may inject keys and pointer events, not just observe
 * @CLAWT_AGENT_CAPS_MOUNTS: has host paths mounted into its computer
 * @CLAWT_AGENT_CAPS_IMAGES: its model accepts image input
 * @CLAWT_AGENT_CAPS_ATTACHMENTS: its channels carry attachments
 * @CLAWT_AGENT_CAPS_STREAMING: emits token deltas rather than whole replies
 * @CLAWT_AGENT_CAPS_EFFORT_LEVELS: its model exposes an effort control
 * @CLAWT_AGENT_CAPS_PEER_COMMS: may message other agents
 * @CLAWT_AGENT_CAPS_INTERRUPT: a running turn can be interrupted
 *
 * What an agent can actually do, derived from its runtime, computer and
 * model rather than from what its config asked for.
 *
 * These exist so no interface offers a control the agent cannot honour, and
 * so the agent is never told it has a tool that is not really there -- an
 * agent that believes it has a computer it cannot reach burns whole turns
 * hunting for it.
 */
typedef enum {
    CLAWT_AGENT_CAPS_NONE           = 0,
    CLAWT_AGENT_CAPS_TOOLS_MCP      = 1 << 0,
    CLAWT_AGENT_CAPS_COMPUTER       = 1 << 1,
    CLAWT_AGENT_CAPS_HOST_CONTROL   = 1 << 2,
    CLAWT_AGENT_CAPS_DESKTOP        = 1 << 3,
    CLAWT_AGENT_CAPS_DESKTOP_INPUT  = 1 << 4,
    CLAWT_AGENT_CAPS_MOUNTS         = 1 << 5,
    CLAWT_AGENT_CAPS_IMAGES         = 1 << 6,
    CLAWT_AGENT_CAPS_ATTACHMENTS    = 1 << 7,
    CLAWT_AGENT_CAPS_STREAMING      = 1 << 8,
    CLAWT_AGENT_CAPS_EFFORT_LEVELS  = 1 << 9,
    CLAWT_AGENT_CAPS_PEER_COMMS     = 1 << 10,
    CLAWT_AGENT_CAPS_INTERRUPT      = 1 << 11
} ClawtAgentCaps;

/**
 * ClawtRuntimeType:
 * @CLAWT_RUNTIME_PROCESS: supervise a libreclaw child process
 * @CLAWT_RUNTIME_EMBEDDED: run an LcApp inside the daemon process
 *
 * How an agent's libreclaw instance is hosted.
 *
 * Process is the default: it isolates crashes, keeps per-agent environments
 * and credentials genuinely separate, and lets the agent live inside its own
 * container.  Embedded is cheaper and is what an in-process host such as
 * cmacs wants, at the cost of sharing a fate with every other embedded agent.
 */
typedef enum {
    CLAWT_RUNTIME_PROCESS = 0,
    CLAWT_RUNTIME_EMBEDDED
} ClawtRuntimeType;

/**
 * ClawtRestartPolicy:
 * @CLAWT_RESTART_NEVER: leave it stopped
 * @CLAWT_RESTART_ON_FAILURE: restart only on a non-zero exit or a signal
 * @CLAWT_RESTART_ALWAYS: restart even after a clean exit
 *
 * What to do when an agent's runtime exits.
 */
typedef enum {
    CLAWT_RESTART_NEVER = 0,
    CLAWT_RESTART_ON_FAILURE,
    CLAWT_RESTART_ALWAYS
} ClawtRestartPolicy;

/**
 * ClawtComputerType:
 * @CLAWT_COMPUTER_NONE: chat only, no command execution
 * @CLAWT_COMPUTER_HOST: the real machine the daemon runs on
 * @CLAWT_COMPUTER_CONTAINER: a podman container
 * @CLAWT_COMPUTER_VM: a virtual machine
 *
 * The kind of computer an agent has.
 *
 * Desktop control is not a type: it is an add-on configured under
 * `computer.desktop` and available alongside any of these.
 */
typedef enum {
    CLAWT_COMPUTER_NONE = 0,
    CLAWT_COMPUTER_HOST,
    CLAWT_COMPUTER_CONTAINER,
    CLAWT_COMPUTER_VM
} ClawtComputerType;

/**
 * ClawtComputerState:
 * @CLAWT_COMPUTER_STATE_ABSENT: nothing provisioned
 * @CLAWT_COMPUTER_STATE_PROVISIONING: being created
 * @CLAWT_COMPUTER_STATE_STOPPED: exists but is not running
 * @CLAWT_COMPUTER_STATE_STARTING: coming up
 * @CLAWT_COMPUTER_STATE_RUNNING: ready for commands
 * @CLAWT_COMPUTER_STATE_STOPPING: shutting down
 * @CLAWT_COMPUTER_STATE_ERROR: unusable; see the last error
 *
 * Lifecycle state of a #ClawtComputer.
 */
typedef enum {
    CLAWT_COMPUTER_STATE_ABSENT = 0,
    CLAWT_COMPUTER_STATE_PROVISIONING,
    CLAWT_COMPUTER_STATE_STOPPED,
    CLAWT_COMPUTER_STATE_STARTING,
    CLAWT_COMPUTER_STATE_RUNNING,
    CLAWT_COMPUTER_STATE_STOPPING,
    CLAWT_COMPUTER_STATE_ERROR
} ClawtComputerState;

/**
 * ClawtVmBackend:
 * @CLAWT_VM_BACKEND_LIBVIRT: podomation's vm_virtmanager module
 * @CLAWT_VM_BACKEND_QEMU: qemu-system-* driven directly over QMP
 *
 * Which implementation backs a VM computer.
 *
 * libvirt is the default and gets snapshots, migration and device hotplug
 * for free.  The QEMU backend exists for hosts with no libvirtd, and trades
 * that surface for having no daemon to install.
 */
typedef enum {
    CLAWT_VM_BACKEND_LIBVIRT = 0,
    CLAWT_VM_BACKEND_QEMU
} ClawtVmBackend;

/**
 * ClawtConfineMode:
 * @CLAWT_CONFINE_NONE: no restriction at all
 * @CLAWT_CONFINE_WORKSPACE: cwd pinned to the agent's root, paths canonicalised
 * @CLAWT_CONFINE_ALLOWLIST: the root plus allow_paths, minus deny_paths
 * @CLAWT_CONFINE_BWRAP: a kernel sandbox built with bubblewrap
 *
 * How much of the host an agent with a host computer may touch.
 *
 * %CLAWT_CONFINE_WORKSPACE and %CLAWT_CONFINE_ALLOWLIST are enforced by
 * canonicalising every path in the command line before running it, which is
 * what closes `..` and symlink escapes together.  They do not stop a program
 * that opens paths itself once it is running -- only %CLAWT_CONFINE_BWRAP
 * does that, because only it involves the kernel.
 *
 * %CLAWT_CONFINE_NONE additionally requires daemon.allow_unconfined_host, so
 * that handing an agent the whole machine takes two deliberate acts rather
 * than one typo.
 */
typedef enum {
    CLAWT_CONFINE_NONE = 0,
    CLAWT_CONFINE_WORKSPACE,
    CLAWT_CONFINE_ALLOWLIST,
    CLAWT_CONFINE_BWRAP
} ClawtConfineMode;

/**
 * ClawtDesktopBackend:
 * @CLAWT_DESKTOP_BACKEND_AUTO: the agent's own VM, else gowl, else GNOME
 * @CLAWT_DESKTOP_BACKEND_GOWL: gowl's MCP socket
 * @CLAWT_DESKTOP_BACKEND_GNOME: the gnome-desktop-mcp stdio server
 * @CLAWT_DESKTOP_BACKEND_GUEST: gnome-desktop-mcp inside the agent's VM
 *
 * Which desktop-control implementation to use.
 *
 * The first three drive the screen clawtilla itself is running on.  The
 * fourth drives a desktop installed inside the agent's own VM, which is
 * the only one where the agent clicking something does not click it on
 * somebody's real screen.
 */
typedef enum {
    CLAWT_DESKTOP_BACKEND_AUTO = 0,
    CLAWT_DESKTOP_BACKEND_GOWL,
    CLAWT_DESKTOP_BACKEND_GNOME,
    CLAWT_DESKTOP_BACKEND_GUEST
} ClawtDesktopBackend;

/**
 * ClawtMountType:
 * @CLAWT_MOUNT_BIND: a host directory or file bound into the computer
 * @CLAWT_MOUNT_VOLUME: a named podman volume
 * @CLAWT_MOUNT_VIRTIOFS: a host directory shared into a VM over virtiofs
 * @CLAWT_MOUNT_9P: a host directory shared into a VM over 9p
 * @CLAWT_MOUNT_TMPFS: an empty in-memory filesystem
 *
 * The kind of mount.  Not every type is valid for every computer backend;
 * #ClawtMountPlan rejects the invalid combinations rather than letting the
 * backend fail later with a less helpful message.
 */
typedef enum {
    CLAWT_MOUNT_BIND = 0,
    CLAWT_MOUNT_VOLUME,
    CLAWT_MOUNT_VIRTIOFS,
    CLAWT_MOUNT_9P,
    CLAWT_MOUNT_TMPFS
} ClawtMountType;

/**
 * ClawtMountMode:
 * @CLAWT_MOUNT_MODE_RO: read-only
 * @CLAWT_MOUNT_MODE_RW: read-write
 *
 * Access mode for a mount.
 */
typedef enum {
    CLAWT_MOUNT_MODE_RO = 0,
    CLAWT_MOUNT_MODE_RW
} ClawtMountMode;

/**
 * ClawtRelabel:
 * @CLAWT_RELABEL_NONE: do not touch SELinux labels
 * @CLAWT_RELABEL_SHARED: relabel so several containers may share it (:z)
 * @CLAWT_RELABEL_PRIVATE: relabel exclusively for this container (:Z)
 *
 * SELinux relabelling for a bind mount.
 *
 * This matters on Fedora Silverblue and friends, where an unlabelled bind
 * mount is visible in the container but every access is denied -- a failure
 * that reads like a permissions bug rather than a labelling one.
 * %CLAWT_RELABEL_PRIVATE rewrites the host directory's labels, so pointing
 * it at a directory something else also uses will break that other user.
 */
typedef enum {
    CLAWT_RELABEL_NONE = 0,
    CLAWT_RELABEL_SHARED,
    CLAWT_RELABEL_PRIVATE
} ClawtRelabel;

/**
 * ClawtMailboxState:
 * @CLAWT_MAILBOX_PENDING: waiting to be delivered
 * @CLAWT_MAILBOX_LEASED: handed to an agent, awaiting acknowledgement
 * @CLAWT_MAILBOX_DELIVERED: the agent received it
 * @CLAWT_MAILBOX_ACKED: the agent finished with it
 * @CLAWT_MAILBOX_FAILED: delivery failed and it will be retried
 * @CLAWT_MAILBOX_DEAD: retries exhausted; parked in the dead-letter view
 *
 * State of a #ClawtMailboxItem.
 *
 * The lease is what makes delivery survive an agent dying mid-turn: the item
 * returns to %CLAWT_MAILBOX_PENDING when the lease expires rather than being
 * lost or silently delivered twice.
 */
typedef enum {
    CLAWT_MAILBOX_PENDING = 0,
    CLAWT_MAILBOX_LEASED,
    CLAWT_MAILBOX_DELIVERED,
    CLAWT_MAILBOX_ACKED,
    CLAWT_MAILBOX_FAILED,
    CLAWT_MAILBOX_DEAD
} ClawtMailboxState;

/**
 * ClawtPriority:
 * @CLAWT_PRIORITY_LOW: drained after everything else
 * @CLAWT_PRIORITY_NORMAL: the default
 * @CLAWT_PRIORITY_HIGH: ahead of normal traffic
 * @CLAWT_PRIORITY_URGENT: ahead of everything
 *
 * Delivery priority for a mailbox item.  Ordering is FIFO within a band;
 * a higher band always drains first, so a busy agent still sees an urgent
 * message promptly.
 */
typedef enum {
    CLAWT_PRIORITY_LOW = 0,
    CLAWT_PRIORITY_NORMAL,
    CLAWT_PRIORITY_HIGH,
    CLAWT_PRIORITY_URGENT
} ClawtPriority;

/**
 * ClawtOverflowPolicy:
 * @CLAWT_OVERFLOW_REJECT: refuse the post and tell the sender
 * @CLAWT_OVERFLOW_DROP_OLDEST: evict the oldest low-priority item
 * @CLAWT_OVERFLOW_BLOCK_SENDER: rate-limit the sender instead of dropping
 *
 * What a mailbox does when it reaches max_depth.
 *
 * All three are noisy on purpose.  A queue that silently discards is a queue
 * that loses work nobody notices until much later.
 */
typedef enum {
    CLAWT_OVERFLOW_REJECT = 0,
    CLAWT_OVERFLOW_DROP_OLDEST,
    CLAWT_OVERFLOW_BLOCK_SENDER
} ClawtOverflowPolicy;

/**
 * ClawtTaskState:
 * @CLAWT_TASK_PENDING: created, not yet picked up
 * @CLAWT_TASK_RUNNING: the assignee is working on it
 * @CLAWT_TASK_COMPLETED: finished with a result
 * @CLAWT_TASK_FAILED: finished with an error
 * @CLAWT_TASK_CANCELLED: cancelled before finishing
 *
 * State of a delegated #ClawtTask.
 */
typedef enum {
    CLAWT_TASK_PENDING = 0,
    CLAWT_TASK_RUNNING,
    CLAWT_TASK_COMPLETED,
    CLAWT_TASK_FAILED,
    CLAWT_TASK_CANCELLED
} ClawtTaskState;

/**
 * ClawtLogLevel:
 * @CLAWT_LOG_ERROR: only failures
 * @CLAWT_LOG_WARNING: failures and things that look wrong
 * @CLAWT_LOG_INFO: the default: lifecycle and routing decisions
 * @CLAWT_LOG_DEBUG: every frame on every link
 *
 * How much the daemon says.
 *
 * %CLAWT_LOG_DEBUG is what you want when an agent will not connect, and
 * noise the rest of the time -- it logs the contents of every link frame.
 */
typedef enum {
    CLAWT_LOG_ERROR = 0,
    CLAWT_LOG_WARNING,
    CLAWT_LOG_INFO,
    CLAWT_LOG_DEBUG
} ClawtLogLevel;

/**
 * ClawtSecretBackend:
 * @CLAWT_SECRET_BACKEND_FILE: read the value from a file
 * @CLAWT_SECRET_BACKEND_ENV: read the value from the daemon's environment
 * @CLAWT_SECRET_BACKEND_COMMAND: run a command and take its stdout
 *
 * How a secret reference in the config is resolved.  There is deliberately
 * no "inline" backend: a literal secret in the config file is the thing
 * these exist to avoid.
 */
typedef enum {
    CLAWT_SECRET_BACKEND_FILE = 0,
    CLAWT_SECRET_BACKEND_ENV,
    CLAWT_SECRET_BACKEND_COMMAND
} ClawtSecretBackend;

/* GType registration */
GType clawt_agent_state_get_type(void) G_GNUC_CONST;
GType clawt_agent_caps_get_type(void) G_GNUC_CONST;
GType clawt_runtime_type_get_type(void) G_GNUC_CONST;
GType clawt_restart_policy_get_type(void) G_GNUC_CONST;
GType clawt_computer_type_get_type(void) G_GNUC_CONST;
GType clawt_computer_state_get_type(void) G_GNUC_CONST;
GType clawt_vm_backend_get_type(void) G_GNUC_CONST;
GType clawt_confine_mode_get_type(void) G_GNUC_CONST;
GType clawt_desktop_backend_get_type(void) G_GNUC_CONST;
GType clawt_mount_type_get_type(void) G_GNUC_CONST;
GType clawt_mount_mode_get_type(void) G_GNUC_CONST;
GType clawt_relabel_get_type(void) G_GNUC_CONST;
GType clawt_mailbox_state_get_type(void) G_GNUC_CONST;
GType clawt_priority_get_type(void) G_GNUC_CONST;
GType clawt_overflow_policy_get_type(void) G_GNUC_CONST;
GType clawt_task_state_get_type(void) G_GNUC_CONST;
GType clawt_secret_backend_get_type(void) G_GNUC_CONST;
GType clawt_log_level_get_type(void) G_GNUC_CONST;

#define CLAWT_TYPE_AGENT_STATE      (clawt_agent_state_get_type())
#define CLAWT_TYPE_AGENT_CAPS       (clawt_agent_caps_get_type())
#define CLAWT_TYPE_RUNTIME_TYPE     (clawt_runtime_type_get_type())
#define CLAWT_TYPE_RESTART_POLICY   (clawt_restart_policy_get_type())
#define CLAWT_TYPE_COMPUTER_TYPE    (clawt_computer_type_get_type())
#define CLAWT_TYPE_COMPUTER_STATE   (clawt_computer_state_get_type())
#define CLAWT_TYPE_VM_BACKEND       (clawt_vm_backend_get_type())
#define CLAWT_TYPE_CONFINE_MODE     (clawt_confine_mode_get_type())
#define CLAWT_TYPE_DESKTOP_BACKEND  (clawt_desktop_backend_get_type())
#define CLAWT_TYPE_MOUNT_TYPE       (clawt_mount_type_get_type())
#define CLAWT_TYPE_MOUNT_MODE       (clawt_mount_mode_get_type())
#define CLAWT_TYPE_RELABEL          (clawt_relabel_get_type())
#define CLAWT_TYPE_MAILBOX_STATE    (clawt_mailbox_state_get_type())
#define CLAWT_TYPE_PRIORITY         (clawt_priority_get_type())
#define CLAWT_TYPE_OVERFLOW_POLICY  (clawt_overflow_policy_get_type())
#define CLAWT_TYPE_TASK_STATE       (clawt_task_state_get_type())
#define CLAWT_TYPE_SECRET_BACKEND   (clawt_secret_backend_get_type())
#define CLAWT_TYPE_LOG_LEVEL        (clawt_log_level_get_type())

/**
 * clawt_enum_to_nick:
 * @enum_type: a registered enum #GType
 * @value: the value to name
 *
 * Returns the registered nickname for @value -- the spelling used in config
 * files and on the wire.
 *
 * Returns: (transfer none) (nullable): the nickname, or %NULL if @value is
 *   not a member of @enum_type
 */
const gchar *clawt_enum_to_nick(GType enum_type, gint value);

/**
 * clawt_enum_from_nick:
 * @enum_type: a registered enum #GType
 * @nick: a nickname to look up
 * @out_value: (out): return location for the value
 *
 * Looks up an enum value by nickname.  Used when reading config and wire
 * frames, so that a typo produces a clear "unknown value" rather than
 * silently selecting whatever happens to be zero.
 *
 * Returns: %TRUE if @nick names a member of @enum_type
 */
gboolean clawt_enum_from_nick(GType enum_type, const gchar *nick, gint *out_value);

/**
 * clawt_flags_to_string:
 * @flags_type: a registered flags #GType
 * @value: the flags to describe
 *
 * Formats a flags value as a pipe-separated list of nicknames, for logs and
 * for the wire.  An empty value formats as "none" rather than "".
 *
 * Returns: (transfer full): a newly allocated string
 */
gchar *clawt_flags_to_string(GType flags_type, guint value);

G_END_DECLS
