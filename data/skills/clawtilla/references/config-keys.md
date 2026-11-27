# Configuration keys

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

Every option in `clawtilla.yaml`: its type, default and the first
sentence of its documentation.  The whole text of each is in
`config/<section>.md` beside this file.  A key under `agents.` is
written inside an agent's block without that prefix; the agent
column names a fleet key's spelling inside an agent, where it has
one.  **D** marks a dangerous key (it hands over real authority;
see security.md).

## `daemon` ([config/daemon.md](config/daemon.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `daemon` | section |  |  | The daemon itself: where it listens, where it keeps state, how loud it is. |
| `daemon.socket` | path | `$XDG_RUNTIME_DIR/clawtilla/daemon.sock` |  | Unix socket the clients connect to. |
| `daemon.git` | boolean | `true` |  | Make the state directory a git repository on first start. |
| `daemon.state_dir` | path | `~/.clawtilla` |  | Where agent workspaces, mailboxes, transcripts and credentials live. |
| `daemon.pod_module_dir` | path |  |  | Where podomation's loadable modules are, if they are not found for you. |
| `daemon.log_level` | enum | `info` |  | How much the daemon says: error, warning, info, debug. |
| `daemon.tcp_enabled` | boolean | `false` |  | Also listen on TCP, so clients on other machines can connect. |
| `daemon.tailscale` | boolean | `true` |  | Also listen on this machine's tailnet address, when there is one. |
| `daemon.tcp_address` | string | `127.0.0.1` |  | Address to bind when tcp_enabled is true. |
| `daemon.tcp_port` | integer | `8792` |  | Port to bind for tcp_enabled and for tailscale. |
| `daemon.tls_cert` | path |  |  | TLS certificate for the TCP listener. |
| `daemon.tls_key` | path |  |  | TLS private key matching tls_cert. |
| `daemon.token_file` | path |  |  | File holding the bearer token remote clients must present. |
| `daemon.device_idle_days` | integer | `90` |  | Days a paired device may go unseen before its token stops working. 0 means never. |
| `daemon.event_log_days` | integer | `30` |  | How many days of per-room event logs to keep. |
| `daemon.routine_history_days` | integer | `62` |  | How many days of each routine's run history to keep: what happened at every scheduled time -- started (and which task), failed, refused, missed while the daemon was down, or folded into a catch-up run. |
| `daemon.calendar_max_days` | integer | `62` |  | The longest window one routine.occurrences request may ask for, in days. |
| `daemon.calendar_max_occurrences` | integer | `5000` |  | The most occurrences one routine.occurrences request may return, across every routine and libreclaw cron in the window. |
| `daemon.allow_unconfined_host` | boolean | `false` |  | **D** Permit agents to use computer.host.confine: none. |
| `daemon.automation_dir` | path | `~/.clawtilla/pods` |  | Where podomation pods that watch the fleet live. |
| `daemon.webhook_enabled` | boolean | `false` |  | Whether to accept inbound trigger deliveries. |
| `daemon.webhook_port` | integer | `8788` |  | Port the trigger receiver listens on. |
| `daemon.update_check` | boolean | `false` |  | Whether to ask, on a timer, if a newer clawtilla exists. |
| `daemon.update_url` | string | `https://gitlab.com/api/v4/projects/zachpodbielniak%2Fclawtilla/releases` |  | Where daemon.update_check asks. |
| `daemon.update_interval_hours` | integer | `24` |  | How often to ask, in hours. |

## `defaults` ([config/defaults.md](config/defaults.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `defaults` | section |  |  | What a new agent gets when its own block does not say. |
| `defaults.provider` | string | `claude-code` | `model.provider` | Default AI provider, naming an entry in the agent's libreclaw providers. |
| `defaults.model` | string | `sonnet` | `model.model` | Default model for new agents. |
| `defaults.computer` | enum | `none` | `computer.type` | Default computer type for new agents. |
| `defaults.container_image` | string | `registry.fedoraproject.org/fedora:45` | `computer.container.image` | Image a container agent gets when it does not name one. |
| `defaults.container_images` | list of strings |  |  | Extra images to offer in a client's image list. |
| `defaults.container` | section |  |  | Container limits every container and distrobox agent gets unless its team or the agent itself says otherwise. |
| `defaults.container.cpus` | string |  |  | The fleet's agents.computer.container.cpus. |
| `defaults.container.cpu_weight` | integer |  |  | The fleet's agents.computer.container.cpu_weight. |
| `defaults.container.cpuset` | string |  |  | The fleet's agents.computer.container.cpuset. |
| `defaults.container.memory` | string |  |  | The fleet's agents.computer.container.memory. |
| `defaults.container.memory_swap` | string |  |  | The fleet's agents.computer.container.memory_swap. |
| `defaults.container.pids_limit` | integer |  |  | The fleet's agents.computer.container.pids_limit. |
| `defaults.workspace_root` | path | `~/.clawtilla/agents` |  | Where per-agent workspaces are scaffolded. |
| `defaults.exchange_dir` | path | `$XDG_DATA_HOME/clawtilla/exchange` |  | Shared drop-box mounted into every computer at /mnt/clawtilla/exchange. |
| `defaults.mounts` | list of mappings |  |  | Host paths shared into *every* agent's computer. |
| `defaults.mounts.source` | path |  |  | Host path to share. |
| `defaults.mounts.target` | path |  |  | Absolute path inside the computer. |
| `defaults.mounts.mode` | enum | `ro` |  | ro or rw. |
| `defaults.mounts.type` | enum | `bind` |  | bind, volume, virtiofs, 9p or tmpfs. |
| `defaults.mounts.relabel` | enum | `shared` |  | SELinux relabelling: none, shared (:z) or private (:Z). |
| `defaults.mounts.scope` | enum | `all` |  | Who gets this folder: all, selected or none. |
| `defaults.mounts.agents` | list of strings |  |  | Agent ids that get this folder, when `scope: selected`. |
| `defaults.mounts.teams` | list of strings |  |  | Team ids that get this folder, when `scope: selected`. |
| `defaults.mounts.create` | boolean | `false` |  | Create the source directory if it is missing. |
| `defaults.mounts.size` | string |  |  | Size for a tmpfs mount, such as 512M. |
| `defaults.mounts.required` | boolean | `true` |  | Whether a missing source stops the agent starting. |
| `defaults.image_dir` | path | `$XDG_DATA_HOME/clawtilla/images` |  | Where downloaded cloud images are kept. |
| `defaults.exchange_max_bytes` | integer | `1073741824` |  | Size cap for the exchange directory. |
| `defaults.libreclaw_binary` | path |  |  | Which libreclaw to run for process-runtime agents. |
| `defaults.restart` | enum | `on-failure` | `runtime.restart` | Default restart policy: never, on-failure or always. |
| `defaults.autostart` | boolean | `false` | `runtime.autostart` | Whether new agents start with the daemon. |
| `defaults.stream_steps` | boolean | `true` | `runtime.stream_steps` | Whether agents report what they are doing while a turn runs. |
| `defaults.confine` | enum | `none` | `runtime.confine` | Whether agents' own CLIs run inside a bubblewrap sandbox: none or bwrap. |
| `defaults.avatar_max_bytes` | integer | `4194304` |  | Largest profile picture the daemon will read and serve. |
| `defaults.skills` | list of strings |  | `skills` | Skills every agent gets unless it says otherwise. |
| `defaults.policy` | section |  |  | What agents may do without asking. |
| `defaults.policy.rules` | list of strings |  | `policy.rules` | Action rules every agent follows unless it has its own list. |
| `defaults.policy.ask_timeout_seconds` | integer | `600` | `policy.ask_timeout_seconds` | How long an `ask` waits for a person before it is refused. |
| `defaults.policy.reviewer` | string |  |  | The decision model `review` asks: typesafe, ollama or laya (ai-glib's deciders). |

## `ai_assist` ([config/ai_assist.md](config/ai_assist.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `ai_assist` | section |  |  | The model that helps design new agents. |
| `ai_assist.enabled` | boolean | `true` |  | Whether AI-assisted agent creation is offered. |
| `ai_assist.provider` | string | `claude` |  | Provider used for the designer. |
| `ai_assist.model` | string | `claude-opus-5` |  | Model used for the designer. |
| `ai_assist.max_turns` | integer | `20` |  | Turn cap for the design conversation, so a model that never commits stops rather than looping. |

## `secrets` ([config/secrets.md](config/secrets.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `secrets` | section |  |  | How secret references in this file are resolved. |
| `secrets.default_backend` | enum | `file` |  | Backend assumed when a reference gives a bare string: file, env or command. |
| `secrets.dir` | path | `~/.clawtilla/secrets` |  | Directory a bare file reference is resolved against. |
| `secrets.command_timeout_seconds` | integer | `10` |  | How long a command-backend secret may take before it is an error. |

## `egress` ([config/egress.md](config/egress.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `egress` | section |  |  | Where an agent with runtime.network: gated may connect. |
| `egress.allow` | list of strings |  |  | Private addresses gated agents may reach anyway. |

## `plugins` ([config/plugins.md](config/plugins.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `plugins` | section |  |  | Loadable plugins: event handlers, extra tools, computer backends and integrations. |
| `plugins.enabled` | boolean | `true` |  | Whether plugins are scanned for at all. |
| `plugins.dirs` | list of strings |  |  | Extra directories to scan, searched before the XDG and system paths. |
| `plugins.disabled` | list of strings |  |  | Plugin ids to skip, by the <id> in libclawt-plugin-<id>.so. |

## `orchestration` ([config/orchestration.md](config/orchestration.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `orchestration` | section |  |  | How agents talk to each other, and what stops them doing it forever. |
| `orchestration.max_hops` | integer | `8` |  | How far a message may travel agent-to-agent before it is refused. |
| `orchestration.rate_limit_per_minute` | integer | `30` |  | Messages one agent may send per minute. |
| `orchestration.daily_fleet_budget_micros` | integer | `0` |  | Daily fleet admission cap in integer millionths of USD; 0 disables. Uses recorded usage since local midnight, including reset archives. Already running work can overshoot; new deliveries wait until reset. |
| `orchestration.daily_agent_budget_micros` | integer | `0` | `daily_agent_budget_micros` | Daily admission cap per agent in integer millionths of USD; 0 disables. Override with daily_agent_budget_micros on an agent. Uses local midnight and persisted usage; in-flight work can overshoot. |
| `orchestration.meshy_credit_micros` | integer | `0` |  | What one Meshy credit costs you, in integer millionths of USD (20000 for two cents); 0, the default, leaves it unknown. |
| `orchestration.task_budget_usd` | number | `5.0` |  | Spend cap for one delegated task and everything it spawns. |
| `orchestration.idle_hours` | integer | `72` |  | How long without a turn before the Fleet page calls an agent idle. |
| `orchestration.cycle_window` | integer | `10` |  | How many recent messages per room are remembered for loop detection. |
| `orchestration.cycle_seconds` | integer | `300` |  | How long a repeated message counts as a loop. |
| `orchestration.ack_check_model` | string | `` |  | A small model that decides whether an answer only acknowledges, as provider/model -- claude/claude-haiku-4-5-20251001, ollama/llama3.2. Empty turns the check off. |
| `orchestration.tree_message_budget` | integer | `60` |  | How many requests one task tree may send between settlements. 0 disables the budget. |
| `orchestration.pair_message_budget` | integer | `12` |  | How many rounds of requests two agents may send each other with no person in between. |
| `orchestration.pair_quiet_minutes` | integer | `30` |  | How long two agents must send each other nothing before their pair budget starts again. |
| `orchestration.ephemeral_max_per_parent` | integer | `5` |  | How many ephemeral agents one agent may have at once. |
| `orchestration.ephemeral_max_fleet` | integer | `20` |  | How many ephemeral agents the whole fleet may have at once, whoever spawned them. |
| `orchestration.ephemeral_reap_reminder_hours` | integer | `24` |  | How long an ephemeral agent may sit with its work finished and nothing assigned before its parent is reminded, once, to reap it. |
| `orchestration.answer_in_request_room` | boolean | `true` |  | Hand an agent's answer to the asker in the conversation it asked from, rather than in the room the two agents share. |
| `orchestration.crosstalk_summary` | enum | `chief` | `crosstalk_summary` | Tell you about agent-to-agent conversations: chief, on or off. |
| `orchestration.crosstalk_summary_to` | enum | `source` | `crosstalk_summary_to` | Where a cross-talk summary is posted: source or chat. |
| `orchestration.config_notes` | enum | `agents` |  | Say in an agent's chat when its configuration was changed: agents, all or off. |
| `orchestration.crosstalk_summary_model` | string | `` |  | A small model that rewrites each cross-talk summary as one friendly paragraph, as provider/model -- the same spelling as orchestration.ack_check_model. |
| `orchestration.mailbox` | section |  |  | Defaults for every agent's mailbox. |
| `orchestration.mailbox.max_depth` | integer | `1000` | `mailbox.max_depth` | How many undelivered items a mailbox holds before overflow applies. |
| `orchestration.mailbox.overflow` | enum | `reject` | `mailbox.overflow` | What a full mailbox does: reject, drop-oldest or block-sender. |
| `orchestration.mailbox.max_attempts` | integer | `5` | `mailbox.max_attempts` | Delivery attempts before an item is dead-lettered rather than retried. |
| `orchestration.mailbox.lease_seconds` | integer | `300` | `mailbox.lease_seconds` | How long an agent has to acknowledge an item before it returns to the queue. |
| `orchestration.mailbox.default_ttl_seconds` | integer | `604800` | `mailbox.default_ttl_seconds` | How long an undelivered item lives before it is swept. |
| `orchestration.mailbox.backoff_seconds` | integer | `30` | `mailbox.backoff_seconds` | Base for the exponential retry backoff, doubling per attempt, with jitter applied so a dependency that failed for every queued message does not bring them all back at the same instant. |
| `orchestration.chief_of_staff` | string |  |  | Id of the agent that receives work addressed to the fleet. |
| `orchestration.repeat_thresholds` | list of strings | `5,10,20` |  | Repeat counts worth reporting when an agent keeps making the same call. |
| `orchestration.repeat_max_keys` | integer | `256` |  | How many distinct calls one turn remembers while watching for repeats. |
| `orchestration.handoff_max_per_turn` | integer | `4` |  | How many handoffs one turn may queue. |
| `orchestration.handoff_busy_retries` | integer | `3` |  | How many times a handoff waits for a busy assignee before giving up and saying so. |
| `orchestration.handoff_receipt_days` | integer | `2` |  | How long a finished handoff's receipt is kept. |
| `orchestration.review_rounds` | integer | `3` |  | How many times a reviewer may send a task back for changes before the operator decides. |
| `orchestration.checkout_keep_days` | integer | `14` |  | How long a finished task's checkout is kept, in days. |
| `orchestration.task_retention_days` | integer | `180` |  | How long a finished task is kept, in days. |
| `orchestration.max_waits_per_agent` | integer | `10` | `max_waits` | How many pending waits one agent may have. |
| `orchestration.incidents` | section |  |  | Reports of runs that broke, sent to whoever leads the team. |
| `orchestration.incidents.enabled` | boolean | `true` |  | Whether broken runs open incidents at all. |
| `orchestration.incidents.to` | enum | `lead` |  | Who an incident report goes to: lead, chief or none. |

## `memories` ([config/memories.md](config/memories.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `memories` | section |  |  | What an agent remembers between conversations. |
| `memories.enabled` | boolean | `true` | `memories.enabled` | Whether agents get a memory store at all. |
| `memories.max_results` | integer | `20` | `memories.max_results` | How many memories one search or listing returns at most. |
| `memories.readers` | string |  | `memories.readers` | Comma-separated ids of agents allowed to read this agent's memories. |
| `memories.scope` | enum | `agent` | `memories.scope` | Where this agent's own memories are written: agent, team or fleet. |
| `memories.recall` | boolean | `true` | `memories.recall` | Whether the agent may search past conversations. |
| `memories.summarise` | boolean | `false` | `memories.summarise` | Whether finished work is summarised into memories automatically. |
| `memories.nudge_turns` | integer | `0` | `memories.nudge_turns` | The cadence the reminder to record what was learned asks for. |
| `memories.journal_keep` | integer | `1000` |  | How many changes each memory database's journal keeps. |
| `memories.operator_profile` | boolean | `false` |  | Whether the fleet keeps a profile of the person it works for. |
| `memories.review` | section |  |  | A periodic review of each agent's memories by a small model, which proposes merges, forgets and pins for a person to accept one at a time. |
| `memories.review.model` | string | `` |  | The model that reviews memories, as provider/model -- claude/claude-haiku-4-5-20251001, ollama/llama3.2. |
| `memories.review.interval_hours` | integer | `24` |  | How often each agent's memories are reviewed, in hours (1 to 720). |
| `memories.review.max_memories` | integer | `60` |  | How many of an agent's memories one pass reads at most, newest first (5 to 500). |

## `sentinel` ([config/sentinel.md](config/sentinel.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `sentinel` | section |  |  | What `clawtilla sentinel` looks for in the event log. |
| `sentinel.days` | integer | `7` |  | How many days of the event log one scan reads, counting today. |
| `sentinel.min_failures` | integer | `3` |  | How many failures of one agent's tool make an issue. |
| `sentinel.queue_wait_seconds` | integer | `600` |  | How long a message may wait in an agent's queue, from being queued to being delivered, before the wait is an issue. |
| `sentinel.daily_spend_micros` | integer | `10000000` |  | One agent's spend in one day, in micro-dollars, that makes an issue. 0 reports only the days a cap was reached. |

## `skills` ([config/skills.md](config/skills.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `skills` | section |  |  | Reusable procedure, shared across the fleet. |
| `skills.enabled` | boolean | `true` |  | Whether skills are scanned and linked at all. |
| `skills.dir` | path | `~/.clawtilla/skills` |  | Where the fleet's skills live. |
| `skills.extra_dirs` | list of strings | `~/.agents/skills` |  | Other directories of skills to read, on the daemon's machine. |
| `skills.teach_max_seconds` | integer | `900` |  | How long a demonstration may record before it stops itself. |
| `skills.teach_max_events` | integer | `20000` |  | How many recorded steps a demonstration keeps. |
| `skills.max_proposals_per_day` | integer | `3` |  | How many skills one agent may propose in any 24 hours. |

## `rooms` ([config/rooms.md](config/rooms.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `rooms` | list of mappings |  |  | Standing rooms, created at startup if they do not exist. |
| `rooms.id` | string |  |  | Identifier used to address the room. |
| `rooms.name` | string |  |  | Display name. |
| `rooms.members` | list of strings |  |  | Agent ids in the room. |
| `rooms.require_mention` | boolean | `false` |  | Only deliver messages that name a member. |
| `rooms.max_hops` | integer |  |  | Overrides orchestration.max_hops for this room. |
| `rooms.max_chain` | integer | `3` |  | How many replies one thread may run to in a round of a group room. 0 removes the cap; the once-per-thread rule below still applies. |
| `rooms.turn_timeout_seconds` | integer | `0` |  | How long one member may hold a room's turn before it is yielded. 0, the default, turns it off. |
| `rooms.order` | integer |  |  | Where this room sits in the sidebar, among the agents. |
| `rooms.team` | string |  |  | Which team's group this room appears under in the sidebar. |
| `rooms.catchup_messages` | integer | `20` |  | How much of a room a member is caught up on when it is named. |

## `projects` ([config/projects.md](config/projects.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `projects` | list of mappings |  |  | Projects: the pieces of work the fleet is pointed at. |
| `projects.id` | string |  |  | Identifier used to address the project: `project` on a delegation or a new task, and the task lists' filter. |
| `projects.name` | string |  |  | Display name; the id when unset. |
| `projects.directory` | path |  |  | The project's repository or directory, as a host path. |
| `projects.goal` | string |  |  | What the project is for, in a sentence or two. |
| `projects.review_by` | string |  |  | Who reviews work in this project, as agents.review_by spells it. |
| `projects.agents` | list of strings |  |  | The agents that work in this project, by id. |
| `projects.monthly_budget_usd` | number | `0` |  | What the project may spend in a calendar month, in US dollars. |
| `projects.budget_warn_percent` | integer | `80` |  | At what share of monthly_budget_usd the operator is warned, once a month. |
| `projects.budget_hard_stop` | boolean | `false` |  | Refuse new work in the project once its monthly budget is spent. |
| `projects.venture_repo_id` | integer | `0` |  | The VENTURE repository (a `forge_repo` id) this project's tasks are mirrored against. |
| `projects.venture_milestone_id` | integer | `0` |  | The VENTURE milestone this project's mirrored tickets are filed under. |
| `projects.venture_goal_id` | integer | `0` |  | The VENTURE goal this project's mirrored tickets count toward. |

## `teams` ([config/teams.md](config/teams.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `teams` | list of mappings |  |  | Teams, so a fleet larger than a handful has a shape. |
| `teams.id` | string |  |  | Identifier used to address the team, and what agents.team names. |
| `teams.name` | string |  |  | Display name. |
| `teams.description` | string |  |  | What this team is for, and what to send it. |
| `teams.color` | string |  |  | Accent colour in the clients, as a hex string. |
| `teams.order` | integer | `0` |  | Where this team sits in the list, lowest first. |
| `teams.tags` | list of strings |  |  | Labels that give this team a job in the software, by tag rather than by name, so the team can be called whatever you like. |
| `teams.skills` | list of strings |  |  | Skills every agent on this team gets. |
| `teams.broker_server` | string |  |  | The broker's gateway server for this team's agents. |
| `teams.computer` | section |  |  | Defaults for the computers of this team's agents. |
| `teams.computer.container` | section |  |  | Container limits for this team's container and distrobox agents. |
| `teams.computer.container.cpus` | string |  |  | The team's agents.computer.container.cpus. |
| `teams.computer.container.cpu_weight` | integer |  |  | The team's agents.computer.container.cpu_weight. |
| `teams.computer.container.cpuset` | string |  |  | The team's agents.computer.container.cpuset. |
| `teams.computer.container.memory` | string |  |  | The team's agents.computer.container.memory. |
| `teams.computer.container.memory_swap` | string |  |  | The team's agents.computer.container.memory_swap. |
| `teams.computer.container.pids_limit` | integer |  |  | The team's agents.computer.container.pids_limit. |

## `computers` ([config/computers.md](config/computers.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `computers` | list of mappings |  |  | Machines several agents share. |
| `computers.id` | string |  |  | Identifier, and what agents.computer.use names. |
| `computers.description` | string |  |  | What this machine is for. |

## `browsers` ([config/browsers.md](config/browsers.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `browsers` | list of mappings |  |  | Web browser sessions several agents share. |
| `browsers.id` | string |  |  | Identifier, and what agents.browser.use names. |
| `browsers.description` | string |  |  | What this session is for -- say which logins it holds. |

## `vault` ([config/vault.md](config/vault.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `vault` | list of mappings |  |  | Secrets an agent can have typed into a web page without ever seeing them: passwords, and TOTP seeds the daemon turns into one-time codes. |
| `vault.name` | string |  |  | What an agent asks for, and what agents.browser.secrets lists. |
| `vault.description` | string |  |  | Whose login this is, or which field it belongs in. |
| `vault.kind` | enum | `password` |  | `password` types the value itself. |
| `vault.secret` | secret reference |  |  | A secret reference, never the value: {file: ...}, {env: ...} or {command: ...}, resolved the way secrets: says, at the moment of the fill and not before. |
| `vault.sites` | list of strings |  |  | The hosts the secret may be typed into: a page on any other host is refused, so an agent steered to a look-alike site cannot be made to fill it there. |

## `accounts` ([config/accounts.md](config/accounts.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `accounts` | list of mappings |  |  | Separate logins for an agent CLI, so two agents on one machine can spend two subscriptions. |
| `accounts.id` | string |  |  | Identifier, and what agents.model.account names. |
| `accounts.provider` | string |  |  | The agent CLI this login is for: claude-code, claude-tmux, codex-cli, grok-build or opencode. |
| `accounts.config_dir` | path |  |  | The directory the CLI keeps this login in. |
| `accounts.description` | string |  |  | Whose login this is, or what it is for. |
| `accounts.pay_per_use` | boolean | `false` |  | This login bills each use -- an API key -- rather than drawing on a subscription's limit. |

## `integrations` ([config/integrations.md](config/integrations.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `integrations` | list of mappings |  |  | Named integrations, each handed to one agent, some agents or all of them. |
| `integrations.name` | string |  |  | What this instance is called. |
| `integrations.type` | string |  |  | Which kind: matrix, email, webhook, local, cmacs, mcp, connector, libresearch, grokbot, meshy, pixellab or notify. |
| `integrations.enabled` | boolean | `true` |  | Whether this instance is live at all. |
| `integrations.scope` | enum | `selected` |  | Who gets it: all, selected or none. |
| `integrations.teams` | list of strings |  |  | Team ids, when `scope: selected`. |
| `integrations.agents` | list of strings |  |  | Agent ids, when `scope: selected`. |
| `integrations.per_agent` | mapping |  |  | Per-agent overrides, keyed by agent id. |
| `integrations.description` | string |  |  | What this is for, in your words. |
| `integrations.homeserver` | string |  |  | matrix: the homeserver base URL, e.g. |
| `integrations.user_id` | string |  |  | matrix: the full user id, e.g. |
| `integrations.access_token` | secret reference |  |  | matrix: the access token, as a secret reference. |
| `integrations.rooms` | list of strings |  |  | matrix: room ids to listen in. |
| `integrations.require_mention` | boolean | `true` |  | matrix: only answer messages that name the agent. |
| `integrations.imap_host` | string |  |  | email: IMAP server hostname. |
| `integrations.imap_port` | integer | `993` |  | email: IMAP port. |
| `integrations.smtp_host` | string |  |  | email: SMTP server hostname. |
| `integrations.smtp_port` | integer | `587` |  | email: SMTP port. |
| `integrations.username` | string |  |  | email: the mailbox to log in as. |
| `integrations.password` | secret reference |  |  | email: the password, as a secret reference. |
| `integrations.folders` | list of strings |  |  | email: folders to watch. |
| `integrations.port` | integer |  |  | webhook: the port to listen on. |
| `integrations.backend` | enum | `desktop` |  | notify: how the notification reaches you. |
| `integrations.token` | secret reference |  |  | notify: the credential for ntfy, gotify or Matrix, as a secret reference. |
| `integrations.apprise_url` | secret reference |  |  | notify: with `backend: apprise`, the Apprise URL -- or several, one per line -- as a secret reference: `{file: ~/.config/clawtilla/apprise.url}`. |
| `integrations.room` | string |  |  | notify: with `backend: matrix`, the room id to post into. |
| `integrations.events` | list of strings | `question,error` |  | notify: what is worth interrupting you for. |
| `integrations.priority` | string | `normal` |  | notify: low, normal, high or urgent. |
| `integrations.quiet_hours` | string |  |  | notify: a local-time range to stay silent in, such as `23:00-07:00`. |
| `integrations.title` | string |  |  | notify: a title to use instead of the agent's name. |
| `integrations.command` | string |  |  | mcp: the program to run, spoken to over stdio. |
| `integrations.args` | list of strings |  |  | mcp: arguments for `command`. |
| `integrations.url` | string |  |  | mcp: an HTTP or SSE endpoint, instead of `command`. |
| `integrations.env` | mapping |  |  | mcp: environment for the server process. |
| `integrations.provider` | string |  |  | connector: which service this is an account on. |
| `integrations.account` | string |  |  | connector: which account on that service, in your own words. |
| `integrations.instance` | string |  |  | connector: the host, for a service you run yourself. |
| `integrations.client_id` | string |  |  | connector: the OAuth application you registered with the provider. |
| `integrations.client_secret` | secret reference |  |  | connector: the application's secret, if the provider insists on one. |
| `integrations.scopes` | string |  |  | connector: what to ask the provider for, space separated. |
| `integrations.token_file` | string |  |  | connector: where the credential is kept, mode 0600. |
| `integrations.credential_name` | string |  |  | connector: the environment variable or header the credential goes in. |
| `integrations.tools` | list of strings |  |  | connector: the only tools the agent may use, if you want fewer. |
| `integrations.directory` | enum | `auto` |  | connector: whether its tools are listed one by one or searched. |
| `integrations.max_result_bytes` | integer | `-1` |  | connector: the most of one tool result the agent is shown, in bytes. |
| `integrations.show_apps` | boolean | `false` |  | connector: show its apps in chat. |
| `integrations.confirm_writes` | boolean | `true` |  | For a connector that can stage its writes, whether it does. |
| `integrations.apply_writes` | enum | `off` |  | connector (venture): whether the agent's writes wait for approval. |
| `integrations.mirror_tasks` | boolean | `false` |  | connector (venture): copy the covered agents' tasks into VENTURE tickets. |
| `integrations.service_token_file` | path |  |  | connector (venture), with mirror_tasks on: the operator's own VENTURE token, for the mirror's writes that no agent makes. |
| `integrations.mirror_intake` | boolean | `true` |  | connector (venture), with mirror_tasks on: a ticket assigned to this agent's VENTURE user starts a task for it. |
| `integrations.poll_seconds` | integer | `60` |  | How often a connector that has something to report is asked. |
| `integrations.push_replies` | boolean | `true` |  | grokbot: hand each agent reply to the bot as a new run of its routine. |

## `connectors` ([config/connectors.md](config/connectors.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `connectors` | section |  |  | Connected accounts: how clawtilla obtains credentials and keeps them. |
| `connectors.dir` | path | `$XDG_CONFIG_HOME/clawtilla/connectors.d` |  | Directory of extra connector definitions. |
| `connectors.refresh_margin_seconds` | integer | `300` |  | How long before expiry to renew a token. |
| `connectors.redirect_port` | integer | `8765` |  | Loopback port for the authorization-code flow's redirect. |
| `connectors.discovery` | boolean | `true` |  | Whether a remote connector finds out for itself how to sign in. |
| `connectors.client_name` | string | `clawtilla` |  | What clawtilla calls itself when it registers with a server. |
| `connectors.registry_enabled` | boolean | `false` |  | Whether the open MCP registry is imported into the catalogue. |
| `connectors.registry_url` | string | `https://registry.modelcontextprotocol.io` |  | Base URL of the MCP registry to import from. |
| `connectors.registry_refresh_hours` | integer | `24` |  | How often the cached registry listing is refreshed. |
| `connectors.directory_min_tools` | integer | `40` |  | More tools than this makes a connector's server a directory. |
| `connectors.directory_min_chars` | integer | `100000` |  | More characters of tool definitions than this makes a directory too. |
| `connectors.max_result_bytes` | integer | `65536` |  | The most of one connector tool result an agent is shown, in bytes. |
| `connectors.apps_web_url` | string | `http://127.0.0.1:8790` |  | Where the web client is reached, for an MCP app's Open in browser. |

## `broker` ([config/broker.md](config/broker.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `broker` | section |  |  | An integration broker clawtilla runs for you, in podman. |
| `broker.enabled` | boolean | `false` |  | Run the broker. |
| `broker.type` | enum | `anythingmcp` |  | Which broker: anythingmcp or jentic. |
| `broker.image` | string |  |  | The gateway's image, pinned to a tag. |
| `broker.database_image` | string |  |  | The database image beside an AnythingMCP gateway, pinned to a tag. |
| `broker.bind` | string | `127.0.0.1` |  | The address the gateway's ports are published on. |
| `broker.port` | integer | `47400` |  | The host port of the gateway's MCP endpoint and API. |
| `broker.dashboard_port` | integer | `47300` |  | The host port of AnythingMCP's dashboard. |
| `broker.connection` | string |  |  | Which podman runs the broker, as computer.container.connection says. |
| `broker.restart` | enum | `always` |  | What to do when a broker container stops without being asked to. |

## `grok` ([config/grok.md](config/grok.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `grok` | section |  |  | Handing work to Grok bots, and hearing back. |
| `grok.callback_host` | string |  |  | The name a Grok bot uses to reach this daemon. |
| `grok.job_ttl_minutes` | integer | `120` |  | How long a job may go unanswered before it is given up. |
| `grok.attention_days` | integer | `3` |  | Days a job that needs somebody keeps counting on the Grok badge. |
| `grok.retention_days` | integer | `30` |  | Days a settled job is kept for the Grok page and `grok jobs`. |

## `notices` ([config/notices.md](config/notices.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `notices` | section |  |  | The notification list the daemon keeps for every client. |
| `notices.keep` | integer | `500` |  | How many notices are kept, newest first. |
| `notices.keep_days` | integer | `30` |  | Days a notice is kept. |

## `office` ([config/office.md](config/office.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `office` | section |  |  | The office: the fleet drawn as people in a building. |
| `office.enabled` | boolean | `false` |  | Whether the daemon keeps the office: the presence fold, the office.presence events and the office.* requests. |
| `office.world` | string | `modern-office` |  | Which world pack to draw: a directory name under one of the world directories (office.world_dirs, <state_dir>/office/worlds, then the shipped data/office/worlds). |
| `office.renderer` | string | `auto` |  | How a renderer should draw the world: `2d`, `3d`, or `auto` -- 3D when the pack has a level, otherwise 2D. |
| `office.world_dirs` | list of strings |  |  | Extra directories holding world packs, searched before the daemon's own and the shipped ones, so your copy of a pack replaces theirs. |
| `office.stale_seconds` | integer | `600` |  | How long a turn that was never seen to end is believed. |
| `office.meeting_min_members` | integer | `3` |  | How many members a group room needs for a round in it to be drawn as a meeting. |
| `office.choreography` | path |  |  | A YAML file of choreography rules -- which zone, pose, prop and emote each activity gets -- applied over the built-in rules and the world pack's own. |
| `office.asset_api` | string |  |  | A service the daemon makes characters with itself, with no model deciding what to ask: `comfyui` (self-hosted, free: office.comfyui_url and office.comfyui_workflow); `higgsfield` (Higgsfield's REST API, a key pair stored with `clawtilla office api-key higgsfield`, billed to the API balance in Higgsfield's console); `higgsfield-mcp` (Higgsfield's MCP server through a connector's sign-in -- office.asset_generator, or the only higgsfield connector -- billed to the account's app credits, the ones the website and the hf CLI spend); `pixellab` (a pixel-art character in four directions with a walk cycle in each, at office.pixel_size; a key stored with `clawtilla office api-key pixellab`, or any agent's pixellab integration); `meshy` (a rigged 3D model that walks, for the 3D view: see office.asset_api_3d); or empty for none. The two Higgsfield roads are billed separately: choose the one your money is in. `clawtilla office character AGENT --direct` makes one; a paid service's estimate is put to you as a decision and nothing is spent until you approve. See docs/office-custodial-team.org. |
| `office.asset_api_3d` | string |  |  | A service that makes each character a 3D model as well: `meshy`, or empty for none. |
| `office.pixel_size` | integer | `48` |  | The size, in pixels a side, of a character PixelLab draws for the office (office.asset_api: pixellab): 16 to 128. |
| `office.mesh_polycount` | integer | `8000` |  | How many polygons Meshy aims for in a character's model (office.asset_api_3d: meshy): 100 to 300000. |
| `office.comfyui_url` | string | `http://127.0.0.1:8188` |  | The ComfyUI server office.asset_api: comfyui makes characters with -- your own, self-hosted. |
| `office.comfyui_workflow` | path |  |  | The ComfyUI workflow characters are made with, exported in API format, with {{prompt}} where the description goes and, if you like, {{image}} (the agent's picture, uploaded), {{seed}} and {{negative_prompt}}. |
| `office.asset_model` | string |  |  | The model office.asset_api makes characters with, in the service's own spelling. |
| `office.asset_generator` | string |  |  | The connector the office's assets and characters are made with: the name of one you added (`clawtilla connector add hf --provider higgsfield`, or a self-hosted ComfyUI). |
| `office.peers` | list of strings |  |  | Other fleets whose people visit this office, each `NAME=WHERE`: WHERE is the other daemon's socket on this machine (`work=/run/user/1000/work/d.sock`) or its tailnet address (`studio=100.64.0.7:8792`), paired once with `clawtilla office peer-pair NAME CODE` using a code `clawtilla device pair` printed on that machine. |
| `office.peer_poll_seconds` | integer | `5` |  | How often, in seconds, each office.peers fleet is asked who is where. A fleet that has not answered for three of these is drawn empty, not frozen. |

## `federation` ([config/federation.md](config/federation.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `federation` | section |  |  | Federation: this fleet linked with other clawtilla fleets -- yours on another machine, or other people's. |
| `federation.enabled` | boolean | `false` |  | Whether this daemon links with other clawtilla fleets -- yours on another machine, or a colleague's. |
| `federation.name` | string |  |  | What other fleets call this one: shown on their sidebar section and in invites. |
| `federation.owner` | string |  |  | Who runs this fleet, as other fleets see it: a name, not an account. Their `owner:` filters and rules match it. |
| `federation.public_url` | string |  |  | The endpoint invites tell other fleets to reach this one at: `https://fleet.example.com` behind a Cloudflare tunnel or a TLS proxy, `http://100.64.0.7:7711` for the federation listener on a tailnet, or `clawtilla://100.64.0.7:8792` for this daemon's own IPC port. |
| `federation.listen` | list of strings |  |  | Where the HTTPS/WebSocket federation listener answers, each `unix:PATH` or `HOST:PORT`. |
| `federation.ipc` | boolean | `true` |  | Whether other fleets may also speak to this daemon over its own IPC port (daemon.tcp_address and the tailnet address) -- the simple daemon connection: `clawtilla://HOST:PORT`, no extra port, no HTTP. |
| `federation.allow_lan_plaintext` | boolean | `false` |  | Whether a link may use a local-network address (RFC 1918, link-local, a name that is not localhost or *.ts.net) without TLS. |
| `federation.seal` | enum | `auto` |  | Whether bodies between fleets are encrypted with a key the two agree (X25519 + ChaCha20-Poly1305) on top of whatever carries them: `auto` on plain-text LAN links only, `always` on every link (so a tunnel's edge sees only ciphertext), `never`. |
| `federation.inbox_agent` | string |  |  | The agent a peer reaches when its grant allows only the inbox (untrusted fleets, by default). |
| `federation.invite_ttl_hours` | integer | `72` |  | How long an invite can be used, in hours, unless the invite says otherwise. |
| `federation.clock_skew_seconds` | integer | `300` |  | How far another fleet's clock may be from this one's before its signed requests are refused as stale. |
| `federation.max_body_bytes` | integer | `1048576` |  | The largest request another fleet may send, in bytes. |
| `federation.ping_seconds` | integer | `30` |  | How often each linked fleet is pinged to keep its link health current. |
| `federation.reconnect_max_seconds` | integer | `300` |  | The longest wait between attempts to reach a fleet that is not answering; the wait doubles from one second up to this. |
| `federation.outbox_hours` | integer | `72` |  | How long a message to a linked fleet that cannot be reached waits in the outbox (<state_dir>/federation/federation.db) before it is given up, with a note in its conversation saying so. |
| `federation.audit_retention_days` | integer | `90` |  | How long the record of every request between fleets (<state_dir>/federation/federation.db) is kept. |
| `federation.rules` | list of strings |  |  | Grant overrides for every linked fleet a selector matches, applied in order after the fleet's tier and before its own overrides, each `SELECTOR: field=value; field=value`. |

## `routines` ([config/routines.md](config/routines.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `routines` | list of mappings |  |  | Standing work: a prompt, an agent, and when to run it. |
| `routines.id` | string |  |  | What this routine is called, and how it is referred to. |
| `routines.description` | string |  |  | One line about what it is for, for a person reading the list. |
| `routines.instructions` | string |  |  | What the agent is asked to do, in full. |
| `routines.agent` | string |  |  | Which agent runs it. |
| `routines.model` | string |  |  | The model this routine's run uses, instead of the agent's own. |
| `routines.enabled` | boolean | `true` |  | Whether it runs on its schedule. |
| `routines.schedule` | enum | `daily` |  | manual, hourly, daily, weekdays, weekly or custom. |
| `routines.at` | string | `09:00` |  | The time of day, for daily, weekdays and weekly. |
| `routines.weekday` | string | `monday` |  | Which day, for `weekly`. |
| `routines.cron` | string |  |  | The expression, for `custom`: minute hour day-of-month month day-of-week. |
| `routines.directory` | path |  |  | Where to run, if not the agent's workspace. |
| `routines.worktree` | boolean | `false` |  | Run in a fresh checkout of `directory` rather than in it. |
| `routines.isolate` | boolean | `false` |  | Give this routine a conversation of its own. |
| `routines.read_only` | boolean | `false` |  | Run with read-only tools: the agent may look at anything it can reach and change nothing -- no messages, no writes, no commands, no screen. |
| `routines.proactive` | boolean | `false` |  | Look for ways to help, unasked, and report them without acting. |
| `routines.catch_up` | boolean | `false` |  | Run once at startup when its time passed while the daemon was down. |
| `routines.jitter_seconds` | integer | `0` |  | Delay each run by up to this many seconds, chosen at random. |
| `routines.max_start_failures` | integer | `3` |  | Turn the routine off after this many runs in a row whose computer did not start. |
| `routines.locked` | boolean | `false` |  | Keep the agent from changing or removing this routine. |

## `triggers` ([config/triggers.md](config/triggers.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `triggers` | list of mappings |  |  | Work started by something happening elsewhere. |
| `triggers.id` | string |  |  | How the trigger is named on the command line and in the clients. |
| `triggers.description` | string |  |  | What this trigger is for, in a line. |
| `triggers.agent` | string |  |  | Primary agent to run when it fires. |
| `triggers.agents` | list of strings |  |  | Additional recipients for the same delivery. |
| `triggers.room` | string |  |  | Where the run reports, if not the agent's own conversation. |
| `triggers.mode` | enum | `run` |  | What a delivery that passed every filter turns into: run or post. |
| `triggers.max_pending` | integer | `4` |  | How many runs this trigger may have unfinished at once, 1 to 50. |
| `triggers.provider` | enum | `generic` |  | Who is calling: forgejo, gitea, github, gitlab, venture or generic. |
| `triggers.secret` | secret reference |  |  | The shared secret the caller proves it knows. |
| `triggers.events` | list of strings |  |  | Which event names are acted on. |
| `triggers.repo` | string |  |  | Only act when the delivery names this repository. |
| `triggers.branch` | string |  |  | Only act when the delivery names this branch. |
| `triggers.header` | string |  |  | For `provider: generic`, the header carrying the event name. |
| `triggers.ignore_actors` | list of strings |  |  | Changes made by these users are recorded as ignored, not run. |
| `triggers.match` | list of mappings |  |  | Fields the delivery's JSON body must contain, or it is ignored. |
| `triggers.match.path` | string |  |  | Where in the JSON body to look, dotted, such as `repository.name`. No spaces, no `=` and no empty part; a bad path is refused at load. |
| `triggers.match.value` | string |  |  | Text the field must contain, ignoring case. |
| `triggers.replay_connector` | string |  |  | venture: the connector whose credential reads the business-event feed, so deliveries missed while the daemon was down are caught up. |
| `triggers.organization_id` | integer | `0` |  | venture: which organization's business events to replay. |
| `triggers.instructions` | string |  |  | What the agent is asked to do, with the event fenced beneath it. |
| `triggers.enabled` | boolean | `true` |  | Whether it fires. |
| `triggers.directory` | path |  |  | Where the run should work, if somewhere particular. |
| `triggers.worktree` | boolean | `false` |  | Run in a fresh checkout of `directory` rather than in it. |
| `triggers.isolate` | boolean | `false` |  | Give the run a conversation of its own rather than the agent's. |

## `agents` ([config/agents.md](config/agents.md))

| Key | Type | Default | Agent | Summary |
|---|---|---|---|---|
| `agents` | list of mappings |  |  | The fleet. |
| `agents.id` | string |  |  | Identifier used everywhere else to address this agent. |
| `agents.name` | string |  |  | Display name shown in the clients and used as the agent's own name. |
| `agents.description` | string |  |  | One line about what this agent is for. |
| `agents.enabled` | boolean | `true` |  | Whether this agent may run at all. |
| `agents.chief_of_staff` | boolean | `false` |  | Marks the agent that receives work addressed to the fleet. At most one agent may hold this. |
| `agents.color` | string |  |  | Accent colour in the clients, as a hex string. |
| `agents.face` | string |  |  | The generated face the agent wears when it has no profile picture. |
| `agents.order` | integer | `0` |  | Where this agent sits in the list, lowest first. |
| `agents.review_by` | string |  |  | Who checks this agent's work before it counts as done. |
| `agents.team` | string |  |  | Which team this agent belongs to, by teams.id. |
| `agents.broker_server` | string |  |  | This agent's own gateway server on the broker. |
| `agents.team_role` | enum | `member` |  | Standing within that team. |
| `agents.avatar` | path |  |  | The agent's profile picture. |
| `agents.skills` | list of strings |  |  | Skills this agent gets, on top of its team's and the fleet's. |
| `agents.workspace` | path |  |  | Workspace directory. |
| `agents.persona` | section |  |  | Who the agent is. |
| `agents.persona.system_prompt` | string |  |  | System prompt, inline. |
| `agents.persona.identity_files` | list of strings |  |  | Files in the workspace concatenated into the system prompt, in order. |
| `agents.prompt_suffix` | string |  |  | Text appended to every turn this agent runs. |
| `agents.memory` | section |  |  | The agent's long-term memory file. |
| `agents.memory.enabled` | boolean | `true` |  | Whether MEMORY.md is loaded into the prompt. |
| `agents.memory.max_lines` | integer | `200` |  | Line budget for MEMORY.md. |
| `agents.memory.max_bytes` | integer | `24000` |  | Byte budget for MEMORY.md, applied alongside max_lines. |
| `agents.model` | section |  |  | Which model this agent thinks with. |
| `agents.model.provider` | string |  |  | Provider name. |
| `agents.model.model` | string |  |  | Model name. |
| `agents.model.effort` | string |  |  | Reasoning effort: low, medium, high, xhigh or max, when the model has one. |
| `agents.model.command` | string |  |  | The command line an acp agent is started from, e.g. "gemini --experimental-acp". |
| `agents.model.account` | string |  |  | Which entry of accounts this agent's CLI logs in as. |
| `agents.model.routing_profile` | string |  |  | Reserved. |
| `agents.model.fallbacks` | list of mappings |  |  | Where this agent's conversations continue, in order, when its own backend refuses a turn: a usage limit, a rate limit, a sign-out or an outage. |
| `agents.model.fallbacks.provider` | string |  |  | The agent CLI: one of the providers model.provider takes. |
| `agents.model.fallbacks.model` | string |  |  | The model that CLI is asked for. |
| `agents.model.fallbacks.account` | string |  |  | Which entry of accounts this fallback's CLI logs in as. |
| `agents.steer` | enum | `delegator` |  | Which messages may be handed to the turn this agent is already running, instead of waiting to become a turn of their own. |
| `agents.session` | section |  |  | How this agent's conversations map to AI sessions. |
| `agents.session.routing_mode` | enum | `sender-room` |  | How many context windows this agent is. |
| `agents.session.timeout_minutes` | integer | `30` |  | Minutes of inactivity before libreclaw drops this agent's session. |
| `agents.session.max_sessions` | integer | `10` |  | How many sessions this agent keeps live at once. |
| `agents.session.max_wait_queue_depth` | integer | `32` |  | How many channel messages may wait for a session slot when every session is busy. |
| `agents.session.max_queue_wait_seconds` | integer | `300` |  | How many seconds a channel message may wait for a session slot. |
| `agents.session.max_parked` | integer | `0` |  | How many parked conversations libreclaw keeps for this agent. |
| `agents.runtime` | section |  |  | How this agent's libreclaw instance is hosted. |
| `agents.runtime.type` | enum | `process` |  | process or embedded. |
| `agents.runtime.confine` | enum |  |  | Run this agent's own CLI inside a bubblewrap sandbox: none or bwrap. Defaults to defaults.confine. |
| `agents.runtime.confine_paths` | list of strings |  |  | More paths to bind read-only into a confined agent's sandbox. |
| `agents.runtime.network` | enum | `open` |  | Where this agent's own process may connect: open, gated or none. |
| `agents.runtime.autostart` | boolean |  |  | Start with the daemon. |
| `agents.runtime.restart` | enum |  |  | never, on-failure or always. |
| `agents.runtime.backoff_seconds` | integer | `5` |  | Base delay before a restart, doubling on each consecutive failure. |
| `agents.runtime.max_restarts` | integer | `10` |  | Consecutive failures before the agent is left in error rather than restarted again. |
| `agents.runtime.stream_steps` | boolean |  |  | Report this agent's steps while its turn runs. |
| `agents.runtime.process_timeout_ms` | integer | `0` |  | Maximum elapsed milliseconds for one AI CLI subprocess, including streaming turns. |
| `agents.runtime.idle_stop_hours` | integer | `0` |  | Stop this agent after this many hours without a turn. |
| `agents.runtime.turn_timeout_seconds` | integer | `0` |  | How long a turn may go without producing anything before it is cancelled. |
| `agents.runtime.first_reply_seconds` | integer | `20` |  | How long a turn may show nothing at all -- no step, no thought, no reply -- before the clients offer to stop it or to run the same message on another model. |
| `agents.runtime.busy_cpu_percent` | integer | `5` |  | How much CPU the agent's processes must use, in percent of one core, for a silent turn to count as working. |
| `agents.runtime.busy_max_seconds` | integer | `7200` |  | The longest a busy process tree may keep a silent turn alive, in seconds since the turn last reported anything. |
| `agents.computer` | section |  |  | What the agent can run commands on. |
| `agents.computer.use` | string |  |  | Share a machine declared under computers: instead of having one of this agent's own. |
| `agents.computer.type` | enum |  |  | Which kind of computer this agent gets. |
| `agents.computer.start` | enum | `on_agent_start` |  | When the machine is started: with the agent, or the first time something runs in it. |
| `agents.computer.exchange` | boolean | `true` |  | Whether the shared exchange directory is visible to this agent. |
| `agents.computer.workspace` | boolean | `true` |  | Whether the agent's own workspace is mounted into its computer at /mnt/clawtilla/workspace. |
| `agents.computer.default_mounts` | boolean | `true` |  | Whether this agent gets the fleet's shared folders from defaults.mounts. |
| `agents.computer.mounts` | list of mappings |  |  | Host paths shared into the computer. |
| `agents.computer.mounts.source` | path |  |  | Host path to share. |
| `agents.computer.mounts.target` | path |  |  | Absolute path inside the computer. |
| `agents.computer.mounts.mode` | enum | `ro` |  | ro or rw. |
| `agents.computer.mounts.type` | enum | `bind` |  | bind, volume, virtiofs, 9p or tmpfs. |
| `agents.computer.mounts.relabel` | enum | `shared` |  | SELinux relabelling for a container bind mount: none, shared or private. |
| `agents.computer.mounts.create` | boolean | `false` |  | Create the source directory if it is missing, rather than failing. |
| `agents.computer.mounts.size` | string |  |  | Size for a tmpfs mount, e.g. |
| `agents.computer.mounts.required` | boolean | `true` |  | Fail to start when the source is missing, rather than warning and carrying on without it. |
| `agents.computer.host` | section |  |  | **D** Settings for computer.type: host -- the real machine clawtilla runs on. |
| `agents.computer.host.confirm_host_control` | boolean | `false` |  | **D** Required before a host computer will start. |
| `agents.computer.host.confine` | enum | `workspace` |  | How much of the machine the agent may touch. |
| `agents.computer.host.root` | path |  |  | Directory the agent works in. |
| `agents.computer.host.allow_paths` | list of strings |  |  | Additional paths reachable under confine: allowlist or bwrap. |
| `agents.computer.host.deny_paths` | list of strings |  |  | Paths refused even when they fall inside an allowed one. |
| `agents.computer.host.allow_network` | boolean | `true` |  | Whether the agent's commands can reach the network. Only enforceable under confine: bwrap, which unshares the namespace. |
| `agents.computer.host.allow_sudo` | boolean | `false` |  | **D** Permit privilege escalation. |
| `agents.computer.host.shell` | path | `/bin/sh` |  | Shell used for interactive sessions and shell-form commands. |
| `agents.computer.host.nice` | integer | `0` |  | Scheduling niceness for the agent's commands, so a runaway build does not make the desktop unusable. |
| `agents.computer.display` | section |  |  | Settings for computer.type: display -- GUI apps on a virtual screen of their own, which the agent drives and a person can watch and take. |
| `agents.computer.display.backend` | enum | `auto` |  | What draws the screen: auto, gowl, xvfb or xvnc. |
| `agents.computer.display.resolution` | string | `1280x800` |  | The screen's size, WIDTHxHEIGHT, each between 64 and 8192. |
| `agents.computer.display.depth` | integer | `24` |  | Colour depth for an X server: 16, 24 or 32. |
| `agents.computer.display.startup` | list of strings |  |  | Apps started on the screen each time it comes up, as shell command lines, under the agent's confinement: e.g. |
| `agents.computer.display.vnc_port` | integer | `0` |  | Xvnc's VNC port on 127.0.0.1. |
| `agents.computer.display.allow_input` | boolean | `true` |  | Whether the agent may click, type and scroll on its display, not only look at it. |
| `agents.computer.display.allow_spawn` | boolean | `true` |  | Whether the agent may start apps on its display with display_launch. |
| `agents.computer.display.gowl_binary` | path |  |  | A gowl to run instead of searching /usr/bin, /usr/sbin and PATH. |
| `agents.computer.container` | section |  |  | Settings for computer.type: container. |
| `agents.computer.container.image` | string | `registry.fedoraproject.org/fedora:45` |  | Image the container is created from. |
| `agents.computer.container.name` | string |  |  | Container name. |
| `agents.computer.container.connection` | string |  |  | Podman connection: a socket path, ssh://user@host/socket, tcp://host:port, or a named connection. |
| `agents.computer.container.command` | string | `sleep infinity` |  | What the container runs. |
| `agents.computer.container.network` | string |  |  | Network to attach to. |
| `agents.computer.container.keep` | boolean | `true` |  | Keep the container when the agent stops, instead of removing it. |
| `agents.computer.container.pids_limit` | integer | `0` |  | Most processes and threads the machine may hold at once. |
| `agents.computer.container.memory` | string |  |  | Most memory the machine may use, e.g. |
| `agents.computer.container.memory_swap` | string |  |  | Memory and swap together, e.g. |
| `agents.computer.container.cpus` | string |  |  | Most CPU time the machine may use, as a count of CPUs: 4, 2.5, 0.5. |
| `agents.computer.container.cpu_weight` | integer | `0` |  | The machine's share of CPU when the host is busy: a cgroup v2 cpu.weight, 1 to 10000, where 100 is everybody's default. |
| `agents.computer.container.cpuset` | string |  |  | Which CPUs the machine may run on, e.g. |
| `agents.computer.container.selinux_level` | string | `auto` |  | The SELinux MCS level the container is created at. |
| `agents.computer.container.init` | boolean | `true` |  | Run podman's init (catatonit) as the container's PID 1. |
| `agents.computer.ssh` | section |  |  | A machine reached over ssh. |
| `agents.computer.ssh.host` | string |  |  | The ssh config alias to connect to. |
| `agents.computer.ssh.workspace` | path |  |  | Where the agent works on that machine. |
| `agents.computer.ssh.shell` | string | `/bin/sh` |  | Which shell runs the command over there. |
| `agents.computer.ssh.allow_sudo` | boolean | `false` |  | Whether sudo and its neighbours may be run there. |
| `agents.computer.ssh.connect_timeout` | integer | `10` |  | Seconds to wait for the connection before failing. |
| `agents.computer.ssh.control_persist` | integer | `600` |  | Seconds an idle multiplexed connection is kept open. |
| `agents.computer.ssh.wake_command` | list of strings |  |  | A program to run when the machine does not answer, to wake it. |
| `agents.computer.distrobox` | section |  |  | Settings for computer.type: distrobox. |
| `agents.computer.distrobox.image` | string |  |  | Image the box is created from. |
| `agents.computer.distrobox.name` | string |  |  | Box name. |
| `agents.computer.distrobox.home` | string |  |  | HOME inside the box. |
| `agents.computer.distrobox.share_home` | boolean | `false` |  | Give the box your real home directory. |
| `agents.computer.distrobox.packages` | string | `gcc make git` |  | Packages installed when the box is first created, space separated. |
| `agents.computer.distrobox.flags` | string |  |  | Extra flags passed through to podman, space separated. Quote anything containing a space. |
| `agents.computer.distrobox.init` | boolean | `false` |  | Run an init system (systemd) inside the box. Needed by anything expecting systemctl or a user unit. |
| `agents.computer.distrobox.keep` | boolean | `true` |  | Keep the box when the agent stops. |
| `agents.computer.vm` | section |  |  | Settings for computer.type: vm. |
| `agents.computer.vm.backend` | enum | `libvirt` |  | libvirt or qemu. |
| `agents.computer.vm.domain` | string |  |  | libvirt domain name. |
| `agents.computer.vm.uri` | string | `qemu:///session` |  | libvirt connection URI. |
| `agents.computer.vm.emulator` | path |  |  | The QEMU binary to run the guest with. |
| `agents.computer.vm.image` | path |  |  | Base disk image. |
| `agents.computer.vm.cpus` | integer | `2` |  | Virtual CPUs. |
| `agents.computer.vm.memory_mb` | integer | `8192` |  | Memory in megabytes. |
| `agents.computer.vm.disk_gb` | integer | `128` |  | Virtual disk size in gigabytes. |
| `agents.computer.vm.resolution` | string | `1280x800` |  | The size of the VM's screen, as WIDTHxHEIGHT. |
| `agents.computer.vm.ssh_user` | string | `root` |  | User commands are run as inside the guest, over SSH. |
| `agents.computer.vm.ssh_key` | path |  |  | Private key used to reach the guest. |
| `agents.computer.vm.ssh_host` | string |  |  | Address that reaches the guest. |
| `agents.computer.vm.ssh_port` | integer | `22` |  | Port SSH is reached on. |
| `agents.computer.vm.packages` | list of strings |  |  | Packages the guest installs at first boot. |
| `agents.computer.vm.cloud_init` | boolean | `true` |  | Hand the guest a cloud-init seed on first boot. |
| `agents.computer.vm.snapshot_on_start` | boolean | `false` |  | Take a snapshot each time the VM starts, so a session can be rolled back. libvirt backend only. |
| `agents.computer.vm.desktop` | section |  |  | How the guest's graphical session is built. |
| `agents.computer.vm.desktop.user` | string |  |  | The account the graphical session runs as. |
| `agents.computer.vm.desktop.autologin` | boolean | `true` |  | Log that account straight in at boot, with no password prompt. |
| `agents.computer.vm.desktop.flavour` | enum | `auto` |  | Which family the guest belongs to, for installing things into it. |
| `agents.computer.vm.desktop.packages` | list of strings |  |  | What to install to get a desktop, if not the usual set. |
| `agents.computer.vm.desktop.mcp` | boolean | `true` |  | Install gnome-desktop-mcp into the guest and switch it on. |
| `agents.computer.vm.desktop.mcp_repo` | string | `https://git.podbielniak.com/zachpodbielniak/gnome-desktop-mcp.git` |  | Where the guest clones gnome-desktop-mcp from. |
| `agents.computer.desktop` | section |  |  | **D** Let the agent see and drive a desktop session. |
| `agents.computer.desktop.enabled` | boolean | `false` |  | Whether desktop tools are offered to the agent. |
| `agents.computer.desktop.backend` | enum | `auto` |  | auto, gowl, gnome or guest. |
| `agents.computer.desktop.socket` | path | `$XDG_RUNTIME_DIR/gowl-mcp.sock` |  | gowl's MCP socket. |
| `agents.computer.desktop.allow_spawn` | boolean | `false` |  | **D** Whether the agent may launch and signal processes through the compositor. |
| `agents.computer.desktop.allow_input` | boolean | `false` |  | Permit key and pointer injection, not just observation. |
| `agents.computer.desktop.observe_fps` | integer | `1` |  | Frames a second while somebody is watching the screen. |
| `agents.computer.desktop.takeover_lease_seconds` | integer | `900` |  | How long a person may hold the screen before the hold lapses. |
| `agents.computer.desktop.driver_idle_seconds` | integer | `120` |  | On a shared computer, how long one agent keeps the screen after its last input before another may type or click. |
| `agents.computer.desktop.allow_recording` | boolean | `false` |  | **D** Whether a demonstration may be recorded on this screen. |
| `agents.browser` | section |  |  | A real web browser the agent can drive (needs a GSURF=1 build). |
| `agents.browser.mode` | enum | `none` |  | Whether this agent can drive a real web browser. |
| `agents.browser.use` | string |  |  | With agents.browser.mode: shared, the browsers: entry to join. |
| `agents.browser.home` | string | `about:blank` |  | The page a session opens on. |
| `agents.browser.allow_scripts` | boolean | `false` |  | **D** Whether the agent is offered clawtilla_browser_eval, which runs its own JavaScript in the page. |
| `agents.browser.idle_minutes` | integer | `15` |  | Stop the session after this many minutes without a browser call. |
| `agents.browser.secrets` | list of strings |  |  | The vault: entries this agent may have filled into a page, by name. Empty, the agent is offered no secret tools. |
| `agents.browser.width` | integer | `1280` |  | The session's window width, in pixels. |
| `agents.browser.height` | integer | `800` |  | The session's window height, in pixels. |
| `agents.env` | mapping |  |  | Environment variables for this agent's process. |
| `agents.credentials` | mapping |  |  | Secret references for this agent, written to credential files at 0600. |
| `agents.integrations` | section |  |  | Channels this agent reaches the outside world through. |
| `agents.integrations.matrix` | section |  |  | Matrix. |
| `agents.integrations.matrix.enabled` | boolean | `false` |  | Whether the Matrix channel is active. |
| `agents.integrations.matrix.homeserver` | string |  |  | Homeserver base URL. |
| `agents.integrations.matrix.user_id` | string |  |  | Full Matrix user id, e.g. |
| `agents.integrations.matrix.access_token` | secret reference |  |  | Access token, as a secret reference. |
| `agents.integrations.matrix.rooms` | list of strings |  |  | Room ids to join. |
| `agents.integrations.matrix.require_mention` | boolean | `true` |  | Only respond when named, so the agent is not a participant in every conversation in the room. |
| `agents.integrations.email` | section |  |  | Email over IMAP and SMTP. |
| `agents.integrations.email.enabled` | boolean | `false` |  | Whether the email channel is active. |
| `agents.integrations.email.imap_host` | string |  |  | IMAP server hostname. |
| `agents.integrations.email.imap_port` | integer | `993` |  | IMAP port. |
| `agents.integrations.email.smtp_host` | string |  |  | SMTP server hostname. |
| `agents.integrations.email.smtp_port` | integer | `587` |  | SMTP port. |
| `agents.integrations.email.username` | string |  |  | Account username. |
| `agents.integrations.email.password` | secret reference |  |  | Account password, as a secret reference. |
| `agents.integrations.email.folders` | list of strings |  |  | Folders to watch. |
| `agents.integrations.webhook` | section |  |  | Inbound HTTP webhooks. |
| `agents.integrations.webhook.enabled` | boolean | `false` |  | Whether the webhook channel is active. |
| `agents.integrations.webhook.port` | integer | `0` |  | Port to listen on. |
| `agents.integrations.webhook.bind_address` | string | `127.0.0.1` |  | Which address to listen on. |
| `agents.integrations.local` | boolean | `false` |  | libreclaw's stdin/stdout channel. |
| `agents.integrations.cmacs` | boolean | `false` |  | libreclaw's in-process cmacs channel. |
| `agents.federation_reach` | list of strings |  |  | Which linked fleets may reach this agent, as peer selectors (`tier:owned`, `tag:client-acme`, `name:lab`, `owner:Dana`, `*`; see docs/federation.org#selectors). |
| `agents.federation_may_reach` | list of strings |  |  | Which linked fleets this agent may write to and hand work to, as peer selectors (`tier:owned`, `tag:client-acme`, `name:lab`, `*`; see docs/federation.org#selectors). |
| `agents.tools` | section |  |  | Which orchestration tools this agent may call. |
| `agents.tools.allow` | list of strings |  |  | Tool names permitted. |
| `agents.tools.deny` | list of strings |  |  | Tool names refused. |
| `agents.policy` | section |  |  | What this agent may do without asking. |
| `agents.policy.rules` | list of strings |  |  | This agent's action rules, MATCH=MODE, first match wins. |
| `agents.policy.ask_timeout_seconds` | integer |  |  | How long an `ask` waits for a person. |
| `agents.tools.manage_fleet` | boolean | `false` |  | **D** Whether this agent may create other agents. |
| `agents.tools.ephemeral` | boolean | `false` |  | **D** Whether this agent may spawn ephemeral agents: temporary helpers that do one piece of work and are then reaped (clawtilla_ephemeral_spawn, clawtilla_ephemeral_reap). |
| `agents.tools.federation` | boolean | `false` |  | **D** Whether this agent may start conversations with agents in linked fleets (clawtilla_message_agent and clawtilla_delegate with `AGENT@ALIAS`) and sees them in clawtilla_list_agents. |
| `agents.tools.office` | boolean | `false` |  | Whether this agent may read, change and apply the office's world packs and import assets into them (clawtilla_office_*). |
| `agents.tools.schedule` | boolean | `true` |  | Whether this agent may schedule its own routines. |
| `agents.tools.schedule_limit` | integer | `8` |  | How many routines may run on this agent before it can add no more of its own. |
| `agents.tools.schedule_min_minutes` | integer | `15` |  | The shortest gap between two runs of a routine this agent schedules, in minutes. |
| `agents.mailbox` | section |  |  | Per-agent overrides of orchestration.mailbox. |
| `agents.libreclaw` | mapping |  |  | Merged verbatim into this agent's generated libreclaw config.yaml. |
