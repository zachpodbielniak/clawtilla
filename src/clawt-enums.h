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
 *
 * Which makes a flag nothing derives the worst kind of entry here: it is
 * always false, so every control bound to it is permanently insensitive
 * for the whole fleet.  `images` and `attachments` were exactly that --
 * registered, documented nowhere but here, never once set -- and neither
 * could be derived from anything this build knows.  Nothing records
 * which models accept image input, and every agent can already send its
 * operator a file, so "attachments" only ever restated
 * %CLAWT_AGENT_CAPS_TOOLS_MCP.  Bits 6 and 7 are left unused rather than
 * reassigned, so the flags below them keep the values a caps string has
 * always meant.
 */
typedef enum {
    CLAWT_AGENT_CAPS_NONE           = 0,
    CLAWT_AGENT_CAPS_TOOLS_MCP      = 1 << 0,
    CLAWT_AGENT_CAPS_COMPUTER       = 1 << 1,
    CLAWT_AGENT_CAPS_HOST_CONTROL   = 1 << 2,
    CLAWT_AGENT_CAPS_DESKTOP        = 1 << 3,
    CLAWT_AGENT_CAPS_DESKTOP_INPUT  = 1 << 4,
    CLAWT_AGENT_CAPS_MOUNTS         = 1 << 5,
    /* 1 << 6 and 1 << 7 were `images` and `attachments`; see above. */
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
 * ClawtTeamRole:
 * @CLAWT_TEAM_MEMBER: talks to anyone, assigns to nobody
 * @CLAWT_TEAM_LEAD: may hand work to the members of its own team
 *
 * An agent's standing within its team.
 *
 * The distinction is about *assigning*, not about talking. A member
 * messages, asks and shares rooms with anybody in the fleet -- handing
 * something over in conversation is not the same as putting it on
 * somebody's list -- and cannot delegate. A lead can, within its own
 * team and nowhere else.
 */
typedef enum {
    CLAWT_TEAM_MEMBER = 0,
    CLAWT_TEAM_LEAD
} ClawtTeamRole;

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
 * @CLAWT_COMPUTER_DISTROBOX: a distrobox container, wired into the host
 * @CLAWT_COMPUTER_VM: a virtual machine
 *
 * The kind of computer an agent has.
 *
 * Desktop control is not a type: it is an add-on configured under
 * `computer.desktop` and available alongside any of these.
 *
 * %CLAWT_COMPUTER_DISTROBOX sits between the container and the host
 * rather than beside the container: a distrobox *is* a podman
 * container, but one deliberately wired into the machine around it --
 * the same uid, the host's sockets, its binaries reachable through
 * distrobox-host-exec. That is what makes it a comfortable place to
 * build things and a poor place to put something that should be
 * contained, and it is why it is its own type rather than a flag on
 * the container one. A reader scanning `computer.type` should see the
 * difference.
 *
 * Appended rather than inserted: the value is written into
 * clawtilla.yaml as a nick, but a config read by an older build that
 * has never heard of it becomes a shadow agent with a reason, and
 * renumbering the others would change what an existing integer means.
 */
typedef enum {
    CLAWT_COMPUTER_NONE = 0,
    CLAWT_COMPUTER_HOST,
    CLAWT_COMPUTER_CONTAINER,
    CLAWT_COMPUTER_VM,
    CLAWT_COMPUTER_DISTROBOX
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
 * @CLAWT_TASK_STALLED: stopped because it was going in circles
 *
 * State of a delegated #ClawtTask.
 *
 * CLAWT_TASK_STALLED is terminal like the three before it, and is its own
 * answer rather than a flavour of failed: work that a limit stopped is
 * work somebody can pick up and finish, where failed work needs
 * diagnosing first.  Reading a stall as a failure sends the reader to
 * look for a bug that is not there.
 */
typedef enum {
    CLAWT_TASK_PENDING = 0,
    CLAWT_TASK_RUNNING,
    CLAWT_TASK_COMPLETED,
    CLAWT_TASK_FAILED,
    CLAWT_TASK_CANCELLED,
    CLAWT_TASK_STALLED
} ClawtTaskState;

/**
 * ClawtStallReason:
 * @CLAWT_STALL_NONE: nothing has stalled
 * @CLAWT_STALL_REPEATED_MESSAGE: the same message went round again
 * @CLAWT_STALL_TURN_TIMEOUT: a turn produced nothing for its whole budget
 * @CLAWT_STALL_ROOM_TIMEOUT: a member held a room's turn for its whole budget
 * @CLAWT_STALL_REPEATED_TOOL_CALL: one turn made the same call over and over
 *
 * Why an exchange or a turn was ended.
 *
 * The four are worth telling apart because two of them clawtilla can
 * genuinely stop and two it can only observe.  #CLAWT_STALL_REPEATED_MESSAGE,
 * #CLAWT_STALL_TURN_TIMEOUT and #CLAWT_STALL_ROOM_TIMEOUT are about things
 * the daemon owns -- the mailbox router, the runtime, the room -- so the
 * exchange really ends.  #CLAWT_STALL_REPEATED_TOOL_CALL is about the loop
 * inside the model's own CLI, which nothing on this side of the socket
 * steers: it is reported, and escalated to an interrupt, and that is the
 * whole of its reach.
 *
 * There is deliberately no _count()/_nth() family: a reason is reported,
 * never chosen from a list, and a chooser nothing calls is the shape this
 * codebase keeps finding at the bottom of its bugs.
 */
typedef enum {
    CLAWT_STALL_NONE = 0,
    CLAWT_STALL_REPEATED_MESSAGE,
    CLAWT_STALL_TURN_TIMEOUT,
    CLAWT_STALL_ROOM_TIMEOUT,
    CLAWT_STALL_REPEATED_TOOL_CALL
} ClawtStallReason;

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

/**
 * ClawtScope:
 * @CLAWT_SCOPE_NONE: nobody; kept in the file but handed to no agent
 * @CLAWT_SCOPE_SELECTED: only the agents named in `agents:`
 * @CLAWT_SCOPE_ALL: every agent in the fleet, including ones added later
 *
 * Which agents an integration instance reaches.
 *
 * %CLAWT_SCOPE_NONE exists so an instance can be parked
 * without being deleted.  Turning a Matrix account off for an afternoon
 * should not mean retyping its homeserver, its user id and its rooms, and
 * a credential that has to be re-entered to be re-enabled tends to end up
 * somewhere worse than the config file.
 */
typedef enum {
    CLAWT_SCOPE_NONE = 0,
    CLAWT_SCOPE_SELECTED,
    CLAWT_SCOPE_ALL
} ClawtScope;

/**
 * clawt_scope_covers:
 * @scope: a #ClawtScope
 * @agents: (nullable) (array zero-terminated=1): agent ids, for
 *   %CLAWT_SCOPE_SELECTED
 * @teams: (nullable) (array zero-terminated=1): team ids, for
 *   %CLAWT_SCOPE_SELECTED
 * @agent_id: the agent being asked about
 * @team: (nullable): the team that agent is on
 *
 * Whether something scoped this way applies to one agent.
 *
 * The one implementation of "who gets this", shared by integrations and
 * by the fleet's shared folders. They were going to be two, and two
 * would have differed exactly once -- on the case nobody wrote a test
 * for, which here is an agent named directly *and* on a listed team.
 *
 * A team is an id, matched against @team. Naming a team is how a rule
 * covers a group without being rewritten every time somebody joins it,
 * which is the whole reason teams exist.
 *
 * %CLAWT_SCOPE_SELECTED with neither list is nobody, not everybody.
 * That is the asymmetry this project already recorded for integrations:
 * a typo that hands a credential -- or a home directory -- to the whole
 * fleet is far worse than one that hands it to nothing and says so.
 *
 * Returns: %TRUE if it applies
 */
gboolean clawt_scope_covers(ClawtScope           scope,
                            const gchar * const *agents,
                            const gchar * const *teams,
                            const gchar         *agent_id,
                            const gchar         *team);

/**
 * ClawtIntegrationKind:
 * @CLAWT_INTEGRATION_KIND_CHANNEL: how people reach the agent
 * @CLAWT_INTEGRATION_KIND_TOOLS: how the agent reaches a service
 * @CLAWT_INTEGRATION_KIND_NOTIFY: how the fleet reaches the operator
 *
 * The direction an integration runs in.
 *
 * The two are configured the same way and do completely different things
 * to an agent, so they are separated here rather than in a comment.  A
 * channel arrives as a conversation and costs the agent a turn; a tools
 * integration arrives as an MCP server in its `.mcp.json` and costs it
 * nothing until it calls one.  Sharing follows from this: a channel
 * shared by two agents means both answer the same person, which is
 * almost never wanted, while a tool server shared by the whole fleet is
 * the ordinary case.
 *
 * @CLAWT_INTEGRATION_KIND_NOTIFY runs in neither direction an agent can
 * see: it is the daemon telling a person something, about an agent,
 * without the agent being involved or knowing it happened.
 */
typedef enum {
    CLAWT_INTEGRATION_KIND_CHANNEL = 0,
    CLAWT_INTEGRATION_KIND_TOOLS,
    CLAWT_INTEGRATION_KIND_NOTIFY
} ClawtIntegrationKind;

/**
 * ClawtNotifyBackend:
 * @CLAWT_NOTIFY_BACKEND_DESKTOP: a desktop notification on the machine the daemon runs on
 * @CLAWT_NOTIFY_BACKEND_NTFY: an ntfy topic
 * @CLAWT_NOTIFY_BACKEND_GOTIFY: a gotify server
 * @CLAWT_NOTIFY_BACKEND_MATRIX: a message into a Matrix room
 * @CLAWT_NOTIFY_BACKEND_COMMAND: run a program and hand it the text
 *
 * How a notification reaches the operator.
 *
 * @CLAWT_NOTIFY_BACKEND_COMMAND is the escape hatch and is meant to be
 * used: a receipt printer, a script, anything already on the machine.
 * The four named ones are there because they are what people actually
 * reach for, not because the list is meant to be closed.
 */
typedef enum {
    CLAWT_NOTIFY_BACKEND_DESKTOP = 0,
    CLAWT_NOTIFY_BACKEND_NTFY,
    CLAWT_NOTIFY_BACKEND_GOTIFY,
    CLAWT_NOTIFY_BACKEND_MATRIX,
    CLAWT_NOTIFY_BACKEND_COMMAND
} ClawtNotifyBackend;

/**
 * ClawtNotifyEvents:
 * @CLAWT_NOTIFY_EVENTS_NONE: nothing
 * @CLAWT_NOTIFY_EVENTS_QUESTION: an agent said something to the operator
 * @CLAWT_NOTIFY_EVENTS_DONE: a task finished
 * @CLAWT_NOTIFY_EVENTS_ERROR: an agent stopped in a way nobody asked for
 * @CLAWT_NOTIFY_EVENTS_ROUTINE: a routine failed to run
 *
 * What is worth interrupting somebody for.
 *
 * The default is @CLAWT_NOTIFY_EVENTS_QUESTION and
 * @CLAWT_NOTIFY_EVENTS_ERROR and nothing else, which is the whole rule:
 * an agent *blocked on you* is worth a buzz, and one that has broken is
 * worth a buzz. Everything a fleet does while it works is not -- a
 * notifier that fires on every turn is one people turn off, and then it
 * is not there for the two that mattered.
 */
typedef enum {
    CLAWT_NOTIFY_EVENTS_NONE     = 0,
    CLAWT_NOTIFY_EVENTS_QUESTION = 1 << 0,
    CLAWT_NOTIFY_EVENTS_DONE     = 1 << 1,
    CLAWT_NOTIFY_EVENTS_ERROR    = 1 << 2,
    CLAWT_NOTIFY_EVENTS_ROUTINE  = 1 << 3
} ClawtNotifyEvents;

/**
 * ClawtSchedule:
 * @CLAWT_SCHEDULE_MANUAL: only when somebody asks
 * @CLAWT_SCHEDULE_HOURLY: every hour
 * @CLAWT_SCHEDULE_DAILY: once a day at a chosen time
 * @CLAWT_SCHEDULE_WEEKDAYS: Monday to Friday at a chosen time
 * @CLAWT_SCHEDULE_WEEKLY: once a week on a chosen day
 * @CLAWT_SCHEDULE_CUSTOM: a cron expression
 *
 * How often a routine runs.
 *
 * Every one of these becomes a cron expression, including the presets:
 * "daily at 09:00" *is* `0 9 * * *`, and keeping them as separate kinds
 * of thing would mean two implementations of "when next", one of which
 * is exercised far less and is therefore the one that is wrong.
 */
typedef enum {
    CLAWT_SCHEDULE_MANUAL = 0,
    CLAWT_SCHEDULE_HOURLY,
    CLAWT_SCHEDULE_DAILY,
    CLAWT_SCHEDULE_WEEKDAYS,
    CLAWT_SCHEDULE_WEEKLY,
    CLAWT_SCHEDULE_CUSTOM
} ClawtSchedule;

/**
 * ClawtRunState:
 * @CLAWT_RUN_NEVER: it has not run yet
 * @CLAWT_RUN_OK: the last run started and finished
 * @CLAWT_RUN_FAILED: the last run could not be started
 * @CLAWT_RUN_MISSED: its time passed while the daemon was not running
 *
 * How a routine's last run went.
 *
 * @CLAWT_RUN_MISSED is separate from @CLAWT_RUN_FAILED on purpose. A
 * routine that did not run because the machine was asleep is not broken,
 * and showing it as broken would train somebody to ignore the one that
 * is.
 */
typedef enum {
    CLAWT_RUN_NEVER = 0,
    CLAWT_RUN_OK,
    CLAWT_RUN_FAILED,
    CLAWT_RUN_MISSED
} ClawtRunState;

/**
 * ClawtGuestFlavour:
 * @CLAWT_GUEST_FLAVOUR_AUTO: work it out from the image
 * @CLAWT_GUEST_FLAVOUR_FEDORA: Fedora
 * @CLAWT_GUEST_FLAVOUR_ENTERPRISE: CentOS Stream, RHEL and their rebuilds
 * @CLAWT_GUEST_FLAVOUR_DEBIAN: Debian
 * @CLAWT_GUEST_FLAVOUR_UBUNTU: Ubuntu
 * @CLAWT_GUEST_FLAVOUR_ARCH: Arch Linux
 *
 * Which family a guest belongs to, for the purpose of installing things
 * into it.
 *
 * cloud-init picks the package *manager* for you and nothing else, so a
 * seed still has to know the package *names* -- and the names are only
 * half of it.  The display manager is `gdm` on Fedora and `gdm3` on
 * Debian; PyGObject is `python3-gobject` on one and `python3-gi` on the
 * other; and a Debian cloud image has neither the `dconf` binary nor
 * `glib-compile-schemas` until something asks for them.  Get any of
 * those wrong and the guest boots, cloud-init reports success, and the
 * agent is looking at a machine with no session on it.
 *
 * Ubuntu is separate from Debian even though almost everything about
 * them is identical here, because the one thing that is not would fail
 * on both: Debian stable ships Firefox as `firefox-esr` and has no
 * `firefox` package at all, while on Ubuntu `firefox` is a transitional
 * package that installs the snap and there is no `firefox-esr`.  A
 * single family would have to pick one and be wrong on the other half
 * of it.
 *
 * There is no member for a family clawtilla has never heard of.  A guest
 * it cannot place is one where the seed says what it assumed, in a
 * warning naming the key that settles it.
 */
typedef enum {
    CLAWT_GUEST_FLAVOUR_AUTO = 0,
    CLAWT_GUEST_FLAVOUR_FEDORA,
    CLAWT_GUEST_FLAVOUR_ENTERPRISE,
    CLAWT_GUEST_FLAVOUR_DEBIAN,
    CLAWT_GUEST_FLAVOUR_UBUNTU,
    CLAWT_GUEST_FLAVOUR_ARCH
} ClawtGuestFlavour;

/**
 * ClawtConnectorAuth:
 * @CLAWT_CONNECTOR_AUTH_NONE: no credential; the server is open
 * @CLAWT_CONNECTOR_AUTH_DEVICE: OAuth 2.0 device authorization grant
 * @CLAWT_CONNECTOR_AUTH_PKCE: OAuth 2.0 authorization code with PKCE
 * @CLAWT_CONNECTOR_AUTH_API_KEY: a key the person already holds
 *
 * How a connector proves who it is.
 *
 * @CLAWT_CONNECTOR_AUTH_DEVICE is the one to prefer and the reason this
 * is feasible in a daemon at all.  It needs no redirect URI, no listening
 * socket and no browser on the same machine: the daemon asks for a code,
 * the person types it into a page on whatever device they are holding,
 * and the daemon polls until they have.  A headless workstation reached
 * over SSH can be connected from a phone.
 *
 * @CLAWT_CONNECTOR_AUTH_PKCE exists because a good many services never
 * implemented device grant.  It costs a loopback listener and a redirect
 * URI registered in advance, which is why it is second choice rather
 * than the default.
 *
 * @CLAWT_CONNECTOR_AUTH_API_KEY is not OAuth and is not pretending to
 * be, but it belongs here: the point of the broker is that the agent
 * never holds the credential, and that is worth as much for a key
 * pasted from a dashboard as for a token a flow negotiated.
 */
typedef enum {
    CLAWT_CONNECTOR_AUTH_NONE = 0,
    CLAWT_CONNECTOR_AUTH_DEVICE,
    CLAWT_CONNECTOR_AUTH_PKCE,
    CLAWT_CONNECTOR_AUTH_API_KEY
} ClawtConnectorAuth;

/**
 * ClawtCredentialPlacement:
 * @CLAWT_CREDENTIAL_PLACEMENT_ENV: an environment variable on the server process
 * @CLAWT_CREDENTIAL_PLACEMENT_HEADER: an HTTP header on every request
 *
 * Where the relay puts the credential when it starts the tool server.
 *
 * There is deliberately no query-parameter member.  A token in a URL is
 * written to every proxy log and every server access log it passes
 * through, and survives there long after the token is revoked; a
 * connector whose service only accepts that is one clawtilla declines to
 * support rather than one it supports badly.
 */
typedef enum {
    CLAWT_CREDENTIAL_PLACEMENT_ENV = 0,
    CLAWT_CREDENTIAL_PLACEMENT_HEADER
} ClawtCredentialPlacement;

/* GType registration */
GType clawt_agent_state_get_type(void) G_GNUC_CONST;
GType clawt_agent_caps_get_type(void) G_GNUC_CONST;
GType clawt_runtime_type_get_type(void) G_GNUC_CONST;
GType clawt_team_role_get_type(void) G_GNUC_CONST;
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
GType clawt_stall_reason_get_type(void) G_GNUC_CONST;
GType clawt_secret_backend_get_type(void) G_GNUC_CONST;
GType clawt_log_level_get_type(void) G_GNUC_CONST;
GType clawt_scope_get_type(void) G_GNUC_CONST;
GType clawt_integration_kind_get_type(void) G_GNUC_CONST;
GType clawt_notify_backend_get_type(void) G_GNUC_CONST;
GType clawt_notify_events_get_type(void) G_GNUC_CONST;
GType clawt_schedule_get_type(void) G_GNUC_CONST;
GType clawt_run_state_get_type(void) G_GNUC_CONST;
GType clawt_guest_flavour_get_type(void) G_GNUC_CONST;
GType clawt_connector_auth_get_type(void) G_GNUC_CONST;
GType clawt_credential_placement_get_type(void) G_GNUC_CONST;

#define CLAWT_TYPE_AGENT_STATE      (clawt_agent_state_get_type())
#define CLAWT_TYPE_AGENT_CAPS       (clawt_agent_caps_get_type())
#define CLAWT_TYPE_RUNTIME_TYPE     (clawt_runtime_type_get_type())
#define CLAWT_TYPE_TEAM_ROLE        (clawt_team_role_get_type())
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
#define CLAWT_TYPE_STALL_REASON     (clawt_stall_reason_get_type())
#define CLAWT_TYPE_SECRET_BACKEND   (clawt_secret_backend_get_type())
#define CLAWT_TYPE_LOG_LEVEL        (clawt_log_level_get_type())
#define CLAWT_TYPE_SCOPE (clawt_scope_get_type())
#define CLAWT_TYPE_INTEGRATION_KIND  (clawt_integration_kind_get_type())
#define CLAWT_TYPE_NOTIFY_BACKEND    (clawt_notify_backend_get_type())
#define CLAWT_TYPE_NOTIFY_EVENTS     (clawt_notify_events_get_type())
#define CLAWT_TYPE_SCHEDULE          (clawt_schedule_get_type())
#define CLAWT_TYPE_RUN_STATE         (clawt_run_state_get_type())
#define CLAWT_TYPE_GUEST_FLAVOUR     (clawt_guest_flavour_get_type())
#define CLAWT_TYPE_CONNECTOR_AUTH    (clawt_connector_auth_get_type())
#define CLAWT_TYPE_CREDENTIAL_PLACEMENT \
    (clawt_credential_placement_get_type())

/**
 * clawt_computer_type_count:
 *
 * How many kinds of computer an agent may be given.
 *
 * Both clients build their control by walking this rather than naming
 * the types. Five hand-written copies of that list is what existed
 * before -- four in the GTK client and one in the web one -- and it is
 * the same shape as the colour schemes, where a palette added to the
 * library was selectable in one client and absent from the other with
 * nothing to say so. A computer type sends no IPC frame of its own and
 * answers no slash command, so `make parity` could not see it either.
 *
 * Returns: the number of types
 */
guint clawt_computer_type_count(void);

/**
 * clawt_computer_type_nth:
 * @n: an index below clawt_computer_type_count()
 *
 * Returns: the type at @n, in the order a client should offer them --
 *   least capable first, so the list reads as an escalation
 */
ClawtComputerType clawt_computer_type_nth(guint n);

/**
 * clawt_computer_type_nth_nick:
 * @n: an index below clawt_computer_type_count()
 *
 * Returns: (transfer none): the stable spelling, as written to
 *   clawtilla.yaml
 */
const gchar *clawt_computer_type_nth_nick(guint n);

/**
 * clawt_computer_type_nth_label:
 * @n: an index below clawt_computer_type_count()
 *
 * What to show a person choosing one.
 *
 * Says what the type *gives away* rather than only what it is, because
 * that is the question somebody is actually answering when they pick
 * one from a list.
 *
 * Returns: (transfer none): the label, never %NULL
 */
const gchar *clawt_computer_type_nth_label(guint n);

/**
 * clawt_computer_type_takes_image:
 * @type: a #ClawtComputerType
 *
 * Whether a container image means anything for @type.
 *
 * Asked rather than branched on, because both clients decide the same
 * thing in five places between them -- whether to offer an image
 * chooser, whether to send an `image` field -- and a type added without
 * touching all five gets a form that quietly drops what somebody typed
 * into it.
 *
 * A VM is %FALSE: it takes a disk image, which is a different setting
 * with a different list behind it.
 *
 * Returns: %TRUE for the container-backed types
 */
gboolean clawt_computer_type_takes_image(ClawtComputerType type);

/**
 * clawt_computer_type_takes_mounts:
 * @type: a #ClawtComputerType
 *
 * Whether @type can be given shared folders.
 *
 * %FALSE for `none`, which has nothing to mount into, and for `host`,
 * where the mount list is the confinement allowlist rather than a set
 * of kernel mounts -- so a "Shared folders" editor there would be
 * editing something else under a name that does not fit.
 *
 * Returns: %TRUE if mounts apply
 */
gboolean clawt_computer_type_takes_mounts(ClawtComputerType type);

/**
 * clawt_computer_type_has_machine:
 * @type: a #ClawtComputerType
 *
 * Whether @type has a machine of its own that can be started and
 * stopped independently of the agent.
 *
 * Asked rather than branched on, so a backend added later reaches every
 * surface that offers those verbs -- the right-click menu, the web
 * computer page, the daemon's own refusal -- without any of them being
 * edited. A type that is offered a Stop it cannot honour is the shape
 * of bug this tree keeps finding: a control that reports success and
 * does nothing.
 *
 * %FALSE for `none`, which has no machine at all, and for `host`, whose
 * machine is the one clawtilla is running on. Stopping that is not a
 * thing to offer carefully; it is a thing not to offer.
 *
 * Returns: %TRUE if there is something to start, stop and restart
 */
gboolean clawt_computer_type_has_machine(ClawtComputerType type);

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
 * clawt_enum_nick_list:
 * @enum_type: a registered enum #GType
 *
 * Every nickname @enum_type has, as English prose: "none, host,
 * container, vm and distrobox".
 *
 * For the refusals that tell somebody what they were allowed to write.
 * Spelling that set out by hand is writing the enum a second time, and
 * the second copy stops being true silently: the unknown-computer-type
 * refusal had said "none, host, container and vm" since before
 * distrobox existed, so the one message whose entire job is to list the
 * types omitted one this build supports.
 *
 * Returns: (transfer full): a newly allocated string
 */
gchar *clawt_enum_nick_list(GType enum_type);

/**
 * clawt_flags_from_nick:
 * @flags_type: a registered flags #GType
 * @nick: one flag's nickname
 * @out_value: (out): the value
 *
 * The flags twin of clawt_enum_from_nick(), and it has to exist
 * separately: g_enum_get_value_by_nick() takes a #GEnumClass, so calling
 * it on a flags type is an assertion failure rather than a lookup that
 * returns nothing.
 *
 * That failure is quiet in the way that matters -- it left every notify
 * integration unable to parse its own event list, so nothing was ever
 * notified, while the "send a test" button worked perfectly because it
 * skips the list on purpose.
 *
 * Returns: %TRUE if @nick names a flag
 */
gboolean clawt_flags_from_nick(GType flags_type, const gchar *nick,
                               guint *out_value);

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

/**
 * ClawtMemoryScope:
 * @CLAWT_MEMORY_SCOPE_AGENT: the agent's own, and nobody else's by default
 * @CLAWT_MEMORY_SCOPE_TEAM: shared with the team the agent is on
 * @CLAWT_MEMORY_SCOPE_FLEET: shared with every agent
 *
 * Where a memory an agent forms is written.
 *
 * Reading fans out across every scope an agent is entitled to; this says
 * only where a new one lands.  Each scope is a separate database file, so
 * "may this agent read that" is answered by which file is opened rather
 * than by a condition in a query -- a permission that is structural
 * cannot be lost to a missing WHERE clause.
 */
typedef enum {
    CLAWT_MEMORY_SCOPE_AGENT = 0,
    CLAWT_MEMORY_SCOPE_TEAM,
    CLAWT_MEMORY_SCOPE_FLEET
} ClawtMemoryScope;

GType clawt_memory_scope_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_MEMORY_SCOPE (clawt_memory_scope_get_type())

/**
 * clawt_memory_scope_count:
 *
 * Returns: how many scopes there are
 */
guint clawt_memory_scope_count(void);

/**
 * clawt_memory_scope_nth:
 * @n: an index below clawt_memory_scope_count()
 *
 * Returns: the scope at @n, narrowest first
 */
ClawtMemoryScope clawt_memory_scope_nth(guint n);

/**
 * clawt_memory_scope_nth_nick:
 * @n: an index below clawt_memory_scope_count()
 *
 * Returns: (transfer none): the spelling used in clawtilla.yaml
 */
const gchar *clawt_memory_scope_nth_nick(guint n);

/**
 * clawt_memory_scope_nth_label:
 * @n: an index below clawt_memory_scope_count()
 *
 * Returns: (transfer none): the wording for a person
 */
const gchar *clawt_memory_scope_nth_label(guint n);

/**
 * ClawtTriggerProvider:
 * @CLAWT_TRIGGER_PROVIDER_GENERIC: a bearer token and nothing forge-shaped
 * @CLAWT_TRIGGER_PROVIDER_FORGEJO: Forgejo
 * @CLAWT_TRIGGER_PROVIDER_GITEA: Gitea
 * @CLAWT_TRIGGER_PROVIDER_GITHUB: GitHub
 * @CLAWT_TRIGGER_PROVIDER_GITLAB: GitLab
 *
 * Who is delivering an event, which decides how it is authenticated.
 *
 * The four forges genuinely disagree, and the disagreement is not
 * cosmetic.  Forgejo and Gitea sign the raw body with HMAC-SHA256 and
 * send the hex bare; GitHub sends the same digest behind a `sha256=`
 * prefix; GitLab signs nothing at all and sends the shared secret
 * verbatim in a header, which still has to be compared in constant time.
 *
 * Forgejo additionally sends GitHub- and Gitea-shaped headers for
 * compatibility, so sniffing cannot be the primary answer: it is a
 * fallback for a delivery nobody declared, and it may never widen what a
 * configured trigger accepts.
 */
typedef enum {
    CLAWT_TRIGGER_PROVIDER_GENERIC = 0,
    CLAWT_TRIGGER_PROVIDER_FORGEJO,
    CLAWT_TRIGGER_PROVIDER_GITEA,
    CLAWT_TRIGGER_PROVIDER_GITHUB,
    CLAWT_TRIGGER_PROVIDER_GITLAB
} ClawtTriggerProvider;

GType clawt_trigger_provider_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_TRIGGER_PROVIDER (clawt_trigger_provider_get_type())

/**
 * clawt_trigger_provider_count:
 *
 * Returns: how many providers are understood
 */
guint clawt_trigger_provider_count(void);

/**
 * clawt_trigger_provider_nth:
 * @n: an index below clawt_trigger_provider_count()
 *
 * Returns: the provider at @n
 */
ClawtTriggerProvider clawt_trigger_provider_nth(guint n);

/**
 * clawt_trigger_provider_nth_nick:
 * @n: an index below clawt_trigger_provider_count()
 *
 * Returns: (transfer none): the spelling used in clawtilla.yaml
 */
const gchar *clawt_trigger_provider_nth_nick(guint n);

/**
 * clawt_trigger_provider_nth_label:
 * @n: an index below clawt_trigger_provider_count()
 *
 * Returns: (transfer none): the wording for a person
 */
const gchar *clawt_trigger_provider_nth_label(guint n);

G_END_DECLS
