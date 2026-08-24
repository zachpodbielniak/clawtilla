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
  "debug logs every frame on every link, which is what you want when an\n"
  "agent will not connect and noise the rest of the time.", "0.1.0" },

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

{ "daemon.automation_dir", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_COMMENTED,
  "~/.clawtilla/pods", NULL,
  "Where podomation pods that watch the fleet live.\n"
  "\n"
  "Every `*.pod` in here is loaded at start. clawtilla registers itself\n"
  "into podomation as a module, so a pod can bind to what the fleet does\n"
  "and act on it in the same file:\n"
  "\n"
  "  pod restart_the_researcher {\n"
  "    source = Clawtilla.New(\"researcher\")\n"
  "    sink   = Clawtilla.New(\"researcher\")\n"
  "    on \"agent.state\" where state == \"error\" {\n"
  "      restart_agent(agent: \"researcher\")\n"
  "      notify(title: \"researcher fell over; restarted it\")\n"
  "    }\n"
  "  }\n"
  "\n"
  "The constructor's arguments are the scope, and it applies both ways:\n"
  "a pod named for one agent neither hears about the others nor can act\n"
  "on them. With no arguments, the whole fleet.\n"
  "\n"
  "A file that does not parse disables that file and warns; the rest are\n"
  "loaded.", "0.2.0" },

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

{ "defaults.container_image", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  "registry.fedoraproject.org/fedora:44", NULL,
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

{ "defaults.image_dir", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_NONE,
  "$XDG_DATA_HOME/clawtilla/images", NULL,
  "Where downloaded cloud images are kept.\n"
  "\n"
  "One copy serves the whole fleet: a VM agent writes to a qcow2 overlay\n"
  "backed by the image here, never to the image itself. Manage them with\n"
  "`clawtilla image vm`, or from Settings in the GTK client.", "0.1.0" },

{ "defaults.exchange_max_bytes", CLAWT_SCHEMA_INT, CLAWT_SCHEMA_FLAG_NONE,
  "1073741824", NULL,
  "Size cap for the exchange directory. 0 disables the limit.", "0.1.0" },

{ "defaults.libreclaw_binary", CLAWT_SCHEMA_PATH, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Which libreclaw to run for process-runtime agents.\n"
  "\n"
  "Unset means whatever is on PATH, which is what you want once libreclaw\n"
  "is installed. Point it at a build tree while developing against an\n"
  "unreleased libreclaw -- otherwise the daemon runs the installed copy\n"
  "and the change you are testing is not the one being exercised.",
  "0.1.0" },

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
/* ── memory ── */
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
  "memory you did not form is not a memory.", "0.1.0" },

{ "orchestration.chief_of_staff", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_COMMENTED,
  NULL, NULL,
  "Id of the agent that receives work addressed to the fleet.\n"
  "\n"
  "At most one agent may hold this. Setting it here is equivalent to\n"
  "chief_of_staff: true on that agent, and the daemon refuses to start if\n"
  "two agents claim it.", "0.1.0" },

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

/* ── integrations ────────────────────────────────────────────────── */
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
  "Which kind: matrix, email, webhook, local, cmacs or mcp.\n"
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
  "selected", clawt_integration_scope_get_type,
  "Who gets it: all, selected or none.\n"
  "\n"
  "`all` includes agents created later, which is the point of it -- a tool\n"
  "server the fleet shares should not need revisiting every time the fleet\n"
  "grows. `selected` uses the `agents:` list below. `none` keeps the\n"
  "instance and its credentials without handing it to anybody.", "0.2.0" },

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
  "read messages. `command` runs a program and hands it the text.", "0.2.0" },

{ "integrations.token", CLAWT_SCHEMA_SECRET, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "notify: the credential for ntfy, gotify or Matrix, as a secret\n"
  "reference.\n"
  "\n"
  "Resolved once when the configuration loads rather than on every\n"
  "notification: a `command` reference to a locked password manager\n"
  "blocks until it times out, and doing that each time something needed\n"
  "saying would make the notifier the slowest thing in the daemon.", "0.2.0" },

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
  "pasted into each agent's .mcp.json by hand.", "0.2.0" },

{ "integrations.args", CLAWT_SCHEMA_STRING_LIST, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL, "mcp: arguments for `command`.", "0.2.0" },

{ "integrations.url", CLAWT_SCHEMA_STRING, CLAWT_SCHEMA_FLAG_NONE,
  NULL, NULL,
  "mcp: an HTTP or SSE endpoint, instead of `command`.\n"
  "\n"
  "One or the other, never both: an entry naming a command and a URL is\n"
  "two different servers and there is no way to tell which was meant.", "0.2.0" },

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
  "is what tells them apart everywhere they are shown.", "0.2.0" },

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


/* ── connectors ──────────────────────────────────────────────────── */
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
  "on the network reaches the port first.", "0.2.0" },

/* ── routines ────────────────────────────────────────────────────── */
{ "routines", CLAWT_SCHEMA_LIST_OF, CLAWT_SCHEMA_FLAG_COMMENTED, NULL, NULL,
  "Standing work: a prompt, an agent, and when to run it.\n"
  "\n"
  "A routine is the thing you would otherwise remember to ask for every\n"
  "morning. It runs as a delegated task, so it has its own session and\n"
  "its own result, and one morning's run never contaminates the next.\n"
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
  "Run in a fresh git worktree of `directory` rather than in it.\n"
  "\n"
  "For a routine that changes files: it keeps a scheduled run from\n"
  "landing on top of whatever you had checked out at the time. The\n"
  "worktree is left behind deliberately -- throwing away work because a\n"
  "schedule fired is not recoverable.", "0.2.0" },

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
  "routines share one rate-limited service.", "0.2.0" },

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
  "registry.fedoraproject.org/fedora:44", NULL,
  "Image the container is created from.\n"
  "\n"
  "Any reference podman can pull. Unset means defaults.container_image.\n"
  "\n"
  "Name the registry: a bare 'fedora:44' is resolved through podman's\n"
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
  "having to exist already, and gets passwordless sudo unless it is root.",
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
