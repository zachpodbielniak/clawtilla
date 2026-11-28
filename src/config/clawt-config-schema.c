/*
 * clawt-config-schema.c - The single source of truth for configuration
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Add an option here and run `make config-files`.  Do not hand-edit the
 * generated YAML or the generated docs table; tests/test-config-schema.c
 * fails when they drift from this table.
 */

#include "clawtilla.h"
#include "config/clawt-config-schema.h"

#include <string.h>

/*
 * The table.
 *
 * Order is the order options appear in the generated file, so it is grouped
 * the way somebody reads a config rather than alphabetically.  Sections come
 * before the keys inside them.
 *
 * Documentation is written as if answering "why would I change this?".  An
 * option whose comment only restates its name is an option nobody can decide
 * about.
 */
static const ClawtSchemaEntry schema[] = {

/* ── daemon ──────────────────────────────────────────────────────── */
{ "daemon", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The daemon itself: where it listens, where it keeps state, how loud it is.\n"
  "\n"
  "clawtillad owns every agent process, every credential and every socket.\n"
  "Clients hold no transports of their own -- they send typed commands and\n"
  "fold one event stream. That is what lets the CLI, the GTK app, the web\n"
  "client and an in-process cmacs embed all be the same program.", "0.1.0" },

{ "daemon.socket", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  "$XDG_RUNTIME_DIR/clawtilla/daemon.sock", NULL,
  "Unix socket the clients connect to.\n"
  "\n"
  "Created with mode 0600 inside a 0700 directory, which is the whole of\n"
  "the access control: the daemon does not check the peer's uid, so\n"
  "anything that can open the file is trusted completely.\n"
  "\n"
  "A unix socket is used rather than a loopback HTTP port because the\n"
  "filesystem can express that at all, and because any web page the user\n"
  "visits can reach a loopback port through DNS rebinding.", "0.1.0" },

{ "daemon.git", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Make the state directory a git repository on first start.\n"
  "\n"
  "The workspaces, the org files and this configuration are worth having\n"
  "a history of. What makes this safe rather than alarming is the\n"
  ".gitignore written beside it: the same directory holds credentials,\n"
  "link tokens, mailboxes and memory databases, and the moment to keep\n"
  "those out of a history is before there is anything in it.\n"
  "\n"
  "clawtilla never commits. It creates the repository and the ignore\n"
  "rules; what goes into a commit stays your decision.\n"
  "\n"
  "Turning this off stops the repository being created. The ignore file\n"
  "is still written, because a state directory somebody put in git by\n"
  "hand needs it more, not less.", "0.1.0" },

{ "daemon.state_dir", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  "~/.clawtilla", NULL,
  "Where agent workspaces, mailboxes, transcripts and credentials live.\n"
  "\n"
  "Created 0700. Never mounted into any agent's computer, whatever the\n"
  "mount configuration says -- an agent that could read this directory\n"
  "could read every other agent's credentials.", "0.1.0" },

{ "daemon.pod_module_dir", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Where podomation's loadable modules are, if they are not found for you.\n"
  "\n"
  "The container and vm computers do their work through podomation's\n"
  "'container' and 'vm_virtmanager' modules, which libreclaw builds. They\n"
  "are looked for next to the running binary and then in the install\n"
  "location, so a normal build and a normal install both work without\n"
  "setting this.\n"
  "\n"
  "Set it when the modules live somewhere else -- a system podomation, or\n"
  "a build tree you are testing against. Naming a directory here means\n"
  "that directory and no other, so a wrong path fails loudly instead of\n"
  "being papered over by a stale set found elsewhere.", "0.1.0" },

{ "daemon.log_level", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "info", clawt_log_level_get_type,
  "How much the daemon says: error, warning, info, debug.\n"
  "\n"
  "Everything at or above the level reaches the log and nothing below.\n"
  "GLib already drops debug messages unless G_MESSAGES_DEBUG names the\n"
  "domain, so debug is what turns clawtilla\'s own on; warning and error\n"
  "are what quieten a daemon that is working.\n"
  "\n"
  "Read by clawtillad only. A host embedding ClawtDaemon owns its own\n"
  "logging, for the same reason it owns its own signal handling.\n"
  "\n"
  "It said `debug logs every frame on every link` until 0.2.0, and did\n"
  "not: nothing read this key at all, and there is no per-frame logging\n"
  "to enable. Turning it up changed nothing and warned about nothing,\n"
  "because it is not flagged inert either.", "0.1.0" },

{ "daemon.tcp_enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_COMMENTED,
  "false", NULL,
  "Also listen on TCP, so clients on other machines can connect.\n"
  "\n"
  "Off by default. The unix socket is authenticated by the kernel; a TCP\n"
  "listener is authenticated by whatever you configure below, so turning\n"
  "this on without TLS and a token puts the whole fleet on the network.", "0.1.0" },

{ "daemon.tailscale", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Also listen on this machine's tailnet address, when there is one.\n"
  "\n"
  "On by default, and the one network where that is defensible: a peer on\n"
  "a tailnet is a device you enrolled and WireGuard authenticated, and\n"
  "nothing outside it can route to a 100.64/10 address at all. It is what\n"
  "lets the client on a laptop reach the fleet on a workstation with no\n"
  "tunnel set up by hand.\n"
  "\n"
  "A token is still required, and generated into <state_dir>/tcp-token if\n"
  "token_file names nothing -- so the listener is never open, only\n"
  "reachable. Print it with `clawtilla daemon token`.\n"
  "\n"
  "Nothing happens when Tailscale is absent, down, or in\n"
  "userspace-networking mode, since there is then no address to bind.", "0.1.0" },

{ "daemon.tcp_address", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_COMMENTED,
  "127.0.0.1", NULL,
  "Address to bind when tcp_enabled is true.", "0.1.0" },

{ "daemon.tcp_port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_COMMENTED,
  "8792", NULL,
  "Port to bind for tcp_enabled and for tailscale. Both use this one.", "0.1.0" },

{ "daemon.tls_cert", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "TLS certificate for the TCP listener. Without one, remote clients talk\n"
  "in clear text and so do their bearer tokens.", "0.1.0" },

{ "daemon.tls_key", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "TLS private key matching tls_cert.", "0.1.0" },

{ "daemon.token_file", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "File holding the bearer token remote clients must present.\n"
  "\n"
  "Ignored for unix socket connections, which the kernel already vouches\n"
  "for. Left unset, a token is generated into <state_dir>/tcp-token the\n"
  "first time a TCP or tailnet listener needs one, so enabling either\n"
  "cannot leave the daemon open by omission.", "0.1.0" },

{ "daemon.device_idle_days", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "90", NULL,
  "Days a paired device may go unseen before its token stops working.\n"
  "0 means never.\n"
  "\n"
  "A device is seen when it connects and, once a minute, for as long as\n"
  "it stays connected -- so a laptop left open on the fleet never lapses\n"
  "while somebody is looking at it. An expired device is removed exactly\n"
  "as `clawtilla device revoke` would remove it, live connections and\n"
  "all; pair it again with a fresh code. The daemon's own token\n"
  "(token_file) is the owner credential and never expires.", "0.15.0" },

{ "daemon.event_log_days", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "30", NULL,
  "How many days of per-room event logs to keep. 0 disables the sweep.\n"
  "\n"
  "The event log is the file you paste into a bug report. Secrets are\n"
  "scrubbed as it is written, not as it is displayed -- a transcript gets\n"
  "replayed into every context rebuild, so a leaked key in one would be\n"
  "permanent.", "0.1.0" },

{ "daemon.routine_history_days", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "62", NULL,
  "How many days of each routine's run history to keep: what happened at\n"
  "every scheduled time -- started (and which task), failed, refused,\n"
  "missed while the daemon was down, or folded into a catch-up run.\n"
  "\n"
  "This is what lets the Schedule calendar put a tick or a cross on a\n"
  "time that has passed (routine.occurrences). It lives in the routine\n"
  "state file beside last_run, never in this config. Each routine also\n"
  "keeps at most 2,000 entries whatever this says, because the state file\n"
  "is rewritten on every run. 0 or less means the default.", "0.78.0" },

{ "daemon.calendar_max_days", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "62", NULL,
  "The longest window one routine.occurrences request may ask for, in\n"
  "days. A longer one is refused with this number in the error.\n"
  "\n"
  "62 covers a month grid with the weeks either side of it. The bound\n"
  "exists because expanding a schedule costs time per run, and a client\n"
  "asking for a year of an every-minute routine would hold the daemon's\n"
  "loop while it did. 0 or less means the default.", "0.78.0" },

{ "daemon.calendar_max_occurrences", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "5000", NULL,
  "The most occurrences one routine.occurrences request may return,\n"
  "across every routine and libreclaw cron in the window. Past it the\n"
  "request is refused, naming the routine that contributes most, rather\n"
  "than answered with a calendar that silently stops part-way through a\n"
  "week. Narrow the window or filter by agent or team. 0 or less means\n"
  "the default.", "0.78.0" },

{ "daemon.allow_unconfined_host", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_DANGEROUS, "false", NULL,
  "Permit agents to use computer.host.confine: none.\n"
  "\n"
  "An unconfined host computer means the agent runs commands as you, with\n"
  "your files, your keys and your network. That needs to be two deliberate\n"
  "acts rather than one typo, so it requires this AND confirm_host_control\n"
  "on the agent itself.", "0.1.0" },

{ "daemon.automation_dir", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_COMMENTED,
  "~/.clawtilla/pods", NULL,
  "Where podomation pods that watch the fleet live.\n"
  "\n"
  "Every `*.pod` in here is loaded at start. clawtilla registers itself\n"
  "into podomation as a module, so a pod can bind to what the fleet does\n"
  "and act on it in the same file:\n"
  "\n"
  "  pod researcher = clawtilla->new(\"researcher\");\n"
  "\n"
  "  researcher->on_agent_state where event->state == \"error\" =>\n"
  "      clawtilla->restart_agent(agent: \"researcher\");\n"
  "\n"
  "  researcher->on_agent_state where event->state == \"error\" =>\n"
  "      clawtilla->notify(title: \"researcher fell over; restarted it\");\n"
  "\n"
  "An event name cannot carry a dot -- podomation's lexer stops at it --\n"
  "so it is on_agent_state rather than agent.state.\n"
  "\n"
  "The constructor's arguments are the scope, and it applies both ways:\n"
  "a pod named for one agent neither hears about the others nor can act\n"
  "on them. With no arguments, the whole fleet.\n"
  "\n"
  "A file that does not parse disables that file and warns; the rest are\n"
  "loaded.", "0.2.0" },

/* ── defaults ────────────────────────────────────────────────────── */
{ "daemon.webhook_enabled", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_COMMENTED, "false", NULL,
  "Whether to accept inbound trigger deliveries.\n"
  "\n"
  "Off by default. A fleet nothing calls into needs no listener, and a\n"
  "port opened because a feature exists rather than because somebody\n"
  "asked for it is a port nobody is watching.\n"
  "\n"
  "The receiver is its own server on its own port and serves only\n"
  "/health and the secret /hooks/... paths -- never the IPC surface --\n"
  "so putting it behind a tunnel exposes nothing else.", "0.2.0" },

{ "daemon.webhook_port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_COMMENTED,
  "8788", NULL,
  "Port the trigger receiver listens on.\n"
  "\n"
  "It binds loopback and the tailnet address when there is one, and\n"
  "loopback alone when there is not -- the same rule the web client\n"
  "follows, and for the same reason: a listener is never widened\n"
  "because an address was missing.", "0.2.0" },

{ "daemon.update_check", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_COMMENTED,
  "false", NULL,
  "Whether to ask, on a timer, if a newer clawtilla exists.\n"
  "\n"
  "Off by default, because it is the only thing in the daemon that\n"
  "reaches the network without somebody asking it to.\n"
  "\n"
  "It checks and reports; it does not install anything. An unattended\n"
  "update must not restart the daemon under running turns, so applying\n"
  "one waits on a hold that can drain first.\n"
  "\n"
  "The result reaches every client through control.status, including\n"
  "when the check itself failed -- a check that has been quietly\n"
  "erroring for a month is worse than none, because nothing to draw\n"
  "reads as up to date.", "0.2.0" },

{ "daemon.update_url", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_COMMENTED,
  "https://gitlab.com/api/v4/projects/zachpodbielniak%2Fclawtilla/releases",
  NULL,
  "Where daemon.update_check asks.\n"
  "\n"
  "Three answer shapes are understood, because three are real and none\n"
  "of them is ours: a bare version as text or as a JSON string, an\n"
  "object carrying version, tag_name or name, and an array of those --\n"
  "which is what Forgejo, Gitea and GitLab all return from a releases\n"
  "endpoint. Point it at a self-hosted forge or a file you publish;\n"
  "nothing here is tied to a particular host.", "0.2.0" },

{ "daemon.update_interval_hours", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_COMMENTED, "24", NULL,
  "How often to ask, in hours. Clamped to at least one.\n"
  "\n"
  "Nothing is asked at daemon start: the first check happens one\n"
  "interval in. Starting a daemon must not reach the network, or every\n"
  "test fixture that builds one does too.", "0.2.0" },

{ "defaults", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "What a new agent gets when its own block does not say.\n"
  "\n"
  "Every key here can be overridden per agent.", "0.1.0" },

{ "defaults.provider", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "claude-code", NULL,
  "Default AI provider, naming an entry in the agent's libreclaw providers.", "0.1.0" },

{ "defaults.model", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "sonnet", NULL,
  "Default model for new agents.", "0.1.0" },

{ "defaults.computer", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "none", clawt_computer_type_get_type,
  "Default computer type for new agents. The permitted values are\n"
  "listed above, from the schema itself rather than written out again\n"
  "here -- the copy that was written out here had already gone stale.\n"
  "\n"
  "none is the default on purpose. An agent that can run commands is a\n"
  "bigger grant than an agent that can only talk, and it should be asked\n"
  "for rather than inherited.", "0.1.0" },

{ "defaults.container_image", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "registry.fedoraproject.org/fedora:45", NULL,
  "Image a container agent gets when it does not name one.\n"
  "\n"
  "Fedora because that is what clawtilla is developed and tested on, and\n"
  "because a container whose package manager you already know is one you\n"
  "can debug. Change it once here rather than per agent.", "0.1.0" },

{ "defaults.container_images", CLAWT_SCHEMA_STRING_LIST,
  CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Extra images to offer in a client's image list.\n"
  "\n"
  "Added to the built-in suggestions rather than replacing them, and\n"
  "shown first: these are yours, and a list where your own images are\n"
  "below a dozen you will never pick is one you scroll past.\n"
  "\n"
  "An entry is a reference, optionally followed by ' -- ' and a note:\n"
  "\n"
  "  container_images:\n"
  "    - \"registry.example.com/team/devbox:latest -- ours, has the "
  "toolchain\"\n"
  "    - \"docker.io/library/haskell:9.10\"\n"
  "\n"
  "Nothing is validated here. A reference podman cannot pull fails at "
  "start with podman's own error.", "0.1.0" },

{ "defaults.container", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Container limits every container and distrobox agent gets unless its\n"
  "team or the agent itself says otherwise.\n"
  "\n"
  "The same six keys as agents.computer.container -- cpus, cpu_weight,\n"
  "cpuset, memory, memory_swap, pids_limit -- with the same meaning. Most\n"
  "specific wins, key by key: the agent's own value, then its team's\n"
  "(teams[].computer.container), then this. `clawtilla agent show` says\n"
  "which one each value came from.\n"
  "\n"
  "Written here rather than under defaults.computer, which is already\n"
  "the computer type.", "0.78.0" },

{ "defaults.container.cpus", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The fleet's agents.computer.container.cpus.", "0.78.0" },

{ "defaults.container.cpu_weight", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The fleet's agents.computer.container.cpu_weight.", "0.78.0" },

{ "defaults.container.cpuset", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The fleet's agents.computer.container.cpuset.", "0.78.0" },

{ "defaults.container.memory", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The fleet's agents.computer.container.memory.", "0.78.0" },

{ "defaults.container.memory_swap", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The fleet's agents.computer.container.memory_swap. Checked against the\n"
  "memory an agent ends up with, wherever that came from: a team's 8g\n"
  "memory under a fleet swap total of 6g is refused.", "0.78.0" },

{ "defaults.container.pids_limit", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The fleet's agents.computer.container.pids_limit.", "0.78.0" },

{ "defaults.workspace_root", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  "~/.clawtilla/agents", NULL,
  "Where per-agent workspaces are scaffolded.\n"
  "\n"
  "Each agent gets a directory here holding its libreclaw config, its\n"
  "identity files and its MEMORY.md. It is the agent's desk, not the whole\n"
  "house.", "0.1.0" },

{ "defaults.exchange_dir", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  "$XDG_DATA_HOME/clawtilla/exchange", NULL,
  "Shared drop-box mounted into every computer at /mnt/clawtilla/exchange.\n"
  "\n"
  "shared/ is writable by every agent; <agent-id>/ is writable by that\n"
  "agent and readable by the others. It exists so agents can hand each\n"
  "other files without anybody wiring up mounts by hand.", "0.1.0" },

{ "defaults.mounts", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Host paths shared into *every* agent's computer.\n"
  "\n"
  "  defaults:\n"
  "    mounts:\n"
  "      - source: \"~/source\"\n"
  "        target: \"/work/source\"\n"
  "        mode: rw\n"
  "\n"
  "The directory you want every agent to see -- your projects tree,\n"
  "your notes -- written once instead of copied into every agent block\n"
  "and then forgotten on the next agent you create.\n"
  "\n"
  "Applied to container, distrobox and VM agents. Not to host agents:\n"
  "there a mount is the confinement allowlist rather than a kernel\n"
  "mount, and quietly widening what a host agent may reach is not\n"
  "something a convenience should do.\n"
  "\n"
  "An agent's own computer.mounts wins on a matching target, so one\n"
  "agent can point the same path somewhere else without turning the\n"
  "default off. agents.computer.default_mounts: false declines them all.",
  "0.1.0" },

{ "defaults.mounts.source", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Host path to share. Not needed for tmpfs.", "0.1.0" },

{ "defaults.mounts.target", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_REQUIRED,
  NULL, NULL,
  "Absolute path inside the computer. Must not overlap another mount.",
  "0.1.0" },

{ "defaults.mounts.mode", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "ro", clawt_mount_mode_get_type,
  "ro or rw. Read-only is the default for the same reason it is on an\n"
  "agent's own mounts, and more so: an entry here reaches the whole\n"
  "fleet, so a mount that says nothing is one every agent may read and\n"
  "none may rewrite.", "0.1.0" },

{ "defaults.mounts.type", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "bind", clawt_mount_type_get_type,
  "bind, volume, virtiofs, 9p or tmpfs. Chosen from the backend when\n"
  "unset, so the same entry is a bind mount in a container and a\n"
  "virtiofs share in a VM.", "0.1.0" },

{ "defaults.mounts.relabel", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "shared", clawt_relabel_get_type,
  "SELinux relabelling: none, shared (:z) or private (:Z).\n"
  "\n"
  "shared when unset, which matters on Silverblue and friends: an\n"
  "unlabelled bind mount appears inside the container with every access\n"
  "denied, which reads as a permissions bug rather than a labelling one.",
  "0.1.0" },

{ "defaults.mounts.scope", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "all", clawt_scope_get_type,
  "Who gets this folder: all, selected or none.\n"
  "\n"
  "`all` includes agents created later, which is the point of a default.\n"
  "`selected` uses the `agents:` and `teams:` lists below. `none` keeps\n"
  "the entry without giving it to anybody, which is how you park one\n"
  "without losing what it said.\n"
  "\n"
  "Naming agents or teams without saying `scope` means `selected` --\n"
  "writing a list and having it ignored would be a rule that reads\n"
  "correctly and does the opposite.\n"
  "\n"
  "A scope that is written and cannot be read reaches nobody, the same\n"
  "way an unrecognised integration scope does: handing somebody's home\n"
  "directory to the whole fleet by typo is far worse than handing it to\n"
  "nothing and saying so.", "0.1.0" },

{ "defaults.mounts.agents", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Agent ids that get this folder, when `scope: selected`.\n"
  "\n"
  "An id that names no agent is ignored rather than refused: an agent\n"
  "removed for the afternoon should not stop the fleet starting.",
  "0.1.0" },

{ "defaults.mounts.teams", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Team ids that get this folder, when `scope: selected`.\n"
  "\n"
  "Naming a team is how a folder covers a group without being rewritten\n"
  "every time somebody joins it, which is the whole reason teams exist.\n"
  "An agent on no team matches no team entry -- including one spelled\n"
  "with an empty string.", "0.1.0" },

{ "defaults.mounts.create", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Create the source directory if it is missing.", "0.1.0" },

{ "defaults.mounts.size", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Size for a tmpfs mount, such as 512M.", "0.1.0" },

{ "defaults.mounts.required", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether a missing source stops the agent starting.\n"
  "\n"
  "Worth turning off for a fleet default that not every machine has --\n"
  "a laptop without your projects tree should still start its agents.",
  "0.1.0" },

{ "defaults.image_dir", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  "$XDG_DATA_HOME/clawtilla/images", NULL,
  "Where downloaded cloud images are kept.\n"
  "\n"
  "One copy serves the whole fleet: a VM agent writes to a qcow2 overlay\n"
  "backed by the image here, never to the image itself. Manage them with\n"
  "`clawtilla image vm`, or from Settings in the GTK client.", "0.1.0" },

{ "defaults.exchange_max_bytes", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "1073741824", NULL,
  "Size cap for the exchange directory. 0 disables the limit.\n"
  "\n"
  "Applied by deleting the oldest files until the total is under the\n"
  "cap: whenever the exchange is opened for use -- when an agent starts,\n"
  "and before a file is put into it -- and again on the daemon's sweep,\n"
  "which is what catches an agent writing through the mount from inside\n"
  "its computer, where nothing on the host sees the write happen.",
  "0.1.0" },

{ "defaults.libreclaw_binary", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Which libreclaw to run for process-runtime agents.\n"
  "\n"
  "Unset means: beside clawtillad, then the deps/libreclaw build this\n"
  "checkout produced, then PATH. A normal build and a normal install\n"
  "both work without setting this.\n"
  "\n"
  "Set it when the one to run is somewhere else -- a second checkout, or\n"
  "an installed copy you want in preference to the build tree. Naming it\n"
  "here wins over all three.",
  "0.1.0" },

{ "defaults.restart", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "on-failure", clawt_restart_policy_get_type,
  "Default restart policy: never, on-failure or always.", "0.1.0" },

{ "defaults.autostart", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Whether new agents start with the daemon.", "0.1.0" },

{ "defaults.stream_steps", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether agents report what they are doing while a turn runs.\n"
  "\n"
  "With this on, the tools an agent reaches for and the prose it writes\n"
  "between them appear in the conversation as they happen, so a turn\n"
  "that takes ten minutes is something to watch rather than a typing\n"
  "dot. With it off, a turn shows the indicator and then its answer.\n"
  "\n"
  "A step is not a message. It is never delivered, never queued, never\n"
  "written to the transcript and never answered -- so an agent watching\n"
  "a peer work does not take a turn over it, which is the failure the\n"
  "old five-minute progress notes had.\n"
  "\n"
  "Tool arguments are previewed, one line each, with secrets redacted.\n"
  "Turn it off for a fleet where even a redacted command line should\n"
  "not reach whoever can open the chat.", "0.2.0" },

{ "defaults.confine", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "none", clawt_runtime_confine_get_type,
  "Whether agents' own CLIs run inside a bubblewrap sandbox: none or\n"
  "bwrap.\n"
  "\n"
  "The fleet-wide answer for agents.runtime.confine, which an agent may\n"
  "override either way. Off by default, so turning it on is a decision\n"
  "rather than something an upgrade did to agents that were running.\n"
  "\n"
  "Set to bwrap, every process-runtime agent that says nothing itself is\n"
  "confined. An embedded agent cannot be -- it is the daemon -- and is\n"
  "disabled with that reason rather than run unconfined; give it\n"
  "runtime.confine: none of its own to keep it embedded.", "0.14.0" },

/* ── ai_assist ───────────────────────────────────────────────────── */
{ "defaults.avatar_max_bytes", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_COMMENTED, "4194304", NULL,
  "Largest profile picture the daemon will read and serve.\n"
  "\n"
  "A picture crosses IPC as bytes rather than as a path, because a\n"
  "client may be on another machine entirely -- so an unbounded\n"
  "picture is an unbounded frame. Over the limit is a refusal naming\n"
  "the limit, never a truncated image, which would surface as a broken\n"
  "file a long way from the cause.", "0.2.0" },

{ "defaults.skills", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Skills every agent gets unless it says otherwise.\n"
  "\n"
  "Resolved together with the team's list and the agent's own through\n"
  "one function, the way integrations already are. Two resolvers would\n"
  "be two behaviours, and the one nobody tested would be the one that\n"
  "ran.\n"
  "\n"
  "Names, not paths. A name that matches no skill is a warning rather\n"
  "than an error -- a fleet is edited by hand -- but it is never\n"
  "silent, because an agent quietly missing the procedure it was\n"
  "configured with looks exactly like one that has it and ignored it.",
  "0.2.0" },

{ "defaults.policy", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "What agents may do without asking. See agents.policy.", "0.3.0" },

{ "defaults.policy.rules", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Action rules every agent follows unless it has its own list.\n"
  "\n"
  "Each is MATCH=MODE. MATCH is an action class -- read, write, delete,\n"
  "message, exec, external, desktop_input, fleet_admin -- or a tool-name\n"
  "glob such as clawtilla_create_* or Bash. MODE is allow, review, ask,\n"
  "hand_back or deny. The first rule that matches decides; a call no\n"
  "rule matches is allowed. An agent's own agents.policy.rules replaces\n"
  "this list rather than adding to it, so the order of one list is the\n"
  "whole answer.", "0.3.0" },

{ "defaults.policy.ask_timeout_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "600", NULL,
  "How long an `ask` waits for a person before it is refused. The\n"
  "decision is then settled as defaulted, so the inbox says nobody\n"
  "chose rather than that somebody said no.", "0.3.0" },

{ "defaults.policy.reviewer", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The decision model `review` asks: typesafe, ollama or laya (ai-glib's\n"
  "deciders). Unset, `review` behaves as `ask`. A reviewer that is\n"
  "unsure, or that fails, also becomes `ask` -- never `allow`.", "0.3.0" },

{ "ai_assist", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The model that helps design new agents.\n"
  "\n"
  "`clawtilla agent new --ai` describes what you want in prose and the\n"
  "model fills in a draft spec through tool calls, shows you the YAML and\n"
  "the workspace files it would create, and only then commits -- through\n"
  "the same code path as manual creation, so there is no second\n"
  "implementation to drift.", "0.1.0" },

{ "ai_assist.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether AI-assisted agent creation is offered.", "0.1.0" },

{ "ai_assist.provider", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "claude", NULL,
  "Provider used for the designer. Must be one that takes tool\n"
  "definitions -- claude, openai, gemini, grok or ollama.\n"
  "\n"
  "Not one of the CLI backends an agent runs on. The designer works\n"
  "entirely through tool calls, and ai-glib's command-line clients drop\n"
  "the tool list rather than passing it on, so claude-code and friends\n"
  "refuse this job however capable the model behind them is.", "0.1.0" },

{ "ai_assist.model", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "claude-opus-5", NULL,
  "Model used for the designer. Worth a capable one: it is a short\n"
  "conversation and the output is a file you live with.", "0.1.0" },

{ "ai_assist.max_turns", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "20", NULL,
  "Turn cap for the design conversation, so a model that never commits\n"
  "stops rather than looping.", "0.1.0" },

/* ── secrets ─────────────────────────────────────────────────────── */
{ "secrets", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "How secret references in this file are resolved.\n"
  "\n"
  "There is deliberately no way to write a literal secret here. Credentials\n"
  "are always a reference:\n"
  "\n"
  "  api_key: {file: ~/.clawtilla/secrets/anthropic}\n"
  "  api_key: {env: ANTHROPIC_API_KEY}\n"
  "  api_key: {command: \"pass show clawtilla/anthropic\"}\n"
  "\n"
  "Resolved values are written to per-agent credential files at mode 0600\n"
  "and injected into that agent's environment. They never appear in an IPC\n"
  "response, a log line or a transcript.", "0.1.0" },

{ "secrets.default_backend", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "file", clawt_secret_backend_get_type,
  "Backend assumed when a reference gives a bare string: file, env or command.", "0.1.0" },

{ "secrets.dir", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  "~/.clawtilla/secrets", NULL,
  "Directory a bare file reference is resolved against.", "0.1.0" },

{ "secrets.command_timeout_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "10", NULL,
  "How long a command-backend secret may take before it is an error.\n"
  "\n"
  "A password manager that is locked will block forever rather than fail,\n"
  "and without a timeout so will daemon startup.", "0.1.0" },

/* ── egress ──────────────────────────────────────────────────────── */
{ "egress", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Where an agent with runtime.network: gated may connect.\n"
  "\n"
  "A gated agent's sandbox has no network of its own. Its one way out is\n"
  "clawtilla's egress gate, an HTTP proxy in the daemon that resolves\n"
  "names itself and connects only to public addresses: never this\n"
  "machine, the LAN (RFC 1918), link-local, CGNAT and tailnets\n"
  "(100.64.0.0/10), IPv6 unique-local, multicast or a cloud metadata\n"
  "service. It judges the address, after resolution, so a public name\n"
  "pointed at 127.0.0.1 is refused too.", "0.17.0" },

{ "egress.allow", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Private addresses gated agents may reach anyway.\n"
  "\n"
  "Each entry is an address (100.101.102.103), a network\n"
  "(192.168.1.0/24, fd7a:115c:a1e0::/48) or a host name\n"
  "(nas.example.ts.net), optionally with a port (10.0.0.5:8080,\n"
  "[fd00::1]:443). A name matches the name the agent asked for and never\n"
  "opens loopback; loopback needs an address. Cloud metadata, multicast,\n"
  "the unspecified address and reserved space cannot be opened, and an\n"
  "entry naming one is refused. One bad entry refuses the whole list, at\n"
  "load and where it is added.", "0.17.0" },

/* ── plugins ─────────────────────────────────────────────────────── */
{ "plugins", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Loadable plugins: event handlers, extra tools, computer backends and\n"
  "integrations.\n"
  "\n"
  "A plugin that fails to load disables itself and nothing else.", "0.1.0" },

{ "plugins.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether plugins are scanned for at all.", "0.1.0" },

{ "plugins.dirs", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Extra directories to scan, searched before the XDG and system paths.", "0.1.0" },

{ "plugins.disabled", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Plugin ids to skip, by the <id> in libclawt-plugin-<id>.so.\n"
  "\n"
  "This is a plain list rather than a per-plugin enabled flag so that a\n"
  "third-party plugin the core has never heard of can still be turned off.", "0.1.0" },

/* ── orchestration ───────────────────────────────────────────────── */
{ "orchestration", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "How agents talk to each other, and what stops them doing it forever.\n"
  "\n"
  "Bot-to-bot messaging is the point of clawtilla and also the thing most\n"
  "able to run away: two agents that each reply politely will do so until\n"
  "something stops them. Every limit here is that something.", "0.1.0" },

{ "orchestration.max_hops", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "8", NULL,
  "How far a message may travel agent-to-agent before it is refused.\n"
  "\n"
  "Every message carries the depth it was sent at. A chief-of-staff\n"
  "delegating to a worker that asks a specialist is three hops; anything\n"
  "much beyond that is usually a loop rather than a plan.", "0.1.0" },

{ "orchestration.rate_limit_per_minute", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "30", NULL,
  "Messages one agent may send per minute. 0 disables the limit.", "0.1.0" },

{ "orchestration.daily_fleet_budget_micros", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "Daily fleet admission cap in integer millionths of USD; 0 disables.\n"
  "Uses recorded usage since local midnight, including reset archives.\n"
  "Already running work can overshoot; new deliveries wait until reset.", "0.1.0" },

{ "orchestration.daily_agent_budget_micros", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_PER_AGENT,
  "0", NULL,
  "Daily admission cap per agent in integer millionths of USD; 0 disables.\n"
  "Override with daily_agent_budget_micros on an agent.\n"
  "Uses local midnight and persisted usage; in-flight work can overshoot.", "0.1.0" },

{ "orchestration.meshy_credit_micros", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "What one Meshy credit costs you, in integer millionths of USD (20000\n"
  "for two cents); 0, the default, leaves it unknown.\n"
  "\n"
  "Meshy prices its jobs in credits, not dollars. With a price here, an\n"
  "agent's Meshy jobs count towards the daily caps\n"
  "(daily_agent_budget_micros, daily_fleet_budget_micros) and the\n"
  "Spending page's total; without one they are listed as credits and\n"
  "leave both alone -- unknown, never free. PixelLab says its price in\n"
  "dollars and needs nothing here.", "0.75.0" },

{ "orchestration.task_budget_usd", CLAWT_SCHEMA_DOUBLE, CLAWT_SCHEMA_FLAG_NONE,
  "5.0", NULL,
  "Spend cap for one delegated task and everything it spawns.\n"
  "\n"
  "A turn's cost is booked on the task it was about and on every task\n"
  "above it, so a task's figure is what its whole subtree cost.  A\n"
  "message on any task is refused once that task, or the root of its\n"
  "tree, has spent the cap -- one budget for a whole fan-out, not one\n"
  "per subtask (per subtask until 0.65.2).  So the default bounds\n"
  "everything a chief's assignment grows into; raise it for fleets that\n"
  "do large delegated work.  orchestration.tree_message_budget bounds the\n"
  "same tree in messages, a project's monthly_budget_usd its month.\n"
  "0 disables the cap.", "0.1.0" },

{ "orchestration.idle_hours", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "72", NULL,
  "How long without a turn before the Fleet page calls an agent idle.\n"
  "\n"
  "Only a label: an idle agent is still running, still holding its\n"
  "process and its computer. `agents.runtime.idle_stop_hours` is what\n"
  "stops one. 0 calls nothing idle.", "0.8.0" },

{ "orchestration.cycle_window", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "10", NULL,
  "How many recent messages per room are remembered for loop detection.\n"
  "\n"
  "A message repeating one already in the window is refused. This catches\n"
  "the case the hop limit does not: two agents alternating the same two\n"
  "replies, each one a fresh chain.\n"
  "\n"
  "This bounds the memory. How far back the check looks in time is\n"
  "orchestration.cycle_seconds.", "0.1.0" },

{ "orchestration.cycle_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "300", NULL,
  "How long a repeated message counts as a loop. 0 disables the check.\n"
  "\n"
  "Without this the cycle window was a count alone, so how far back it\n"
  "looked was however long the room's last cycle_window messages had\n"
  "taken -- ten messages in a quiet room is hours. An agent that hit a\n"
  "spawn failure and reported the same error string every turn had its\n"
  "first report delivered and every one after it refused, so a wedged\n"
  "agent was indistinguishable from an idle one for ten hours.\n"
  "\n"
  "The default suits the runaway this check exists for -- two agents\n"
  "alternating the same two replies with nothing pacing them, where a\n"
  "turn is seconds. Raise it for a fleet whose turns are minutes; a\n"
  "repeat is only caught if it lands inside the window.", "0.1.0" },

{ "orchestration.ack_check_model", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "", NULL,
  "A small model that decides whether an answer only acknowledges, as\n"
  "provider/model -- claude/claude-haiku-4-5-20251001, ollama/llama3.2.\n"
  "Empty turns the check off.\n"
  "\n"
  "An answer to a deliberate message wakes the agent that sent it, and\n"
  "that turn costs as much for \"Thanks, noted.\" as for a page of\n"
  "findings. With a model set, an answer is put to it only when the\n"
  "answering turn made no tool calls (so ran no commands, changed no\n"
  "files and sent nothing), its steps were streamed so that is known,\n"
  "the answer is at most 600 characters, and the message it answers was\n"
  "not a task. The model is asked one question, ACK or NEEDS, with both\n"
  "texts fenced as data, and has 8 seconds.\n"
  "\n"
  "Only a plain ACK skips the wake. NEEDS, a failure, a timeout or any\n"
  "other reply wakes the sender as before. An acknowledged answer is\n"
  "still recorded in the room, marked \"acknowledged (no wake)\", and\n"
  "the sender's next turn opens with a one-line note of it, so nothing\n"
  "is dropped.\n"
  "\n"
  "A provider ai-glib reaches over HTTP, with its key configured the way\n"
  "ai_assist's is. A command-line provider would spend the 8 seconds\n"
  "starting. A message sent with reply: none never wakes its sender,\n"
  "with or without this.", "0.17.0" },

{ "orchestration.tree_message_budget", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "60", NULL,
  "How many requests one task tree may send between settlements.\n"
  "0 disables the budget.\n"
  "\n"
  "A tree is a root task and everything delegated from it. max_hops\n"
  "bounds how deep a chain goes and says nothing about how wide: a chief\n"
  "that hands work to ten agents, each of which asks two more, is three\n"
  "hops deep and can still talk all night. This counts the deliberate\n"
  "messages sent from inside one tree -- clawtilla_message_agent,\n"
  "clawtilla_ask_agent, clawtilla_delegate, clawtilla_handoff,\n"
  "clawtilla_post_room and an @mention in a room -- one per call, however\n"
  "many teammates the call reaches.\n"
  "\n"
  "Answers are not counted and never refused, and neither is the\n"
  "follow-up turn a delegator is owed when its work settles. The count\n"
  "starts again whenever any task in the tree settles, because a\n"
  "settlement is progress and chatter is not.\n"
  "\n"
  "At the limit the messaging tools are withheld from that tree's turns,\n"
  "exactly as max_hops withholds them, and one alert names the tree.", "0.20.0" },

{ "orchestration.pair_message_budget", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "12", NULL,
  "How many rounds of requests two agents may send each other with no\n"
  "person in between. 0 disables the budget.\n"
  "\n"
  "Every other limit here can be started again by the agents\n"
  "themselves. The hop count begins at zero on a notice of the daemon's\n"
  "(a task settling, a wait ending), and the tree budget renews when a\n"
  "task settles -- so two agents trading one job at a time, or asking\n"
  "each other something after every notice, could do it for as long as\n"
  "both were willing and nothing was ever refused. This is the count\n"
  "nothing an agent does renews.\n"
  "\n"
  "A round is every request one agent sends the other from one turn --\n"
  "clawtilla_message_agent, clawtilla_ask_agent, clawtilla_delegate, a\n"
  "handoff -- so a chief handing a worker ten jobs at once has used one\n"
  "round, and a pair passing one job back and forth has used a round\n"
  "each time. Answers are not counted and never refused. Group rooms\n"
  "are not counted: rounds and rooms.max_chain bound those.\n"
  "\n"
  "The count starts again when a person, a routine or a trigger writes\n"
  "to either agent, or when the two have sent each other nothing for\n"
  "orchestration.pair_quiet_minutes. At the limit a request between\n"
  "the two is refused, saying so, and one alert names the pair.", "0.34.0" },

{ "orchestration.pair_quiet_minutes", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "30", NULL,
  "How long two agents must send each other nothing before their pair\n"
  "budget starts again. 0 means only a person renews it.\n"
  "\n"
  "A fleet driven from somewhere the daemon does not see -- a Matrix\n"
  "room, a webhook -- never shows it a person, so without this two\n"
  "agents that had ever done pair_message_budget rounds of honest work\n"
  "could not ask each other anything again until somebody opened a\n"
  "chat. A loop is never quiet, so it does not earn this; once it has\n"
  "been stopped nothing is left running to start it again.", "0.34.0" },

{ "orchestration.ephemeral_max_per_parent", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "5", NULL,
  "How many ephemeral agents one agent may have at once. A spawn past it\n"
  "is refused with the list of the ones it has, so the answer to the\n"
  "refusal is to reap one. 0 means none at all.", "0.55.0" },

{ "orchestration.ephemeral_max_fleet", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "20", NULL,
  "How many ephemeral agents the whole fleet may have at once, whoever\n"
  "spawned them. Each one is a running CLI process, so this is a ceiling\n"
  "on what a busy afternoon can start. 0 means none at all.", "0.55.0" },

{ "orchestration.ephemeral_reap_reminder_hours", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "24", NULL,
  "How long an ephemeral agent may sit with its work finished and nothing\n"
  "assigned before its parent is reminded, once, to reap it. Nothing is\n"
  "reaped automatically: the parent decides when the work is really\n"
  "done. 0 means never remind.", "0.55.0" },

{ "orchestration.answer_in_request_room", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_NONE, "true", NULL,
  "Hand an agent's answer to the asker in the conversation it asked\n"
  "from, rather than in the room the two agents share.\n"
  "\n"
  "An agent that asks a teammate something from your chat, or from a\n"
  "group, is woken with the answer there: it has the conversation\n"
  "behind it, what it writes at the end of that turn reaches you as any\n"
  "reply does, and anything more it wants from the teammate is a tool\n"
  "call. The answer itself stays on the record in the two agents' own\n"
  "room, where Flow shows the exchange.\n"
  "\n"
  "Off, the answer wakes the asker in the two agents' room -- a\n"
  "separate session that holds none of your conversation -- as it did\n"
  "before 0.34.0. A request made while answering somewhere the daemon\n"
  "does not see (a Matrix room) is always answered that way.", "0.34.0" },

{ "orchestration.crosstalk_summary", CLAWT_SCHEMA_ENUM,
  CLAWT_SCHEMA_FLAG_PER_AGENT, "chief", clawt_crosstalk_summary_get_type,
  "Tell you about agent-to-agent conversations: chief, on or off.\n"
  "\n"
  "When an exchange between agents ends -- a message and its answer, a\n"
  "delegated task settling, a Grok bot reaching out and being answered,\n"
  "a group room's round -- a short summary of it is posted into your own\n"
  "conversation with the agent that took part: who reached out, what\n"
  "they asked, what the agent did (tool calls by kind, tasks it handed\n"
  "on, decisions it raised) and how it ended, with a link that opens the\n"
  "exchange in Flow. Without it, that conversation is only in Flow, and\n"
  "you have to go and look.\n"
  "\n"
  "chief, the default: only the chief of staff's conversation with you\n"
  "gets them. on: every agent's. off: none. Set per agent as\n"
  "`crosstalk_summary` to turn one agent on or off whatever this says.\n"
  "\n"
  "The summary is the daemon's note, built from the record and costing\n"
  "no model call: it is never delivered to the agent, never starts a\n"
  "turn, invites no reply and counts towards no hop, budget or incident.\n"
  "An exchange ends when nobody in it has a turn running or a message\n"
  "waiting there, no task it carries is still running, and it has been\n"
  "quiet for 15 seconds; exchanges ending within a few seconds of each\n"
  "other are posted as one note. Work a trigger or routine started is\n"
  "not cross-talk and is never summarised.", "0.30.0" },

{ "orchestration.crosstalk_summary_to", CLAWT_SCHEMA_ENUM,
  CLAWT_SCHEMA_FLAG_PER_AGENT, "source", clawt_crosstalk_summary_to_get_type,
  "Where a cross-talk summary is posted: source or chat.\n"
  "\n"
  "source, the default: back where the request came from. An agent that\n"
  "reached out to another agent or a Grok bot while answering somebody in\n"
  "a Matrix room has the summary posted in that room, from its own Matrix\n"
  "account, so the person who asked hears how it went. Asked in a\n"
  "clawtilla room you are in -- a group -- it is recorded there. Anything\n"
  "else comes to your chat with the agent, as does a Matrix post that\n"
  "fails (with a line saying why).\n"
  "\n"
  "chat: always your chat with the agent.\n"
  "\n"
  "Only the agent whose turn began the exchange posts it at the source;\n"
  "another agent in it that is told (`crosstalk_summary`) is told in your\n"
  "chat with it. An exchange picked up again after a daemon restart has\n"
  "lost where it came from and comes to your chat. Set per agent as\n"
  "`crosstalk_summary_to`.", "0.33.0" },

{ "orchestration.config_notes", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "agents", clawt_config_notes_get_type,
  "Say in an agent's chat when its configuration was changed: agents,\n"
  "all or off.\n"
  "\n"
  "When an agent changes another agent's settings -- or its own -- or\n"
  "adds, edits, turns on or off or removes a routine or trigger that runs\n"
  "as it, a one-line note appears in your chat with the agent that was\n"
  "changed: what changed and who changed it. Without it the only trace\n"
  "is the acting agent's own tool step, in the acting agent's chat.\n"
  "\n"
  "agents, the default: changes an agent or another fleet made. all:\n"
  "your own changes from a client or the CLI as well. off: none. A\n"
  "hand edit of this file names nobody and is never noted.\n"
  "\n"
  "The note is the daemon's, recorded and never delivered: it starts no\n"
  "turn and the changed agent is not told by it. A credential's value is\n"
  "never shown, and nothing in an agent's env: block is.", "0.74.0" },

{ "orchestration.crosstalk_summary_model", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_COMMENTED, "", NULL,
  "A small model that rewrites each cross-talk summary as one friendly\n"
  "paragraph, as provider/model -- the same spelling as\n"
  "orchestration.ack_check_model. Empty, the default, posts the summary\n"
  "built from the record and asks no model anything.\n"
  "\n"
  "The model is given the record-built summary, fenced as data, and 10\n"
  "seconds. Nothing waits on it: the note is posted when it answers. A\n"
  "failure, a timeout, an empty reply or one longer than 1200\n"
  "characters posts the record-built summary instead, so turning this on\n"
  "can delay a note and never lose one.", "0.30.0" },

{ "orchestration.mailbox", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Defaults for every agent's mailbox.\n"
  "\n"
  "Each agent has a durable queue, so a message to a stopped agent waits\n"
  "for it rather than failing. Any of these can be overridden per agent.", "0.1.0" },

{ "orchestration.mailbox.max_depth", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_PER_AGENT,
  "1000", NULL,
  "How many undelivered items a mailbox holds before overflow applies.", "0.1.0" },

{ "orchestration.mailbox.overflow", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_PER_AGENT,
  "reject", clawt_overflow_policy_get_type,
  "What a full mailbox does: reject, drop-oldest or block-sender.\n"
  "\n"
  "All three are noisy on purpose. A queue that silently discards is a\n"
  "queue that loses work nobody notices until much later.", "0.1.0" },

{ "orchestration.mailbox.max_attempts", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_PER_AGENT,
  "5", NULL,
  "Delivery attempts before an item is dead-lettered rather than retried.", "0.1.0" },

{ "orchestration.mailbox.lease_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_PER_AGENT,
  "300", NULL,
  "How long an agent has to acknowledge an item before it returns to the queue.\n"
  "\n"
  "This is what makes delivery survive an agent dying mid-turn: the item\n"
  "comes back rather than being lost or silently delivered twice.", "0.1.0" },

{ "orchestration.mailbox.default_ttl_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_PER_AGENT,
  "604800", NULL,
  "How long an undelivered item lives before it is swept. 0 means forever.", "0.1.0" },

{ "orchestration.mailbox.backoff_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_PER_AGENT,
  "30", NULL,
  "Base for the exponential retry backoff, doubling per attempt, with\n"
  "jitter applied so a dependency that failed for every queued message does\n"
  "not bring them all back at the same instant.\n"
  "\n"
  "0 retries immediately.", "0.1.0" },

/* Last of `orchestration`, and it has to be: clawt-genconfig walks this
 * table in order and opens a YAML section when it meets one, so a key
 * sitting after the `memories` rows is emitted *inside* them.  This one
 * did, for as long as it existed -- `data/example-config.yaml` shipped
 * `chief_of_staff:` under `memories:`, so a daemon loading the file
 * clawtilla itself generates warned `unknown configuration key
 * 'memories.chief_of_staff'`.  The rule was already in CLAUDE.md; what it
 * cost was that the one example documenting the key could not set it. */
{ "orchestration.chief_of_staff", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Id of the agent that receives work addressed to the fleet.\n"
  "\n"
  "At most one agent may hold this. Setting it here is equivalent to\n"
  "chief_of_staff: true on that agent, and the daemon refuses to start if\n"
  "two agents claim it.", "0.1.0" },

/* ── memories ────────────────────────────────────────────────────── */
{ "orchestration.repeat_thresholds", CLAWT_SCHEMA_STRING_LIST,
  CLAWT_SCHEMA_FLAG_COMMENTED, "5,10,20", NULL,
  "Repeat counts worth reporting when an agent keeps making the same\n"
  "call.\n"
  "\n"
  "A count lands on a threshold exactly once, so the fifth identical\n"
  "call reports and the sixth does not. Reporting every repeat past a\n"
  "floor turns a signal into noise, and noise is what gets skipped.\n"
  "\n"
  "This one observes rather than intervenes. clawtilla does not own the\n"
  "model's own tool-call loop, so it cannot steer a turn that is already\n"
  "running: it counts the calls it serves, writes a note into the\n"
  "thread, and at the highest threshold interrupts the turn.\n"
  "\n"
  "An empty list turns the counting off.", "0.2.0" },

{ "orchestration.repeat_max_keys", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_COMMENTED, "256", NULL,
  "How many distinct calls one turn remembers while watching for\n"
  "repeats.\n"
  "\n"
  "Bounded on purpose: an unlimited set of unique arguments lets one\n"
  "pathological turn grow the daemon for as long as it runs. The least\n"
  "recently seen call is dropped first, and a dropped one starts\n"
  "counting again from one.", "0.2.0" },

{ "orchestration.handoff_max_per_turn", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_COMMENTED, "4", NULL,
  "How many handoffs one turn may queue.\n"
  "\n"
  "Small deliberately. A blocking ask gets backpressure for free\n"
  "because the caller waits; an asynchronous handoff does not, so this\n"
  "is the only thing standing between a confused chief of staff and a\n"
  "fan-out of real turns that each cost money.\n"
  "\n"
  "Counted as handoffs still *waiting*, not as a tally reset each turn.\n"
  "Usually the same number, because a queue drains when the turn that\n"
  "filled it ends -- and deliberately not the same when it does not: a\n"
  "transfer still waiting for a busy recipient goes on counting, so an\n"
  "agent with three stuck cannot queue four more on top of them.\n"
  "\n"
  "The refusal says how many are waiting and suggests doing the piece\n"
  "rather than queueing another. 0 removes the limit.",
  "0.2.0" },

{ "orchestration.handoff_busy_retries", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_COMMENTED, "3", NULL,
  "How many times a handoff waits for a busy assignee before giving up\n"
  "and saying so.\n"
  "\n"
  "Giving up is its own outcome rather than a failure: 'nobody was\n"
  "free' and 'it went wrong' need different answers from whoever reads\n"
  "the receipt.\n"
  "\n"
  "These are tries, not seconds. A queued handoff is looked at every\n"
  "time any turn in the fleet ends, so three tries means three turn\n"
  "boundaries went by with the recipient still mid-turn. 0 waits\n"
  "indefinitely, which is only sensible in a fleet where every turn\n"
  "ends.", "0.2.0" },

{ "orchestration.handoff_receipt_days", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_COMMENTED, "2", NULL,
  "How long a finished handoff's receipt is kept.\n"
  "\n"
  "A receipt says how one handoff went: queued, delivered, refused. The\n"
  "task itself is kept for orchestration.task_retention_days; this is\n"
  "the record of the move.\n"
  "\n"
  "A count bounds the file as well as this age, because a fleet that\n"
  "passes work around all day fills two days faster than a quiet one\n"
  "fills two years. A handoff that has not run yet is never pruned\n"
  "however old it is: it is undrained rather than finished. 0 keeps\n"
  "receipts until the count alone drops them.", "0.2.0" },

{ "orchestration.review_rounds", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_COMMENTED, "3", NULL,
  "How many times a reviewer may send a task back for changes before the\n"
  "operator decides.\n"
  "\n"
  "Reviewed work goes back and forth between its assignee and its\n"
  "reviewer (agents.review_by); past this many rounds of changes the task\n"
  "stays in review and the verdict becomes the operator's, as a decision\n"
  "in the inbox, rather than the two going round again. 0 removes the\n"
  "limit.", "0.38.0" },

{ "orchestration.checkout_keep_days", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_COMMENTED, "14", NULL,
  "How long a finished task's checkout is kept, in days.\n"
  "\n"
  "A task given a repository (`repo` on clawtilla_delegate or a new task)\n"
  "works in a checkout of its own under its assignee's workspace, at\n"
  "tasks/<task>, on the branch clawtilla/<task>. Once the task has been\n"
  "finished this long the checkout is removed and its branch kept: a\n"
  "worktree's branch is in the repository already, and a clone's is\n"
  "fetched back into it first. A checkout with uncommitted changes is\n"
  "never removed this way -- that is the operator's call, from the\n"
  "task's Changes. 0 keeps every checkout.", "0.39.0" },

{ "orchestration.task_retention_days", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_COMMENTED, "180", NULL,
  "How long a finished task is kept, in days.\n"
  "\n"
  "Every task is written to tasks.db in the state directory as it\n"
  "changes, with a journal of what happened to it, so a daemon restart\n"
  "keeps the list, a delegator can still ask what became of its work,\n"
  "and a parent still knows its children. A task that has finished is\n"
  "removed once it is older than this, with its journal; one that has\n"
  "not finished is never removed however old it is, because somebody\n"
  "may still be waiting on it. The clients' Tasks lists show the tasks\n"
  "that have not finished and those that finished in the last day; the\n"
  "rest are in the history (task.list with history: true, or\n"
  "`clawtilla task list --history`). 0 keeps every task.", "0.35.0" },

{ "orchestration.max_waits_per_agent", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_PER_AGENT, "10", NULL,
  "How many pending waits one agent may have.\n"
  "\n"
  "A wait (clawtilla_wait_until) is a condition clawtilla checks by code\n"
  "-- a URL, a file, a task, the agent's computer, a command -- so the\n"
  "model is woken once when it holds rather than on every look. A pending\n"
  "wait costs no turns, but each one is a probe: a URL fetched once a\n"
  "minute, a command run on the agent's computer. This bounds how many\n"
  "of those one agent can leave running, and the refusal names its\n"
  "pending waits so it can fold a new one into an old one.\n"
  "\n"
  "Override with max_waits on an agent. 0 removes the limit.", "0.15.0" },

{ "orchestration.incidents", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Reports of runs that broke, sent to whoever leads the team.\n"
  "\n"
  "A delegated task that failed or stalled, an agent that would not start\n"
  "with work waiting for it, and a routine run that did not happen each\n"
  "open an incident: one per run (the same task failing again updates it),\n"
  "filed in a room per team called \"Problems to look at\", and delivered\n"
  "as a report the recipient can act on with clawtilla_task_retry. A\n"
  "recipient that is mid-turn has its report held until the turn ends.\n"
  "See docs/orchestration.org, \"Incidents\".", "0.17.0" },

{ "orchestration.incidents.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether broken runs open incidents at all.\n"
  "\n"
  "Off, nothing is filed and nobody is told; tasks still fail and stall\n"
  "exactly as before, and their delegators still get the settle notice.\n"
  "Retrying a failed task works either way.", "0.17.0" },

{ "orchestration.incidents.to", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "lead", clawt_incident_recipient_get_type,
  "Who an incident report goes to: lead, chief or none.\n"
  "\n"
  "lead is the failing agent's team lead, or the chief of staff when the\n"
  "team has none (or two). chief is always the chief of staff. none spends\n"
  "nobody's turn: the report is recorded in the room for you to read.\n"
  "Never the agent whose run broke, and never the agent that delegated\n"
  "the task -- it already has the settle notice.", "0.17.0" },

{ "memories", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "What an agent remembers between conversations.\n"
  "\n"
  "Not agents.memory, which is the MEMORY.md size budget and a different\n"
  "thing entirely. This is the searchable store.\n"
  "\n"
  "Each agent gets its own SQLite database beside its mailbox, holding\n"
  "what it chose to write down: decisions, preferences, things that\n"
  "turned out to be true. It searches its own with clawtilla_memory_*.\n"
  "\n"
  "One database per agent rather than one table with an agent column, so\n"
  "isolation is a property of the filesystem: a query that forgot to\n"
  "filter still cannot reach another agent's memories, because they are\n"
  "not in the file being read.", "0.1.0" },

{ "memories.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_PER_AGENT,
  "true", NULL,
  "Whether agents get a memory store at all.\n"
  "\n"
  "Off means the clawtilla_memory_* tools are not offered, rather than\n"
  "offered and failing. An agent that can see a tool will try it.", "0.1.0" },

{ "memories.max_results", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_PER_AGENT,
  "20", NULL,
  "How many memories one search or listing returns at most.\n"
  "\n"
  "This lands in the agent's context on every call, so the limit is a\n"
  "budget rather than a formality.", "0.1.0" },

{ "memories.readers", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_PER_AGENT |
  CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Comma-separated ids of agents allowed to read this agent's memories.\n"
  "\n"
  "Empty by default, which is the whole point: an agent's memories are\n"
  "its own unless somebody says otherwise. Reading only -- there is no\n"
  "setting that lets one agent write into another's memory, because a\n"
  "memory you did not form is not a memory.\n"
  "\n"
  "A comma-separated string, not a YAML list: `readers: chief, deputy`.\n"
  "Written as `readers: [chief]` it reads back as unset and grants\n"
  "nothing, so the loader warns about that spelling rather than leaving\n"
  "a permission somebody granted silently denied.", "0.1.0" },

{ "memories.scope", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_PER_AGENT |
  CLAWT_SCHEMA_FLAG_COMMENTED, "agent", clawt_memory_scope_get_type,
  "Where this agent's own memories are written: agent, team or fleet.\n"
  "\n"
  "Reading always fans out across every scope the agent is entitled to;\n"
  "this says only where a new memory lands. A fact the whole fleet\n"
  "needs is worth writing once rather than being relearned by each\n"
  "agent in turn.\n"
  "\n"
  "Each scope is a separate database, so what an agent may read is\n"
  "decided by which file is opened rather than by a WHERE clause. A\n"
  "permission that is structural cannot be lost to a missing\n"
  "condition.\n"
  "\n"
  "`team` needs the agent to be on one. An agent with no `team:` that\nasks for team scope is refused rather than quietly written to its own\ndatabase -- a memory whose whole purpose was to reach the team is worse\nin the wrong place than not written at all.", "0.2.0" },

{ "memories.recall", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_PER_AGENT |
  CLAWT_SCHEMA_FLAG_COMMENTED, "true", NULL,
  "Whether the agent may search past conversations.\n"
  "\n"
  "Separate from `memories.enabled`, which is about facts the agent\n"
  "chose to record. This is about the transcript itself, which it never\n"
  "chose and which is much larger.\n"
  "\n"
  "Off means `clawtilla_recall` is not offered, rather than offered and\nrefused. Only rooms the agent is a member of are ever searched, so this\nis about the agent reading its own past rather than anybody else's.",
  "0.2.0" },

{ "memories.summarise", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_PER_AGENT |
  CLAWT_SCHEMA_FLAG_COMMENTED, "false", NULL,
  "Whether finished work is summarised into memories automatically.\n"
  "\n"
  "Off by default because it is a model call nobody asked for, billed\n"
  "to whoever set it. On, a completed task or a closed routine run is\n"
  "distilled into memories tagged with the transcript they came from.\n"
  "\n"
  "It uses the `ai_assist` provider, so that has to be configured too,\nand the memories land in whichever scope `memories.scope` names. A\nsummary that produced nothing is the common answer and not a failure.",
  "0.2.0" },

{ "memories.nudge_turns", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_PER_AGENT |
  CLAWT_SCHEMA_FLAG_COMMENTED, "0", NULL,
  "The cadence the reminder to record what was learned asks for. 0 is\n"
  "off.\n"
  "\n"
  "The reminder rides the per-agent prompt suffix rather than being a\n"
  "mechanism of its own, so it is visible in the same place as every\n"
  "other per-turn instruction -- `clawtilla config render` shows it\n"
  "beside the computer directive.\n"
  "\n"
  "A prompt suffix reaches every turn, so this number is what the\nreminder asks the agent for rather than something clawtilla counts.\nCounting turns from the daemon would need a per-turn channel to the\nagent, and the only one there is is the delivery preamble -- which not\nevery turn has.", "0.2.0" },

{ "memories.journal_keep", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "1000", NULL,
  "How many changes each memory database's journal keeps. 0 keeps every\n"
  "one.\n"
  "\n"
  "Every remember, forget, pin and unpin is written to a journal beside\n"
  "the memories it changed, with who made it, and the Memory review page\n"
  "in each client lists it with an Undo per row. The oldest rows go first\n"
  "once there are more than this, and a change whose row has gone can no\n"
  "longer be undone -- the page says so rather than offering a button\n"
  "that cannot work. One journal per database: an agent's own, each\n"
  "team's and the fleet's.", "0.26.0" },

{ "memories.operator_profile", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_COMMENTED, "false", NULL,
  "Whether the fleet keeps a profile of the person it works for.\n"
  "\n"
  "A fleet-scope memory a newly created agent inherits, so it starts\n"
  "knowing how you work instead of learning it again. Plain text and\n"
  "editable: a model of a person that the person cannot read is not\n"
  "something to build.\n"
  "\n"
  "Two halves: `<state_dir>/OPERATOR.org`, which you write, and\nfleet-scope memories in the `operator` category, which agents record.\nBoth are written into a marked region of every agent's USER.org, so a\nchange reaches the whole fleet rather than only agents made afterwards.\nOff removes the region again.", "0.2.0" },

{ "memories.review", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "A periodic review of each agent's memories by a small model, which\n"
  "proposes merges, forgets and pins for a person to accept one at a\n"
  "time. Off unless a model is named.", "0.29.0" },

{ "memories.review.model", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "", NULL,
  "The model that reviews memories, as provider/model --\n"
  "claude/claude-haiku-4-5-20251001, ollama/llama3.2. Empty turns the\n"
  "review off, which is the default.\n"
  "\n"
  "Off by default because every pass is a model call nobody asked for,\n"
  "billed to whoever set this. On, each agent's own memories are read\n"
  "by the model at most once per interval, fenced as data, and what it\n"
  "proposes -- merge two memories into one, forget one, pin or unpin\n"
  "one -- is never applied. It is listed as a proposal in the Memory\n"
  "review group of every client, and only a person accepting it changes\n"
  "anything, through the same journaled store as every other change, so\n"
  "an accepted proposal can be undone like any other.\n"
  "\n"
  "A provider ai-glib reaches over HTTP, with its key configured the way\n"
  "ai_assist's is. The model has a hard limit of 60 seconds per pass; a\n"
  "timeout, a failure and a reply that cannot be read propose nothing.",
  "0.29.0" },

{ "memories.review.interval_hours", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "24", NULL,
  "How often each agent's memories are reviewed, in hours (1 to 720).\n"
  "\n"
  "A pass is skipped while the agent still has proposals nobody has\n"
  "answered, so an unread list never grows by itself.", "0.29.0" },

{ "memories.review.max_memories", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "60", NULL,
  "How many of an agent's memories one pass reads at most, newest first\n"
  "(5 to 500). This is the size of the request, so it is the cost of\n"
  "one pass.", "0.29.0" },

/* ── sentinel ────────────────────────────────────────────────────── */
{ "sentinel", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "What `clawtilla sentinel` looks for in the event log.\n"
  "\n"
  "The sentinel is a read-only scan of <state_dir>/events/*.ndjson that\n"
  "groups failing tool calls, long queue waits, spending and stalled turns\n"
  "into issues and writes them as YAML. It runs out of process: the\n"
  "clients' Issues view asks the daemon to start `clawtilla sentinel`\n"
  "and then shows what it wrote. It never writes to the event log.", "0.26.0" },

{ "sentinel.days", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "7", NULL,
  "How many days of the event log one scan reads, counting today. 0\n"
  "reads every file there is, which daemon.event_log_days bounds.", "0.26.0" },

{ "sentinel.min_failures", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "3", NULL,
  "How many failures of one agent's tool make an issue. A tool that\n"
  "failed once is a turn's ordinary business; one that failed this many\n"
  "times in the window is a pattern somebody should look at.", "0.26.0" },

{ "sentinel.queue_wait_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "600", NULL,
  "How long a message may wait in an agent's queue, from being queued to\n"
  "being delivered, before the wait is an issue. Messages still queued\n"
  "past it when the scan ends are reported too.", "0.26.0" },

{ "sentinel.daily_spend_micros", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "10000000", NULL,
  "One agent's spend in one day, in micro-dollars, that makes an issue.\n"
  "0 reports only the days a cap was reached.\n"
  "\n"
  "A line to report, not a limit: nothing is refused at it. The caps that\n"
  "do refuse work are orchestration.daily_fleet_budget_micros and each\n"
  "agent's own, and every time one is reached is an issue whatever this\n"
  "says.", "0.26.0" },

/* ── skills ──────────────────────────────────────────────────────── */
{ "skills", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Reusable procedure, shared across the fleet.\n"
  "\n"
  "A skill is a directory holding a SKILL.md, in the layout every\n"
  "supported CLI understands. clawtilla links the ones an agent is\n"
  "assigned into that agent's workspace at whichever path its provider\n"
  "actually reads -- the paths differ per provider and are asked of the\n"
  "library rather than written down here.", "0.2.0" },

{ "skills.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_COMMENTED,
  "true", NULL,
  "Whether skills are scanned and linked at all.\n"
  "\n"
  "Off means off rather than empty: nothing is scanned, no directory is\n"
  "watched, and no link is written into any workspace. Existing links\n"
  "clawtilla made are left where they are, so turning this off does not\n"
  "quietly rewrite every agent -- turn it back on, or remove the\n"
  "assignments, if that is what you meant.", "0.2.0" },

{ "skills.dir", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_COMMENTED,
  "~/.clawtilla/skills", NULL,
  "Where the fleet's skills live.\n"
  "\n"
  "One directory per skill, each holding a SKILL.md whose front matter\n"
  "names it and says when to use it. The directory name and the `name`\n"
  "must agree, because that name is the traversal gate: no dots, no\n"
  "slashes, and no way to call a skill `..`.\n"
  "\n"
  "A skill lives here once and is linked into whichever workspaces need\n"
  "it, at whatever path that agent's own CLI reads -- which differs per\n"
  "provider and is asked of the library rather than written down. The\n"
  "directory is watched, so editing a SKILL.md takes effect without a\n"
  "reload. It is not created for you: a read with a side effect is how a\n"
  "typo in this path leaves an empty directory to puzzle over later.",
  "0.2.0" },

{ "skills.extra_dirs", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_COMMENTED,
  "~/.agents/skills", NULL,
  "Other directories of skills to read, on the daemon's machine.\n"
  "\n"
  "~/.agents/skills by default: the directory ai-glib's harnesses, Claude\n"
  "Code and others share, so a skill installed for them is a skill the\n"
  "fleet has. Read after skills.dir -- a skill of the same name there\n"
  "wins -- and never written: removing one is refused, and turning one\n"
  "on or off is kept under the daemon's state directory. A directory\n"
  "that does not exist is skipped without a word. An empty list reads\n"
  "only skills.dir.", "0.9.2" },

{ "skills.teach_max_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_COMMENTED,
  "900", NULL,
  "How long a demonstration may record before it stops itself.\n"
  "\n"
  "A recording that has been forgotten is worse than one that ended\n"
  "early, because what it captures is whatever you did next.\n"
  "\n"
  "The deadline is armed when the recording starts and is not a\nrequest: it fires whether or not anything is still listening, and\nit is passed to the compositor as well, so a clawtilla that dies\nmid-demonstration does not leave one running. Zero means the\ndefault rather than no limit.", "0.2.0" },

{ "skills.teach_max_events", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_COMMENTED,
  "20000", NULL,
  "How many recorded steps a demonstration keeps.\n"
  "\n"
  "Bounded, and the count of what was dropped is reported rather than\n"
  "hidden: a silently truncated demonstration teaches half a task.\n"
  "\n"
  "The earliest steps are the ones kept, because the first part of a\nprocedure is a usable prefix of it while a slice out of the middle\nis not. Pointer motion is never counted: a drag is hundreds of\nevents and would spend the whole budget on the mouse travelling,\nso every click and scroll carries the position it happened at\ninstead.",
  "0.2.0" },

{ "skills.max_proposals_per_day", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_COMMENTED,
  "3", NULL,
  "How many skills one agent may propose in any 24 hours.\n"
  "\n"
  "clawtilla_skill_propose lets an agent write down a technique it\n"
  "worked out -- a new skill, or a change to one that exists -- as a\n"
  "draft that reaches no prompt, with a decision in your inbox to\n"
  "approve, edit or reject it. Every proposal spends your attention, so\n"
  "this bounds how often one agent can ask for it; a rejected proposal\n"
  "still counts. 0 turns proposing off, and the tool is not offered.",
  "0.19.0" },

{ "rooms", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Standing rooms, created at startup if they do not exist.\n"
  "\n"
  "A room is a conversation with members. Posting into one enqueues to\n"
  "every member's mailbox. A direct chat is just a room with two members,\n"
  "so there is one mechanism rather than two.", "0.1.0" },

{ "rooms.id", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "Identifier used to address the room.", "0.1.0" },

{ "rooms.name", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Display name.", "0.1.0" },

{ "rooms.members", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Agent ids in the room. The human is always implicitly a member.", "0.1.0" },

{ "rooms.require_mention", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Only deliver messages that name a member.\n"
  "\n"
  "Defaults to the room's shape when it is not set: off for two members,\n"
  "where everything said is for the other one, and on for three or more,\n"
  "where delivering every remark to everybody costs a model turn each\n"
  "and is never what anybody meant. Setting it explicitly wins either\n"
  "way.\n"
  "\n"
  "A member is named as @its-id or as a bare word; matching ignores\n"
  "case, and an agent's display name counts when it is a single word.\n"
  "@all reaches everybody and belongs to you -- an agent writing it\n"
  "reaches nobody, because one broadcast per reply is how a room of five\n"
  "turns one remark into a runaway.\n"
  "\n"
  "A message that names nobody is still recorded and everyone can read\n"
  "it. That is the ordinary way to take part, and each delivery says so.",
  "0.1.0" },

{ "rooms.max_hops", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Overrides orchestration.max_hops for this room.\n"
  "\n"
  "It may loosen the fleet limit as well as tighten it, which is the\n"
  "whole reason it exists: in a conversation three agents each reply one\n"
  "hop deeper, so an ordinary standup reaches the fleet ceiling on its\n"
  "own, and the only other remedy is raising orchestration.max_hops for\n"
  "every delegation chain in the fleet to fix one room.\n"
  "\n"
  "It reaches the hop count and nothing else. The rate limit, the task\n"
  "budget and the cycle detector are untouched, so a loop that costs\n"
  "money still has three limits on it.\n"
  "\n"
  "In a group room this is the backstop rather than the limit that\n"
  "fires: rooms.max_chain ends a thread of replies first, and the depth\n"
  "starts again at each round.", "0.1.0" },

{ "rooms.max_chain", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "3", NULL,
  "How many replies one thread may run to in a round of a group room.\n"
  "0 removes the cap; the once-per-thread rule below still applies.\n"
  "\n"
  "A round starts at a message from you, a routine or a trigger. Each\n"
  "member that answers it starts a thread, and an @mention in a reply\n"
  "carries the thread on. Within one thread a member answers once: a\n"
  "mention of a member already in that thread is recorded in the room\n"
  "and not delivered, with the reason shown beside it. A member that\n"
  "spoke in another thread of the same round still answers.\n"
  "\n"
  "Without this the hop count built up across a room's history, so an\n"
  "ordinary standup reached max_hops by itself. The depth now starts\n"
  "again at every round, and max_hops stays as the backstop.", "0.20.0" },

{ "rooms.turn_timeout_seconds", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "0", NULL,
  "How long one member may hold a room's turn before it is yielded.\n"
  "0, the default, turns it off.\n"
  "\n"
  "Off for the same reason as agents.runtime.turn_timeout_seconds, and\n"
  "the five minutes it used to default to were worse: an agent doing\n"
  "real work in the operator's own room was stopped mid-turn and the\n"
  "room stalled, five minutes in, and the notice explaining it read as\n"
  "though the agent had misbehaved.\n"
  "\n"
  "Set a number for a busy shared room, where the point is that one\n"
  "member must not hold the floor indefinitely. Counted in work, not\n"
  "wall time: the clock holds while the turn is parked on an open\n"
  "decision and resumes with what is left. Stopping a turn under a\n"
  "question nobody has answered manufactures a stranded decision, which\n"
  "the daemon then has to repair.\n"
  "\n"
  "Different from the runtime's turn timeout: that one catches a wedged\n"
  "worker, this one catches a wedged conversation.\n"
  "\n"
  "Anything below 60 is raised to 60.", "0.2.0" },

{ "rooms.order", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Where this room sits in the sidebar, among the agents.\n"
  "\n"
  "The same scale as agents.order, because a client draws one list and\n"
  "a room can sit beside the agents it concerns. Written by the clients\n"
  "when a room is dragged or moved, numbered from one in steps of ten so\n"
  "there is room to hand-place something between two.", "0.2.0" },

{ "rooms.team", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Which team's group this room appears under in the sidebar.\n"
  "\n"
  "Presentation and nothing else: it does not change who is in the room\n"
  "or who a message reaches. A room with no team sits with the agents\n"
  "that have none.", "0.2.0" },

{ "rooms.catchup_messages", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "20", NULL,
  "How much of a room a member is caught up on when it is named.\n"
  "\n"
  "In a room that requires mentions an agent only receives the messages\n"
  "that named it, so without this it has no idea what was being\n"
  "discussed -- the transcript holds the conversation and the model does\n"
  "not. Each delivery carries the messages since that member last heard\n"
  "from the room, capped here, with a count of anything dropped.\n"
  "\n"
  "0 turns it off. Anything older is clawtilla_room_history, which the\n"
  "delivery preamble names.", "0.2.0" },

/* ── teams ───────────────────────────────────────────────────────── */
{ "projects", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Projects: the pieces of work the fleet is pointed at.\n"
  "\n"
  "A task belongs to at most one project: the one its delegation or the\n"
  "operator named, else its parent's, else the one whose directory its\n"
  "checkout came from (by path, or by a shared git remote). Its assignee\n"
  "is told the project's name and goal; the project's spend is summed\n"
  "over its tasks each month, against an optional budget; and every\n"
  "agent working in it finds it in PROJECTS.org. Optional: a fleet with\n"
  "none works exactly as before.", "0.40.0" },

{ "projects.id", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "Identifier used to address the project: `project` on a delegation or\n"
  "a new task, and the task lists' filter.", "0.40.0" },

{ "projects.name", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Display name; the id when unset.", "0.40.0" },

{ "projects.directory", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The project's repository or directory, as a host path.\n"
  "\n"
  "A task whose checkout is of this directory -- or of any repository\n"
  "sharing one of its git remotes, compared as host/owner/repo -- belongs\n"
  "to this project without being told.", "0.40.0" },

{ "projects.goal", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "What the project is for, in a sentence or two.\n"
  "\n"
  "Every assignment in the project starts with it, so the agent doing a\n"
  "piece of work knows what the piece is for.", "0.40.0" },

{ "projects.review_by", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL,
  NULL,
  "Who reviews work in this project, as agents.review_by spells it.\n"
  "\n"
  "Between a delegation's own `review` and the assignee's\n"
  "agents.review_by: the project's rule beats the agent's habit.",
  "0.40.0" },

{ "projects.agents", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE, NULL,
  NULL,
  "The agents that work in this project, by id.\n"
  "\n"
  "Only decides whose PROJECTS.org lists it; empty lists it for every\n"
  "agent. Anyone may still be given its work.", "0.40.0" },

{ "projects.monthly_budget_usd", CLAWT_SCHEMA_DOUBLE, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "What the project may spend in a calendar month, in US dollars.\n"
  "\n"
  "Summed from every agent's usage ledger over the project's tasks\n"
  "made this month. 0 means no budget: spend is still shown.", "0.40.0" },

{ "projects.budget_warn_percent", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "80", NULL,
  "At what share of monthly_budget_usd the operator is warned, once a\n"
  "month.", "0.40.0" },

{ "projects.budget_hard_stop", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Refuse new work in the project once its monthly budget is spent.\n"
  "\n"
  "A delegation, a new task and a held task's release are refused, as\n"
  "the spending caps refuse, with the reason. Work already handed over\n"
  "is not stopped.", "0.40.0" },

{ "projects.venture_repo_id", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "The VENTURE repository (a `forge_repo` id) this project's tasks are\n"
  "mirrored against.\n"
  "\n"
  "Only with a venture connector's `mirror_tasks` on. Without it, a\n"
  "task's checkout remotes are matched against VENTURE's repositories;\n"
  "with it, the project's tasks file there whatever their remotes say.\n"
  "0 for none. See docs/venture.org.", "0.44.0" },

{ "projects.venture_milestone_id", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "The VENTURE milestone this project's mirrored tickets are filed under.\n"
  "\n"
  "Its name, due date and state are read once a day and shown in the\n"
  "project's agents' PROJECTS.org and each task's assignment. 0 for none.\n"
  "See docs/venture.org.", "0.46.0" },

{ "projects.venture_goal_id", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "The VENTURE goal this project's mirrored tickets count toward.\n"
  "\n"
  "Its name and progress (current against target) are read once a day\n"
  "and shown with the milestone. 0 for none. See docs/venture.org.",
  "0.46.0" },

{ "teams", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Teams, so a fleet larger than a handful has a shape.\n"
  "\n"
  "An agent names its team in agents.team, and its standing there in\n"
  "agents.team_role. A lead may hand work to the members of its own team;\n"
  "a member may talk to anyone and assign to nobody. The chief of staff\n"
  "sits above all of them and hands work to the leads.\n"
  "\n"
  "Teams are optional. A fleet that declares none behaves exactly as it\n"
  "did before there were any.", "0.1.0" },

{ "teams.id", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "Identifier used to address the team, and what agents.team names.",
  "0.1.0" },

{ "teams.name", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Display name. Defaults to the id.", "0.1.0" },

{ "teams.description", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "What this team is for, and what to send it.\n"
  "\n"
  "Written for a chief-of-staff deciding where a piece of work goes: it\n"
  "reads these to choose a team, so say what the team handles and what it\n"
  "does not, rather than naming the people on it.", "0.1.0" },

{ "teams.color", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Accent colour in the clients, as a hex string.", "0.1.0" },

{ "teams.order", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "0", NULL,
  "Where this team sits in the list, lowest first. Ties keep file order.",
  "0.1.0" },

{ "teams.tags", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Labels that give this team a job in the software, by tag rather than\n"
  "by name, so the team can be called whatever you like.\n"
  "\n"
  "A well-known tag is one a feature looks for: `ephemeral` is the team\n"
  "ephemeral agents join (clawtilla makes one called Ephemeral the first\n"
  "time it needs it; tag a team of your own to use that instead).  Some\n"
  "tags belong to one team only, and a second team carrying one is a\n"
  "fleet warning -- the first in the team list keeps it.  Any other tag\n"
  "is kept and shown, for your own use or a plugin's; a plugin's tags\n"
  "are written `vendor:name`.  A tag is lowercase letters, digits and\n"
  "hyphens.  `clawtilla team tag list --well-known` lists the tags this\n"
  "clawtilla knows.  See docs/team-tags.org.", "0.55.0" },

/* ── integrations ────────────────────────────────────────────────── */
{ "teams.skills", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Skills every agent on this team gets.\n"
  "\n"
  "Between the fleet default and the agent's own list, resolved by the\n"
  "same function as both. The three are additive: a skill named in two\n"
  "of them is one skill, and the most specific place it was asked for is\n"
  "what gets reported.", "0.2.0" },

{ "teams.broker_server", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The broker's gateway server for this team's agents.\n"
  "\n"
  "For anythingmcp, the id of an MCP server made in its dashboard (the\n"
  "last part of the server's MCP URL); its connectors are what the\n"
  "team's agents may use. For jentic, the Jentic agent the team acts\n"
  "as, whose API key `clawtilla broker key team:<id>` stores. An agent\n"
  "with its own agents.broker_server uses that one instead. Unused\n"
  "unless broker.enabled is on.", "0.24.0" },

{ "teams.computer", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Defaults for the computers of this team's agents.", "0.78.0" },

{ "teams.computer.container", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Container limits for this team's container and distrobox agents.\n"
  "\n"
  "Between the fleet's defaults.container and the agent's own\n"
  "computer.container, key by key: a member's own cpus beats the team's,\n"
  "and the team's memory still applies to it. Set from a client with\n"
  "`clawtilla team set <team> computer.container.cpus 4`; running\n"
  "members take the change in place.", "0.78.0" },

{ "teams.computer.container.cpus", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The team's agents.computer.container.cpus.", "0.78.0" },

{ "teams.computer.container.cpu_weight", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The team's agents.computer.container.cpu_weight.", "0.78.0" },

{ "teams.computer.container.cpuset", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The team's agents.computer.container.cpuset.", "0.78.0" },

{ "teams.computer.container.memory", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The team's agents.computer.container.memory.", "0.78.0" },

{ "teams.computer.container.memory_swap", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The team's agents.computer.container.memory_swap.", "0.78.0" },

{ "teams.computer.container.pids_limit", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The team's agents.computer.container.pids_limit.", "0.78.0" },

/* ── computers ───────────────────────────────────────────────────── */
{ "computers", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Machines several agents share.\n"
  "\n"
  "Each entry takes an id and the same keys as an agent's computer:\n"
  "block -- type, container, vm, desktop, mounts, and the rest; every\n"
  "one is documented under agents.computer. An agent joins one with\n"
  "agents.computer.use. Agents A and B on `alpha` and C, D and E on\n"
  "`beta` are two machines, not five.\n"
  "\n"
  "Each agent keeps its own workspace, mounted at\n"
  "/mnt/clawtilla/workspaces/<agent>, and its own exchange folder. The\n"
  "machine stops when its last running agent stops, and is rebuilt only\n"
  "when every agent on it is stopped. A desktop is one screen, driven by\n"
  "one agent at a time.\n"
  "\n"
  "A shared container's container.selinux_level under auto is its\n"
  "agents' team's level when they are all on one team, and podman's\n"
  "random pair when they are not.", "0.5.0" },

{ "computers.id", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "Identifier, and what agents.computer.use names. The machine is called\n"
  "clawt-c.<id> in podman, distrobox and libvirt -- a dot no agent id can\n"
  "hold, so it never shares a name with an agent's own machine.", "0.5.0" },

{ "computers.description", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "What this machine is for.", "0.5.0" },

/* ── browsers ────────────────────────────────────────────────────── */
{ "browsers", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Web browser sessions several agents share.\n"
  "\n"
  "Each entry takes an id and the same keys as an agent's browser:\n"
  "block -- home, allow_scripts, idle_minutes, width, height. An agent\n"
  "joins one with agents.browser.mode: shared and agents.browser.use.\n"
  "Agents on one entry share one profile -- its cookies, its logins, its\n"
  "open page -- and drive it one at a time: a call while another agent\n"
  "drives is refused, naming that agent. Needs a GSURF=1 build.", "0.9.0" },

{ "browsers.id", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "Identifier, and what agents.browser.use names.", "0.9.0" },

{ "browsers.description", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "What this session is for -- say which logins it holds.", "0.9.0" },

/* ── vault ───────────────────────────────────────────────────────── */
{ "vault", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Secrets an agent can have typed into a web page without ever seeing\n"
  "them: passwords, and TOTP seeds the daemon turns into one-time codes.\n"
  "\n"
  "  vault:\n"
  "    - name: github\n"
  "      secret: {command: \"pass show web/github\"}\n"
  "      sites: [github.com]\n"
  "    - name: github-2fa\n"
  "      kind: totp\n"
  "      secret: {file: github-totp}\n"
  "      sites: [github.com]\n"
  "\n"
  "An agent may ask only for the names its agents.browser.secrets lists.\n"
  "It calls clawtilla_secret_ref for a one-use reference bound to it and\n"
  "to the conversation it is in, then clawtilla_browser_fill_secret with\n"
  "that reference and a field. The daemon resolves the value, types it\n"
  "into the field as key input, and the agent hears only that it was\n"
  "filled. Screenshots and clawtilla_browser_eval are refused on that\n"
  "page until the browser navigates away. Needs a GSURF=1 build.", "0.25.0" },

{ "vault.name", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "What an agent asks for, and what agents.browser.secrets lists. The\n"
  "name is all an agent or a client ever sees of the entry's secret.", "0.25.0" },

{ "vault.description", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Whose login this is, or which field it belongs in.", "0.25.0" },

{ "vault.kind", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "password", clawt_vault_kind_get_type,
  "`password` types the value itself. `totp` reads the value as a TOTP\n"
  "seed -- base32, or an otpauth://totp/ URI with its algorithm, digits\n"
  "and period -- and types the RFC 6238 code for the current time.", "0.25.0" },

{ "vault.secret", CLAWT_SCHEMA_SECRET, CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "A secret reference, never the value: {file: ...}, {env: ...} or\n"
  "{command: ...}, resolved the way secrets: says, at the moment of the\n"
  "fill and not before.", "0.25.0" },

{ "vault.sites", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_REQUIRED,
  NULL, NULL,
  "The hosts the secret may be typed into: a page on any other host is\n"
  "refused, so an agent steered to a look-alike site cannot be made to\n"
  "fill it there. `example.com` also matches its subdomains\n"
  "(login.example.com); a page whose address cannot be read is refused.\n"
  "An entry without sites is never filled.", "0.25.0" },

/* ── accounts ────────────────────────────────────────────────────── */
{ "accounts", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Separate logins for an agent CLI, so two agents on one machine can\n"
  "spend two subscriptions.\n"
  "\n"
  "Each account is a directory the CLI keeps its login and settings in;\n"
  "an agent picks one with agents.model.account. Claude Code and the tmux\n"
  "variant use CLAUDE_CONFIG_DIR, codex CODEX_HOME, grok GROK_HOME and\n"
  "opencode its XDG directories. cursor and antigravity keep their login\n"
  "under $HOME and cannot take an account: an agent that names one for\n"
  "them is disabled with that reason rather than run on the default\n"
  "login.\n"
  "\n"
  "The directory is not created. Log in once with `clawtilla account\n"
  "login ID`, which runs the CLI's own login against it.", "0.2.0" },

{ "accounts.id", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "Identifier, and what agents.model.account names.", "0.2.0" },

{ "accounts.provider", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED,
  NULL, NULL,
  "The agent CLI this login is for: claude-code, claude-tmux, codex-cli,\n"
  "grok-build or opencode. An agent may use the account only when its own\n"
  "provider is the same CLI.", "0.2.0" },

{ "accounts.config_dir", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_REQUIRED,
  NULL, NULL,
  "The directory the CLI keeps this login in. ~ is expanded.", "0.2.0" },

{ "accounts.description", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Whose login this is, or what it is for.", "0.2.0" },

{ "accounts.pay_per_use", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "This login bills each use -- an API key -- rather than drawing on a\n"
  "subscription's limit.\n"
  "\n"
  "Only agents.model.fallbacks reads it. Failing over to another sign-in\n"
  "of the same subscription is refused, because rotating accounts to\n"
  "get past a usage limit breaks the provider's terms; a pay-per-use\n"
  "login on the same CLI pays for the work it does and is allowed.", "0.16.0" },

{ "integrations", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Named integrations, each handed to one agent, some agents or all of them.\n"
  "\n"
  "An integration is a connection to something outside the fleet, kept in\n"
  "one place and pointed at whichever agents should have it. That is the\n"
  "difference from the per-agent `integrations:` block inside an agent,\n"
  "which still works and is still the right thing for a connection only\n"
  "that one agent will ever use: this one is configured once and scoped.\n"
  "\n"
  "Which keys below apply depends on `type`. Each names the type it\n"
  "belongs to; a key belonging to another type is ignored rather than\n"
  "rejected, so changing a draft's type does not mean deleting fields\n"
  "first.", "0.2.0" },

{ "integrations.name", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "What this instance is called.\n"
  "\n"
  "Unique across the file, and used everywhere the instance is referred\n"
  "to -- the CLI, the UI, and the key it gets in an agent's .mcp.json. Two\n"
  "instances with the same name is an error rather than a merge, because\n"
  "the second silently winning is how a credential ends up pointing at the\n"
  "wrong account.", "0.2.0" },

{ "integrations.type", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "Which kind: matrix, email, webhook, local, cmacs, mcp, connector,\n"
  "libresearch, grokbot, meshy, pixellab or notify.\n"
  "\n"
  "An unknown type disables that instance and nothing else, with the\n"
  "reason recorded -- the same treatment a shadow agent gets, and for the\n"
  "same reason: a config written by a newer build must survive an older\n"
  "one.", "0.2.0" },

{ "integrations.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether this instance is live at all.\n"
  "\n"
  "Independent of `scope`: this is the switch, scope is the audience.", "0.2.0" },

{ "integrations.scope", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "selected", clawt_scope_get_type,
  "Who gets it: all, selected or none.\n"
  "\n"
  "`all` includes agents created later, which is the point of it -- a tool\n"
  "server the fleet shares should not need revisiting every time the fleet\n"
  "grows. `selected` uses the `agents:` list below. `none` keeps the\n"
  "instance and its credentials without handing it to anybody.", "0.2.0" },

{ "integrations.teams", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Team ids, when `scope: selected`.\n"
  "\n"
  "Beside `agents:` rather than instead of it, and either can match.\n"
  "Naming a team is how an instance covers a group without being\n"
  "rewritten every time somebody joins it.\n"
  "\n"
  "Worth thinking about for a *channel*: two agents sharing one Matrix\n"
  "account answer as the same person, so a team is rarely what you want\n"
  "there. For a tools integration it is the ordinary case.", "0.2.0" },

{ "integrations.agents", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Agent ids, when `scope: selected`.\n"
  "\n"
  "An id that names no agent is a warning rather than an error: an agent\n"
  "removed for the afternoon should not stop the daemon starting.", "0.2.0" },

{ "integrations.per_agent", CLAWT_SCHEMA_MAPPING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Per-agent overrides, keyed by agent id.\n"
  "\n"
  "This is how one instance covers several agents that cannot share an\n"
  "identity. A Matrix account is one login: two agents on the same one\n"
  "both receive every message and both answer as the same user. So the\n"
  "homeserver and the room list live at the top and each agent's own\n"
  "user_id and access_token go here.\n"
  "\n"
  "Any key from this section may be overridden, not only the identity\n"
  "ones -- a room list that differs per agent is an ordinary thing to\n"
  "want.", "0.2.0" },

{ "integrations.description", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "What this is for, in your words.\n"
  "\n"
  "Written into the agent's TOOLS.org beside the integration, so it is\n"
  "the sentence the model reads when deciding whether this is the thing\n"
  "to reach for. Worth filling in for anything whose name is not\n"
  "self-explanatory.", "0.2.0" },

{ "integrations.homeserver", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "matrix: the homeserver base URL, e.g. https://matrix.example.org.", "0.2.0" },

{ "integrations.user_id", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "matrix: the full user id, e.g. @agent:example.org.\n"
  "\n"
  "Must differ per agent. Put it under `per_agent` when the instance\n"
  "covers more than one.", "0.2.0" },

{ "integrations.access_token", CLAWT_SCHEMA_SECRET, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "matrix: the access token, as a secret reference.\n"
  "\n"
  "`clawtilla integration matrix-login` turns a password into one of\n"
  "these without the password ever being written down.", "0.2.0" },

{ "integrations.rooms", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "matrix: room ids to listen in.\n"
  "\n"
  "Empty means every room the account is joined to. Bridged rooms --\n"
  "Discord, Signal, anything else with a Matrix bridge -- are just rooms,\n"
  "so an agent reaches them without knowing they are bridged.", "0.2.0" },

{ "integrations.require_mention", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "matrix: only answer messages that name the agent.\n"
  "\n"
  "Defaults on, unlike a clawtilla room. A Matrix room usually has people\n"
  "in it talking to each other, and an agent that takes a turn on every\n"
  "line of it is both expensive and unbearable.", "0.2.0" },

{ "integrations.imap_host", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "email: IMAP server hostname.", "0.2.0" },

{ "integrations.imap_port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "993", NULL, "email: IMAP port.", "0.2.0" },

{ "integrations.smtp_host", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "email: SMTP server hostname.", "0.2.0" },

{ "integrations.smtp_port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "587", NULL, "email: SMTP port.", "0.2.0" },

{ "integrations.username", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "email: the mailbox to log in as. Must differ per agent.", "0.2.0" },

{ "integrations.password", CLAWT_SCHEMA_SECRET, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "email: the password, as a secret reference.", "0.2.0" },

{ "integrations.folders", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "email: folders to watch. Empty means INBOX.", "0.2.0" },

{ "integrations.port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "webhook: the port to listen on. Must differ per agent -- two agents\n"
  "cannot bind the same one, and the second to start simply fails.", "0.2.0" },

{ "integrations.backend", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "desktop", clawt_notify_backend_get_type,
  "notify: how the notification reaches you.\n"
  "\n"
  "`desktop` raises one on the machine the daemon runs on, which is the\n"
  "wrong answer when the daemon is on a workstation and you are not.\n"
  "`ntfy` and `gotify` reach a phone. `matrix` posts into a room, which\n"
  "on a bridged homeserver means it reaches you wherever you already\n"
  "read messages. `command` runs a program and hands it the text.\n"
  "`apprise` (since 0.22.0) hands it to the apprise CLI, which reaches a\n"
  "hundred-odd services -- Telegram, Signal, email, Pushover, Gotify, SMS\n"
  "-- through one URL scheme; the URL is `apprise_url`.", "0.2.0" },

{ "integrations.token", CLAWT_SCHEMA_SECRET, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "notify: the credential for ntfy, gotify or Matrix, as a secret\n"
  "reference. libresearch: the server's token. grokbot: the routine's\n"
  "webhook key (`crsr_...`), which is the whole of what lets anybody\n"
  "start that bot. meshy and pixellab: the service's API key, which\n"
  "spends that account's credits.\n"
  "\n"
  "Resolved once when the configuration loads rather than on every\n"
  "notification: a `command` reference to a locked password manager\n"
  "blocks until it times out, and doing that each time something needed\n"
  "saying would make the notifier the slowest thing in the daemon.", "0.2.0" },

{ "integrations.apprise_url", CLAWT_SCHEMA_SECRET, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "notify: with `backend: apprise`, the Apprise URL -- or several, one per\n"
  "line -- as a secret reference: `{file: ~/.config/clawtilla/apprise.url}`.\n"
  "\n"
  "A secret because an Apprise URL nearly always carries a credential\n"
  "(`tgram://BOT_TOKEN/CHAT_ID`, `mailtos://user:password@host`). It\n"
  "reaches apprise in a 0600 file passed with `--config`, deleted when\n"
  "apprise exits, and never on its command line, which any user on the\n"
  "machine can read. `command` names the apprise binary when it is not on\n"
  "PATH.", "0.23.0" },

{ "integrations.room", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "notify: with `backend: matrix`, the room id to post into.\n"
  "\n"
  "A room of your own with nobody else in it works well: it keeps the\n"
  "fleet's noise out of a room people are talking in, and still arrives\n"
  "on every device you read Matrix on.", "0.2.0" },

{ "integrations.events", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  "question,error", NULL,
  "notify: what is worth interrupting you for.\n"
  "\n"
  "`question` -- an agent said something to you and is waiting.\n"
  "`done` -- a task finished.\n"
  "`error` -- an agent stopped in a way nobody asked for.\n"
  "`routine` -- a scheduled run failed.\n"
  "`update` -- a newer clawtilla exists.\n"
  "`budget` -- a daily spending cap refused work\n"
  "(`orchestration.daily_fleet_budget_micros`, or an agent's\n"
  "`orchestration.daily_agent_budget_micros`).\n"
  "\n"
  "The default is question and error, which is the whole rule: blocked\n"
  "on you, or broken. A notifier that fires on every turn is one people\n"
  "turn off, and then it is not there for the two that mattered.", "0.2.0" },

{ "integrations.priority", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "normal", NULL,
  "notify: low, normal, high or urgent.\n"
  "\n"
  "Mapped onto whatever the backend understands -- ntfy's five levels,\n"
  "gotify's ten, the two urgency hints a desktop has.", "0.2.0" },

{ "integrations.quiet_hours", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "notify: a local-time range to stay silent in, such as `23:00-07:00`.\n"
  "\n"
  "Silences this instance completely. If you want to be woken for a\n"
  "broken agent but not for a question, that is two instances -- one\n"
  "with quiet hours and one without, each with its own `events` -- which\n"
  "is why instances have names.", "0.2.0" },

{ "integrations.title", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "notify: a title to use instead of the agent's name.\n"
  "\n"
  "Worth setting when several fleets notify the same phone, so a buzz\n"
  "says which machine it came from.", "0.2.0" },

{ "integrations.command", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "mcp: the program to run, spoken to over stdio.\n"
  "\n"
  "This is the general way to give the fleet anything with an MCP server:\n"
  "one entry here reaches every agent in scope, instead of the same block\n"
  "pasted into each agent's .mcp.json by hand.\n"
  "\n"
  "notify: with `backend: command`, the program to run; with\n"
  "`backend: apprise`, the apprise binary when it is not on PATH.", "0.2.0" },

{ "integrations.args", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "mcp: arguments for `command`.", "0.2.0" },

{ "integrations.url", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "mcp: an HTTP or SSE endpoint, instead of `command`.\n"
  "\n"
  "One or the other, never both: an entry naming a command and a URL is\n"
  "two different servers and there is no way to tell which was meant.\n"
  "\n"
  "libresearch: where its server answers. grokbot: the Grok routine's\n"
  "webhook address, as Grok shows it under \"POST to\". meshy and\n"
  "pixellab: empty for the service itself; set only for a proxy or a\n"
  "test server.\n"
  "\n"
  "connector: a remote MCP server, instead of the catalogue's. With\n"
  "`provider: remote` -- or no provider at all -- this is the only thing\n"
  "a connector needs: the server says how to sign in to it (see\n"
  "connectors.discovery).", "0.2.0" },

{ "integrations.env", CLAWT_SCHEMA_MAPPING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "mcp: environment for the server process.\n"
  "\n"
  "A value may be a secret reference, which is resolved when the agent\n"
  "starts and written into its .mcp.json -- a file that is already 0600\n"
  "and already holds the agent's own token.", "0.2.0" },


{ "integrations.provider", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "connector: which service this is an account on.\n"
  "\n"
  "One of the ids from `clawtilla connector catalog`. The catalogue says\n"
  "how to authenticate and, where there is a well-known one, which MCP\n"
  "server fronts it -- so a connector integration is usually a provider,\n"
  "a client id and nothing else.", "0.2.0" },

{ "integrations.account", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "connector: which account on that service, in your own words.\n"
  "\n"
  "Two connectors for the same provider are the ordinary case -- work\n"
  "and personal, or one GitLab org and another -- and the account label\n"
  "is what tells them apart everywhere they are shown.\n"
  "\n"
  "Required, and unique per provider, for every account after the first:\n"
  "adding a second one without a label, or with one another account on\n"
  "that service already uses, is refused. An agent granted several\n"
  "accounts on one service sees each account's tools prefixed with its\n"
  "label -- `work_create_issue`, `personal_create_issue` -- and a call to\n"
  "the bare name is refused rather than run as whichever account it\n"
  "happened to reach.", "0.2.0" },

{ "integrations.instance", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "connector: the host, for a service you run yourself.\n"
  "\n"
  "Only meaningful for a self-hostable provider such as gitlab or\n"
  "forgejo, where the endpoints are paths joined onto this. Left unset\n"
  "it is the provider's flagship host.", "0.2.0" },

{ "integrations.client_id", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "connector: the OAuth application you registered with the provider.\n"
  "\n"
  "clawtilla ships none. It has no application of its own to lend, and\n"
  "borrowing another project's would mean every clawtilla identifying\n"
  "itself as something it is not. Registering one takes a few minutes\n"
  "and `clawtilla connector catalog` says where for each provider.",
  "0.2.0" },

{ "integrations.client_secret", CLAWT_SCHEMA_SECRET, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "connector: the application's secret, if the provider insists on one.\n"
  "\n"
  "Many do not for the device flow, and one that does not must not be\n"
  "sent an empty string -- some providers authenticate that and fail.\n"
  "Leave it unset rather than blank.", "0.2.0" },

{ "integrations.scopes", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "connector: what to ask the provider for, space separated.\n"
  "\n"
  "Defaults to the catalogue's suggestion. Ask for less than that when\n"
  "the agent needs less: a scope granted is a scope every agent sharing\n"
  "this connector has, for as long as the token lives.", "0.2.0" },

{ "integrations.token_file", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "connector: where the credential is kept, mode 0600.\n"
  "\n"
  "Written by the daemon when the flow completes, and named here rather\n"
  "than in the config so that nothing has to put the value itself in a\n"
  "file people keep in git. Set automatically; there is rarely a reason\n"
  "to write it by hand.", "0.2.0" },

{ "integrations.credential_name", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "connector: the environment variable or header the credential goes in.\n"
  "\n"
  "Overrides the catalogue, which is what makes the generic `api-key`\n"
  "and `bearer` providers cover a service that has no entry of its own.",
  "0.2.0" },

{ "integrations.tools", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "connector: the only tools the agent may use, if you want fewer.\n"
  "\n"
  "Empty means every tool the server offers. Naming some is enforced in\n"
  "the relay: the others are removed from the agent's tool list rather\n"
  "than refused when called, because a tool that is advertised and then\n"
  "refused is worse than one that was never there -- the agent plans\n"
  "around it and has to work out from an error that it never had it.",
  "0.2.0" },

{ "integrations.directory", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "auto", clawt_tool_directory_mode_get_type,
  "connector: whether its tools are listed one by one or searched.\n"
  "\n"
  "A server with hundreds of tools puts every description into every\n"
  "model call. As a directory it is three tools instead --\n"
  "<name>_search_tools, <name>_describe_tool and <name>_call_tool -- and\n"
  "the agent looks up what it needs. The search, the description and the\n"
  "call all work from the tools this agent may use, so narrowing `tools:`\n"
  "narrows what can be found as well as what can be called.\n"
  "\n"
  "`auto` makes a directory of a server past\n"
  "connectors.directory_min_tools or connectors.directory_min_chars, for\n"
  "an agent whose CLI does not search tools itself; Claude Code does, so\n"
  "its agents get every tool as before. `always` makes one whatever the\n"
  "size or the CLI, and `never` lists every tool however many there\n"
  "are. See docs/connectors.org, \"Servers with many tools\".", "0.18.0" },

{ "integrations.max_result_bytes", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "-1", NULL,
  "connector: the most of one tool result the agent is shown, in bytes.\n"
  "\n"
  "A result bigger than this is trimmed before the agent reads it -- the\n"
  "text cut on a character boundary, a JSON array cut to the records that\n"
  "fit, structuredContent trimmed the same way -- with a [clawtilla] note\n"
  "at the start and the end saying what was cut. The whole result is kept\n"
  "under the agent's state directory and the agent reads the rest with\n"
  "clawtilla_mcp_result_query. -1 (the default) means the fleet's\n"
  "connectors.max_result_bytes, 0 means no budget for this connector,\n"
  "and anything below 1024 is read as 1024. See docs/connectors.org,\n"
  "\"Results bigger than the budget\".", "0.27.0" },

{ "integrations.show_apps", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "connector: show its apps in chat.\n"
  "\n"
  "An MCP server can ship an interactive view (an MCP App, a ui://\n"
  "resource) for a tool. With this on, the relay tells the server it can\n"
  "show them, reads the view when the tool is called, and the call\n"
  "appears in the conversation as an app: the web client draws it in a\n"
  "sandboxed frame when somebody opens it, and the GTK and lrg clients\n"
  "offer it in the browser. Off by default, because the view is the\n"
  "server's own code running in your browser. See docs/connectors.org,\n"
  "\"Apps in chat\".", "0.27.0" },


/* ── connectors ──────────────────────────────────────────────────── */
{ "integrations.confirm_writes", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_NONE, "true", NULL,
  "For a connector that can stage its writes, whether it does.\n"
  "\n"
  "A staged write becomes a decision in the operator's own inbox rather\n"
  "than something to be found later in the other system's web\n"
  "interface. Turning this off means the agent's writes land\n"
  "immediately and unreviewed.\n"
  "\n"
  "Read by the `venture` connector, which is the one built-in that\n"
  "stages. It does not switch staging on -- `venturectl mcp` decides\n"
  "that for itself and stages by default -- it decides whether\n"
  "clawtilla polls the queue and raises what is waiting as a decision.\n"
  "Turning it off leaves the changes in VENTURE for somebody to find in\n"
  "its own web interface.", "0.2.0" },

{ "integrations.apply_writes", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "off", clawt_apply_writes_get_type,
  "connector (venture): whether the agent's writes wait for approval.\n"
  "\n"
  "`off` keeps every create, update and delete the agent makes in\n"
  "VENTURE staged: VENTURE holds it, and with confirm_writes on it\n"
  "reaches the operator's decision inbox to approve or reject. `all`\n"
  "starts `venturectl mcp --apply-writes`, so every write lands at once,\n"
  "unreviewed, as the agent's own VENTURE account -- opt in only for an\n"
  "account whose memberships already limit what it can reach. With one\n"
  "connector per agent this is per agent.\n"
  "\n"
  "`low_risk` (since 0.43.0) starts `venturectl mcp\n"
  "--apply-writes=low-risk`: ordinary writes -- tickets, comments,\n"
  "worklogs, links -- land at once, and VENTURE still stages anything\n"
  "touching money, people, access or credentials, anything sent outward,\n"
  "and every delete. It needs VENTURE 0.10.0 or later; against an older\n"
  "venturectl it falls back to `off` with a warning, never to `all`.\n"
  "\n"
  "Ignored when `args` is set on the integration: those are the\n"
  "server's arguments, as written. See docs/venture-setup.org, step 2.",
  "0.36.1" },

{ "integrations.mirror_tasks", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "connector (venture): copy the covered agents' tasks into VENTURE\n"
  "tickets.\n"
  "\n"
  "A task is mirrored only when this is on for an agent it involves and\n"
  "its repository matches a VENTURE `forge_repo` -- a checkout remote\n"
  "that /api/v1/forge_repo/resolve knows, or its project's\n"
  "`venture_repo_id`. The delegator's account creates the ticket and\n"
  "the assignee's keeps it current: status, assignee, results and\n"
  "comments. Nothing on an agent's path waits on VENTURE: changes are\n"
  "queued in tasks.db and sent behind. Needs VENTURE 0.10.0 or later.\n"
  "See docs/venture.org.", "0.44.0" },

{ "integrations.service_token_file", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "connector (venture), with mirror_tasks on: the operator's own VENTURE\n"
  "token, for the mirror's writes that no agent makes.\n"
  "\n"
  "Files the operator's own tasks, and tasks whose delegator and assignee\n"
  "have no VENTURE account, as this account. Keep it outside\n"
  "secrets.dir/connectors/ so no agent's box ever mounts it. See\n"
  "docs/venture.org.", "0.46.0" },

{ "integrations.mirror_intake", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "connector (venture), with mirror_tasks on: a ticket assigned to this\n"
  "agent's VENTURE user starts a task for it.\n"
  "\n"
  "Only an internal ticket still `todo` and carrying no other system's\n"
  "reference, at most ten an hour per agent; its text reaches the agent\n"
  "fenced, as data. Anyone who can edit tickets in that VENTURE\n"
  "organization can then hand this agent work: turn it off where that is\n"
  "more than you want. See docs/venture.org.", "0.45.0" },

{ "integrations.poll_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "60", NULL,
  "How often a connector that has something to report is asked.\n"
  "\n"
  "Polling, because the other end is another process with its own\n"
  "event loop. It runs on a timer attached to the daemon's context --\n"
  "never from a request handler, which would make a client wait on\n"
  "somebody else's network.\n"
  "\n"
  "One timer covers every connector, so the shortest interval anybody\n"
  "asked for is the one that runs. The first poll is one interval after\n"
  "the daemon starts, never during it.", "0.2.0" },

{ "integrations.push_replies", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "grokbot: hand each agent reply to the bot as a new run of its routine.\n"
  "\n"
  "When a Grok bot starts a conversation with an agent, the agent's\n"
  "reply is POSTed to the bot's routine, so the bot answers on its own.\n"
  "Each reply is a Grok run -- 200 an hour per routine, and each one\n"
  "billed. Off, replies wait in clawtilla until the bot reads them with\n"
  "clawtilla_grok_replies. A reply Grok refuses waits the same way.",
  "0.11.0" },

{ "connectors", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Connected accounts: how clawtilla obtains credentials and keeps them.\n"
  "\n"
  "A connector integration gives an agent the tools of a service without\n"
  "giving it the credential. The daemon runs the authorization, holds the\n"
  "token, renews it, and injects it into the tool server -- so the value\n"
  "is never in the agent's .mcp.json, its environment or its transcript.\n"
  "\n"
  "What that is worth depends on where the agent runs. For a container\n"
  "or VM agent the boundary is real: the token file and the relay are on\n"
  "the host and the agent cannot reach either. For an unconfined host\n"
  "agent it is protection against leaking a credential by accident, not\n"
  "against one that goes looking -- see docs/security.org.", "0.2.0" },

{ "connectors.dir", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  "$XDG_CONFIG_HOME/clawtilla/connectors.d", NULL,
  "Directory of extra connector definitions.\n"
  "\n"
  "Each `.yaml` file holds a `connectors:` list, and an entry whose id\n"
  "matches a built-in one replaces it. These are somebody else's URLs:\n"
  "a provider that moves an endpoint should be fixable here rather than\n"
  "by waiting for a clawtilla release.", "0.2.0" },



{ "connectors.refresh_margin_seconds", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "300", NULL,
  "How long before expiry to renew a token.\n"
  "\n"
  "An access token often lasts an hour, and one that expires while the\n"
  "request carrying it is in flight fails exactly like a wrong one --\n"
  "the agent sees only a refusal from a service it was using a minute\n"
  "ago.", "0.2.0" },

{ "connectors.redirect_port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "8765", NULL,
  "Loopback port for the authorization-code flow's redirect.\n"
  "\n"
  "Only used by providers with no device flow. It has to be a fixed\n"
  "number rather than whatever the kernel offers, because the redirect\n"
  "URI is registered with the provider in advance and most match it\n"
  "exactly: register `http://127.0.0.1:8765/callback`.\n"
  "\n"
  "Bound on 127.0.0.1 only. A redirect carries an authorization code in\n"
  "a URL, and a listener on every interface offers that code to whoever\n"
  "on the network reaches the port first.\n"
  "\n"
  "A client clawtilla registers for itself by dynamic client registration\n"
  "is registered with this redirect, so changing the port means signing\n"
  "in again.", "0.2.0" },

{ "connectors.discovery", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether a remote connector finds out for itself how to sign in.\n"
  "\n"
  "A connector reached by URL whose catalogue entry names no endpoints\n"
  "asks the server: its RFC 9728 protected-resource document names an\n"
  "authorization server, whose RFC 8414 metadata names the endpoints,\n"
  "and RFC 7591 dynamic client registration supplies a client id when\n"
  "the connector has none. Endpoints a catalogue entry names by hand\n"
  "always win, and so does a configured `client_id`.\n"
  "\n"
  "Only when somebody asks for a sign-in -- never at daemon start. No\n"
  "request in it follows a redirect, and nothing it finds may be plain\n"
  "http unless the server itself is on loopback. Off, such a connector\n"
  "takes a pasted token only.", "0.20.0" },

{ "connectors.client_name", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "clawtilla", NULL,
  "What clawtilla calls itself when it registers with a server.\n"
  "\n"
  "Sent as `client_name` in a dynamic client registration, so it is what\n"
  "the server's consent screen shows the person approving it -- worth\n"
  "setting to something that says which machine is asking when several\n"
  "fleets sign in to one service.", "0.20.0" },

/* ── routines ────────────────────────────────────────────────────── */
{ "connectors.registry_enabled", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_COMMENTED, "false", NULL,
  "Whether the open MCP registry is imported into the catalogue.\n"
  "\n"
  "Off by default because it reaches the network. When on, the fetch\n"
  "happens on a timer into a cache on disk -- never at daemon start and\n"
  "never from a request handler, so a client pressing a button waits on\n"
  "this machine and not on somebody else's. `connector.registry_refresh`\n"
  "asks for one sooner than the timer would.\n"
  "\n"
  "An imported entry only ever fills a gap: it never replaces a\n"
  "built-in connector or one from connectors.dir, which are both a\n"
  "choice somebody already made on purpose.", "0.2.0" },

{ "connectors.registry_url", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_COMMENTED,
  "https://registry.modelcontextprotocol.io", NULL,
  "Base URL of the MCP registry to import from.\n"
  "\n"
  "Somewhere else entirely is a legitimate answer: the registry is an\n"
  "ordinary HTTP service and an internal mirror serves the same\n"
  "shape.", "0.2.0" },

{ "connectors.registry_refresh_hours", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_COMMENTED, "24", NULL,
  "How often the cached registry listing is refreshed.\n"
  "\n"
  "Checked against the cache's own `fetched_at`, not against when the\n"
  "daemon last started -- a restart must not reset this clock, or a\n"
  "fleet that bounces every hour would refresh every hour regardless of\n"
  "what this key says.", "0.2.0" },

{ "connectors.directory_min_tools", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "40", NULL,
  "More tools than this makes a connector's server a directory.\n"
  "\n"
  "Counted after the connector's `tools:` narrowing, because that is the\n"
  "list the agent would otherwise read. Forty is where a tool list starts\n"
  "to cost more than it helps -- and API-model engines refuse more than\n"
  "128 tools a turn across every server they have. Only read when a\n"
  "connector's `directory` is `auto`.", "0.18.0" },

{ "connectors.directory_min_chars", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "100000", NULL,
  "More characters of tool definitions than this makes a directory too.\n"
  "\n"
  "The names, descriptions and input schemas together, which is what the\n"
  "model actually reads on every call. A server with thirty tools can\n"
  "still be a hundred thousand characters when each one carries a page of\n"
  "documentation; about where Claude Code starts searching its own tools\n"
  "rather than listing them. Only read when a connector's `directory` is\n"
  "`auto`.", "0.18.0" },

{ "connectors.max_result_bytes", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "65536", NULL,
  "The most of one connector tool result an agent is shown, in bytes.\n"
  "\n"
  "For every connector that does not set max_result_bytes itself. Past\n"
  "it the result is trimmed, announced at both ends, and kept whole\n"
  "under the agent's state directory for clawtilla_mcp_result_query, so\n"
  "one listing of a big repository does not spend the whole context. 0\n"
  "turns the budget off; anything below 1024 is read as 1024. Whatever\n"
  "this says, the relay holds at most 64 MiB of one message in memory --\n"
  "anything longer goes to disk as it arrives.", "0.27.0" },

{ "connectors.apps_web_url", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "http://127.0.0.1:8790", NULL,
  "Where the web client is reached, for an MCP app's Open in browser.\n"
  "\n"
  "The GTK and lrg clients cannot draw an app's HTML, so they link to the\n"
  "web client's view of it: <this>/a/<agent>/app/<app>. The address\n"
  "clawtilla-web listens on by default; change it when the web client\n"
  "runs elsewhere or on another port.", "0.27.0" },

/* ── broker ──────────────────────────────────────────────────────── */
{ "broker", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "An integration broker clawtilla runs for you, in podman.\n"
  "\n"
  "Ready-made app integrations -- hundreds of SaaS and business systems\n"
  "-- behind one MCP gateway on this machine. clawtilla pulls the\n"
  "pinned image, keeps the gateway's database, keys and token under\n"
  "<daemon.state_dir>/broker/, starts it with the daemon and keeps it\n"
  "running. Each agent or team names its own gateway server in\n"
  "agents.broker_server or teams.broker_server and reaches it as a\n"
  "connector, through the same relay as every other connector.\n"
  "\n"
  "Not the provider-key broker of runtime.network: gated, which is a\n"
  "different thing with a similar name. See docs/connectors.org,\n"
  "\"An integration broker\".", "0.24.0" },

{ "broker.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Run the broker.\n"
  "\n"
  "Off by default, because turning it on pulls an image of several\n"
  "hundred megabytes and opens ports on this machine. Started in the\n"
  "background when the daemon starts or this is turned on, never in the\n"
  "way of either: broker.status says starting while the image pulls.", "0.24.0" },

{ "broker.type", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "anythingmcp", clawt_broker_type_get_type,
  "Which broker: anythingmcp or jentic.\n"
  "\n"
  "anythingmcp is an MCP gateway (AGPL-3.0) whose adapters turn REST,\n"
  "SOAP, GraphQL and SQL services into tools, with OAuth through your\n"
  "own client. jentic is Jentic One (Apache-2.0, a public beta whose\n"
  "schemas may change), a credential broker that checks a per-agent\n"
  "rule before it injects a stored key. Jentic One's own caveat is\n"
  "that an agent running as the same OS user can read its state, so it\n"
  "is refused unless every agent using it has runtime.confine: bwrap.\n"
  "Its telemetry and release check are always switched off.", "0.24.0" },

{ "broker.image", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "The gateway's image, pinned to a tag.\n"
  "\n"
  "Empty means the image this release was checked against:\n"
  "docker.io/helpcodeai/anythingmcp:v0.54.0 for anythingmcp and\n"
  "ghcr.io/jentic/jentic-one-app:0.44.0 for jentic. An image with no\n"
  "tag, or tagged latest, is refused: an upgrade should be a change\n"
  "somebody made. AnythingMCP publishes amd64 only, so an aarch64\n"
  "machine names a locally built one here -- docs/connectors.org says\n"
  "how.", "0.24.0" },

{ "broker.database_image", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "The database image beside an AnythingMCP gateway, pinned to a tag.\n"
  "\n"
  "Empty means docker.io/library/postgres:17.12-alpine. The database\n"
  "publishes no port: the two containers share a unix socket under\n"
  "<daemon.state_dir>/broker/run. Jentic One keeps SQLite files and\n"
  "runs no database container, so this is unused for jentic.", "0.24.0" },

{ "broker.bind", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "127.0.0.1", NULL,
  "The address the gateway's ports are published on.\n"
  "\n"
  "Loopback by default: the relay that reaches it runs on this machine.\n"
  "Anything wider puts the gateway's dashboard and MCP endpoint on that\n"
  "network, and the dashboard's own sign-in is all that guards it.", "0.24.0" },

{ "broker.port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "47400", NULL,
  "The host port of the gateway's MCP endpoint and API.\n"
  "\n"
  "For jentic this is also its dashboard, which shares the port.", "0.24.0" },

{ "broker.dashboard_port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "47300", NULL,
  "The host port of AnythingMCP's dashboard.\n"
  "\n"
  "Where you register the first (admin) account, sign in to SaaS apps\n"
  "with your own OAuth clients and make a gateway server for each agent\n"
  "or team. Unused for jentic, whose dashboard is on broker.port.", "0.24.0" },

{ "broker.connection", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Which podman runs the broker, as computer.container.connection says.\n"
  "\n"
  "Empty is this user's rootless podman. A remote podman runs the\n"
  "containers there while broker.bind and the relay still dial this\n"
  "machine, so a remote one also needs a tunnel to its ports.", "0.24.0" },

{ "broker.restart", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "always", clawt_restart_policy_get_type,
  "What to do when a broker container stops without being asked to.\n"
  "\n"
  "always starts it again however it ended. on-failure starts it again\n"
  "only when it exited non-zero or was killed for running out of\n"
  "memory; a clean exit, a container that never ran and one removed\n"
  "from podman are left down. never leaves it down whatever the exit.\n"
  "A container left down leaves the broker failed, and broker status\n"
  "says how it ended and why it was not restarted. on-failure needs\n"
  "podomation 1.0.3 or later, whose inspect reports the exit code;\n"
  "with an older one it cannot tell a failure and restarts nothing.\n"
  "Five restarts within ten minutes stop the retries, so a gateway\n"
  "that cannot start is reported rather than restarted for ever.", "0.24.0" },

/* ── grok ────────────────────────────────────────────────────────── */
{ "grok", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Handing work to Grok bots, and hearing back.\n"
  "\n"
  "Each bot is a `grokbot` integration named for it -- `chief`,\n"
  "`research` -- whose url is a Grok routine's webhook. A job is POSTed\n"
  "there with a callback address minted for it, and the bot reports\n"
  "back to the daemon's webhook listener over the tailnet, by HTTP or\n"
  "by the clawtilla MCP tools. These keys are about that return path.\n"
  "See docs/grok.org.", "0.10.0" },

{ "grok.callback_host", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The name a Grok bot uses to reach this daemon.\n"
  "\n"
  "Empty means the machine's own tailnet name, asked of tailscaled as\n"
  "soon as the daemon starts -- the FQDN `tailscale status` shows for\n"
  "it -- and a job sent before it answers waits for the answer. This is\n"
  "an override, for a machine whose tailscaled the daemon's user cannot\n"
  "read or a name other than its MagicDNS one. Either way the listener\n"
  "(daemon.webhook_enabled) has to be bound on the tailnet\n"
  "(daemon.tailscale): a job is refused rather than sent with an\n"
  "address nothing answers on.", "0.10.0" },

{ "grok.job_ttl_minutes", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "120", NULL,
  "How long a job may go unanswered before it is given up.\n"
  "\n"
  "Longer than the half hour an unattended Grok run's approval card\n"
  "waits, because a run that stalls on one is the usual reason a\n"
  "report is late. When it lapses the agent is told, with Grok's run\n"
  "id, and a report arriving afterwards is refused. A job may ask for\n"
  "less or more; this is the default.", "0.10.0" },

{ "grok.attention_days", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "3", NULL,
  "Days a job that needs somebody keeps counting on the Grok badge.\n"
  "\n"
  "A job that failed, was refused, expired or asked for input needs\n"
  "somebody. It counts on the badge for this many days after it\n"
  "settled, then stops asking -- it stays listed until it is removed\n"
  "or grok.retention_days passes. 0 means it counts until it is\n"
  "removed. Clamped to 0..365.", "0.52.0" },

{ "grok.retention_days", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "30", NULL,
  "Days a settled job is kept for the Grok page and `grok jobs`.\n"
  "\n"
  "Pruned at start and then hourly. A job still open is never pruned:\n"
  "it expires through grok.job_ttl_minutes first, and its agent is\n"
  "told. Clamped to 1..3650.", "0.52.0" },

{ "notices", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The notification list the daemon keeps for every client.\n"
  "\n"
  "Errors and notices -- a refused message, a turn that timed out, an\n"
  "agent that stopped -- are recorded in <state_dir>/notices.db, read\n"
  "and dismissed there, and drawn by every client from notice.list. Each\n"
  "agent can be muted. These keys say how much of it is kept. See\n"
  "docs/clients.org, \"Notifications\".", "0.52.0" },

{ "notices.keep", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "500", NULL,
  "How many notices are kept, newest first.\n"
  "\n"
  "Read and dismissed ones count too: the list is a record, and a\n"
  "dismissed row is still there with include_dismissed. Pruned at start\n"
  "and then hourly. 0 keeps every one, until notices.keep_days.", "0.52.0" },

{ "notices.keep_days", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "30", NULL,
  "Days a notice is kept.\n"
  "\n"
  "Pruned at start and then hourly, whichever of this and notices.keep\n"
  "drops a notice first. 0 keeps them until notices.keep.", "0.52.0" },

/* ── office (0.56.0) ─────────────────────────────────────────────── */
{ "office", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The office: the fleet drawn as people in a building.\n"
  "\n"
  "The daemon folds what it sees -- turn edges, tool steps, messages,\n"
  "decisions, incidents -- into an activity for every agent, and every\n"
  "client draws who is where from it; with OFFICE=1 the office itself is\n"
  "drawn in 2D or 3D. Nothing is invented: an agent at its desk is one\n"
  "the daemon saw start a turn. See docs/office.org.", "0.56.0" },

{ "office.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE, "false", NULL,
  "Whether the daemon keeps the office: the presence fold, the\n"
  "office.presence events and the office.* requests. Off, nothing is\n"
  "computed, and every client's Office page says how to turn it on.\n"
  "Takes effect on the next reload.", "0.56.0" },

{ "office.world", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, "modern-office", NULL,
  "Which world pack to draw: a directory name under one of the world\n"
  "directories (office.world_dirs, <state_dir>/office/worlds, then the\n"
  "shipped data/office/worlds). Two ship: modern-office and cozy-town.\n"
  "`clawtilla office worlds` lists what is available. See\n"
  "docs/office-world-packs.org.", "0.56.0" },

{ "office.renderer", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, "auto", NULL,
  "How a renderer should draw the world: `2d`, `3d`, or `auto` -- 3D\n"
  "when the pack has a level, otherwise 2D. A renderer's own option\n"
  "(clawtilla-office --2d) wins over this.", "0.56.0" },

{ "office.world_dirs", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Extra directories holding world packs, searched before the daemon's\n"
  "own and the shipped ones, so your copy of a pack replaces theirs.", "0.56.0" },

{ "office.stale_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "600", NULL,
  "How long a turn that was never seen to end is believed. A process\n"
  "that dies mid-turn never closes its turn edge; after this long its\n"
  "agent goes back to the lounge rather than sitting at its desk for\n"
  "ever.", "0.56.0" },

{ "office.meeting_min_members", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "3", NULL,
  "How many members a group room needs for a round in it to be drawn\n"
  "as a meeting. Fewer than 3 is read as 3: two agents talking is a\n"
  "call, not a meeting.", "0.56.0" },

{ "office.choreography", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "A YAML file of choreography rules -- which zone, pose, prop and emote\n"
  "each activity gets -- applied over the built-in rules and the world\n"
  "pack's own. `clawtilla office choreography` prints the built-in rules,\n"
  "which are also the format. See docs/office-choreography.org.", "0.56.0" },

{ "office.asset_api", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "A service the daemon makes characters with itself, with no model\n"
  "deciding what to ask: `comfyui` (self-hosted, free: office.comfyui_url\n"
  "and office.comfyui_workflow); `higgsfield` (Higgsfield's REST API, a\n"
  "key pair stored with `clawtilla office api-key higgsfield`, billed to\n"
  "the API balance in Higgsfield's console); `higgsfield-mcp` (Higgsfield's\n"
  "MCP server through a connector's sign-in -- office.asset_generator, or\n"
  "the only higgsfield connector -- billed to the account's app credits,\n"
  "the ones the website and the hf CLI spend); `pixellab` (a pixel-art\n"
  "character in four directions with a walk cycle in each, at\n"
  "office.pixel_size; a key stored with `clawtilla office api-key\n"
  "pixellab`, or any agent's pixellab integration); `meshy` (a rigged 3D\n"
  "model that walks, for the 3D view: see office.asset_api_3d); or empty\n"
  "for none.\n"
  "The two Higgsfield roads are billed separately: choose the one your\n"
  "money is in.\n"
  "`clawtilla office character AGENT --direct` makes one; a paid service's\n"
  "estimate is put to you as a decision and nothing is spent until you\n"
  "approve.\n"
  "See docs/office-custodial-team.org.", "0.60.0" },

{ "office.asset_api_3d", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "A service that makes each character a 3D model as well: `meshy`, or\n"
  "empty for none.\n"
  "\n"
  "Meshy makes a model from the agent's picture (or, with none, its\n"
  "description) and rigs it; the rig comes with a walking clip, which the\n"
  "3D view plays when the character walks. Beside office.asset_api, so\n"
  "one request can make a PixelLab sprite for the 2D view and a Meshy\n"
  "model for the 3D one. The key is `clawtilla office api-key meshy` or\n"
  "any agent's meshy integration; the price, in Meshy credits, is put to\n"
  "you before anything runs. Meshy deletes what it made after three days;\n"
  "the office keeps its copy.", "0.76.0" },

{ "office.pixel_size", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "48", NULL,
  "The size, in pixels a side, of a character PixelLab draws for the\n"
  "office (office.asset_api: pixellab): 16 to 128. The 2D office's tiles\n"
  "are 32 pixels, so 48 stands a head taller than a desk.", "0.76.0" },

{ "office.mesh_polycount", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "8000", NULL,
  "How many polygons Meshy aims for in a character's model\n"
  "(office.asset_api_3d: meshy): 100 to 300000. A fleet's worth of\n"
  "characters is drawn at once, so the default is light.", "0.76.0" },

{ "office.comfyui_url", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "http://127.0.0.1:8188", NULL,
  "The ComfyUI server office.asset_api: comfyui makes characters with --\n"
  "your own, self-hosted. A token for an auth proxy in front of it is\n"
  "stored with `clawtilla office api-key comfyui`.", "0.61.0" },

{ "office.comfyui_workflow", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The ComfyUI workflow characters are made with, exported in API format,\n"
  "with {{prompt}} where the description goes and, if you like,\n"
  "{{image}} (the agent's picture, uploaded), {{seed}} and\n"
  "{{negative_prompt}}. See docs/office-custodial-team.org.", "0.61.0" },

{ "office.asset_model", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The model office.asset_api makes characters with, in the service's own\n"
  "spelling. Empty is each road's own default: for `higgsfield` the REST\n"
  "endpoint path higgsfield-ai/soul/v2/standard, for `higgsfield-mcp` the\n"
  "MCP catalogue's nano_banana_2 -- the two name models differently, and\n"
  "one road's name is refused by the other.",
  "0.60.0" },

{ "office.asset_generator", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The connector the office's assets and characters are made with: the\n"
  "name of one you added (`clawtilla connector add hf --provider\n"
  "higgsfield`, or a self-hosted ComfyUI). Set, creating an agent while\n"
  "the office is on files a task to the custodial team (or the chief) to\n"
  "make its character from its picture, and installing the custodial team\n"
  "makes every call to it ask you first. Empty, nothing is generated.\n"
  "See docs/office-custodial-team.org.", "0.58.0" },

{ "office.peers", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Other fleets whose people visit this office, each `NAME=WHERE`:\n"
  "WHERE is the other daemon's socket on this machine\n"
  "(`work=/run/user/1000/work/d.sock`) or its tailnet address\n"
  "(`studio=100.64.0.7:8792`), paired once with `clawtilla office\n"
  "peer-pair NAME CODE` using a code `clawtilla device pair` printed on\n"
  "that machine. Their people are drawn as visitors, read-only: nothing\n"
  "they send can act here, and each fleet publishes only its own people,\n"
  "so two fleets that peer with each other never echo. Asked only once a\n"
  "client is drawing the office. See docs/office.org#federation.\n"
  "\n"
  "Superseded by linking fleets (`clawtilla federation link`), whose people\n"
  "visit without the owner-level device token this needs, as each fleet's\n"
  "grant allows. A fleet both linked and listed here is drawn once, from\n"
  "the link. Still read; it will go in a later release.",
  "0.64.0" },

{ "office.peer_poll_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "5", NULL,
  "How often, in seconds, each office.peers fleet is asked who is where.\n"
  "A fleet that has not answered for three of these is drawn empty, not\n"
  "frozen.", "0.64.0" },

{ "federation", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Federation: this fleet linked with other clawtilla fleets -- yours on\n"
  "another machine, or other people's.\n"
  "\n"
  "Each fleet has a key; every request between fleets is signed by it and\n"
  "answered signed, whatever carries it: a Cloudflare tunnel (https://),\n"
  "a tailnet or LAN address (http://, or clawtilla:// for the daemon's\n"
  "own IPC port), or a socket on this machine. A linked fleet has a tier\n"
  "-- owned, trusted, limited, untrusted -- that presets what it may do\n"
  "here, with tags and rules to adjust it. Peers themselves are kept in\n"
  "<state_dir>/federation/peers.yaml and managed with `clawtilla\n"
  "federation`, not edited here. See docs/federation.org.", "0.66.0" },

{ "federation.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE, "false", NULL,
  "Whether this daemon links with other clawtilla fleets -- yours on\n"
  "another machine, or a colleague's. On, it keeps a fleet key\n"
  "(<secrets.dir>/federation/fleet.ed25519, whose fingerprint becomes the\n"
  "fleet id), answers on federation.listen and, with federation.ipc, on its\n"
  "own IPC port, and dials every linked fleet that has an endpoint. Every\n"
  "request between fleets is signed, whatever carries it. Off, none of\n"
  "that exists. Takes effect at the next daemon start. See\n"
  "docs/federation.org.", "0.66.0" },

{ "federation.name", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "What other fleets call this one: shown on their sidebar section and in\n"
  "invites. Empty is the machine's host name.", "0.66.0" },

{ "federation.owner", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Who runs this fleet, as other fleets see it: a name, not an account.\n"
  "Their `owner:` filters and rules match it.", "0.66.0" },

{ "federation.public_url", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "The endpoint invites tell other fleets to reach this one at:\n"
  "`https://fleet.example.com` behind a Cloudflare tunnel or a TLS proxy,\n"
  "`http://100.64.0.7:7711` for the federation listener on a tailnet, or\n"
  "`clawtilla://100.64.0.7:8792` for this daemon's own IPC port. Empty,\n"
  "an invite names the tailnet IPC address when federation.ipc is on and\n"
  "the daemon is on a tailnet, and otherwise asks for this to be set.\n"
  "`clawtilla federation expose --cloudflare` prints a tunnel config and\n"
  "the value to put here.", "0.66.0" },

{ "federation.listen", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Where the HTTPS/WebSocket federation listener answers, each `unix:PATH`\n"
  "or `HOST:PORT`. Empty is `unix:<state_dir>/federation.sock`, which\n"
  "cloudflared reaches directly (`service: unix:...`) and nothing else on\n"
  "the network can. A HOST:PORT must be loopback, a tailnet address, or\n"
  "-- with federation.allow_lan_plaintext -- a LAN address: the listener\n"
  "speaks plain HTTP and leaves TLS to whatever stands in front of it. A\n"
  "listener that cannot bind is a warning, not a failed start.", "0.66.0" },

{ "federation.ipc", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE, "true", NULL,
  "Whether other fleets may also speak to this daemon over its own IPC\n"
  "port (daemon.tcp_address and the tailnet address) -- the simple\n"
  "daemon connection: `clawtilla://HOST:PORT`, no extra port, no HTTP. A\n"
  "connection that says `fed.hello` is a peer and may only send\n"
  "federation frames; it never sees the event stream or any other\n"
  "request.", "0.66.0" },

{ "federation.allow_lan_plaintext", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE, "false", NULL,
  "Whether a link may use a local-network address (RFC 1918, link-local,\n"
  "a name that is not localhost or *.ts.net) without TLS. Signed\n"
  "envelopes keep it authentic either way; federation.seal keeps it\n"
  "private. Plain text to a public address is refused whatever this says.",
  "0.66.0" },

{ "federation.seal", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE, "auto", clawt_fed_seal_get_type,
  "Whether bodies between fleets are encrypted with a key the two agree\n"
  "(X25519 + ChaCha20-Poly1305) on top of whatever carries them: `auto`\n"
  "on plain-text LAN links only, `always` on every link (so a tunnel's edge\n"
  "sees only ciphertext), `never`.", "0.66.0" },

{ "federation.inbox_agent", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The agent a peer reaches when its grant allows only the inbox\n"
  "(untrusted fleets, by default). Empty is the chief of staff.", "0.66.0" },

{ "federation.invite_ttl_hours", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "72", NULL,
  "How long an invite can be used, in hours, unless the invite says\n"
  "otherwise. An invite is single-use.", "0.66.0" },

{ "federation.clock_skew_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "300", NULL,
  "How far another fleet's clock may be from this one's before its\n"
  "signed requests are refused as stale. Each nonce is remembered for\n"
  "this long, across restarts.", "0.66.0" },

{ "federation.max_body_bytes", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "1048576", NULL,
  "The largest request another fleet may send, in bytes. Refused before\n"
  "it is read whole.", "0.66.0" },

{ "federation.ping_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "30", NULL,
  "How often each linked fleet is pinged to keep its link health\n"
  "current. Three missed answers mark it offline.", "0.66.0" },

{ "federation.reconnect_max_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "300", NULL,
  "The longest wait between attempts to reach a fleet that is not\n"
  "answering; the wait doubles from one second up to this.", "0.66.0" },

{ "federation.outbox_hours", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "72", NULL,
  "How long a message to a linked fleet that cannot be reached waits in\n"
  "the outbox (<state_dir>/federation/federation.db) before it is given up,\n"
  "with a note in its conversation saying so. It is sent the moment the\n"
  "fleet answers again; nothing is dropped silently.", "0.68.0" },

{ "federation.audit_retention_days", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, "90", NULL,
  "How long the record of every request between fleets\n"
  "(<state_dir>/federation/federation.db) is kept.", "0.66.0" },

{ "federation.rules", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Grant overrides for every linked fleet a selector matches, applied in\n"
  "order after the fleet's tier and before its own overrides, each\n"
  "`SELECTOR: field=value; field=value`. A selector names fleets by\n"
  "`tier:`, `tag:`, `owner:`, `name:`, `online`, with `!`, `+` (and) and\n"
  "`,` (or): `tag:client-acme: reach=listed; agents=team:frontend;\n"
  "cap.exec=deny`. `clawtilla federation explain ALIAS` shows which rule\n"
  "set what. See docs/federation.org#rules.", "0.66.0" },

{ "routines", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Standing work: a prompt, an agent, and when to run it.\n"
  "\n"
  "A routine is the thing you would otherwise remember to ask for every\n"
  "morning. It runs as a delegated task, so it has its own result.\n"
  "\n"
  "It does not get a session of its own. libreclaw keys a session on\n"
  "channel, room and sender and deliberately ignores the thread, so a\n"
  "routine lands in the operator's session and queues behind whatever\n"
  "the operator is doing -- and inherits that conversation's context.\n"
  "Point a routine at an agent that is not also somewhere you chat, or\n"
  "give it `isolate` so it runs in a room of its own.\n"
  "\n"
  "Routines only fire while the daemon is running. A machine that was\n"
  "asleep at nine o'clock has missed nine o'clock, and the routine says\n"
  "so rather than pretending otherwise -- see `catch_up`.", "0.2.0" },

{ "routines.id", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "What this routine is called, and how it is referred to.", "0.2.0" },

{ "routines.description", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "One line about what it is for, for a person reading the list.", "0.2.0" },

{ "routines.instructions", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED,
  NULL, NULL,
  "What the agent is asked to do, in full.\n"
  "\n"
  "Sent as the whole of the prompt, so it should read as an instruction\n"
  "rather than a reminder: the agent has no memory of the last run and\n"
  "nobody is there to answer a question about what was meant.", "0.2.0" },

{ "routines.agent", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED,
  NULL, NULL,
  "Which agent runs it.", "0.2.0" },

{ "routines.model", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The model this routine's run uses, instead of the agent's own.\n"
  "\n"
  "For that one turn only: the agent's next turn is back on its own\n"
  "model. A scheduled check that reads a page and compares a number does\n"
  "not need the model the agent plans with, and a run costs what its\n"
  "model costs. A model name for the agent's provider, as in\n"
  "`agents.model.model`; like that key it is not checked against the\n"
  "catalogue, which goes stale, so a name the provider does not know\n"
  "fails the run rather than the save. Empty is the agent's own model.", "0.7.7" },

{ "routines.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether it runs on its schedule. A disabled routine can still be\n"
  "started by hand, which is how you try one before trusting it.", "0.2.0" },

{ "routines.schedule", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "daily", clawt_schedule_get_type,
  "manual, hourly, daily, weekdays, weekly or custom.\n"
  "\n"
  "All of them become a cron expression, including the presets -- so\n"
  "there is one answer to \"when does this next run\" rather than two.", "0.2.0" },

{ "routines.at", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "09:00", NULL,
  "The time of day, for daily, weekdays and weekly.\n"
  "\n"
  "Local time, because somebody who wrote 09:00 means nine o'clock where\n"
  "they are. With `hourly` only the minute is used.", "0.2.0" },

{ "routines.weekday", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "monday", NULL,
  "Which day, for `weekly`.", "0.2.0" },

{ "routines.cron", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The expression, for `custom`: minute hour day-of-month month day-of-week.\n"
  "\n"
  "Ranges, lists and steps all work, and months and weekdays may be\n"
  "named. Note the oldest oddity in cron: when *both* day fields are\n"
  "restricted the match is day-of-month OR day-of-week, so `0 0 13 * 5`\n"
  "is the thirteenth and every Friday, not Friday the thirteenth.", "0.2.0" },

{ "routines.directory", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Where to run, if not the agent's workspace.\n"
  "\n"
  "A path on the agent's computer, which for a container or a VM is a\n"
  "path inside it and not on the host.", "0.2.0" },

{ "routines.worktree", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Run in a fresh checkout of `directory` rather than in it.\n"
  "\n"
  "For a routine that changes files: it keeps a scheduled run from\n"
  "landing on top of whatever you had checked out at the time. Since\n"
  "0.39.0 clawtilla makes the checkout itself, for each run, in the\n"
  "agent's workspace at tasks/<task> on the branch clawtilla/<task> (a\n"
  "git worktree, or a local clone for a confined or containerised agent),\n"
  "and tells the agent where it is; the run fails if it cannot be made.\n"
  "The run's Changes show what it did, and the checkout is removed after\n"
  "orchestration.checkout_keep_days with its branch kept.", "0.2.0" },

{ "routines.isolate", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Give this routine a conversation of its own.\n"
  "\n"
  "A run is sent from `user` to the agent by default, so it lands in the\n"
  "operator's room from the operator's sender -- and libreclaw keys a\n"
  "session on channel, room and sender. One session, one queue: Monday's\n"
  "run is in Tuesday's context, a 09:00 brief starts whenever the agent\n"
  "next goes idle, and both happen in the operator's transcript.\n"
  "\n"
  "Set this and the run goes to a room of its own from a sender of its\n"
  "own, which is a session of its own: no shared context, no waiting\n"
  "behind a conversation. The cost is that the prompt and the answer are\n"
  "no longer in the operator's chat -- they are in the task result and on\n"
  "the Flow tab, which is where a delegated task's output already lives.\n"
  "\n"
  "Off by default because moving somebody's output is not a thing to do\n"
  "to them silently. Pointing the routine at an agent nobody talks to\n"
  "solves the same problem and needs no setting.", "0.1.0" },

{ "routines.read_only", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Run with read-only tools: the agent may look at anything it can reach\n"
  "and change nothing -- no messages, no writes, no commands, no screen.\n"
  "\n"
  "The run gets a room of its own (as routines.isolate does) and every\n"
  "turn in that room is offered only clawtilla's read tools; its own\n"
  "tools are refused through the PreToolUse hook. That hook exists only\n"
  "for claude-code and claude-tmux, so a read-only routine on any other\n"
  "backend is refused rather than run with its own tools unchecked.\n"
  "What it finds it reports with clawtilla_ask_decision.", "0.4.0" },

{ "routines.proactive", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Look for ways to help, unasked, and report them without acting.\n"
  "\n"
  "Implies read_only. With no instructions, the agent is told to go\n"
  "through what it is connected to -- its integrations and connectors --\n"
  "for anything that needs attention, and to file each finding as a\n"
  "decision for the operator rather than acting on it.", "0.4.0" },

{ "routines.catch_up", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Run once at startup when its time passed while the daemon was down.\n"
  "\n"
  "Off by default, and worth leaving off for anything that talks to\n"
  "somebody: a laptop opened after a long weekend would otherwise deliver\n"
  "a stack of good mornings at once. Only ever one run, however many\n"
  "were missed.", "0.2.0" },

{ "routines.jitter_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "Delay each run by up to this many seconds, chosen at random.\n"
  "\n"
  "Zero here, unlike a hosted scheduler, because there is no shared\n"
  "server to spread load across: this is your machine, and a routine set\n"
  "for 09:00 should run at 09:00. Worth setting only when several\n"
  "routines share one rate-limited service.\n"
  "\n"
  "Only a scheduled run is delayed. `clawtilla routine run` starts at\n"
  "once, because somebody waiting at a terminal is not load to spread;\n"
  "so does a catch-up, because a run that was already missed does not\n"
  "need to be later still.",
  "0.2.0" },

{ "routines.max_start_failures", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "3", NULL,
  "Turn the routine off after this many runs in a row whose computer\n"
  "did not start. 0 never turns it off.\n"
  "\n"
  "A dead VM or a missing image otherwise fails the routine on every\n"
  "tick, and nobody hears about it. At the limit the routine is turned\n"
  "off -- `enabled: false`, so it shows as off everywhere and is turned\n"
  "back on the same way -- and an alert names the routine and the\n"
  "computer's error. The count is run state, kept beside the config and\n"
  "never in it. Any run whose computer starts sets it back to zero, and\n"
  "so does editing the routine; a run that began before the edit is not\n"
  "counted. A negative value reads as the default.",
  "0.13.0" },

{ "routines.locked", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Keep the agent from changing or removing this routine.\n"
  "\n"
  "An agent with tools.schedule may adjust the routines that run on it --\n"
  "move a check, split one, drop one it no longer needs. Lock the ones\n"
  "that are yours to decide: it can still read a locked routine, and it\n"
  "is told why a change was refused rather than left guessing. Only a\n"
  "client or the config can set or clear this; an agent cannot.", "0.7.1" },

/* ── agents ──────────────────────────────────────────────────────── */
/* ── triggers ────────────────────────────────────────────────────── */
{ "triggers", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Work started by something happening elsewhere.\n"
  "\n"
  "A routine is a clock; a trigger is an event. Both end in the same\n"
  "queued run against the same agent, so a trigger inherits the\n"
  "routine's ordering behind a busy agent and its durable receipts\n"
  "rather than growing a second set.", "0.2.0" },

{ "triggers.id", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "How the trigger is named on the command line and in the clients.\n"
  "\n"
  "Not the address it is called on: that is a separate unguessable\n"
  "endpoint, so renaming a trigger does not tell anybody where it\n"
  "lives.", "0.2.0" },

{ "triggers.description", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "What this trigger is for, in a line.", "0.2.0" },

{ "triggers.agent", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED,
  NULL, NULL,
  "Primary agent to run when it fires.", "0.2.0" },

{ "triggers.agents", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Additional recipients for the same delivery. The primary agent runs first;\n"
  "duplicates are removed in order. Every recipient has a separate durable\n"
  "task and outcome. A batch must fit the trigger's max_pending slots in\n"
  "full.", "0.2.0" },

{ "triggers.room", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Where the run reports, if not the agent's own conversation.",
  "0.2.0" },

{ "triggers.mode", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "run", clawt_trigger_mode_get_type,
  "What a delivery that passed every filter turns into: run or post.\n"
  "\n"
  "run starts the agent with the expanded instructions, which costs a\n"
  "turn. post writes the instructions and the event into `room` as a\n"
  "system message and runs nothing -- for the deploy that finished or\n"
  "the monitor that fired, which somebody wants to see and no agent\n"
  "needs to act on. A post wakes nobody and costs nothing.\n"
  "\n"
  "post needs `room`: a trigger set to post with no room is refused at\n"
  "load, and refuses every delivery until one is named.", "0.15.0" },

{ "triggers.max_pending", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "4", NULL,
  "How many runs this trigger may have unfinished at once, 1 to 50.\n"
  "\n"
  "A push that fires a five-minute run, twenty times while the first is\n"
  "still going, is twenty agents on one machine. A delivery past the cap\n"
  "is answered 429, so the sender retries it later, and recorded as\n"
  "dropped in the trigger's deliveries -- never silently. Every recipient\n"
  "of a fan-out takes a slot. A value outside 1 to 50 written by hand is\n"
  "read as the default, with a warning at load.", "0.15.0" },

{ "triggers.provider", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "generic", clawt_trigger_provider_get_type,
  "Who is calling: forgejo, gitea, github, gitlab, venture or generic.\n"
  "\n"
  "This decides how the delivery is authenticated, and the four forges\n"
  "genuinely disagree. Forgejo and Gitea sign the body with HMAC-SHA256\n"
  "and send the hex bare; GitHub sends the same digest behind a\n"
  "`sha256=` prefix; GitLab sends the secret itself, verbatim, and\n"
  "signs nothing.\n"
  "\n"
  "Naming it matters because Forgejo also sends GitHub- and\n"
  "Gitea-shaped headers for compatibility. Sniffing is a fallback for\n"
  "when nobody said, and it can never widen what a configured trigger\n"
  "accepts.\n"
  "\n"
  "venture is VENTURE's own webhook: the same `sha256=` HMAC as GitHub\n"
  "under X-Venture-Signature, so the webhook must be created with this\n"
  "trigger's secret as its signing secret. Its record type fills\n"
  "{{kind}}, the record's label {{title}}, its id {{number}} and the\n"
  "user who made the change {{actor}}.", "0.2.0" },

{ "triggers.secret", CLAWT_SCHEMA_SECRET, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The shared secret the caller proves it knows.\n"
  "\n"
  "Shown once, when it is created or rotated, and never again -- not in\n"
  "a listing, a log line, an event or a transcript. Rotating it stops\n"
  "the old one working immediately.", "0.2.0" },

{ "triggers.events", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Which event names are acted on. Empty means all of them.\n"
  "\n"
  "An event outside the list is answered normally and recorded as\n"
  "ignored, because a caller that gets an error for a delivery you\n"
  "simply did not want will keep retrying it.", "0.2.0" },

{ "triggers.repo", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Only act when the delivery names this repository.", "0.2.0" },

{ "triggers.branch", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Only act when the delivery names this branch.", "0.2.0" },

{ "triggers.header", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "For `provider: generic`, the header carrying the event name.",
  "0.2.0" },

{ "triggers.ignore_actors", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Changes made by these users are recorded as ignored, not run.\n"
  "\n"
  "The loop guard. An agent that writes what its own trigger watches --\n"
  "a VENTURE ticket it updates, a branch it pushes -- is called back by\n"
  "its own write, and the sender cannot tell: to it every write is an\n"
  "ordinary user's change. Name the account the agent's credential\n"
  "belongs to. Matched against {{actor}}, ignoring case.", "0.9.3" },

{ "triggers.match", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Fields the delivery's JSON body must contain, or it is ignored.\n"
  "\n"
  "  match:\n"
  "    - path: pull_request.title\n"
  "      value: release\n"
  "    - path: labels.name\n"
  "      value: deploy\n"
  "\n"
  "Up to 10 pairs, and all of them must match. The path is dotted; a\n"
  "number indexes an array (`commits.0.message`), and a name on an array\n"
  "looks in every element (`labels.name`). The value must be contained in\n"
  "what the path holds, ignoring case. Checked before anything is queued:\n"
  "a miss is recorded in the deliveries as ignored, naming the pair that\n"
  "failed, and starts no run. Empty, or null, means no filter.", "0.15.0" },

{ "triggers.match.path", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED,
  NULL, NULL,
  "Where in the JSON body to look, dotted, such as `repository.name`.\n"
  "No spaces, no `=` and no empty part; a bad path is refused at load.",
  "0.15.0" },

{ "triggers.match.value", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED,
  NULL, NULL,
  "Text the field must contain, ignoring case. An object or array is\n"
  "searched as its JSON text.", "0.15.0" },

{ "triggers.replay_connector", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "venture: the connector whose credential reads the business-event\n"
  "feed, so deliveries missed while the daemon was down are caught up.\n"
  "\n"
  "The name of a `venture` connector integration bound to this\n"
  "trigger's agent. Its token must belong to an owner or administrator\n"
  "of the organization, which is who VENTURE lets read the feed. The\n"
  "feed is read every poll interval and replayed through the same\n"
  "deduplication as a pushed delivery, on the event's stable id, so a\n"
  "delivery that arrives both ways runs once.\n"
  "\n"
  "The first read only finds where the feed ends: replaying an\n"
  "organization's whole history into an agent is never what somebody\n"
  "turning this on meant. Only the durable lane (`business_events` on\n"
  "the VENTURE webhook) has a feed; ordinary record webhooks are never\n"
  "retried by VENTURE and cannot be caught up.", "0.9.3" },

{ "triggers.organization_id", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "venture: which organization's business events to replay.\n"
  "\n"
  "The `organization_id` VENTURE scopes every request by. 0 sends none,\n"
  "which VENTURE answers for the token's default organization.", "0.9.3" },

{ "triggers.instructions", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED,
  NULL, NULL,
  "What the agent is asked to do, with the event fenced beneath it.\n"
  "\n"
  "Placeholders -- {{event}}, {{repo}}, {{ref}}, {{actor}}, {{title}},\n"
  "{{url}}, {{number}}, {{kind}} -- are expanded by hand rather than by a format\n"
  "function, because this string comes out of a configuration file and\n"
  "a stray percent sign must be a percent sign.\n"
  "\n"
  "The payload arrives marked as untrusted, and the agent is told to\n"
  "read it as data: a webhook body is somebody else's text.", "0.2.0" },

{ "triggers.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether it fires.\n"
  "\n"
  "A new trigger starts unverified: the first authenticated delivery is\n"
  "captured and shown to you rather than run, so you can see what the\n"
  "caller actually sends before an agent acts on it.", "0.2.0" },

{ "triggers.directory", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Where the run should work, if somewhere particular.", "0.2.0" },

{ "triggers.worktree", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Run in a fresh checkout of `directory` rather than in it.\n"
  "\n"
  "Since 0.39.0 clawtilla makes the checkout for each run, as for\n"
  "routines.worktree: in the agent's workspace at tasks/<task>, on the\n"
  "branch clawtilla/<task>, and tells the agent where it is above the\n"
  "trigger's payload. The run fails if it cannot be made.", "0.2.0" },

{ "triggers.isolate", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Give the run a conversation of its own rather than the agent's.",
  "0.2.0" },

{ "agents", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The fleet.\n"
  "\n"
  "Each entry is one libreclaw agent: its persona, its model, its\n"
  "credentials, its integrations and its computer. This file is the source\n"
  "of truth -- the daemon renders each agent's libreclaw config.yaml into\n"
  "its workspace from what is here, and regenerates it when this changes.\n"
  "\n"
  "An agent whose block cannot be understood becomes a shadow: it is\n"
  "listed, it says why, and it refuses to run. That is what lets a config\n"
  "written by a newer clawtilla round-trip through an older one without\n"
  "losing anything.", "0.1.0" },

{ "agents.id", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "Identifier used everywhere else to address this agent. Must be unique.", "0.1.0" },

{ "agents.name", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Display name shown in the clients and used as the agent's own name.", "0.1.0" },

{ "agents.description", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "One line about what this agent is for.\n"
  "\n"
  "Other agents see it when they list the fleet, so it is how a\n"
  "chief-of-staff decides who to delegate to. Worth writing properly.", "0.1.0" },

{ "agents.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether this agent may run at all.", "0.1.0" },

{ "agents.chief_of_staff", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Marks the agent that receives work addressed to the fleet.\n"
  "At most one agent may hold this.", "0.1.0" },

{ "agents.color", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Accent colour in the clients, as a hex string.", "0.1.0" },

{ "agents.face", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The generated face the agent wears when it has no profile picture.\n"
  "\n"
  "Unset, the agent gets a shape picked from its id -- the same one in\n"
  "every client, every time. Set it to choose: pebble, cloud, cushion,\n"
  "clover, star, bean, flower, drop, gem, petal, puff, squircle, wedge\n"
  "or bloom. initials keeps the letter disc instead. The face is\n"
  "painted in agents.color, and a profile picture always takes its\n"
  "place. An unknown name is a warning and the automatic face is used.",
  "0.53.0" },

{ "agents.order", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "Where this agent sits in the list, lowest first.\n"
  "\n"
  "Set by dragging a row in the clients rather than written by hand,\n"
  "though there is nothing wrong with writing it. Agents sharing a\n"
  "number keep the order they appear in this file, so leaving them all\n"
  "at 0 changes nothing.",
  "0.1.0" },

{ "agents.review_by", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Who checks this agent's work before it counts as done.\n"
  "\n"
  "`lead` (its team's lead, or the chief of staff when the team has\n"
  "none), `delegator` (whoever handed it the task), `operator` (you,\n"
  "from the decision inbox or any client's task page), an agent id, or\n"
  "empty for no review. When the agent finishes a task it goes to\n"
  "`review` with the result instead of `completed`, and only an accept\n"
  "completes it; asking for changes sends it back with the notes, and\n"
  "after orchestration.review_rounds of that the operator decides. A\n"
  "delegation's own `review:` and an operator task's `review` override\n"
  "this. The reviewer is never the agent itself: a setting that would\n"
  "make it so means no review.", "0.38.0" },

{ "agents.team", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Which team this agent belongs to, by teams.id.\n"
  "\n"
  "Unset means no team: the agent still messages and is messaged, it is\n"
  "simply not grouped and no team lead may assign to it.", "0.1.0" },

{ "agents.broker_server", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "This agent's own gateway server on the broker.\n"
  "\n"
  "For anythingmcp, the id of an MCP server made in its dashboard (the\n"
  "last part of the server's MCP URL); the connectors assigned to that\n"
  "server are this agent's permission, and clawtilla reaches it as the\n"
  "connector broker-agent-<id>. For jentic, the Jentic agent this one\n"
  "acts as, whose API key `clawtilla broker key <id>` stores. Unset\n"
  "means the team's teams.broker_server, or no broker at all. An agent\n"
  "with runtime.network gated or none cannot reach the gateway on\n"
  "loopback and is not given it.", "0.24.0" },

{ "agents.team_role", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "member", clawt_team_role_get_type,
  "Standing within that team.\n"
  "\n"
  "A lead may hand work to the members of its own team and to nobody\n"
  "else. A member may message, ask and share a room with anyone --\n"
  "handing something over in conversation is not assigning it -- but may\n"
  "not delegate at all.\n"
  "\n"
  "At most one lead per team; a second is refused rather than picked\n"
  "between. The chief of staff outranks every lead and needs no team.",
  "0.1.0" },

{ "agents.avatar", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The agent's profile picture. Optional, and usually unnecessary.\n"
  "\n"
  "A file called profile-picture.png, .jpg, .jpeg or .webp in the\n"
  "agent's own directory is found without being configured, which is\n"
  "the expected way to set one: drop the file in and it appears. This\n"
  "key is for pointing somewhere else, and is resolved against the\n"
  "workspace when it is relative.\n"
  "\n"
  "A path named here that does not exist warns and falls back to the\n"
  "initials, rather than falling back silently -- a mistyped path and\n"
  "no picture at all otherwise look identical.\n"
  "\n"
  "The picture reaches a client as bytes, never as a path, because a\n"
  "client may be running on another machine.", "0.1.0" },

{ "agents.skills", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Skills this agent gets, on top of its team's and the fleet's.\n"
  "\n"
  "Named, not paths: a skill lives once under `skills.dir` and is\n"
  "linked into the workspace at whatever path this agent's provider\n"
  "reads. A selector naming a skill that does not exist warns rather\n"
  "than silently reaching nobody.\n"
  "\n"
  "An imported skill arrives disabled and stays that way until somebody\n"
  "has read it, so assigning one is not the same as the agent having it.\n"
  "Both states are reported.", "0.2.0" },

{ "agents.workspace", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Workspace directory. Defaults to defaults.workspace_root/<id>.\n"
  "Scaffolded on first start if it does not exist.", "0.1.0" },

{ "agents.persona", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Who the agent is.", "0.1.0" },

{ "agents.persona.system_prompt", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "System prompt, inline. Mutually exclusive with identity_files.", "0.1.0" },

{ "agents.persona.identity_files", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Files in the workspace concatenated into the system prompt, in order.\n"
  "\n"
  "Leave it unset to get the standard set, which is what every workspace\n"
  "is scaffolded with: SOUL.org for character, IDENTITY.org for role,\n"
  "USER.org for who you are, AGENTS.org for how to work, TOOLS.org for\n"
  "what the agent has, TOOL_GOTCHAS.org for what has already bitten it,\n"
  "PROJECTS.org for the work. Splitting them means one can be edited\n"
  "without rewriting the rest -- `clawtilla agent edit <id>` opens them.\n"
  "\n"
  "Set it to take control of the list: to reorder, to drop one, or to add\n"
  "a file of your own. An inline system_prompt replaces the lot.", "0.1.0" },

{ "agents.prompt_suffix", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Text appended to every turn this agent runs.\n"
  "\n"
  "For a rule that has to hold on turn 200 as much as turn 1. A resumed\n"
  "session never re-receives the system prompt, and a long conversation\n"
  "drifts away from anything said only once at the start.\n"
  "\n"
  "clawtilla already adds one of its own for an agent with a computer,\n"
  "naming the container or VM and telling it to run commands there\n"
  "rather than on the host. Yours is appended to that, not instead of\n"
  "it.\n"
  "\n"
  "Costs tokens on every turn, so keep it to rules that genuinely have\n"
  "to be repeated -- anything else belongs in the agent's org files.",
  "0.1.0" },

{ "agents.memory", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The agent's long-term memory file.\n"
  "\n"
  "MEMORY.md in the workspace is loaded into the system prompt every turn,\n"
  "under a budget. memory/<topic>.md files are read on demand by the agent\n"
  "instead, so a large memory does not cost every turn.", "0.1.0" },

{ "agents.memory.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_INERT,
  "true", NULL,
  "Whether MEMORY.md is loaded into the prompt.\n"
  "\n"
  "Not implemented in this build. No memory block is rendered into the\n"
  "agent's config.yaml at all, so MEMORY.md is loaded on libreclaw's own\n"
  "defaults and this cannot turn it off.", "0.1.0" },

{ "agents.memory.max_lines", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_INERT,
  "200", NULL,
  "Line budget for MEMORY.md. Beyond it the file is truncated with a note.\n"
  "\n"
  "Not implemented in this build, for the same reason as memory.enabled:\n"
  "the budget never reaches libreclaw, which applies its own.", "0.1.0" },

{ "agents.memory.max_bytes", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_INERT,
  "24000", NULL,
  "Byte budget for MEMORY.md, applied alongside max_lines.\n"
  "\n"
  "Not implemented in this build, for the same reason as memory.enabled:\n"
  "the budget never reaches libreclaw, which applies its own.", "0.1.0" },

{ "agents.model", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Which model this agent thinks with.", "0.1.0" },

{ "agents.model.provider", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Provider name. Defaults to defaults.provider.", "0.1.0" },

{ "agents.model.model", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Model name. Defaults to defaults.model.", "0.1.0" },

{ "agents.model.effort", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Reasoning effort: low, medium, high, xhigh or max, when the model has one.", "0.1.0" },

{ "agents.model.command", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The command line an acp agent is started from, e.g.\n"
  "\"gemini --experimental-acp\". Only read when model.provider is acp.\n"
  "\n"
  "acp runs any program that speaks the Agent Client Protocol (v1,\n"
  "JSON-RPC over stdio) as the agent's engine, so which program is this\n"
  "setting. It is split like a shell would split it, with no shell: no\n"
  "pipes, no variables. Left empty, libreclaw falls back to\n"
  "$AI_ACP_COMMAND in the agent's environment, and a turn with neither\n"
  "fails saying so.\n"
  "\n"
  "The model is the agent's own default: ACP has no request that picks\n"
  "one, so model.model is not sent and the program's own configuration\n"
  "(or its command line) chooses.", "0.28.0" },

{ "agents.model.account", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Which entry of accounts this agent's CLI logs in as. Unset is the CLI's\n"
  "default login. The account's provider must be this agent's provider.", "0.2.0" },

{ "agents.model.routing_profile", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_INERT, NULL, NULL,
  "Reserved. Not implemented in this build: nothing reads it, and\n"
  "nothing will until it can mean something here.\n"
  "\n"
  "libreclaw's smart routing (off, eco, auto, premium, free) picks a\n"
  "model per message by scoring the prompt against a price table of\n"
  "named providers. An agent here runs one CLI backend with one model,\n"
  "so there is nothing for a profile to choose between -- rendering it\n"
  "would switch on a router with a single candidate. Moving off a\n"
  "backend that refuses is model.fallbacks; a cheaper model for one\n"
  "scheduled job is routines.model.", "0.1.0" },

{ "agents.model.fallbacks", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Where this agent's conversations continue, in order, when its own\n"
  "backend refuses a turn: a usage limit, a rate limit, a sign-out or\n"
  "an outage.\n"
  "\n"
  "  fallbacks:\n"
  "    - provider: codex-cli\n"
  "      model: gpt-5.5\n"
  "    - provider: claude-code\n"
  "      model: sonnet\n"
  "\n"
  "libreclaw does the failing over: the next turn runs on the first\n"
  "entry nothing is holding, and the agent returns to its own backend\n"
  "at the reset the provider named, or after a backoff when it named\n"
  "none. A move to another CLI or another login starts a fresh\n"
  "conversation there; the agent's own conversation resumes when it\n"
  "comes back. Each switch raises an agent.failover event and alert,\n"
  "and agent status shows the fallback until the agent is back.\n"
  "\n"
  "Another sign-in to the same subscription is refused: an entry on\n"
  "this agent's CLI (claude-code and claude-tmux are one Claude login)\n"
  "with a different account, unless that account has pay_per_use set.\n"
  "Rotating accounts to get past a usage limit breaks the provider's\n"
  "terms. The agent is a shadow with the reason until the list is\n"
  "fixed, and agent set refuses such a list where it arrives.\n"
  "\n"
  "On the command line the list is one string:\n"
  "`clawtilla agent set ID model.fallbacks \"codex-cli:gpt-5.5,\n"
  "claude-code:sonnet@backup\"` -- provider:model, then @account.", "0.16.0" },

{ "agents.model.fallbacks.provider", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "The agent CLI: one of the providers model.provider takes.", "0.16.0" },

{ "agents.model.fallbacks.model", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_REQUIRED, NULL, NULL,
  "The model that CLI is asked for.", "0.16.0" },

{ "agents.model.fallbacks.account", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Which entry of accounts this fallback's CLI logs in as. Unset is the\n"
  "CLI's default login. The account's provider must be this entry's\n"
  "provider.", "0.16.0" },

{ "agents.steer", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "delegator", clawt_steer_policy_get_type,
  "Which messages may be handed to the turn this agent is already\n"
  "running, instead of waiting to become a turn of their own.\n"
  "\n"
  "Without steering, a message for a busy agent waits and becomes its\n"
  "next turn: a correction (\"stop, use the staging DB\") arrives after\n"
  "the work it was meant to change has finished, and a follow-up costs a\n"
  "whole extra turn that rereads everything. A steered message reaches\n"
  "the running turn at the next point its engine accepts input -- before\n"
  "the next model call -- and the turn's closing answer answers it too.\n"
  "Nothing is killed or restarted.\n"
  "\n"
  "never: nothing steers. delegator, the default: a message steers when\n"
  "its sender handed out the work the running turn is on -- the agent\n"
  "that turn is answering, or the delegator of the task it is working.\n"
  "peers: as delegator, and any other agent's message in the same room.\n"
  "all: as peers, and your own message in your conversation with this\n"
  "agent (otherwise held and sent when the turn ends).\n"
  "\n"
  "No level lifts these: a message never steers into another room's\n"
  "turn; never into a consultation unless the consulting agent is asking\n"
  "again, and a consultation never steers into a turn that can act; a\n"
  "result held for the end of a turn, a message naming another task or\n"
  "a model of its own, and anything in a group room take turns of their\n"
  "own. A steered message is still a hop and still passes the loop guard\n"
  "when it is sent.\n"
  "\n"
  "Only some engines can be steered: Claude Code, claude-tmux and codex\n"
  "on libreclaw's ai.codex.transport app-server. On any other the agent\n"
  "says no and the message waits as before. Needs libreclaw 0.30.0 or\n"
  "later; an older agent is never asked.", "0.28.0" },

{ "agents.session", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "How this agent's conversations map to AI sessions.", "0.2.0" },

{ "agents.session.routing_mode", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "sender-room", lc_routing_mode_get_type,
  "How many context windows this agent is.\n"
  "\n"
  "sender-room, the default, gives every (room, sender) pair a session\n"
  "of its own: the conversation with the operator, the conversation\n"
  "with each peer, each a separate AI context that shares nothing with\n"
  "the others. room drops the sender, so everybody in one room shares\n"
  "a session. agent is one session for everything -- every room, every\n"
  "sender, one conversation.\n"
  "\n"
  "The default suits an agent that serves several people and should\n"
  "keep them apart. It is the wrong shape for an orchestrator: a chief\n"
  "of staff told \"only use oryx\" in the operator's room has never\n"
  "heard that constraint when a peer's reply wakes the sibling session,\n"
  "and what looks like one agent contradicting itself is three contexts\n"
  "that have never met. Give such an agent agent mode. The cost is\n"
  "concurrency -- one session runs one turn at a time, so three busy\n"
  "rooms take turns -- and the mode changes every session key, so each\n"
  "conversation starts a fresh context once when it is switched.\n"
  "\n"
  "The values are libreclaw's own (its session.routing_mode); the\n"
  "daemon renders this straight through.\n"
  "\n"
  "The default is role-dependent: an agent marked chief_of_staff --\n"
  "on the agent or through orchestration.chief_of_staff -- or given\n"
  "team_role: lead defaults to agent mode, because an orchestrator's\n"
  "job assumes the shape. An explicit value here always wins, and\n"
  "dropping the role restores the ordinary default; toggling a role\n"
  "never writes this key on your behalf.\n"
  "\n"
  "A member of a room with more than two members defaults to room\n"
  "mode instead, and sender-room is refused for one. That mode gives\n"
  "an agent a session per speaker in the same room, and every piece\n"
  "of the daemon's per-room turn state is keyed on the room alone --\n"
  "the typing indicator carries the room and not the session, so two\n"
  "such turns cannot be told apart: the second to start would run\n"
  "holding the first one's depth and origin, and the first to finish\n"
  "would settle both. It is reported and replaced with room rather\n"
  "than refused, because refusing would take the agent out of the\n"
  "fleet over a setting.", "0.2.0" },

{ "agents.session.timeout_minutes", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "30", NULL,
  "Minutes of inactivity before libreclaw drops this agent's session.\n"
  "\n"
  "The next message after the timeout starts a fresh context rather\n"
  "than continuing the old one. -1 means never expire: the session\n"
  "lives as long as the agent does, and the tradeoff is one context\n"
  "that only ever grows until the provider compacts it or somebody\n"
  "runs /reset. 0 and anything below -1 are meaningless to libreclaw's\n"
  "own session manager, which takes only a positive number of minutes\n"
  "or -1: `clawtilla config validate` rejects such a value, and if one\n"
  "reaches the daemon anyway it is reported with a warning and\n"
  "replaced with 30 in the rendered config.\n"
  "\n"
  "This is libreclaw's own value (its session.timeout_minutes); the\n"
  "daemon renders this straight through.", "0.7.5" },

{ "agents.session.max_sessions", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "10", NULL,
  "How many sessions this agent keeps live at once.\n"
  "\n"
  "Every conversation the routing mode tells apart is a session -- each\n"
  "room under room, each (room, sender) pair under sender-room -- and\n"
  "with timeout_minutes: -1 a session is never reaped, so an agent that\n"
  "speaks in many rooms fills this quickly: the operator's room, each\n"
  "peer, each task room, each linked fleet. Past the limit libreclaw\n"
  "parks the least recently used idle session to make room, and resumes\n"
  "its conversation when that room speaks again; a message that finds\n"
  "every session mid-turn waits for one to finish. Raise this for an\n"
  "agent that should keep more conversations warm at once -- each live\n"
  "session can run a turn, so it is also how many turns this agent may\n"
  "have running together.\n"
  "\n"
  "A positive whole number. `clawtilla config validate` rejects 0, a\n"
  "negative number or one too large for libreclaw, and if one reaches\n"
  "the daemon anyway it is reported with a warning and replaced with 10\n"
  "in the rendered config.\n"
  "\n"
  "This is libreclaw's own value (its session.max_sessions); the daemon\n"
  "renders this straight through.", "0.77.2" },

{ "agents.session.max_wait_queue_depth", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "32", NULL,
  "How many channel messages may wait for a session slot when every\n"
  "session is busy.\n"
  "\n"
  "Past this depth the newest message is refused, not the oldest: the\n"
  "oldest has waited longest and keeps its place. The sender is told the\n"
  "agent is busy. 0 does not wait at all -- a message that finds every\n"
  "session busy is refused at once, and the sender still gets the notice.\n"
  "\n"
  "This is not libreclaw's session.max_queue_depth, which is the message\n"
  "queue inside one session while its turn runs.\n"
  "\n"
  "A whole number from 0 to 2147483647. `clawtilla config validate`\n"
  "rejects a negative number or one too large for libreclaw, and if one\n"
  "reaches the daemon anyway it is reported with a warning and replaced\n"
  "with 32 in the rendered config.\n"
  "\n"
  "This is libreclaw's own value (its session.max_wait_queue_depth); the\n"
  "daemon renders this straight through.", "0.78.0" },

{ "agents.session.max_queue_wait_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "300", NULL,
  "How many seconds a channel message may wait for a session slot.\n"
  "\n"
  "A waiting message older than this is refused, with the same busy\n"
  "notice as one refused for want of room in the line. 0 is no age\n"
  "limit.\n"
  "\n"
  "A whole number from 0 to 2147483647. `clawtilla config validate`\n"
  "rejects a negative number or one too large for libreclaw, and if one\n"
  "reaches the daemon anyway it is reported with a warning and replaced\n"
  "with 300 in the rendered config.\n"
  "\n"
  "This is libreclaw's own value (its session.max_queue_wait_seconds);\n"
  "the daemon renders this straight through.", "0.78.0" },

{ "agents.session.max_parked", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "How many parked conversations libreclaw keeps for this agent.\n"
  "\n"
  "A conversation is parked when max_sessions makes libreclaw give up its\n"
  "live session; it is resumed when its room speaks again. Past this\n"
  "count the oldest parked conversation is forgotten. 0 keeps all of\n"
  "them.\n"
  "\n"
  "A whole number from 0 to 2147483647. `clawtilla config validate`\n"
  "rejects a negative number or one too large for libreclaw, and if one\n"
  "reaches the daemon anyway it is reported with a warning and replaced\n"
  "with 0 in the rendered config.\n"
  "\n"
  "This is libreclaw's own value (its session.max_parked); the daemon\n"
  "renders this straight through.", "0.78.0" },

{ "agents.runtime", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "How this agent's libreclaw instance is hosted.", "0.1.0" },

{ "agents.runtime.type", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "process", clawt_runtime_type_get_type,
  "process or embedded.\n"
  "\n"
  "process supervises a real libreclaw child: crashes stay contained, the\n"
  "environment and credentials are genuinely separate, and the agent can\n"
  "live inside its own container. embedded runs it inside the daemon,\n"
  "which is cheaper and is what an in-process host such as cmacs wants, at\n"
  "the cost of sharing a fate with every other embedded agent.", "0.1.0" },

{ "agents.runtime.confine", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  NULL, clawt_runtime_confine_get_type,
  "Run this agent's own CLI inside a bubblewrap sandbox: none or bwrap.\n"
  "Defaults to defaults.confine.\n"
  "\n"
  "computer.host.confine governs clawtilla_computer_exec and nothing\n"
  "else. The agent's own Bash, Read and Write belong to the AI CLI that\n"
  "libreclaw spawns, and without this they reach the whole filesystem as\n"
  "the daemon's user. bwrap wraps the libreclaw child -- and so the CLI,\n"
  "every command it runs and its MCP servers -- in one sandbox that\n"
  "binds the workspace and the agent's state directory read-write, the\n"
  "CLI's login directory read-write (it refreshes its OAuth tokens there),\n"
  "the agent's mounts as declared, the programs its .mcp.json names\n"
  "read-only, and the daemon's sockets. /usr and /etc are read-only, /tmp\n"
  "and $HOME are private and empty apart from those binds, and TMPDIR,\n"
  "HOME and XDG_* are set by the sandbox -- an agent's env: cannot point\n"
  "them somewhere else. The network is left alone.\n"
  "\n"
  "Only runtime.type: process can be confined; bwrap with embedded\n"
  "disables the agent. bubblewrap missing disables it too, with the\n"
  "reason, and it is never started unconfined instead. Takes effect at\n"
  "the agent's next start.", "0.14.0" },

{ "agents.runtime.confine_paths", CLAWT_SCHEMA_STRING_LIST,
  CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "More paths to bind read-only into a confined agent's sandbox.\n"
  "\n"
  "The sandbox finds libreclaw, the AI CLI and the programs in .mcp.json\n"
  "for itself. A CLI installed somewhere it cannot follow -- a node\n"
  "under ~/.nvm, a tool a skill shells out to -- is named here. Read-only\n"
  "on purpose: something the agent should write is a mount, which says\n"
  "so in its mode. Ignored unless runtime.confine is bwrap.", "0.14.0" },

{ "agents.runtime.network", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "open", clawt_runtime_network_get_type,
  "Where this agent's own process may connect: open, gated or none.\n"
  "\n"
  "open shares the machine's network, LAN and tailnet included. gated\n"
  "gives the sandbox no network of its own: its only route is\n"
  "clawtilla's egress gate, an HTTP proxy reaching public addresses only\n"
  "(plus egress.allow), handed to the CLI as HTTPS_PROXY and HTTP_PROXY.\n"
  "none gives it no route at all.\n"
  "\n"
  "Under gated or none, provider keys whose CLI can be pointed elsewhere\n"
  "(ANTHROPIC_API_KEY for Claude Code, OPENAI_API_KEY for Codex) are\n"
  "brokered: the agent holds a token for its own process, and the daemon\n"
  "swaps it for the real key on the way to a fixed upstream. Others are\n"
  "passed as before and listed as unbrokered. An OAuth login is\n"
  "unaffected.\n"
  "\n"
  "Anything but open needs runtime.confine: bwrap -- outside the sandbox\n"
  "a process can ignore a proxy -- and without it the agent is disabled\n"
  "with that reason, never run on an open network instead. Takes effect\n"
  "at the agent's next start.", "0.17.0" },

{ "agents.runtime.autostart", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Start with the daemon. Defaults to defaults.autostart.", "0.1.0" },

{ "agents.runtime.restart", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  NULL, clawt_restart_policy_get_type,
  "never, on-failure or always. Defaults to defaults.restart.", "0.1.0" },

{ "agents.runtime.backoff_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "5", NULL,
  "Base delay before a restart, doubling on each consecutive failure.", "0.1.0" },

{ "agents.runtime.max_restarts", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "10", NULL,
  "Consecutive failures before the agent is left in error rather than\n"
  "restarted again. 0 means never give up.", "0.1.0" },

{ "agents.runtime.stream_steps", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Report this agent's steps while its turn runs. Defaults to\n"
  "defaults.stream_steps.", "0.2.0" },

{ "agents.runtime.process_timeout_ms", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "0", NULL,
  "Maximum elapsed milliseconds for one AI CLI subprocess, including\n"
  "streaming turns. 0, the default, disables the limit so an agent can\n"
  "work for several hours. Set a positive integer to opt into a deadline.\n"
  "Rendered explicitly as libreclaw ai.process_timeout_ms for every agent.\n"
  "The tmux provider uses its own command and inactivity bounds instead.\n"
  "This is a duration limit; runtime.turn_timeout_seconds separately\n"
  "controls the turn watchdog. Values must be between 0 and 2147483647.",
  "0.2.0" },

{ "agents.runtime.background_wait_ms", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "1800000", NULL,
  "How long, in milliseconds, a turn keeps going after the agent's CLI\n"
  "ended it with background work of its own still running -- a Claude\n"
  "Code sub-agent or background shell. The CLI runs another turn by\n"
  "itself when that work finishes, and its text is part of the reply.\n"
  "When the bound is reached the CLI is stopped, the reply says what was\n"
  "left running, and a task the turn was on fails with that as its\n"
  "reason. 0 waits as long as the CLI runs (then only\n"
  "runtime.process_timeout_ms bounds it). The default is 30 minutes.\n"
  "Rendered as libreclaw ai.background_wait_ms. Values must be between 0\n"
  "and 2147483647.",
  "0.77.3" },

{ "agents.runtime.idle_stop_hours", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "Stop this agent after this many hours without a turn. 0 never does.\n"
  "\n"
  "An idle agent still holds its process, its ports and its computer.\n"
  "Stopped this way it is woken again by the next message queued for\n"
  "it -- from you, a routine, a trigger or another agent -- so sleeping\n"
  "loses no conversation; the message waits in its mailbox while it\n"
  "starts. An agent that has never taken a turn is measured from when\n"
  "it started. Stopping it by hand is not this: it stays stopped.", "0.8.0" },

{ "agents.runtime.turn_timeout_seconds", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "0", NULL,
  "How long a turn may go without producing anything before it is\n"
  "cancelled. 0, the default, disables it.\n"
  "\n"
  "Off by default because the number was always a guess, and the guess\n"
  "was wrong in the expensive direction: a twenty-minute ceiling killed\n"
  "real work -- a long refactor, a slow test suite -- and reported it\n"
  "as a wedged turn. Every step an agent takes now counts as activity,\n"
  "so a turn that is working says so continuously and a person can see\n"
  "a stalled one rather than having a timer guess for them.\n"
  "\n"
  "Set a number to bring the watchdog back. It counts activity, not\n"
  "duration: a turn may legitimately run for an hour while steps keep\n"
  "arriving, and one that has produced nothing at all for this long is\n"
  "wedged. A turn parked on a decision is exempt, because waiting for a\n"
  "person is not a stall.\n"
  "\n"
  "Watched in two places, on purpose. The daemon watches activity and\n"
  "publishes turn.timed_out; the same number is rendered into the\n"
  "agent's own session.watchdog_timeout_seconds. That inner watchdog\n"
  "measures total duration, even while output arrives or a decision is\n"
  "pending; a positive value therefore also caps the whole turn -- at\n"
  "this number plus runtime.busy_max_seconds while the busy check\n"
  "(runtime.busy_cpu_percent) is on, and not at all when that ceiling\n"
  "is 0.",
  "0.2.0" },

{ "agents.runtime.first_reply_seconds", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "20", NULL,
  "How long a turn may show nothing at all -- no step, no thought, no\n"
  "reply -- before the clients offer to stop it or to run the same\n"
  "message on another model. 0 never offers.\n"
  "\n"
  "Nothing is stopped by this; it is an offer, shown beside the\n"
  "conversation and withdrawn the moment the turn shows anything. It\n"
  "needs runtime.stream_steps, since a turn whose steps are not\n"
  "streamed is silent until it answers whether or not it is working.\n"
  "\n"
  "Silence here is what the daemon can see, which is less than what the\n"
  "CLI is doing: libreclaw does not forward the CLI's own status lines,\n"
  "and holds prose until a tool call or the end of the turn. So the\n"
  "offer says the turn has shown nothing, never that it is stuck. The\n"
  "models offered are the model.fallbacks entries on the agent's own\n"
  "CLI and login, since one turn's model is a name on the CLI it is\n"
  "already running.", "0.26.0" },

{ "agents.runtime.busy_cpu_percent", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "5", NULL,
  "How much CPU the agent's processes must use, in percent of one core,\n"
  "for a silent turn to count as working. 0 turns the check off.\n"
  "\n"
  "A turn running one long command -- a test suite, a build -- reports\n"
  "nothing between the tool starting and the tool finishing, so\n"
  "runtime.turn_timeout_seconds would read forty minutes of compiling as\n"
  "a wedged turn. Each watchdog sweep (every five seconds) reads the CPU\n"
  "time of the agent's process tree from /proc; a tree that used at\n"
  "least this much since the last sweep counts as a sign of life. A\n"
  "command waiting on the network uses none and is still stopped.\n"
  "\n"
  "Only for runtime.type: process. An embedded agent shares the daemon's\n"
  "process, so its tree cannot be told apart from the fleet's and the\n"
  "check does nothing there. Nothing is read while\n"
  "runtime.turn_timeout_seconds is 0.", "0.26.0" },

{ "agents.runtime.busy_max_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "7200", NULL,
  "The longest a busy process tree may keep a silent turn alive, in\n"
  "seconds since the turn last reported anything. 0 is no ceiling.\n"
  "\n"
  "A command stuck in a loop uses CPU too, and without a ceiling it would\n"
  "hold its turn for ever. Past this many seconds with no step, message\n"
  "or tool call the CPU stops counting and runtime.turn_timeout_seconds\n"
  "applies as it would to any silent turn.", "0.26.0" },

/* ── agents.computer ─────────────────────────────────────────────── */
{ "agents.computer", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "What the agent can run commands on.", "0.1.0" },

{ "agents.computer.use", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Share a machine declared under computers: instead of having one of\n"
  "this agent's own. Set, every other computer.* key comes from that\n"
  "entry and this agent's own computer block is ignored, so several\n"
  "agents naming one entry run on exactly one machine.", "0.5.0" },

{ "agents.computer.type", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  NULL, clawt_computer_type_get_type,
  "Which kind of computer this agent gets. Defaults to\n"
  "defaults.computer. The permitted values are listed above, from the\n"
  "schema itself -- the copy that used to be written out here had\n"
  "already gone stale by omitting distrobox.\n"
  "\n"
  "Desktop control is not a type -- it is the desktop block below, and\n"
  "works alongside any of these.", "0.1.0" },

{ "agents.computer.start", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "on_agent_start", clawt_computer_start_get_type,
  "When the machine is started: with the agent, or the first time\n"
  "something runs in it.\n"
  "\n"
  "on_agent_start brings the computer up before the agent's first turn,\n"
  "so a broken image or a dead libvirtd stops the agent at once, with the\n"
  "reason, instead of a turn later. That is the default, and what every\n"
  "agent did before this key existed.\n"
  "\n"
  "on_first_use leaves it down until the first clawtilla_computer_exec,\n"
  "`computer exec`, wait condition or desktop call. The agent's own bash,\n"
  "read and write run on the host either way, so an agent that mostly\n"
  "talks, plans or delegates never pays for a VM it does not touch.\n"
  "Callers that arrive while it is coming up share the one start, and\n"
  "the time it takes is not counted against the command's timeout.\n"
  "\n"
  "Applies to container, distrobox, vm and ssh computers; a host computer\n"
  "is a local check and always starts with the agent. A computer stopped\n"
  "by hand under a running agent is started again by its next use.\n"
  "\n"
  "An agent with a VM desktop is the exception in practice: its MCP\n"
  "client asks for the desktop when the agent starts, and that request\n"
  "is a use.", "0.19.0" },

{ "agents.computer.exchange", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether the shared exchange directory is visible to this agent.\n"
  "\n"
  "On by default, because otherwise every pair of agents that wants to\n"
  "hand a file across needs its own hand-wired mount. Turn it off for an\n"
  "agent that should not see what the others are passing around.",
  "0.1.0" },

{ "agents.computer.workspace", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether the agent's own workspace is mounted into its computer at\n"
  "/mnt/clawtilla/workspace.\n"
  "\n"
  "On by default. The workspace holds the agent's persona, its notes and\n"
  "its MEMORY.md, and without this it exists only on the host -- so the\n"
  "agent reads the files that describe it with one set of tools and\n"
  "works in a machine that cannot see them, and anything it writes there\n"
  "from inside goes somewhere the host never looks.\n"
  "\n"
  "Turn it off for an agent whose computer should hold nothing of\n"
  "clawtilla's.",
  "0.1.0" },

{ "agents.computer.default_mounts", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_NONE, "true", NULL,
  "Whether this agent gets the fleet's shared folders from\n"
  "defaults.mounts.\n"
  "\n"
  "On by default -- that is what makes them defaults. Turn it off for an\n"
  "agent that should see only what it declares itself, which is the\n"
  "point of giving one a container in the first place.\n"
  "\n"
  "Declining all of them is different from replacing one: an agent that\n"
  "declares its own mount at the same target already wins there, with\n"
  "no need to turn the rest off.", "0.1.0" },

{ "agents.computer.mounts", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Host paths shared into the computer.\n"
  "\n"
  "  mounts:\n"
  "    - source: \"~/src/projects\"\n"
  "      target: \"/work/projects\"\n"
  "      mode: rw\n"
  "      relabel: shared\n"
  "\n"
  "For a container these become bind mounts; for a VM, virtiofs shares;\n"
  "for a host computer they are the confinement allowlist itself.", "0.1.0" },

{ "agents.computer.mounts.source", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Host path to share. Not needed for tmpfs.", "0.1.0" },

{ "agents.computer.mounts.target", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_REQUIRED,
  NULL, NULL,
  "Absolute path inside the computer. Must not overlap another mount.", "0.1.0" },

{ "agents.computer.mounts.mode", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "ro", clawt_mount_mode_get_type,
  "ro or rw. Read-only is the default: an agent that only needs to read\n"
  "your notes should not be able to rewrite them.", "0.1.0" },

{ "agents.computer.mounts.type", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "bind", clawt_mount_type_get_type,
  "bind, volume, virtiofs, 9p or tmpfs.\n"
  "\n"
  "Usually leave this alone: bind for containers and virtiofs for VMs are\n"
  "chosen automatically from the computer type.", "0.1.0" },

{ "agents.computer.mounts.relabel", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "shared", clawt_relabel_get_type,
  "SELinux relabelling for a container bind mount: none, shared or private.\n"
  "\n"
  "This matters on Fedora Silverblue and friends, where an unlabelled bind\n"
  "mount is visible in the container but every access is denied -- which\n"
  "reads like a permissions bug rather than a labelling one.\n"
  "\n"
  "shared (:z) is the default because it is what makes a shared folder\n"
  "work. It used to default to none, which meant every mount anyone\n"
  "declared failed with permission denied on an SELinux system while\n"
  "this very text said shared was usually what you wanted. On a machine\n"
  "without SELinux it does nothing.\n"
  "\n"
  "private (:Z) rewrites the host directory's labels exclusively for this\n"
  "container, which will break anything else using that directory. none\n"
  "leaves the labels alone, which is right when the source is already\n"
  "labelled for container access.", "0.1.0" },

{ "agents.computer.mounts.create", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Create the source directory if it is missing, rather than failing.", "0.1.0" },

{ "agents.computer.mounts.size", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Size for a tmpfs mount, e.g. \"512M\".", "0.1.0" },

{ "agents.computer.mounts.required", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Fail to start when the source is missing, rather than warning and\n"
  "carrying on without it.", "0.1.0" },

/* ── agents.computer.host ────────────────────────────────────────── */
{ "agents.computer.host", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_DANGEROUS,
  NULL, NULL,
  "Settings for computer.type: host -- the real machine clawtilla runs on.\n"
  "\n"
  "This is the most useful and the most dangerous backend. Read\n"
  "docs/security.org before enabling it: what each confinement mode\n"
  "actually prevents, and what none of them do, is spelled out there.", "0.1.0" },

{ "agents.computer.host.confirm_host_control", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_DANGEROUS, "false", NULL,
  "Required before a host computer will start.\n"
  "\n"
  "There is no default that makes this safe, so there is no default that\n"
  "skips it. The daemon refuses to start the agent without it.", "0.1.0" },

{ "agents.computer.host.confine", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "workspace", clawt_confine_mode_get_type,
  "How much of the machine the agent may touch.\n"
  "\n"
  "  workspace  cwd pinned to root; every path in the command line is\n"
  "             canonicalised and rejected if it escapes\n"
  "  allowlist  root plus allow_paths, minus deny_paths\n"
  "  bwrap      a real kernel sandbox via bubblewrap\n"
  "  none       no restriction at all\n"
  "\n"
  "workspace and allowlist are enforced by inspecting the command before\n"
  "running it, which closes '..' and symlink escapes together. They do not\n"
  "stop a program that opens paths itself once it is running. Only bwrap\n"
  "does, because only bwrap involves the kernel.\n"
  "\n"
  "All of it applies to clawtilla_computer_exec. The agent's own bash,\n"
  "read and write tools are its CLI's, and that CLI is not wrapped: they\n"
  "reach the whole filesystem as the user running the daemon whatever\n"
  "this is set to. That is true of every computer type -- only exec\n"
  "enters the computer -- but host is the type where the setting reads\n"
  "like a boundary around the agent rather than around one tool.\n"
  "\n"
  "If bwrap is asked for and not installed, the agent becomes a shadow with\n"
  "that as its reason. It is never silently downgraded.", "0.1.0" },

{ "agents.computer.host.root", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Directory the agent works in. Defaults to its workspace.", "0.1.0" },

{ "agents.computer.host.allow_paths", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Additional paths reachable under confine: allowlist or bwrap.", "0.1.0" },

{ "agents.computer.host.deny_paths", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Paths refused even when they fall inside an allowed one.\n"
  "\n"
  "Checked after allow_paths, so ~/.ssh stays out even when all of ~ is in.\n"
  "Under bwrap a denied directory is covered with an empty tmpfs and a\n"
  "denied file with /dev/null, so the kernel enforces it rather than the\n"
  "argument scan; a path outside every bind needs nothing, since bwrap\n"
  "starts from nothing.", "0.1.0" },

{ "agents.computer.host.allow_network", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether the agent's commands can reach the network.\n"
  "Only enforceable under confine: bwrap, which unshares the namespace.", "0.1.0" },

{ "agents.computer.host.allow_sudo", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_DANGEROUS, "false", NULL,
  "Permit privilege escalation.\n"
  "\n"
  "With this false, sudo, pkexec, doas, run0 and machinectl shell are\n"
  "refused at the exec layer, including through a shell one-liner, and the\n"
  "agent is told so rather than left guessing why its command failed.", "0.1.0" },

{ "agents.computer.host.shell", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_INERT,
  "/bin/sh", NULL,
  "Shell used for interactive sessions and shell-form commands.\n"
  "\n"
  "Not implemented in this build. Nothing reads it, and there is no\n"
  "shell-form command for it to apply to: clawtilla_computer_exec takes\n"
  "an argv, every element of which is quoted before it is run, so a host\n"
  "computer never invokes a shell at all.", "0.1.0" },

{ "agents.computer.host.nice", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "Scheduling niceness for the agent's commands, so a runaway build does\n"
  "not make the desktop unusable.", "0.1.0" },

/* ── agents.computer.display ─────────────────────────────────────── */
{ "agents.computer.display", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Settings for computer.type: display -- GUI apps on a virtual screen of\n"
  "their own, which the agent drives and a person can watch and take.\n"
  "\n"
  "Commands run on the host exactly as computer.type: host runs them, so\n"
  "computer.host.confirm_host_control and computer.host.confine apply\n"
  "here too. See docs/computers.org, \"display\".", "0.51.0" },

{ "agents.computer.display.backend", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "auto", clawt_display_backend_get_type,
  "What draws the screen: auto, gowl, xvfb or xvnc.\n"
  "\n"
  "  gowl  gowl run headless, in a runtime directory of its own; Wayland\n"
  "        apps draw on it directly, X apps through its XWayland, and the\n"
  "        agent gets gowl's own window tools as well\n"
  "  xvnc  TigerVNC's X server: the Computer tab's viewer button opens it\n"
  "        live in a VNC viewer\n"
  "  xvfb  an X server drawing into memory; the lightest of the three\n"
  "\n"
  "auto takes the first that runs, in that order. Each candidate is run\n"
  "before it is chosen, so a broken binary first on PATH is skipped, and\n"
  "the Computer tab says what was skipped and why.", "0.51.0" },

{ "agents.computer.display.resolution", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "1280x800", NULL,
  "The screen's size, WIDTHxHEIGHT, each between 64 and 8192.\n"
  "\n"
  "The agent's clicks are in these pixels. Larger screens cost more to\n"
  "picture, and every picture the agent asks for is part of its turn.", "0.51.0" },

{ "agents.computer.display.depth", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "24", NULL,
  "Colour depth for an X server: 16, 24 or 32. gowl ignores it.", "0.51.0" },

{ "agents.computer.display.startup", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Apps started on the screen each time it comes up, as shell command\n"
  "lines, under the agent's confinement: e.g. [firefox, \"foot -e htop\"].\n"
  "One that does not start is logged; the others still are.", "0.51.0" },

{ "agents.computer.display.vnc_port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "Xvnc's VNC port on 127.0.0.1. 0 picks a free one at each start.\n"
  "\n"
  "The VNC server listens on loopback only and asks for no password: it\n"
  "is reachable from this machine alone, which is the trust the daemon's\n"
  "own socket gives.", "0.51.0" },

{ "agents.computer.display.allow_input", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether the agent may click, type and scroll on its display, not only\n"
  "look at it.\n"
  "\n"
  "On by default, unlike computer.desktop.allow_input: that one is about\n"
  "the operator's own screen, and this is a private one nobody is sitting\n"
  "at. A person who takes the screen still refuses the agent's input for\n"
  "as long as they hold it.", "0.51.0" },

{ "agents.computer.display.allow_spawn", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether the agent may start apps on its display with display_launch.\n"
  "\n"
  "Off leaves it the apps in computer.display.startup and whatever it\n"
  "starts through clawtilla_computer_exec, which is its own setting.", "0.51.0" },

{ "agents.computer.display.gowl_binary", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "A gowl to run instead of searching /usr/bin, /usr/sbin and PATH.\n"
  "\n"
  "For when the first gowl found is not the one wanted -- an old build in\n"
  "/usr/local/bin that cannot load its wlroots, say. It must answer\n"
  "--version or it is not used.", "0.51.0" },

/* ── agents.computer.container ───────────────────────────────────── */
{ "agents.computer.container", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Settings for computer.type: container.", "0.1.0" },

{ "agents.computer.container.image", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "registry.fedoraproject.org/fedora:45", NULL,
  "Image the container is created from.\n"
  "\n"
  "Any reference podman can pull. Unset means defaults.container_image.\n"
  "\n"
  "Name the registry: a bare 'fedora:45' is resolved through podman's\n"
  "unqualified-search list, which is per-machine, so the same config can\n"
  "pull a different image on two hosts.", "0.1.0" },

{ "agents.computer.container.name", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Container name. Defaults to clawt-<agent-id>.", "0.1.0" },

{ "agents.computer.container.connection", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Podman connection: a socket path, ssh://user@host/socket, tcp://host:port,\n"
  "or a named connection. Defaults to the user's podman socket.", "0.1.0" },

{ "agents.computer.container.command", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_COMMENTED, "sleep infinity", NULL,
  "What the container runs.\n"
  "\n"
  "The default keeps the container alive, because a container computer\n"
  "exists to be exec'd into: a plain base image's entrypoint exits the\n"
  "moment it starts, and an agent whose container has already gone is an\n"
  "agent whose every command fails for no visible reason.\n"
  "\n"
  "Set it when the image has an entrypoint worth running. A JSON array is\n"
  "used as-is; anything else is split on spaces.", "0.1.0" },

{ "agents.computer.container.network", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_INERT, NULL, NULL,
  "Network to attach to. Leave unset for podman's default.\n"
  "\n"
  "Not implemented in this build. The value reaches the container object\n"
  "through clawt_container_computer_set_network() and stops there: the\n"
  "podman create request is built without it, so the container is always\n"
  "on podman's default network however this is set.", "0.1.0" },

{ "agents.computer.container.keep", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Keep the container when the agent stops, instead of removing it.\n"
  "\n"
  "Stopping an agent stops its machine, and with this off the container\n"
  "is removed as well: everything the agent installed goes with it and\n"
  "the next start builds a fresh one from the image.\n"
  "\n"
  "On by default. It reads as the smaller of the two settings and it is\n"
  "not -- a bare image has no toolchain, so the first thing an agent does\n"
  "in a new container is install one, and discarding that on every stop\n"
  "means paying for it again on every start. The distrobox backend has\n"
  "defaulted to keeping for exactly this reason since it was written.\n"
  "\n"
  "Turned off, `computer stop` and `computer restart` refuse without\n"
  "`--remove`, and both graphical clients ask first: the contents are\n"
  "gone rather than offline, which is not what the word stop suggests.",
  "0.1.0" },

{ "agents.computer.container.pids_limit", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "0", NULL,
  "Most processes and threads the machine may hold at once.\n"
  "\n"
  "What stops a fork bomb, or a build with -j0, taking the host down with\n"
  "it. 0 leaves podman's own default (2048 on Fedora, from\n"
  "containers.conf); -1 removes the limit. Anything else is the limit.\n"
  "\n"
  "Read by the distrobox backend too, which is a podman container\n"
  "underneath: computer.container.* limits reach either. Passed to podman\n"
  "when the machine is created; a changed value is applied to the\n"
  "existing one with `podman update` at the next start, without a\n"
  "rebuild and without losing what the agent installed.", "0.19.0" },

{ "agents.computer.container.memory", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Most memory the machine may use, e.g. 512m or 4g.\n"
  "\n"
  "A whole number with an optional unit -- b, k, m or g, in powers of\n"
  "1024 -- and at least 6m, which is podman's floor. Unset means no limit\n"
  "beyond the host's. Past it the kernel's OOM killer picks a process\n"
  "inside the machine, which is the point: the agent's container dies\n"
  "rather than your desktop.\n"
  "\n"
  "Applies to container and distrobox computers, at creation and then\n"
  "with `podman update` whenever it changes -- in place on a running\n"
  "machine. Inherited from the team (teams[].computer.container) and\n"
  "then the fleet (defaults.container) when the agent does not set it.\n"
  "An OOM kill inside the machine is filed as an incident naming this\n"
  "limit.", "0.19.0" },

{ "agents.computer.container.memory_swap", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Memory and swap together, e.g. 8g; -1 for unlimited swap.\n"
  "\n"
  "podman's own meaning, and easy to misread: this is the total, not the\n"
  "swap alone, so memory: 4g with memory_swap: 6g allows 2g of swap. Equal\n"
  "to memory means no swap at all. Unset lets podman allow as much swap\n"
  "again as memory. It needs memory to be set and must not be smaller\n"
  "than it; both are refused when they arrive rather than when podman\n"
  "reads them.", "0.19.0" },

{ "agents.computer.container.cpus", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Most CPU time the machine may use, as a count of CPUs: 4, 2.5, 0.5.\n"
  "\n"
  "podman's --cpus, a hard cap: 4 on a 20-CPU host lets the machine use\n"
  "four CPUs' worth of time however many it spreads across, so one\n"
  "agent's make -j32 cannot take the host's load to 54. More than zero\n"
  "and no more than this host's CPUs; refused otherwise, naming the key.\n"
  "Unset means no cap.\n"
  "\n"
  "Takes its default from the agent's team (teams[].computer.container)\n"
  "and then the fleet (defaults.container); the agent's own value wins.\n"
  "Changed on a running machine with `podman update`, in place, with no\n"
  "restart.", "0.78.0" },

{ "agents.computer.container.cpu_weight", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "0", NULL,
  "The machine's share of CPU when the host is busy: a cgroup v2\n"
  "cpu.weight, 1 to 10000, where 100 is everybody's default.\n"
  "\n"
  "Relative, not a cap: an agent at 200 gets twice the CPU of one at 100\n"
  "while they compete, and all of it when nobody else wants any. 0\n"
  "leaves podman's default. podman takes shares rather than a weight, so\n"
  "this is passed as --cpu-shares (100 -> 1024, 1 -> 2, 10000 -> 262144);\n"
  "the runtime turns it back into cpu.weight.\n"
  "\n"
  "Inherited from the team and then the fleet like cpus, and changed in\n"
  "place the same way.", "0.78.0" },

{ "agents.computer.container.cpuset", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Which CPUs the machine may run on, e.g. 0-7 or 0,2,4-6.\n"
  "\n"
  "Pinning, for keeping a noisy agent off the cores something else\n"
  "needs. Every CPU named must exist on this host. podman's\n"
  "--cpuset-cpus; rootless podman needs the cpuset controller delegated\n"
  "to the user's systemd (Fedora does not by default), and where podman\n"
  "cannot change it on a running machine it is applied at the next\n"
  "restart instead, and the Fleet page says so.", "0.78.0" },

{ "agents.computer.container.selinux_level", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_NONE, "auto", NULL,
  "The SELinux MCS level the container is created at.\n"
  "\n"
  "podman gives every container a random pair of categories, and a file\n"
  "created inside a container is labelled with that pair. On a folder\n"
  "several containers share -- ~/source, mounted rw with relabel: shared\n"
  "-- a file one agent writes from inside its container is then\n"
  "unreadable inside every other one, even at mode 0644. It reads as a\n"
  "permissions bug and is a labelling one. Files written on the host\n"
  "(an agent's own tools run there) are not affected.\n"
  "\n"
  "auto (the default) puts an agent on a team at its team's level: a\n"
  "pair clawtilla picks the first time the team needs one, distinct\n"
  "from every other team's and from every explicit level in the config,\n"
  "and keeps in tasks.db under the state directory, so it survives a\n"
  "daemon restart and a rebuild. An agent on no team gets podman's\n"
  "random pair. random always gets podman's random pair. A level such as\n"
  "s0:c100,c200 (or one category, s0:c5) is used as written: categories\n"
  "c0..c1023, ascending, no leading zeros. disable is refused -- it\n"
  "would switch SELinux off for the container, not choose its level.\n"
  "\n"
  "Applied only when the container is created. Changing it, or the\n"
  "agent's team under auto, takes effect when the container is next\n"
  "made: stop the agent and start it, or run `computer rebuild`.\n"
  "`config validate` warns when two containers share a writable folder\n"
  "at different levels.", "0.78.0" },

{ "agents.computer.container.init", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_NONE, "true", NULL,
  "Run podman's init (catatonit) as the container's PID 1.\n"
  "\n"
  "Without it PID 1 is the container's command -- sleep infinity by\n"
  "default -- which never waits for anything, so every process orphaned\n"
  "when a process tree is killed (an interrupted build, a cancelled\n"
  "turn) stays a zombie until the container stops. With it, catatonit\n"
  "is PID 1, runs the command as its child and reaps whatever is\n"
  "re-parented to it.\n"
  "\n"
  "On by default. It needs catatonit on the host: Fedora's catatonit\n"
  "package, which podman pulls in (/usr/libexec/podman/catatonit).\n"
  "\n"
  "Applied when the container is created. A kept container whose setting\n"
  "differs -- every one made before this key existed -- is recreated by\n"
  "its next start, once, with the reason in the log and on the Fleet\n"
  "page; anything installed inside it is lost. A value that is not a\n"
  "boolean disables the agent with the reason rather than reading as\n"
  "false.", "0.78.0" },

/* ── agents.computer.ssh ─────────────────────────────────────────── */
{ "agents.computer.ssh", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "A machine reached over ssh.\n"
  "\n"
  "The only backend clawtilla does not own. A container, a distrobox and\n"
  "a VM are things it creates and can destroy; this one was running\n"
  "before the fleet existed. So clawtilla will not start, stop or take\n"
  "apart an ssh computer, and both clients leave those buttons out\n"
  "rather than offering them and failing.\n"
  "\n"
  "Nothing is mounted into it either -- there is no mount to make across\n"
  "an ssh connection. That means the agent does NOT get the workspace at\n"
  "/mnt/clawtilla/workspace or the shared exchange, which every other\n"
  "backend does get, and fleet-wide shared folders do not apply. What\n"
  "the mount list does instead is described under workspace below.",
  "0.2.0" },

{ "agents.computer.ssh.host", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The ssh config alias to connect to.\n"
  "\n"
  "An alias from ~/.ssh/config, not a hostname -- so the identity file,\n"
  "the jump host, the port and the user stay where ssh already keeps\n"
  "them, and clawtilla never reimplements ssh's own configuration.\n"
  "\n"
  "Letters, digits, '.', '_' and '-' only, and never a leading '-'. ssh\n"
  "reads an argument beginning with '-' as an option, so an alias called\n"
  "-oProxyCommand=... would be a command rather than a destination.\n"
  "\n"
  "Run `ssh <alias> true` once by hand first. The host key is accepted\n"
  "on your terms, never automatically: an unknown or changed key is a\n"
  "refusal that names this as the remedy. clawtilla does not pass\n"
  "StrictHostKeyChecking=no, because accepting a key is your decision.",
  "0.2.0" },

{ "agents.computer.ssh.workspace", CLAWT_SCHEMA_PATH,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Where the agent works on that machine.\n"
  "\n"
  "A path as that machine spells it. It is not expanded here: '~' and\n"
  "$XDG_* would be resolved against this machine's home and produce a\n"
  "path that exists here and means nothing over there.\n"
  "\n"
  "There is no kernel mount to make over ssh, so the mount list becomes\n"
  "the allowlist instead -- the same shape `confine: allowlist` has on\n"
  "the host backend. Each mount's *target* is the grant, because that is\n"
  "the path inside the computer and the computer is the other machine;\n"
  "the source names a directory on this one and is not used.\n"
  "\n"
  "That check reads the paths in a command and resolves '.' and '..' by\n"
  "text. It cannot follow a symlink, because the symlink is on the other\n"
  "machine -- so it is a boundary against mistakes and against paths an\n"
  "agent constructs, not against a filesystem laid out to defeat it. The\n"
  "directory must already exist; clawtilla creates nothing on a machine\n"
  "it does not own, and a mount marked required that is not there makes\n"
  "the agent a SHADOW with that reason rather than starting it anyway.",
  "0.2.0" },

{ "agents.computer.ssh.shell", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "/bin/sh", NULL,
  "Which shell runs the command over there.\n"
  "\n"
  "ssh hands the remote login shell one string whatever we do, so this\n"
  "is not a way to avoid a shell -- it is a way to name one that behaves\n"
  "the same everywhere. A login shell that is fish or csh reads the\n"
  "`cd <dir> && ...` prologue differently, and naming /bin/sh is what\n"
  "makes an agent behave identically on every host in a fleet.",
  "0.2.0" },

{ "agents.computer.ssh.allow_sudo", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_NONE, "false", NULL,
  "Whether sudo and its neighbours may be run there.\n"
  "\n"
  "Off refuses sudo, pkexec, doas, run0, su, setpriv and machinectl at\n"
  "the point the command is built, including inside a shell one-liner,\n"
  "and the agent is told so rather than discovering it a turn later.\n"
  "\n"
  "It is an argument check, not a kernel boundary: it stops a command\n"
  "that names one of those, and cannot stop a program that escalates\n"
  "once it is already running. Nothing on this backend can -- the kernel\n"
  "doing the enforcing is on the other machine.",
  "0.2.0" },

{ "agents.computer.ssh.connect_timeout", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "10", NULL,
  "Seconds to wait for the connection before failing.\n"
  "\n"
  "Bounds the one failure that otherwise has none. A machine that has\n"
  "dropped off the network accepts nothing and refuses nothing, and ssh\n"
  "will sit out the kernel's TCP timeout waiting -- turning a turn that\n"
  "should have failed in ten seconds into one that hangs. Zero is\n"
  "treated as one second rather than as no limit.",
  "0.2.0" },

{ "agents.computer.ssh.control_persist", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "600", NULL,
  "Seconds an idle multiplexed connection is kept open.\n"
  "\n"
  "Without multiplexing every single command pays a full TCP connect,\n"
  "key exchange and authentication, which over a WAN is most of the time\n"
  "an agent spends running anything. With it the first command opens a\n"
  "master and every later one is a channel on the connection already\n"
  "there.\n"
  "\n"
  "Zero turns it off. It also turns itself off, with a warning, when the\n"
  "control socket path would exceed the 108 bytes the kernel allows in a\n"
  "unix address -- an over-long path does not fail at bind time, it just\n"
  "means the master is never created and every command silently pays a\n"
  "fresh handshake.",
  "0.2.0" },

{ "agents.computer.ssh.wake_command", CLAWT_SCHEMA_STRING_LIST,
  CLAWT_SCHEMA_FLAG_ARGV, NULL, NULL,
  "A program to run when the machine does not answer, to wake it.\n"
  "\n"
  "For a machine that sleeps: a homelab box that suspends when idle, a\n"
  "cloud server that is paused. Wake-on-LAN is the usual answer --\n"
  "[wakeonlan, aa:bb:cc:dd:ee:ff] -- or a provider's own resume command.\n"
  "It should return once the machine is on its way up; clawtilla then\n"
  "connects as usual, and ssh's connect_timeout covers the rest of the\n"
  "boot.\n"
  "\n"
  "A program and its arguments, as a list. It is never run through a\n"
  "shell, so a pipe or a redirect would be text; written as one line in\n"
  "a client it is split into words and refused if it uses one. Nothing\n"
  "is substituted into it.\n"
  "\n"
  "Run only when a connection attempt finds nothing answering, never for\n"
  "a refused key or login. Commands that arrive while it runs wait for\n"
  "that one run rather than starting their own, and each is told how\n"
  "long it waited. It gets at most 120 seconds, after which it is killed\n"
  "along with everything it started, and the time is not taken out of\n"
  "the command's own timeout. A wake that fails is reported with the\n"
  "connection failure, saying which.", "0.19.0" },

/* ── agents.computer.distrobox ───────────────────────────────────── */
{ "agents.computer.distrobox", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Settings for computer.type: distrobox.\n"
  "\n"
  "A distrobox is a podman container deliberately wired into the machine\n"
  "around it: the agent runs as the same user as you, sees your sockets\n"
  "and displays, and can reach the host itself with distrobox-host-exec.\n"
  "That makes it a good place to build things and a poor place to put\n"
  "something that should be contained -- use computer.type: container for\n"
  "that, which shares none of it.", "0.1.0" },

{ "agents.computer.distrobox.image", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Image the box is created from. Unset uses distrobox's own default,\n"
  "which is a Fedora toolbox image.\n"
  "\n"
  "Name the registry, for the reason the container image does: a bare\n"
  "'fedora:45' is resolved through podman's unqualified-search list,\n"
  "which is per-machine.", "0.1.0" },

{ "agents.computer.distrobox.name", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Box name. Defaults to clawt-<agent-id>.\n"
  "\n"
  "Prefixed on purpose. These machines already have boxes called 'dev'\n"
  "and 'util', and an agent adopting one of those would be a fleet\n"
  "operation reaching into somebody's development environment.", "0.1.0" },

{ "agents.computer.distrobox.home", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "HOME inside the box.\n"
  "\n"
  "Unset gives the agent a directory of its own under\n"
  "$XDG_DATA_HOME/clawtilla/boxes/<agent-id>/home, which is NOT what\n"
  "distrobox does on its own -- see share_home.", "0.1.0" },

{ "agents.computer.distrobox.share_home", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_NONE, "false", NULL,
  "Give the box your real home directory.\n"
  "\n"
  "This is distrobox's own default, and clawtilla's is the opposite,\n"
  "because the two look identical from inside the box and are completely\n"
  "different in what they touch: an agent sharing your home can read your\n"
  "ssh keys, your shell configuration and any credential you keep there.\n"
  "\n"
  "Turn it on when the point is to work in your environment -- an agent\n"
  "maintaining your dotfiles, say. Leave it off otherwise. It is fixed\n"
  "when the box is created, so changing it means removing the box.",
  "0.1.0" },

{ "agents.computer.distrobox.packages", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_COMMENTED, "gcc make git", NULL,
  "Packages installed when the box is first created, space separated.\n"
  "\n"
  "Read once, at creation, the way a VM's cloud-init reads its seed --\n"
  "so adding a name here and restarting the agent does nothing visible.\n"
  "The agent can install more itself; this is for what it needs before\n"
  "it can.", "0.1.0" },

{ "agents.computer.distrobox.flags", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Extra flags passed through to podman, space separated.\n"
  "Quote anything containing a space.", "0.1.0" },

{ "agents.computer.distrobox.init", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Run an init system (systemd) inside the box.\n"
  "Needed by anything expecting systemctl or a user unit.", "0.1.0" },

{ "agents.computer.distrobox.keep", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Keep the box when the agent stops.\n"
  "\n"
  "On by default, unlike the plain container: making one pulls an image\n"
  "and then runs a package install inside it, and a distrobox is the kind\n"
  "of computer somebody chose so that what the agent installs survives.",
  "0.1.0" },

/* ── agents.computer.vm ──────────────────────────────────────────── */
{ "agents.computer.vm", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Settings for computer.type: vm.", "0.1.0" },

{ "agents.computer.vm.backend", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "libvirt", clawt_vm_backend_get_type,
  "libvirt or qemu.\n"
  "\n"
  "libvirt is the default and brings snapshots, migration and device\n"
  "hotplug with it. qemu drives qemu-system-* directly over QMP and needs\n"
  "no libvirtd, at the cost of that surface.", "0.1.0" },

{ "agents.computer.vm.domain", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "libvirt domain name. Defaults to clawt-<agent-id>.", "0.1.0" },

{ "agents.computer.vm.uri", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "qemu:///session", NULL,
  "libvirt connection URI. qemu:///system for system-wide VMs.", "0.1.0" },

{ "agents.computer.vm.emulator", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The QEMU binary to run the guest with.\n"
  "\n"
  "Left unset, clawtilla looks in /usr/bin and then on PATH, and writes\n"
  "what it finds into the domain.\n"
  "\n"
  "Naming it matters because libvirt otherwise picks one itself, by\n"
  "searching the session daemon's PATH -- so a host with another package\n"
  "manager ahead of /usr/bin gets a domain pointing into a home\n"
  "directory. SELinux refuses that: svirt_t cannot start a binary\n"
  "labelled user_home_t, and the guest never boots. Set this only for a\n"
  "host whose QEMU is somewhere clawtilla does not look.",
  "0.1.0" },

{ "agents.computer.vm.image", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Base disk image. A qcow2 overlay is created on top, so the base is\n"
  "never written to and several agents can share one.\n"
  "\n"
  "clawtilla ships no image and downloads none: point this at a qcow2 you\n"
  "already have. A distribution's cloud image is the easy answer, because\n"
  "cloud_init below can give it a login without the image being touched.",
  "0.1.0" },

{ "agents.computer.vm.cpus", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "2", NULL,
  "Virtual CPUs.", "0.1.0" },

{ "agents.computer.vm.memory_mb", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "8192", NULL,
  "Memory in megabytes.\n"
  "\n"
  "Sized for a guest running a GNOME session, because that is what\n"
  "computer.desktop.enabled installs into one. A headless VM is happy in\n"
  "a quarter of it.", "0.1.0" },

{ "agents.computer.vm.disk_gb", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "128", NULL,
  "Virtual disk size in gigabytes.\n"
  "\n"
  "A cloud image's own disk is only a few gigabytes, which a desktop and\n"
  "a toolchain fill immediately. The overlay is created at this size\n"
  "instead and cloud-init grows the guest's filesystem to match on first\n"
  "boot.\n"
  "\n"
  "qcow2 is sparse, so this costs what is written and not what is asked\n"
  "for. Raising it grows the disk on the next provision; lowering it is\n"
  "refused, because shrinking a disk destroys whatever was past the new\n"
  "end.", "0.1.0" },

{ "agents.computer.vm.resolution", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "1280x800", NULL,
  "The size of the VM's screen, as WIDTHxHEIGHT.\n"
  "\n"
  "1280x800 is the hypervisor's own default, which is why an agent's\n"
  "screenshots come back that size unless this says otherwise. The\n"
  "virtual GPU reports it as the preferred mode and GNOME takes it, so\n"
  "nothing inside the guest has to be configured -- and unlike anything\n"
  "in the cloud-init seed, changing it applies at the VM's next boot\n"
  "rather than needing the machine rebuilt.\n"
  "\n"
  "It costs guest memory: the framebuffer is width x height x 4 bytes.",
  "0.1.0" },

{ "agents.computer.vm.ssh_user", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "root", NULL,
  "User commands are run as inside the guest, over SSH.\n"
  "\n"
  "With cloud_init on, this account is created in the guest rather than\n"
  "having to exist already, and a non-root one gets passwordless sudo.\n"
  "\n"
  "root, because of how a shared directory is mapped. An unprivileged\n"
  "libvirt session runs virtiofsd in a user namespace where the *guest's*\n"
  "root is the host user who started the daemon, and every other guest\n"
  "id lands in that user's subuid range. So a share of your own files\n"
  "appears inside the guest as root:root 0755, and an agent running as\n"
  "any other account can read it and cannot write to it -- including its\n"
  "own workspace, which arrives as 0700 root and is not even readable.\n"
  "\n"
  "This was `clawt` for a while, on the reasoning that the guest's first\n"
  "account is uid 1000 and so lines up with the host user. That is true\n"
  "of the numbers and false of the mapping, and booting one is the only\n"
  "way to find out: guest uid 1000 maps to a subuid around 525287, which\n"
  "owns nothing. Naming an <idmap> on the share does not help either --\n"
  "virtiofsd's sandbox cannot set one up for a session daemon at all, and\n"
  "the domain then fails to start rather than starting without it.\n"
  "\n"
  "The cost is that GDM will not log root in, so a VM with a desktop gets\n"
  "a second account for the screen -- see computer.vm.desktop.user, which\n"
  "is created for exactly this. That account is a normal guest user, so\n"
  "it cannot write to a share either; anything the session produces for\n"
  "you goes through a path root writes.",
  "0.1.0" },

{ "agents.computer.vm.ssh_key", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Private key used to reach the guest.\n"
  "\n"
  "Left unset, an ed25519 key is generated per agent under the VM's state\n"
  "directory and authorised through cloud-init. It cannot be encrypted:\n"
  "the daemon uses it with nobody present to type a passphrase.",
  "0.1.0" },

{ "agents.computer.vm.ssh_host", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Address that reaches the guest.\n"
  "\n"
  "Leave it unset and clawtilla forwards a port on 127.0.0.1 to the\n"
  "guest's SSH and fills this in itself -- the port is chosen once and\n"
  "remembered, so it survives a restart. Set it when the VM is somewhere\n"
  "clawtilla did not put it: a bridged network, another host, or a VM that\n"
  "existed before this agent did.\n"
  "\n"
  "The libvirt backend can only forward a port through passt, because\n"
  "libvirt has no port forwarding for the SLIRP backend. Without passt\n"
  "installed the VM still runs, and this key is the way to reach it.",
  "0.1.0" },

{ "agents.computer.vm.ssh_port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "22", NULL,
  "Port SSH is reached on. Ignored when clawtilla is forwarding a port of\n"
  "its own choosing, which is the default.", "0.1.0" },

{ "agents.computer.vm.packages", CLAWT_SCHEMA_STRING_LIST,
  CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Packages the guest installs at first boot.\n"
  "\n"
  "  packages: [git, inetutils]\n"
  "\n"
  "A cloud image is deliberately small, and what is missing differs by\n"
  "distribution -- Arch's has no `hostname`, which agents reach for\n"
  "reflexively. This is where to put what yours needs, rather than\n"
  "having every agent install it again on every fresh machine.\n"
  "\n"
  "Names are the guest's, not yours: the same tool is `inetutils` on\n"
  "Arch and `hostname` on Fedora. cloud-init treats a package it cannot\n"
  "find as a failure of the *whole* install, so one wrong name costs\n"
  "every other package here -- and on a guest with a desktop, the\n"
  "desktop with them.\n"
  "\n"
  "Read once, by cloud-init, from a seed written before the guest ever\n"
  "ran. Adding one to a VM that already exists needs computer.rebuild,\n"
  "like everything else in that seed.", "0.1.0" },

{ "agents.computer.vm.cloud_init", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Hand the guest a cloud-init seed on first boot.\n"
  "\n"
  "A cloud image has no account, no password and no authorised key: it\n"
  "expects to be given them. Without this it boots perfectly and nothing\n"
  "can get in, which looks exactly like a VM that failed to boot.\n"
  "\n"
  "The seed is a small ISO labelled cidata, attached as a CD-ROM, holding\n"
  "the login and public key. Building it needs xorriso (Fedora: xorriso).\n"
  "Turn this off for an image that already has a login and a key.",
  "0.1.0" },

{ "agents.computer.vm.snapshot_on_start", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Take a snapshot each time the VM starts, so a session can be rolled back.\n"
  "libvirt backend only.", "0.1.0" },

/* ── agents.computer.vm.desktop ──────────────────────────────────── */
{ "agents.computer.vm.desktop", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "How the guest's graphical session is built.\n"
  "\n"
  "None of this happens unless computer.desktop.enabled is also true --\n"
  "that key is the grant, and these are the details of what it installs.\n"
  "A cloud image has no desktop at all, so an agent given one on a VM has\n"
  "to have it put there first.", "0.1.0" },

{ "agents.computer.vm.desktop.user", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The account the graphical session runs as.\n"
  "\n"
  "Left unset this is computer.vm.ssh_user, unless that is root: GDM\n"
  "refuses to log root in, so a VM whose ssh_user is root -- the default\n"
  "-- gets a separate `clawt` account for the session and keeps running\n"
  "commands as root. Both are created by cloud-init and both authorise\n"
  "the same key.\n"
  "\n"
  "cloud-init acts on first boot only, so changing this later means\n"
  "deleting the overlay rather than restarting.", "0.1.0" },

{ "agents.computer.vm.desktop.autologin", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_NONE, "true", NULL,
  "Log that account straight in at boot, with no password prompt.\n"
  "\n"
  "There is nobody at the VM's console to type one, and until a session\n"
  "exists there is no desktop to control: the shell extension the agent\n"
  "talks to runs inside GNOME Shell. Turn this off and something else has\n"
  "to start the session.", "0.1.0" },

{ "agents.computer.vm.desktop.flavour", CLAWT_SCHEMA_ENUM,
  CLAWT_SCHEMA_FLAG_NONE, "auto", clawt_guest_flavour_get_type,
  "Which family the guest belongs to, for installing things into it.\n"
  "\n"
  "`auto` reads it off `computer.vm.image` -- a catalog id says outright,\n"
  "and an image you downloaded yourself keeps the distribution in its\n"
  "filename. Set it when neither does: clawtilla says which family it\n"
  "assumed rather than guessing quietly.\n"
  "\n"
  "`enterprise` covers CentOS Stream, RHEL and the rebuilds; `arch` is\n"
  "Arch Linux, whose names drop the 3 everyone else puts in their Python\n"
  "packages and which is upgraded fully before anything is installed --\n"
  "refreshing pacman's index without upgrading is the partial upgrade it\n"
  "warns about. Ubuntu is\n"
  "separate from Debian for one reason: Debian stable has no `firefox`\n"
  "package, only `firefox-esr`, and Ubuntu has no `firefox-esr` -- so a\n"
  "single family would be wrong on half of it, and a package that does\n"
  "not exist fails the whole install rather than merely missing.\n"
  "\n"
  "It decides more than package names -- the display\n"
  "manager is gdm on Fedora and gdm3 on Debian, PyGObject is\n"
  "python3-gobject there and python3-gi here, and a Debian cloud image\n"
  "has neither the dconf binary nor glib-compile-schemas until asked.",
  "0.2.0" },

{ "agents.computer.vm.desktop.packages", CLAWT_SCHEMA_STRING_LIST,
  CLAWT_SCHEMA_FLAG_NONE,
  NULL,
  NULL,
  "What to install to get a desktop, if not the usual set.\n"
  "\n"
  "Left unset this is the list for the guest's `flavour`, which is\n"
  "almost always what you want: the names differ per distribution and\n"
  "nobody should have to know that to change image. Each family's list\n"
  "is deliberately small rather than the full workstation group -- the\n"
  "agent needs a session to drive, not an office suite to download.\n"
  "\n"
  "A list here replaces that one rather than adding to it, so it is also\n"
  "how you take something out. It does not reach the packages the MCP\n"
  "server needs: those are not the desktop, and trimming this list must\n"
  "not silently switch automation off.", "0.1.0" },

{ "agents.computer.vm.desktop.mcp", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_NONE, "true", NULL,
  "Install gnome-desktop-mcp into the guest and switch it on.\n"
  "\n"
  "This is the part the agent actually talks to: a GNOME Shell extension\n"
  "exposing screenshots, window management and input injection over\n"
  "D-Bus, and a stdio MCP server in front of it. GNOME on Wayland refuses\n"
  "those to any process outside the compositor, which is why an extension\n"
  "is involved at all.\n"
  "\n"
  "Without this the guest has a desktop and the agent has no way to see\n"
  "it.", "0.1.0" },

{ "agents.computer.vm.desktop.mcp_repo", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_NONE,
  "https://git.podbielniak.com/zachpodbielniak/gnome-desktop-mcp.git", NULL,
  "Where the guest clones gnome-desktop-mcp from.\n"
  "\n"
  "Cloned rather than installed from PyPI because the extension is only\n"
  "in the repository, and the two halves have to be the same version --\n"
  "the server calls D-Bus methods the extension has to already export.",
  "0.1.0" },

/* ── agents.computer.desktop ─────────────────────────────────────── */
{ "agents.computer.desktop", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_DANGEROUS,
  NULL, NULL,
  "Let the agent see and drive a desktop session.\n"
  "\n"
  "An add-on rather than a computer type, so it works alongside whichever\n"
  "one the agent has. An agent with this can read anything on your screen\n"
  "and click anything on it.", "0.1.0" },

{ "agents.computer.desktop.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Whether desktop tools are offered to the agent.", "0.1.0" },

{ "agents.computer.desktop.backend", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "auto", clawt_desktop_backend_get_type,
  "auto, gowl, gnome or guest.\n"
  "\n"
  "guest is the agent's own VM, and auto picks it whenever the agent has\n"
  "one -- an agent with its own machine should drive that screen rather\n"
  "than yours. It is also the only backend where a misjudged click lands\n"
  "somewhere recoverable.\n"
  "\n"
  "Otherwise auto tries gowl's MCP socket and falls back to\n"
  "gnome-desktop-mcp on the host. gowl is the better fit where it is\n"
  "available: same tool vocabulary, native, and no Python or GNOME Shell\n"
  "extension in the way.", "0.1.0" },

{ "agents.computer.desktop.socket", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  "$XDG_RUNTIME_DIR/gowl-mcp.sock", NULL,
  "gowl's MCP socket. Requires gowl built with MCP=1 and modules.mcp\n"
  "enabled in its config.", "0.1.0" },

{ "agents.computer.desktop.allow_spawn", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_DANGEROUS, "false", NULL,
  "Whether the agent may launch and signal processes through the\n"
  "compositor.\n"
  "\n"
  "Separate from allow_input, and off by default. The compositor's spawn\n"
  "tool starts a process through the compositor's own socket, so none of\n"
  "clawtilla's confinement applies to it -- not the path checks, not the\n"
  "sudo block, and not bwrap. Turning this on gives the agent a way to\n"
  "run anything, whatever computer.host.confine says.",
  "0.1.0" },

{ "agents.computer.desktop.allow_input", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Permit key and pointer injection, not just observation.\n"
  "\n"
  "With this false the agent can take screenshots and query windows but\n"
  "cannot act, which is a useful amount of access on its own and a much\n"
  "smaller grant.", "0.1.0" },

/* ── agents: the rest ────────────────────────────────────────────── */
{ "agents.computer.desktop.observe_fps", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "1", NULL,
  "Frames a second while somebody is watching the screen.\n"
  "\n"
  "A supervision view, not a video call. Grabbing shares the one\n"
  "channel the agent is using for its own work, so every frame is\n"
  "latency taken from the task being watched. Nothing is captured at\n"
  "all unless a client is subscribed.\n"
  "\n"
  "Clamped to 1-4. A zero means the default rather than never: a\n"
  "client that subscribed and then received nothing would show an\n"
  "empty panel with nothing to explain it.", "0.2.0" },

{ "agents.computer.desktop.takeover_lease_seconds", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "900", NULL,
  "How long a person may hold the screen before the hold lapses.\n"
  "\n"
  "While it is held the agent's input is refused rather than queued: a\n"
  "queued click lands after the person has moved on, on whatever\n"
  "happens to be under it then. The lease expires so that a browser tab\n"
  "closed mid-takeover cannot lock an agent out for good.\n"
  "\n"
  "Taking the screen again while you already hold it extends the lease,\n"
  "which is what a client does while somebody is still working.", "0.2.0" },

{ "agents.computer.desktop.driver_idle_seconds", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "120", NULL,
  "On a shared computer, how long one agent keeps the screen after its\n"
  "last input before another may type or click.\n"
  "\n"
  "A shared machine has one desktop. The first agent to act on it holds\n"
  "it; another agent's input is refused, naming who is driving, until\n"
  "the holder has been idle this long. Looking is never refused.", "0.5.0" },

{ "agents.computer.desktop.allow_recording", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_DANGEROUS, "false", NULL,
  "Whether a demonstration may be recorded on this screen.\n"
  "\n"
  "Its own setting, and not part of `allow_input`, because capturing\n"
  "what a person types is categorically different from injecting\n"
  "keystrokes. An agent allowed to click must not thereby be allowed to\n"
  "watch you type.\n"
  "\n"
  "While a recording runs the desktop says so visibly and the\n"
  "recording stops itself after `skills.teach_max_seconds`.\n"
  "\n"
  "Capture pauses for a password field only as far as the compositor\ncan see one, and that is less far than it sounds. In a guest, GNOME\nShell recognises its own password entries -- the lock screen, its\npolkit and keyring prompts -- and pauses. On the host, gowl cannot:\nunder Wayland a client's widget tree is private, so it suppresses\ncapture while the session is locked and while the focused window's\napp-id or title matches its deny list of credential applications,\nand a password typed into a form inside any other window IS\nrecorded. Every trace carries that sentence. Read a recording\nbefore you turn it into a skill.", "0.2.0" },

{ "agents.browser", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "A real web browser the agent can drive (needs a GSURF=1 build).", "0.9.0" },

{ "agents.browser.mode", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "none", clawt_browser_mode_get_type,
  "Whether this agent can drive a real web browser.\n"
  "\n"
  "`own` gives it a session of its own, `shared` one declared under\n"
  "browsers: (agents.browser.use names it), `none` neither -- the agent\n"
  "still has clawtilla_web_fetch. A session is gsurf on a private X\n"
  "display, started on the agent's first browser call and stopped after\n"
  "agents.browser.idle_minutes without one; it runs JavaScript, so it\n"
  "reads pages clawtilla_web_fetch cannot, and the agent can click and\n"
  "type. Watch it from the Computer page's Browser view.\n"
  "\n"
  "Needs clawtilla built with GSURF=1: an agent asking for a browser\n"
  "from a build without one is disabled with that reason rather than\n"
  "run without the tools it was told it has.", "0.9.0" },

{ "agents.browser.use", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "With agents.browser.mode: shared, the browsers: entry to join. Set,\n"
  "every other browser.* key comes from that entry.", "0.9.0" },

{ "agents.browser.home", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "about:blank", NULL,
  "The page a session opens on.", "0.9.0" },

{ "agents.browser.allow_scripts", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_DANGEROUS, "false", NULL,
  "Whether the agent is offered clawtilla_browser_eval, which runs its\n"
  "own JavaScript in the page.\n"
  "\n"
  "Off by default. Reading, clicking and typing cover what a person does;\n"
  "a script can read any field and post anything as the logged-in\n"
  "user without a single visible step.", "0.9.0" },

{ "agents.browser.idle_minutes", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "15", NULL,
  "Stop the session after this many minutes without a browser call. The\n"
  "profile stays, so a login survives; the open page does not. 0 keeps\n"
  "it running until its agent stops.", "0.9.0" },

{ "agents.browser.secrets", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "The vault: entries this agent may have filled into a page, by name.\n"
  "Empty, the agent is offered no secret tools. Always the agent's own,\n"
  "even on a shared browser: what an agent may ask for is about the\n"
  "agent, not the session.", "0.25.0" },

{ "agents.browser.width", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "1280", NULL,
  "The session's window width, in pixels.", "0.9.0" },

{ "agents.browser.height", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "800", NULL,
  "The session's window height, in pixels.", "0.9.0" },

{ "agents.env", CLAWT_SCHEMA_MAPPING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Environment variables for this agent's process.\n"
  "\n"
  "Only these and a small fixed set are passed through. The daemon's own\n"
  "environment is not inherited: a stray ANTHROPIC_API_KEY leaking into a\n"
  "subscription CLI would quietly move it onto pay-as-you-go billing\n"
  "nobody agreed to.", "0.1.0" },

{ "agents.credentials", CLAWT_SCHEMA_MAPPING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Secret references for this agent, written to credential files at 0600.\n"
  "\n"
  "  credentials:\n"
  "    anthropic_api_key: {env: ANTHROPIC_API_KEY}\n"
  "    matrix_token: {file: ~/.clawtilla/secrets/matrix-chief}\n"
  "\n"
  "Values are never echoed back over IPC; clients see only which keys are\n"
  "configured.", "0.1.0" },

{ "agents.integrations", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Channels this agent reaches the outside world through.\n"
  "\n"
  "libreclaw implements all of these; clawtilla renders the configuration,\n"
  "writes the credential files and health-checks them.", "0.1.0" },

{ "agents.integrations.matrix", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Matrix. Needs homeserver, user_id and an access token.", "0.1.0" },

{ "agents.integrations.matrix.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL, "Whether the Matrix channel is active.", "0.1.0" },

{ "agents.integrations.matrix.homeserver", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "Homeserver base URL.", "0.1.0" },

{ "agents.integrations.matrix.user_id", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "Full Matrix user id, e.g. @agent:example.com.", "0.1.0" },

{ "agents.integrations.matrix.access_token", CLAWT_SCHEMA_SECRET, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "Access token, as a secret reference.", "0.1.0" },

{ "agents.integrations.matrix.rooms", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "Room ids to join.", "0.1.0" },

{ "agents.integrations.matrix.require_mention", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_NONE, "true", NULL,
  "Only respond when named, so the agent is not a participant in every\n"
  "conversation in the room.", "0.1.0" },

{ "agents.integrations.email", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Email over IMAP and SMTP.", "0.1.0" },

{ "agents.integrations.email.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL, "Whether the email channel is active.", "0.1.0" },

{ "agents.integrations.email.imap_host", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "IMAP server hostname.", "0.1.0" },

{ "agents.integrations.email.imap_port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "993", NULL, "IMAP port.", "0.1.0" },

{ "agents.integrations.email.smtp_host", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "SMTP server hostname.", "0.1.0" },

{ "agents.integrations.email.smtp_port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "587", NULL, "SMTP port.", "0.1.0" },

{ "agents.integrations.email.username", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "Account username.", "0.1.0" },

{ "agents.integrations.email.password", CLAWT_SCHEMA_SECRET, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "Account password, as a secret reference.", "0.1.0" },

{ "agents.integrations.email.folders", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "Folders to watch. Defaults to INBOX.", "0.1.0" },

{ "agents.integrations.webhook", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL, "Inbound HTTP webhooks.", "0.1.0" },

{ "agents.integrations.webhook.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL, "Whether the webhook channel is active.", "0.1.0" },

{ "agents.integrations.webhook.port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "Port to listen on. 0 asks the kernel for a free one, which is what you\n"
  "want with several agents: a fixed port means the second agent to start\n"
  "fails to bind.", "0.1.0" },

{ "agents.integrations.webhook.bind_address", CLAWT_SCHEMA_STRING,
  CLAWT_SCHEMA_FLAG_NONE, "127.0.0.1", NULL,
  "Which address to listen on.\n"
  "\n"
  "The loopback by default. An endpoint in shell_command mode runs a\n"
  "command, and bearer tokens are otherwise the only thing between that\n"
  "and whoever can reach the port -- so widening it is something you say\n"
  "rather than something you get. Set it to 0.0.0.0 for every interface,\n"
  "or to a private interface's address.\n"
  "\n"
  "The routing itself is libreclaw's channels.webhook.endpoints, which\n"
  "goes in the agent's `libreclaw:` block: it is a list of objects with\n"
  "nested targets and has no sensible spelling as a flat key. clawtilla\n"
  "merges that block into the channel it renders rather than refusing\n"
  "it.", "0.1.0" },

{ "agents.integrations.local", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "libreclaw's stdin/stdout channel.\n"
  "\n"
  "At most one agent may have this, and only with runtime.type: process --\n"
  "it takes over file descriptors 0 and 1, which an embedded agent would be\n"
  "taking from the daemon.", "0.1.0" },

{ "agents.integrations.cmacs", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "libreclaw's in-process cmacs channel.", "0.1.0" },

{ "agents.federation_reach", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Which linked fleets may reach this agent, as peer selectors\n"
  "(`tier:owned`, `tag:client-acme`, `name:lab`, `owner:Dana`, `*`; see\n"
  "docs/federation.org#selectors). A fleet must match one of them *and* be\n"
  "allowed by its own grant: this can only take reach away, never give it.\n"
  "Unset, the grant alone decides. A conversation already under way is not\n"
  "cut off by a change here; a new one is.", "0.68.0" },

{ "agents.federation_may_reach", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Which linked fleets this agent may write to and hand work to, as peer\n"
  "selectors (`tier:owned`, `tag:client-acme`, `name:lab`, `*`; see\n"
  "docs/federation.org#selectors). Unset, an agent with tools.federation may\n"
  "reach any linked fleet; set, only those that match -- and it sees only\n"
  "their agents in clawtilla_list_agents. The other fleet's grant still\n"
  "decides what lands there.", "0.72.0" },

{ "agents.tools", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Which orchestration tools this agent may call.\n"
  "\n"
  "The clawtilla_* tools let an agent message its peers, delegate work,\n"
  "read its mailbox and drive its computer. A worker usually wants fewer\n"
  "of them than a chief-of-staff.", "0.1.0" },

{ "agents.tools.allow", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Tool names permitted. Unset means all of them except those needing a\n"
  "capability this agent does not have.", "0.1.0" },

{ "agents.tools.deny", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Tool names refused. Applied after allow, so a deny always wins.", "0.1.0" },

{ "agents.policy", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "What this agent may do without asking.\n"
  "\n"
  "Enforced on clawtilla's own tools, connectors and the desktop relay\n"
  "for every backend, and on the CLI's own tools (Bash, Write, ...) for\n"
  "claude-code and claude-tmux through a PreToolUse hook. Other CLIs run\n"
  "their own tools unchecked; docs/security.org says so in full.", "0.3.0" },

{ "agents.policy.rules", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "This agent's action rules, MATCH=MODE, first match wins. Defaults to\n"
  "defaults.policy.rules.", "0.3.0" },

{ "agents.policy.ask_timeout_seconds", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "How long an `ask` waits for a person. Defaults to\n"
  "defaults.policy.ask_timeout_seconds.", "0.3.0" },

{ "agents.tools.manage_fleet", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_DANGEROUS, "false", NULL,
  "Whether this agent may create other agents.\n"
  "\n"
  "Off by default, and worth thinking about before turning on: an agent\n"
  "that can create agents can give one a computer, and a container or a\n"
  "VM is a machine that runs code. It cannot exceed what you could\n"
  "create yourself -- every agent it makes goes through the same\n"
  "validation -- but it can do it without being asked twice.\n"
  "\n"
  "Intended for a chief-of-staff you talk to about what the fleet needs.",
  "0.1.0" },

{ "agents.tools.ephemeral", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_DANGEROUS, "false", NULL,
  "Whether this agent may spawn ephemeral agents: temporary helpers that\n"
  "do one piece of work and are then reaped (clawtilla_ephemeral_spawn,\n"
  "clawtilla_ephemeral_reap).\n"
  "\n"
  "On for the chief of staff unless set otherwise, off for every other\n"
  "agent. A helper copies how its parent runs -- runtime, confinement,\n"
  "model, tools, skills -- and can never do more: it cannot spawn helpers\n"
  "of its own, cannot create agents, and gets a computer no stronger than\n"
  "its parent's. It stays, across restarts, until the agent that spawned\n"
  "it reaps it; only that agent or you can. See docs/ephemeral-agents.org.",
  "0.55.0" },

{ "agents.tools.federation", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_DANGEROUS, "false", NULL,
  "Whether this agent may start conversations with agents in linked\n"
  "fleets (clawtilla_message_agent and clawtilla_delegate with\n"
  "`AGENT@ALIAS`) and sees them in clawtilla_list_agents.\n"
  "\n"
  "Off by default: what this agent writes leaves this machine for\n"
  "somebody else's fleet. Answering a message another fleet sent it needs\n"
  "nothing -- the answer goes back where the question came from. What the\n"
  "other fleet lets it reach is that fleet's grant, not this key. With\n"
  "tools.manage_fleet as well it may also manage links\n"
  "(clawtilla_federation_*). See docs/federation.org#messages.",
  "0.68.0" },

{ "agents.tools.office", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Whether this agent may read, change and apply the office's world packs\n"
  "and import assets into them (clawtilla_office_*).\n"
  "\n"
  "On for the chief of staff unless set otherwise, so the chief can run\n"
  "the office alone; on for the custodial team's agents, which are made\n"
  "with it; off for every other agent, and never offered to an ephemeral\n"
  "helper. Writes land only under the state directory, and a change that\n"
  "would stop a pack loading is refused. See\n"
  "docs/office-custodial-team.org.", "0.58.0" },

{ "agents.tools.schedule", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether this agent may schedule its own routines.\n"
  "\n"
  "With it on, the agent is offered clawtilla_routine_add, _update and\n"
  "_remove: it can set up a check on something for itself, move one to a\n"
  "better time, or split a long watchlist into several -- the way you\n"
  "would ask a colleague to remind themselves rather than doing it for\n"
  "them. Only ever routines that run on this agent, never another's,\n"
  "and never one you marked `locked`.\n"
  "\n"
  "On by default because a routine opens nothing -- unlike a trigger,\n"
  "which is an address on your network and stays yours to create. What\n"
  "it can cost is bounded by `tools.schedule_limit` and\n"
  "`tools.schedule_min_minutes`.", "0.7.1" },

{ "agents.tools.schedule_limit", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "8", NULL,
  "How many routines may run on this agent before it can add no more\n"
  "of its own.\n"
  "\n"
  "Counts every routine whose agent is this one, including those you\n"
  "wrote: the bound is on how often the agent is woken, not on who\n"
  "arranged it. Routines you add are never refused for it.", "0.7.1" },

{ "agents.tools.schedule_min_minutes", CLAWT_SCHEMA_INT,
  CLAWT_SCHEMA_FLAG_NONE, "15", NULL,
  "The shortest gap between two runs of a routine this agent schedules,\n"
  "in minutes.\n"
  "\n"
  "An agent that writes `*/1 * * * *` has written itself a turn a\n"
  "minute, which is a bill rather than a schedule. Checked over the\n"
  "schedule's next runs, so a cron expression whose runs bunch up --\n"
  "`0,1,2 9 * * *` -- is refused as well as one that is simply too\n"
  "frequent. Applies only to what an agent schedules for itself.", "0.7.1" },

{ "agents.mailbox", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Per-agent overrides of orchestration.mailbox.", "0.1.0" },

{ "agents.libreclaw", CLAWT_SCHEMA_MAPPING, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Merged verbatim into this agent's generated libreclaw config.yaml.\n"
  "\n"
  "An escape hatch for libreclaw settings clawtilla does not model. What is\n"
  "here wins over what clawtilla generates, so it can also be used to\n"
  "override something deliberately.", "0.1.0" }
};

const ClawtSchemaEntry *
clawt_config_schema_get(gsize *n_entries)
{
    if (n_entries != NULL)
        *n_entries = G_N_ELEMENTS(schema);

    return schema;
}


/* ── Options settable in two places ──────────────────────────────── */

/*
 * The agent-relative name for every option that also has a fleet-level
 * one.
 *
 * This is data rather than a rule because it is not derivable. The
 * mailbox keys drop their section -- an agent writes `mailbox.max_depth`
 * for `orchestration.mailbox.max_depth` -- while the memories keys keep
 * their whole name, and the `defaults.*` inheritances rename outright:
 * `computer.type` takes its default from `defaults.computer`.
 *
 * It lives here, beside the schema, because it *is* schema: it says
 * where an option can be written. It used to live in two other files,
 * privately, and the cost was concrete -- the daemon could not tell a
 * client which options an agent had, so neither client could offer the
 * nine that only appear here.
 *
 * tests/test-config-schema.c checks both halves of it against the schema
 * and against a real agent, so a PER_AGENT key added without an entry
 * here is a test failure rather than an option nobody can set.
 */
static const ClawtSchemaAgentKey agent_keys[] = {
    { "daily_agent_budget_micros", "orchestration.daily_agent_budget_micros" },
    { "max_waits",                 "orchestration.max_waits_per_agent" },
    { "crosstalk_summary",         "orchestration.crosstalk_summary" },
    { "crosstalk_summary_to",      "orchestration.crosstalk_summary_to" },
    /*
     * Fleet policy an agent may override. The `orchestration.` section
     * is the fleet's own, so the per-agent form drops it.
     */
    { "mailbox.max_depth",          "orchestration.mailbox.max_depth" },
    { "mailbox.overflow",           "orchestration.mailbox.overflow" },
    { "mailbox.max_attempts",       "orchestration.mailbox.max_attempts" },
    { "mailbox.lease_seconds",      "orchestration.mailbox.lease_seconds" },
    { "mailbox.default_ttl_seconds",
      "orchestration.mailbox.default_ttl_seconds" },
    { "mailbox.backoff_seconds",    "orchestration.mailbox.backoff_seconds" },

    /*
     * The memory store keeps its whole name inside an agent, because
     * `memories` is a subsystem rather than a section of fleet policy --
     * and there is no `defaults.memories` to inherit from, so the
     * top-level key *is* the fleet-wide setting.
     */
    { "memories.enabled",           "memories.enabled" },
    { "memories.max_results",       "memories.max_results" },
    { "memories.readers",           "memories.readers" },
    { "memories.scope",             "memories.scope" },
    { "memories.recall",            "memories.recall" },
    { "memories.summarise",         "memories.summarise" },
    { "memories.nudge_turns",       "memories.nudge_turns" },
    { "skills",                     "defaults.skills" },

    /*
     * `agents.*` options whose default comes from the `defaults:`
     * section. This is what makes that section mean anything: an agent
     * that says nothing about its model follows the fleet's choice
     * rather than the schema's.
     */
    { "model.provider",             "defaults.provider" },
    { "model.model",                "defaults.model" },
    { "computer.type",              "defaults.computer" },
    { "runtime.autostart",          "defaults.autostart" },
    { "policy.rules",               "defaults.policy.rules" },
    { "policy.ask_timeout_seconds", "defaults.policy.ask_timeout_seconds" },
    { "runtime.stream_steps",       "defaults.stream_steps" },
    { "runtime.restart",            "defaults.restart" },
    { "runtime.confine",            "defaults.confine" },
    { "computer.container.image",   "defaults.container_image" }
};

const ClawtSchemaAgentKey *
clawt_config_schema_agent_keys(gsize *n_entries)
{
    g_return_val_if_fail(n_entries != NULL, NULL);

    *n_entries = G_N_ELEMENTS(agent_keys);

    return agent_keys;
}

const gchar *
clawt_config_schema_agent_key_for(const gchar *fleet_key)
{
    gsize i;

    g_return_val_if_fail(fleet_key != NULL, NULL);

    for (i = 0; i < G_N_ELEMENTS(agent_keys); i++) {
        if (g_strcmp0(agent_keys[i].fleet_key, fleet_key) == 0)
            return agent_keys[i].agent_key;
    }

    return NULL;
}

const gchar *
clawt_config_schema_agent_name(const ClawtSchemaEntry *entry)
{
    g_return_val_if_fail(entry != NULL, NULL);

    if (entry->type == CLAWT_SCHEMA_SECTION ||
        entry->type == CLAWT_SCHEMA_MAPPING ||
        entry->type == CLAWT_SCHEMA_LIST_OF)
        return NULL;

    if (g_str_has_prefix(entry->key, "agents."))
        return entry->key + strlen("agents.");

    if (entry->flags & CLAWT_SCHEMA_FLAG_PER_AGENT)
        return clawt_config_schema_agent_key_for(entry->key);

    return NULL;
}

const gchar *
clawt_config_schema_fleet_key_for(const gchar *agent_key)
{
    gsize i;

    g_return_val_if_fail(agent_key != NULL, NULL);

    for (i = 0; i < G_N_ELEMENTS(agent_keys); i++) {
        if (g_strcmp0(agent_keys[i].agent_key, agent_key) == 0)
            return agent_keys[i].fleet_key;
    }

    return NULL;
}

const ClawtSchemaEntry *
clawt_config_schema_lookup(const gchar *key)
{
    gsize i;

    g_return_val_if_fail(key != NULL, NULL);

    for (i = 0; i < G_N_ELEMENTS(schema); i++)
    {
        if (g_strcmp0(schema[i].key, key) == 0)
            return &schema[i];
    }

    /*
     * A shared computer takes the keys of an agent's computer block, so
     * `computers.<k>` is answered by `agents.computer.<k>` -- one rule,
     * not a second copy of eighty rows to drift. `use` is the agent's own
     * and means nothing inside a machine.
     */
    if (g_str_has_prefix(key, "computers.") &&
        g_strcmp0(key, "computers.use") != 0) {
        g_autofree gchar *agent_key =
            g_strconcat("agents.computer.", key + strlen("computers."), NULL);

        for (i = 0; i < G_N_ELEMENTS(schema); i++)
            if (g_strcmp0(schema[i].key, agent_key) == 0)
                return &schema[i];
    }

    return NULL;
}

const gchar *
clawt_config_schema_type_name(ClawtSchemaType type)
{
    switch (type) {
    case CLAWT_SCHEMA_SECTION:     return "section";
    case CLAWT_SCHEMA_STRING:      return "string";
    case CLAWT_SCHEMA_BOOLEAN:     return "boolean";
    case CLAWT_SCHEMA_INT:         return "integer";
    case CLAWT_SCHEMA_DOUBLE:      return "number";
    case CLAWT_SCHEMA_ENUM:        return "enum";
    case CLAWT_SCHEMA_STRING_LIST: return "list of strings";
    case CLAWT_SCHEMA_PATH:        return "path";
    case CLAWT_SCHEMA_SECRET:      return "secret reference";
    case CLAWT_SCHEMA_MAPPING:     return "mapping";
    case CLAWT_SCHEMA_LIST_OF:     return "list of mappings";
    default:                       return "unknown";
    }
}

GPtrArray *
clawt_config_schema_comment_for(const gchar *key)
{
    const ClawtSchemaEntry *entry = clawt_config_schema_lookup(key);
    GPtrArray *out;
    g_auto(GStrv) lines = NULL;
    guint i;

    if (entry == NULL || entry->doc == NULL)
        return NULL;

    out = g_ptr_array_new_with_free_func(g_free);
    lines = g_strsplit(entry->doc, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
        g_ptr_array_add(out, g_strdup(lines[i]));

    return out;
}
