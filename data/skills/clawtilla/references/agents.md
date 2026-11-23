# Agents: configuration, persona, models, lifecycle

One agent = one libreclaw process (`runtime.type: process`, default) or one in-daemon `LcApp` (`embedded`), with its own persona, model, credentials, integrations and computer. Sources: `docs/agents.org`, `docs/configuration.org`, `src/config/clawt-config-schema.c`. Every key with type/default: [config-keys.md](config-keys.md). Teams/ephemeral helpers: [teams-and-ephemeral.md](teams-and-ephemeral.md). Computers: [computers.md](computers.md).

## Key spelling: agent-relative vs fleet

Inside an `agents[]` entry a key is written without the `agents.` prefix (`computer.vm.image`, not `agents.computer.vm.image`). Some fleet options can be overridden per agent under a *different* name (`clawt_config_schema_agent_keys()`); resolution is agent value, then fleet value, then schema default:

| Inside an agent | Falls back to |
|---|---|
| `model.provider` / `model.model` | `defaults.provider` (claude-code) / `defaults.model` (sonnet) |
| `computer.type` | `defaults.computer` (none) |
| `computer.container.image` | `defaults.container_image` |
| `runtime.autostart` / `runtime.restart` / `runtime.confine` / `runtime.stream_steps` | `defaults.autostart` (**false**) / `defaults.restart` (on-failure) / `defaults.confine` (none) / `defaults.stream_steps` |
| `policy.rules`, `policy.ask_timeout_seconds` | `defaults.policy.*` |
| `skills` | `defaults.skills` (additive with the team's list) |
| `mailbox.{max_depth,overflow,max_attempts,lease_seconds,default_ttl_seconds,backoff_seconds}` | `orchestration.mailbox.*` |
| `memories.{enabled,max_results,readers,scope,recall,summarise,nudge_turns}` | same name at top level |
| `max_waits`, `daily_agent_budget_micros`, `crosstalk_summary`, `crosstalk_summary_to` | `orchestration.max_waits_per_agent`, `orchestration.*` |

## The important per-agent keys

| Key | Notes |
|---|---|
| `id`, `name`, `description` | `description` is load-bearing: peers/chief read it to choose whom to delegate to, and it seeds `SOUL.org`/`IDENTITY.org`. Write what the agent is for. |
| `enabled` | default true |
| `chief_of_staff` | receives fleet-addressed work; at most one agent (or name it with `orchestration.chief_of_staff`). Does **not** grant `tools.manage_fleet`. |
| `team`, `team_role` (`member`/`lead`) | a lead may assign to its own team only; a member may talk to anyone but not delegate. One lead per team. |
| `review_by` | `lead`, `delegator`, `operator`, an agent id, or empty. See [tasks-and-projects.md](tasks-and-projects.md). |
| `model.provider/model/effort/account/fallbacks/command` | below |
| `persona.identity_files` / `persona.system_prompt` | mutually exclusive; both set = shadow |
| `prompt_suffix` | appended to every turn (after clawtilla's own suffix for computer agents). Costs tokens every turn. |
| `session.routing_mode` | `sender-room` (default), `room`, `agent`. Role default: chief and leads get `agent`; a member of a room with >2 members gets `room` (explicit `sender-room` there is replaced). Change needs restart (re-keys sessions). |
| `session.timeout_minutes` | default 30; `-1` never expires |
| `steer` | `never`/`delegator` (default)/`peers`/`all`: which messages may join a running turn. Only Claude Code, claude-tmux and codex (app-server) can be steered. |
| `runtime.*` | `type`, `autostart`, `restart` (`never`/`on-failure`/`always`), `backoff_seconds`, `max_restarts` (0 = never give up), `confine`, `confine_paths`, `network`, `idle_stop_hours`, `turn_timeout_seconds` (0 = off), `process_timeout_ms` (0 = off), `first_reply_seconds` (20) |
| `env` | allowlisted env for the child; daemon env is **not** inherited. Not seen by embedded agents. |
| `credentials` | secret refs (`{env: X}`, `{file: path}`), written 0600; never echoed over IPC. See [security.md](security.md). |
| `workspace` | default `<defaults.workspace_root>/<id>` (`~/.clawtilla/agents/<id>`) |
| `avatar`, `face`, `color`, `order` | presentation |
| `tools.*`, `skills`, `memories.*`, `memory.*` | below |
| `integrations.*`, `broker_server` | [integrations.md](integrations.md), [connectors.md](connectors.md) |
| `browser.*` | [computers.md](computers.md#browser-sessions) |
| `libreclaw` | verbatim passthrough into the rendered libreclaw config; may not redeclare `agent:`, `ai:`, `session:`, `database:`, `skills:` (refused; `channels:` merges per channel) |

`clawtilla agent set ID KEY VALUE` writes one key (schema-typed; lists are comma strings). The reply says whether the agent is (still) a shadow and whether `restart_required`. Check `clawtilla agent show ID` afterwards.

## What a change needs

| Change | Remedy |
|---|---|
| `persona.*`, editing `SOUL.org` etc. | `clawtilla agent reset ID` -- a resumed CLI session is never handed a system prompt, restart is NOT enough. Old sessions are moved aside, not deleted. |
| `tools.*`, `chief_of_staff`, `team_role`, `session.routing_mode`, `runtime.confine`, `runtime.confine_paths`, `runtime.network` | `clawtilla agent restart ID` (CLI lists tools at session start; sandbox/netns built at spawn). `agent set` prints the advice only while the agent is exactly `running`. |
| restart policy, `computer.start`, container limits, VM cpus/memory/disk/resolution | next agent start (reread at every start) |
| anything in a VM's cloud-init seed, distrobox image/packages/share_home | `computer rebuild` -- see [computers.md](computers.md) |
| mounts | next start of the computer |
| hand edit of clawtilla.yaml | `clawtilla config reload` (reconciles agents; shadow decisions retaken both ways) |

Managed regions (`# BEGIN clawtilla tools|integrations|computer|skills` in `TOOLS.org`, `# BEGIN clawtilla operator profile` in `USER.org`, `# BEGIN clawtilla projects` in `PROJECTS.org`) are refreshed on every agent start, daemon start, reload and fleet-changing verb, written only when they differ. Everything outside the markers belongs to whoever edits the file.

## Backends (model.provider)

libreclaw drives CLIs only. `lc_provider_type_normalize()` accepts eight (AGENTS.md's "six" predates `codex-cli` and `acp`); anything else is an error and every turn is refused until fixed.

| Provider | Aliases | Notes |
|---|---|---|
| `claude-code` | `claude` | `claude --print`; models are aliases `opus`, `sonnet`, `fable` (prose), `haiku`. Prompt via `--system-prompt-file` (no size ceiling). |
| `claude-tmux` | | same CLI driven through tmux, bills as subscription use; prompt not sent |
| `grok-build` | `grok`, `grok_build` | xAI `grok` CLI headless; self-authenticating. Prompt is an argv word: 131072-byte ceiling. |
| `opencode` | | routes to xAI/Google/OpenAI itself |
| `antigravity` | `agy` | Google `agy`; effort only low/medium/high; `AGY_PATH` overrides binary; no account support |
| `cursor` | `cursor-agent`, `cursor_agent` | effort baked into model id; `CURSOR_AGENT_PATH`; no account support |
| `codex-cli` | `codex`, `codex_cli` | `codex exec`; never given an MCP server's `env` |
| `acp` | `acp-agent`, `acp_agent` | any Agent Client Protocol program from `model.command` (or `$AI_ACP_COMMAND`); model is always `default`; cannot steer |

Trap: `claude`/`grok` in `model.provider` mean the CLI, not the HTTP API (HTTP providers only drive the agent *designer*). `clawtilla model` lists providers and models (refreshing from the CLIs) and marks which can run an agent. `model.effort`: low/medium/high/xhigh/max. `model.routing_profile` is reserved and read by nothing.

## Accounts (separate logins)

```yaml
accounts:
  - id: work
    provider: claude-code
    config_dir: ~/.claude-work
    pay_per_use: false      # true = API key billed per use; may back up a subscription
```

```bash
clawtilla account add work claude-code ~/.claude-work "the work subscription"
clawtilla account login work        # runs the CLI's own login; run it ON THE DAEMON HOST
clawtilla account list
clawtilla agent set researcher model.account work
clawtilla account rm work           # agents still naming it are disabled
```

Rendered as libreclaw `ai.config_dir` (sets `CLAUDE_CONFIG_DIR`, `CODEX_HOME`, `GROK_HOME`, or opencode's XDG roots); works for embedded too. Refused (shadow if reached by hand edit): undeclared id, account for a different CLI, account on cursor/antigravity. Change provider? Clear `model.account` first.

## Failover (model.fallbacks)

```bash
clawtilla agent set researcher model.fallbacks "codex-cli:gpt-5.5, claude-code:sonnet@claude-api"
clawtilla agent set researcher model.fallbacks ""
```

Entries `provider:model[@account]`. libreclaw (`ai.fallbacks`) moves a turn refused for usage limit, rate limit, sign-out or outage to the first free entry; holds the refused backend until the provider's reset (or backoff 1h/1min/5min/2min, doubling to 4h). Same CLI+login keeps the conversation; another CLI or login starts fresh. Visible as `agent.failover` event, `[on fallback]` in `agent list`. **Refused**: another sign-in to the same subscription (claude-code and claude-tmux are one family) unless that account is `pay_per_use: true` -- account rotation breaks provider terms. Checked at load (shadow), on `agent set model.fallbacks`, and on `model.provider`/`model.account` changes.

## Workspace and persona files

Scaffolded on first start (or on `agent files`/`edit`); existing files are never overwritten. Prompt order: `SOUL.org`, `IDENTITY.org`, `USER.org`, `AGENTS.org`, `TOOLS.org`, `TOOL_GOTCHAS.org`, `PROJECTS.org`. Also `README.org`, loaders `AGENTS.md` (written once, from the standard list -- edit by hand after customising `identity_files`) and `CLAUDE.md` (`@AGENTS.md`), `.mcp.json` (clawtilla owns keys prefixed `clawtilla-`), `skills/`, `MEMORY.md` (agent-written).

```bash
clawtilla agent files researcher
clawtilla agent edit researcher [TOOLS.org]   # $VISUAL/$EDITOR, local terminal; plain filenames only
clawtilla agent show researcher | grep identity
```

Ceiling: the concatenated prompt must stay under 131072 bytes (`MAX_ARG_STRLEN`) for argv backends (grok-build). `agent show` reports `identity: N bytes`; >80% warns and emits `agent.identity`. Fix by moving history into a sibling `.org` not listed in `identity_files`.

## Tools, skills, memories

- `tools.allow` (unset = all the agent's capabilities permit) and `tools.deny` (always wins) cover only the `clawtilla_*` MCP tools, not the CLI's own Bash/Read/Write. A tool the agent cannot use is not offered at all. Full list: [mcp-tools.md](mcp-tools.md).
- Role defaults: `tools.manage_fleet` false for everyone (dangerous; enables `clawtilla_create_agent`); `tools.ephemeral` and `tools.office` default **true for the chief of staff**, false otherwise (custodial agents are made with `tools.office` on); `tools.schedule` true (bounded by `tools.schedule_limit` 8, `tools.schedule_min_minutes` 15). Delegation (`clawtilla_delegate`, `clawtilla_task_link`) is offered only to an agent that may assign to somebody (the chief, a team's lead); the target is checked at call time.
- `skills:` names library skills; assigned is not in effect (imported skills start disabled); unknown name warns. See [skills-and-teach.md](skills-and-teach.md).
- `memories.*` = searchable per-agent SQLite store (`<state_dir>/agents/<id>/memory.db`; team/fleet scopes under `<state_dir>/memories/`) with `clawtilla_memory_*` tools. `memories.readers` is a **comma string**, not a YAML list (a list reads as unset). `memory.*` (singular) is the libreclaw MEMORY.md budget and is currently not rendered at all. CLI: `clawtilla memory list|search|journal|undo|proposals|accept|reject|review ID ...`.

## Creating, importing, removing

```bash
clawtilla agent create --id researcher --model sonnet --computer container [--provider X] \
    [--name X] [--description X] [--image REF] [--vm-image PATH] [--vm-cpus N] [--vm-memory-mb N] \
    [--vm-disk-gb N] [--team T] [--team-role lead] [--workspace DIR] [--face SHAPE] [--color HEX] [--no-start]
clawtilla agent new                      # six questions, a model drafts config + org files
clawtilla agent new --ai "reads my notes" [--commit] [--designer P --designer-model M]
clawtilla agent import scribe --from DIR | --link DIR | --git URL [--keep-git]
clawtilla agent discover                 # dirs on disk not in config
clawtilla agent import ghost             # adopt one in place (same as create)
clawtilla agent forget ghost             # renames to <path>.discarded; deletes nothing
clawtilla agent rm researcher [--with-computer] [--purge]
clawtilla agent git-init [agent]
```

`create` starts the agent (a computer is built only at first start). `--computer vm` needs `--vm-image`. `rm` keeps mailbox/transcripts; `--with-computer` destroys its own container/VM (never a shared machine); `--purge` deletes workspace, `<state_dir>/agents/<id>` and its transcripts (irreversible; a `--link` workspace keeps the target). Adding to `clawtilla.yaml` by hand needs `config reload`.

## Faces and avatars

`profile-picture.{png,jpg,jpeg,webp}` in the workspace is used automatically; `avatar` overrides. Otherwise a generated face (`face`: pebble, cloud, cushion, clover, star, bean, flower, drop, gem, petal, puff, squircle, wedge, bloom, or `initials`), painted in `color`.

```bash
clawtilla agent avatar show|set|clear ID [FILE]
clawtilla agent face --list ; clawtilla agent face ID cloud --color '#22b573' ; clawtilla agent face ID random
```

## Lifecycle and states

States: `stopped`, `starting`, `running`, `degraded` (process up, link down -- only the link), `stopping` (someone asked), `error` (failed start or unrequested unclean exit; retryable), `shadow` (config/mailbox/secret/computer cannot be built; listed, explains itself, refuses to run; retaken on reload and `agent set`, in both directions; a running agent is not touched).

Start order: validate integrations, resolve secrets and link token, render `config.yaml`, create+start computer (unless `computer.start: on_first_use`), start runtime, wait for link, drain backlog.

```bash
clawtilla agent list           # STATE, WORK, QUEUE, LINK columns
clawtilla agent show ID        # state/shadow reason, work, pid, uptime, restarts, identity size, failover
clawtilla agent start|stop|restart|interrupt|reset|logs ID
clawtilla status               # fleet summary, who is working
```

- `interrupt` kills the CLI mid-turn and keeps the process, session, mailbox and computer; `stop` stops all of it (container stopped; turns marked interrupted). Embedded agents cannot be interrupted.
- `restart: on-failure` (default) leaves an agent stopped after a clean exit; a stopped agent with QUEUE > 0 is stalled, not idle -- use `always` for mailbox-driven agents or `idle_stop_hours` (woken by the next message).
- Out of session allowance: agent is paused until the stated reset, its mailbox is not drained, and the whole fleet on that account stops together. Do not restart things; see [operating.md](operating.md).
- Embedded: no env/credentials of its own, no bwrap (refused/disabled), no interrupt.

## Confinement of the agent's own process

`runtime.confine: bwrap` puts libreclaw, the CLI, everything it runs and its MCP servers in bubblewrap (workspace, state dir, mounts, CLI login dir, programs; private `$HOME` and `/tmp`). Missing bwrap = shadow, never unconfined. `runtime.network`: `open` (default), `gated` (empty netns; only route is the egress gate, public addresses only; private exceptions in `egress.allow`), `none` (only a brokered model key gets out). `gated`/`none` require `runtime.confine: bwrap`. Both take effect at next start. Details: [security.md](security.md). Host-computer `computer.host.confine` is a different key ([computers.md](computers.md)).
