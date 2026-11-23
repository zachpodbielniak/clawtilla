# The `clawtilla` CLI, part 2: spend, computers, config, integrations, schedules, skills, backups

Continues [cli.md](cli.md) (global options, clawtillad, agents, messages,
tasks). Verified against clients/cli/main.c and clients/cli/backup.c.

## Spend, limits, health

| Command | What it does |
|---|---|
| `clawtilla cost [--today \| --days N] [--agent ID] [--by-day]` | spend per agent and total, last turn, idle flag (`orchestration.idle_hours`); `--by-day` adds 14 days (or N) |
| `clawtilla cost --by agent\|model\|origin\|day [--csv] [--today \| --days N] [--agent ID]` | group the ledger; UNPRICED turns are counted, not priced (unknown is not free) |
| `clawtilla limits [--cached] [--json]` | each provider login's plan windows, credits and resets; runs each CLI (up to 60 s) unless `--cached` |
| `clawtilla sentinel [--events DIR] [--output FILE\|-] [--days N] [--min-failures N] [--queue-wait SECONDS] [--daily-spend MICROS]` | read-only scan of the event log; writes `<state_dir>/sentinel.yaml` (the Issues view) |
| `clawtilla memory list <agent> [--category C]` / `search <agent> <query>` / `journal <agent>` | what an agent wrote down (read-only from the CLI) |
| `clawtilla memory undo <agent> <change> [--scope agent\|team\|fleet]` | revert one memory change |
| `clawtilla memory proposals <agent>` / `accept <agent> <proposal>` / `reject <agent> <proposal>` / `review <agent>` | shared-memory proposals |

See [spending-and-usage.md](spending-and-usage.md).

## Computers, files, browsers, folders

| Command | What it does |
|---|---|
| `clawtilla computer exec <agent> [--] COMMAND...` | run a command (an argv, not a shell line; use `bash -c '...'` for pipes) |
| `clawtilla computer status <agent>` | the machine's state |
| `clawtilla computer start <agent>` | power it on |
| `clawtilla computer stop <agent> [--remove]` / `restart <agent> [--remove]` | `--remove` is required when stopping destroys it (a container's default) |
| `clawtilla computer rebuild <agent>` | rebuild the machine (refused while the agent runs) |
| `clawtilla computer screen <agent>` / `takeover <agent>` / `release <agent>` | its screen; take the controls; give them back |
| `clawtilla computer shared` | machines agents share, and who is on each |
| `clawtilla computer desktop-mcp <agent>` | the guest-desktop MCP relay `.mcp.json` runs; not for people |
| `clawtilla cp <src> <dst>` | copy; either side may be `<agent>:<path>` |
| `clawtilla browser [list]` | every browser session (`agents.browser.mode`) |
| `clawtilla browser open AGENT URL` / `click AGENT X Y` / `screenshot AGENT FILE` / `stop AGENT` / `secrets AGENT` | drive one; `secrets` lists vault entries it may fill |
| `clawtilla folders [list]` | host folders shared with the fleet |
| `clawtilla folders add <path> [inside] [--ro] [--team T] [--agent A] [--relabel WHICH]` | share one (no flag = every agent with a computer; flags repeat) |
| `clawtilla folders rm <inside>` | stop sharing |
| `clawtilla image` | container images known (add to `defaults.container_images`) |
| `clawtilla image vm list` / `catalog` / `get <url-or-id> [name]` / `rm <name> [--force]` / `cancel <name>` | VM disk images |

See [computers.md](computers.md).

## Configuration and models

| Command | What it does |
|---|---|
| `clawtilla config show` | the daemon's loaded config |
| `clawtilla config validate` | validate what the running daemon holds |
| `clawtilla config validate --file PATH [--strict]` | validate a local YAML without a daemon; `--strict` makes warnings errors |
| `clawtilla config render <agent>` | the libreclaw config.yaml clawtilla writes for that agent |
| `clawtilla config edit` | open the local config in `$EDITOR`, then reload |
| `clawtilla config reload` | make the daemon re-read its config (as SIGHUP) |
| `clawtilla model` | providers, their models, and which run agents (takes no verb) |

There is **no `clawtilla config set`**. Fleet-level keys change through `config edit`, or an
edit plus `config reload`; agent keys through `clawtilla agent set`; team and
project fields through `team set` / `project set`. Every key:
[config-keys.md](config-keys.md).

## Integrations, connectors, the broker

| Command | What it does |
|---|---|
| `clawtilla integration types` | kinds there are (matrix, mcp, notify, grokbot, ...) |
| `clawtilla integration list [agent]` / `show <name>` | instances, and what an agent has |
| `clawtilla integration add <name> <type> [key=value...]` | add one |
| `clawtilla integration set <name> [key=value...]` / `set <name> --agent <id> key=value...` | change it, fleet-wide or for one agent |
| `clawtilla integration scope <name> all\|none\|<ids>` | who gets it |
| `clawtilla integration rm <name>` / `health <agent> [name]` / `test <name>` | remove; reachability; send one notification |
| `clawtilla integration matrix-login <name> <homeserver> <user>` | sign in (password on stdin, never stored) |
| `clawtilla integration matrix-rooms <name>` | rooms that account is in |
| `clawtilla connector catalog` / `list` / `registry-refresh` | known services; yours; import the MCP registry (`connectors.registry_enabled`) |
| `clawtilla connector add <name> --provider <id> [--account L] [--client-id ID] [--instance URL] [--scopes "a b"] [--scope all\|selected\|none] [--agents a,b] [--command CMD] [--url URL] [--tools a,b] [--apply-writes off\|all]` | add one; `--url` alone means any remote MCP server |
| `clawtilla connector connect <name>` / `discover <name>` | authorize or sign in / ask a remote server how |
| `clawtilla connector key <name>` | paste a token on stdin instead |
| `clawtilla connector refresh <name>` / `revoke <name>` / `rm <name>` | renew; forget here and withdraw there; delete |
| `clawtilla broker status` / `start` / `stop` / `restart` / `logs` | the AnythingMCP/Jentic broker in podman (`broker.enabled`) |
| `clawtilla broker key <agent\|team:<id>>` | store a gateway server's key (stdin) |

See [integrations.md](integrations.md), [connectors.md](connectors.md).

## Routines, triggers, Grok

| Command | What it does |
|---|---|
| `clawtilla routine list` | schedules and next runs |
| `clawtilla routine add <id> <agent> [key=value...]` / `set <id> [key=value...]` | keys from `routines.*`: description, instructions, schedule (manual, hourly, daily, weekdays, weekly, custom), at, weekday, cron, model, enabled, catch_up, ... |
| `clawtilla routine rm <id>` / `run <id>` | remove; run now |
| `clawtilla trigger list` | webhooks that can start a run |
| `clawtilla trigger add <id> <agent> [key=value...]` | prints the secret and address ONCE; starts disabled and unverified. Providers: generic, forgejo, gitea, github, gitlab, venture; mode run or post |
| `clawtilla trigger set <id> [key=value...]` | e.g. `enabled=true`, `match=path=value` (repeat, at most 10), `mode=post room=ops`, `max_pending=2` |
| `clawtilla trigger rm <id>` / `rotate <id>` | remove; new secret and address |
| `clawtilla trigger test <id> [--run]` / `replay <id> [receipt] [--run]` | show the prompt or run it; replay a stored delivery |
| `clawtilla trigger capture <id>` / `check <id> [path=value...]` / `deliveries [id]` | first delivery as it arrived; try a filter; what came of each |
| `clawtilla grok bots` / `jobs [--bot B] [--agent A] [--open]` | Grok bots and their jobs |
| `clawtilla grok send <bot> <instructions...> --agent A [--context TEXT] [--timeout MINUTES]` / `test <bot> --agent A` | hand a job over; ping |
| `clawtilla grok routine-prompt <bot>` / `mcp-key <bot>` / `remove <job>` | paste-once instructions; mint MCP key (printed once); cancel or forget |

See [routines-and-triggers.md](routines-and-triggers.md), [grok-and-venture.md](grok-and-venture.md).

## Skills, teaching, attachments, plugins, the office

| Command | What it does |
|---|---|
| `clawtilla skill list` / `show <name>` | library, provenance |
| `clawtilla skill new <name> <description>` / `import <dir> [origin]` / `edit <name>` / `rm <name>` | write; import (lands disabled, markdown only); edit; delete |
| `clawtilla skill enable <name>` / `disable <name>` | let it reach its agents, or not |
| `clawtilla skill assign <name> <agent\|team:<id>\|fleet>` / `unassign <name> <target>` | who has it |
| `clawtilla skill reload` / `proposals` / `approve <proposal>` / `reject <proposal> [why]` | rescan; agent proposals |
| `clawtilla skill tests <name>` / `test <name> [test] [--agent A]` | saved tests; run them |
| `clawtilla teach list` / `start <agent> [agent\|host-demo\|guest-demo] [goal]` / `stop <id>` / `show <id>` / `rm <id>` / `draft <id>` / `commit <id>` | record a task, draft a SKILL.md, add it (disabled) |
| `clawtilla attachment list <agent\|room>` / `get <id> [-o FILE\|-]` | files a conversation carried |
| `clawtilla plugin list` | loaded plugins |
| `clawtilla office who` / `worlds` / `layout` / `choreography` | who is where; world packs; seats; built-in rules |
| `clawtilla office on` / `off` / `use WORLD` / `renderer auto\|2d\|3d` / `open` | switch it, pick a world, renderer, start clawtilla-office |
| `clawtilla office custodians [install [--team NAME]]` / `character AGENT [--direct]` / `api-key higgsfield\|comfyui` | custodial team; regenerate a character; store the asset key (stdin) |
| `clawtilla office peers` / `peer-pair NAME CODE` | office visitors from another daemon (superseded by federation) |
| `clawtilla federation status` / `tiers` | this fleet's id, safety words, address; the tier presets |
| `clawtilla federation invite [--tier T] [--confirm] [--endpoint URL] [--note N] [--access-id ID] [--qr]` / `invites` / `uninvite ID` | make, list, withdraw one-use invites |
| `clawtilla federation inspect INVITE` / `accept INVITE --tier T [--alias A] [--tag X]...` | read an invite; link with its fleet |
| `clawtilla federation link SOCKET\|HOST:PORT [--alias A] [--tag X]` | link another daemon you own (tier owned both sides) |
| `clawtilla federation list [--filter SEL]` / `show ALIAS` / `explain ALIAS` / `log [ALIAS]` | linked fleets; one fleet's grant and sources; the audit |
| `clawtilla federation set ALIAS [--tier T] [--label L] [--owner O] [--endpoint URL] [FIELD=VALUE]...` / `tag ALIAS +X -Y` | change tier, details, grant overrides, tags |
| `clawtilla federation hide\|unhide\|mute\|unmute\|approve\|ping\|revoke\|forget ALIAS` | per-fleet actions |
| `clawtilla federation transcript ALIAS (--task PROXY-ID \| --agent AGENT) [--limit N]` | read back work you gave that fleet, or its conversations with an agent, per its `transcripts` grant to you |
| `clawtilla federation rotate-key` / `expose --cloudflare --hostname H` | new fleet key; print a cloudflared config |

See [skills-and-teach.md](skills-and-teach.md), [plugins.md](plugins.md), [office.md](office.md), [office-custodial.md](office-custodial.md), [federation.md](federation.md).

## Backups (offline; stop the daemon first)

| Command | What it does |
|---|---|
| `clawtilla backup create STATE_DIR ARCHIVE` | verified archive of the fleet: includes config.yaml; excludes generated configs, credentials, secrets, tokens, sessions, .git, .env, .ssh, .mcp.json |
| `clawtilla backup verify ARCHIVE` / `preview ARCHIVE` | check it; list what is in it |
| `clawtilla backup restore ARCHIVE NEW_DIRECTORY` | restore into a new directory (never over the old) |

Limits: 256 MiB archive, 64 MiB per file, symlinks refused. See
[operating.md](operating.md).
