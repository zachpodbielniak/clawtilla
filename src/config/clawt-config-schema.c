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
  "Created with mode 0600, and the daemon checks the peer's uid via\n"
  "SO_PEERCRED. A unix socket is used rather than a loopback HTTP port\n"
  "because a port has no such notion of who is calling, and any web page\n"
  "the user visits can reach a loopback port through DNS rebinding.", "0.1.0" },

{ "daemon.state_dir", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  "~/.clawtilla", NULL,
  "Where agent workspaces, mailboxes, transcripts and credentials live.\n"
  "\n"
  "Created 0700. Never mounted into any agent's computer, whatever the\n"
  "mount configuration says -- an agent that could read this directory\n"
  "could read every other agent's credentials.", "0.1.0" },

{ "daemon.log_level", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "info", clawt_log_level_get_type,
  "How much the daemon says: error, warning, info, debug.\n"
  "\n"
  "debug logs every frame on every link, which is what you want when an\n"
  "agent will not connect and noise the rest of the time.", "0.1.0" },

{ "daemon.tcp_enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_COMMENTED,
  "false", NULL,
  "Also listen on TCP, so clients on other machines can connect.\n"
  "\n"
  "Off by default. The unix socket is authenticated by the kernel; a TCP\n"
  "listener is authenticated by whatever you configure below, so turning\n"
  "this on without TLS and a token puts the whole fleet on the network.", "0.1.0" },

{ "daemon.tcp_address", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_COMMENTED,
  "127.0.0.1", NULL,
  "Address to bind when tcp_enabled is true.", "0.1.0" },

{ "daemon.tcp_port", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_COMMENTED,
  "8792", NULL,
  "Port to bind when tcp_enabled is true.", "0.1.0" },

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
  "Ignored for unix socket connections, which the kernel already vouches for.", "0.1.0" },

{ "daemon.event_log_days", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "30", NULL,
  "How many days of per-room event logs to keep. 0 disables the sweep.\n"
  "\n"
  "The event log is the file you paste into a bug report. Secrets are\n"
  "scrubbed as it is written, not as it is displayed -- a transcript gets\n"
  "replayed into every context rebuild, so a leaked key in one would be\n"
  "permanent.", "0.1.0" },

{ "daemon.allow_unconfined_host", CLAWT_SCHEMA_BOOLEAN,
  CLAWT_SCHEMA_FLAG_DANGEROUS, "false", NULL,
  "Permit agents to use computer.host.confine: none.\n"
  "\n"
  "An unconfined host computer means the agent runs commands as you, with\n"
  "your files, your keys and your network. That needs to be two deliberate\n"
  "acts rather than one typo, so it requires this AND confirm_host_control\n"
  "on the agent itself.", "0.1.0" },

/* ── defaults ────────────────────────────────────────────────────── */
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
  "Default computer type for new agents: none, host, container or vm.\n"
  "\n"
  "none is the default on purpose. An agent that can run commands is a\n"
  "bigger grant than an agent that can only talk, and it should be asked\n"
  "for rather than inherited.", "0.1.0" },

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

{ "defaults.exchange_max_bytes", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "1073741824", NULL,
  "Size cap for the exchange directory. 0 disables the limit.", "0.1.0" },

{ "defaults.restart", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  "on-failure", clawt_restart_policy_get_type,
  "Default restart policy: never, on-failure or always.", "0.1.0" },

{ "defaults.autostart", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Whether new agents start with the daemon.", "0.1.0" },

/* ── ai_assist ───────────────────────────────────────────────────── */
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
  "claude-code", NULL,
  "Provider used for the designer.", "0.1.0" },

{ "ai_assist.model", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "opus", NULL,
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

{ "orchestration.task_budget_usd", CLAWT_SCHEMA_DOUBLE, CLAWT_SCHEMA_FLAG_NONE,
  "5.0", NULL,
  "Spend cap for one delegated task and everything it spawns.\n"
  "0 disables the cap.", "0.1.0" },

{ "orchestration.cycle_window", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "10", NULL,
  "How many recent messages per room are remembered for loop detection.\n"
  "\n"
  "A message repeating one already in the window is refused. This catches\n"
  "the case the hop limit does not: two agents alternating the same two\n"
  "replies, each one a fresh chain.", "0.1.0" },

{ "orchestration.chief_of_staff", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Id of the agent that receives work addressed to the fleet.\n"
  "\n"
  "At most one agent may hold this. Setting it here is equivalent to\n"
  "chief_of_staff: true on that agent, and the daemon refuses to start if\n"
  "two agents claim it.", "0.1.0" },

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

/* ── rooms ───────────────────────────────────────────────────────── */
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
  "Only deliver messages that name an agent.\n"
  "\n"
  "Worth turning on for a busy room: without it every agent takes a turn\n"
  "on every message, which is expensive and rarely wanted.", "0.1.0" },

{ "rooms.max_hops", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Overrides orchestration.max_hops for this room.", "0.1.0" },

/* ── agents ──────────────────────────────────────────────────────── */
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

{ "agents.avatar", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Path to an avatar image.", "0.1.0" },

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
  "The convention is SOUL.md for character, IDENTITY.md for role,\n"
  "AGENTS.md for how to work with others, TOOLS.md for what it has and\n"
  "USER.md for who you are. Splitting them means one can be edited without\n"
  "rewriting the rest.", "0.1.0" },

{ "agents.memory", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "The agent's long-term memory file.\n"
  "\n"
  "MEMORY.md in the workspace is loaded into the system prompt every turn,\n"
  "under a budget. memory/<topic>.md files are read on demand by the agent\n"
  "instead, so a large memory does not cost every turn.", "0.1.0" },

{ "agents.memory.enabled", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "true", NULL,
  "Whether MEMORY.md is loaded into the prompt.", "0.1.0" },

{ "agents.memory.max_lines", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "200", NULL,
  "Line budget for MEMORY.md. Beyond it the file is truncated with a note.", "0.1.0" },

{ "agents.memory.max_bytes", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "24000", NULL,
  "Byte budget for MEMORY.md, applied alongside max_lines.", "0.1.0" },

{ "agents.model", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Which model this agent thinks with.", "0.1.0" },

{ "agents.model.provider", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Provider name. Defaults to defaults.provider.", "0.1.0" },

{ "agents.model.model", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Model name. Defaults to defaults.model.", "0.1.0" },

{ "agents.model.effort", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "Reasoning effort: low, medium, high, xhigh or max, when the model has one.", "0.1.0" },

{ "agents.model.routing_profile", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "libreclaw smart-routing profile: off, eco, auto, premium or free.", "0.1.0" },

{ "agents.model.fallbacks", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Models to fall back to, in order, when the primary is unavailable.", "0.1.0" },

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

/* ── agents.computer ─────────────────────────────────────────────── */
{ "agents.computer", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE, NULL, NULL,
  "What the agent can run commands on.", "0.1.0" },

{ "agents.computer.type", CLAWT_SCHEMA_ENUM, CLAWT_SCHEMA_FLAG_NONE,
  NULL, clawt_computer_type_get_type,
  "none, host, container or vm. Defaults to defaults.computer.\n"
  "\n"
  "Desktop control is not a type -- it is the desktop block below, and\n"
  "works alongside any of these.", "0.1.0" },

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
  "none", clawt_relabel_get_type,
  "SELinux relabelling for a container bind mount: none, shared or private.\n"
  "\n"
  "This matters on Fedora Silverblue and friends, where an unlabelled bind\n"
  "mount is visible in the container but every access is denied -- which\n"
  "reads like a permissions bug rather than a labelling one. shared (:z)\n"
  "is usually what you want. private (:Z) rewrites the host directory's\n"
  "labels exclusively for this container, which will break anything else\n"
  "using that directory.", "0.1.0" },

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
  "Checked after allow_paths, so ~/.ssh stays out even when all of ~ is in.", "0.1.0" },

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

{ "agents.computer.host.shell", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  "/bin/sh", NULL,
  "Shell used for interactive sessions and shell-form commands.", "0.1.0" },

{ "agents.computer.host.nice", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "0", NULL,
  "Scheduling niceness for the agent's commands, so a runaway build does\n"
  "not make the desktop unusable.", "0.1.0" },

/* ── agents.computer.container ───────────────────────────────────── */
{ "agents.computer.container", CLAWT_SCHEMA_SECTION, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Settings for computer.type: container.", "0.1.0" },

{ "agents.computer.container.image", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "docker.io/library/debian:stable-slim", NULL,
  "Image the container is created from.", "0.1.0" },

{ "agents.computer.container.name", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Container name. Defaults to clawt-<agent-id>.", "0.1.0" },

{ "agents.computer.container.connection", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Podman connection: a socket path, ssh://user@host/socket, tcp://host:port,\n"
  "or a named connection. Defaults to the user's podman socket.", "0.1.0" },

{ "agents.computer.container.network", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Network to attach to. Leave unset for podman's default.", "0.1.0" },

{ "agents.computer.container.keep", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Keep the container when the agent stops, instead of removing it.\n"
  "Useful when the agent installs things it should not have to reinstall.", "0.1.0" },

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

{ "agents.computer.vm.image", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Base disk image. A qcow2 overlay is created on top, so the base is\n"
  "never written to and several agents can share one.", "0.1.0" },

{ "agents.computer.vm.cpus", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "2", NULL,
  "Virtual CPUs.", "0.1.0" },

{ "agents.computer.vm.memory_mb", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "2048", NULL,
  "Memory in megabytes.", "0.1.0" },

{ "agents.computer.vm.ssh_user", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "root", NULL,
  "User commands are run as inside the guest, over SSH.", "0.1.0" },

{ "agents.computer.vm.ssh_key", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "Private key used to reach the guest.", "0.1.0" },

{ "agents.computer.vm.snapshot_on_start", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Take a snapshot each time the VM starts, so a session can be rolled back.\n"
  "libvirt backend only.", "0.1.0" },

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
  "auto, gowl or gnome.\n"
  "\n"
  "auto tries gowl's MCP socket first and falls back to gnome-desktop-mcp.\n"
  "gowl is the better fit where it is available: same tool vocabulary,\n"
  "native, and no Python or GNOME Shell extension in the way.", "0.1.0" },

{ "agents.computer.desktop.socket", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  "$XDG_RUNTIME_DIR/gowl-mcp.sock", NULL,
  "gowl's MCP socket. Requires gowl built with MCP=1 and modules.mcp\n"
  "enabled in its config.", "0.1.0" },

{ "agents.computer.desktop.allow_input", CLAWT_SCHEMA_BOOLEAN, CLAWT_SCHEMA_FLAG_NONE,
  "false", NULL,
  "Permit key and pointer injection, not just observation.\n"
  "\n"
  "With this false the agent can take screenshots and query windows but\n"
  "cannot act, which is a useful amount of access on its own and a much\n"
  "smaller grant.", "0.1.0" },

/* ── agents: the rest ────────────────────────────────────────────── */
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
