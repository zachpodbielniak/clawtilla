# The `clawtilla` CLI, part 1: connecting, the fleet, agents, messages, tasks

Every top-level command comes from the verb table in clients/cli/main.c
(`static const ClawtVerb verbs[]`); `scripts/list-cli-commands.sh` prints
them. Part 2 ([cli-2.md](cli-2.md)) covers spend, computers, config,
integrations, routines, skills and backups. Task-shaped sequences are in
[recipes.md](recipes.md).

Most commands talk to a running `clawtillad` over its socket. The ones that
do not: `clawtilla backup`, `clawtilla remote`, `clawtilla daemon`,
`clawtilla config validate --file`, `clawtilla agent face --list/--svg`,
`clawtilla team tag list --well-known`, and every command's usage text.

## Global options (before the command only)

Global flags are parsed only from words *before* the verb, so a message body
can never be eaten as a flag. `clawtilla agent list -s SOCK` does not work;
`clawtilla -s SOCK agent list` does.

| Option | Meaning |
|---|---|
| `-c, --config FILE` | fleet config (default `~/.clawtilla/config.yaml`) |
| `-s, --socket FILE` | the daemon's unix socket |
| `-p, --profile NAME` | a saved connection (`clawtilla remote list`) |
| `-H, --host HOST` | reach a daemon at this address, not the local socket |
| `--port PORT` | port for `--host` (default 8792) |
| `--token TOKEN` | bearer token for `--host` |
| `-V, --version` | clawtilla and libreclaw versions |
| `--license` | AGPL-3.0-or-later notice |
| `--generate-config` | print a starter config YAML (data/default-config.yaml) |
| `--generate-systemd-service` | print a `systemd --user` unit (`clawtilla.service`; `ExecReload` sends SIGHUP) |

No arguments prints the command list and exits non-zero.

## `clawtillad` (clients/daemon/main.c)

| Option | Meaning |
|---|---|
| `-c, --config FILE` | config path |
| `-f, --foreground` | stay in the foreground, log to stderr |
| `-b, --bind IP[:PORT]` | listen here *instead of* the configured network addresses; repeatable (`'[fd7a::1]:9000'` for IPv6) |
| `--no-bind` | unix socket only, no network listener |
| `-V, --version` | version |

SIGHUP reloads the config; SIGINT/SIGTERM shut down. `clawtilla daemon`
runs the same daemon in the foreground.

## Daemon, connections, devices

| Command | What it does |
|---|---|
| `clawtilla daemon` | run the daemon in the foreground (same code as clawtillad) |
| `clawtilla daemon token` | print the TCP/tailnet bearer token, read from `daemon.token_file` or `<state_dir>/tcp-token` (never over IPC) |
| `clawtilla remote list` | saved connections (`connections.yaml`; tokens never printed) |
| `clawtilla remote add <name> <host> [--port N] [--token T] [--tls] [--insecure]` | save a connection |
| `clawtilla remote pair <name> <host[:port]> <code> [--device-name N] [--tls] [--insecure]` | trade a pairing code for a device token |
| `clawtilla remote rm <name>` | forget one |
| `clawtilla device list` | paired devices (never tokens) |
| `clawtilla device pair [--name NAME] [--qr [--qr-light]]` | print a six-digit, five-minute, single-use code (owner only) |
| `clawtilla device revoke <id>` | kill that device's token and its live connections |
| `clawtilla status` | what this daemon runs now, including any hold and its draining count |
| `clawtilla pause [agent]` | hold the fleet (or one agent): stop delivery, let turns finish |
| `clawtilla resume [agent]` | lift the hold; queued work goes out in order |
| `clawtilla activity` | every turn running across the fleet, with its last step |
| `clawtilla policy-hook --agent ID --token-file PATH` | Claude Code's PreToolUse hook; the daemon writes it into agent settings -- not for people |

## `clawtilla agent <verb>`

| Verb | What it does |
|---|---|
| `list` | every agent, state, queue depth |
| `show <agent>` | one agent in full |
| `new` / `new --ai "<description>" [--id X] [--name X]` | design an agent interactively or with the model; commits and starts it |
| `create --id <id> [--name X] [--model X] [--computer X] [--image X] [--vm-image PATH] [--face SHAPE] [--color HEX] [--no-start]` | add from flags and start it. Any other `--some-key value` is sent as field `some_key`. `--computer`: none, host, container, vm, distrobox, ssh, display; `--vm-image` is required for vm |
| `rm <agent> [--purge]` | remove; `--purge` deletes its files |
| `reap <agent> [--force]` | end an ephemeral agent; `--force` drops its open work |
| `start` / `stop` / `restart <agent>` | lifecycle |
| `interrupt <agent>` | stop what it is doing now; leave it running |
| `reset <agent>` | clear its session -- the only way an identity (`persona.*`) change applies |
| `logs <agent>` | its log |
| `set <agent> <key> <value>` | change one agent-relative key (`model.model`, `tools.allow`, ...); lists are comma-separated; prints whether to restart or reset |
| `files <agent>` / `edit <agent> [FILE...]` | the org files that become its prompt; open in `$EDITOR` |
| `mount list <agent>` | its own shared folders |
| `mount add <agent> <source> <target> [ro\|rw] [--relabel WHICH]` | share a folder with just this agent (default ro; next start) |
| `mount rm <agent> <target>` | stop sharing it |
| `avatar show <agent> [FILE]` / `avatar set <agent> <file>` / `avatar clear <agent>` | profile picture |
| `face <agent> [SHAPE\|initials\|auto\|random] [--color HEX\|auto]`, `face --list`, `face --svg <agent\|SHAPE> [--color HEX] [--size N]` | the generated face |
| `git-init [agent]` | version an agent's state directory |
| `discover` | agents on disk the config does not know |
| `import <id> [--from DIR \| --link DIR \| --git URL] [--keep-git]` | adopt one, keeping its state |
| `forget <id>` | move it aside; nothing deleted |

Keys and their effects: [config-keys.md](config-keys.md), [agents.md](agents.md).

## Messages, rooms, notices

| Command | What it does |
|---|---|
| `clawtilla send <agent-or-room-or-AGENT@FLEET> <message...>` | send; words after the target are joined, quoting optional. Mid-turn it is held (steered) until the turn ends. `AGENT@FLEET` writes to a linked fleet's agent and prints the `fed:` room (read it with `room history`) |
| `clawtilla chat <agent>` | interactive chat |
| `clawtilla mailbox list <agent>` / `dead <agent>` | queued items / dead-lettered ones (a running agent's mailbox is empty: delivery acks at the socket) |
| `clawtilla mailbox ack <agent> <item>` / `requeue <agent> <item>` | settle or retry one item |
| `clawtilla mailbox purge <agent>` | drop expired items |
| `clawtilla room list` | rooms and members |
| `clawtilla room create <room> [--members a,b]` | make a group room |
| `clawtilla room add <room> <agent>` | add a member |
| `clawtilla room history <room>` | its transcript |
| `clawtilla room share <room> <fleet-alias>` / `unshare` | share a declared room with a linked fleet (both grants need `join_rooms`); it appears there as `fedroom:HOST:ROOM` |
| `clawtilla room read <agent\|room>` / `room read --all` | mark read |
| `clawtilla notices [--agent A] [--all]` | what wants you, and alerts |
| `clawtilla notices read <ID\|--all>` / `dismiss <ID>` / `clear [--agent A]` | settle notices |
| `clawtilla mute <agent> [--for 90s\|30m\|2h\|3d]` | silence its notifications (bare number = minutes) |
| `clawtilla unmute <agent>` | let them through again |
| `clawtilla event list [subject]` | the daemon's event history; subject is an agent, room or task |

See [mailboxes-and-rooms.md](mailboxes-and-rooms.md).

## Teams, projects, accounts, history, fleet packs

| Command | What it does |
|---|---|
| `clawtilla team list` | teams, leads, how many running |
| `clawtilla team create <id> [DESCRIPTION]` | add one |
| `clawtilla team set <id> <key> <value>` | name, description or color |
| `clawtilla team rm <id>` | remove; its agents become teamless |
| `clawtilla team assign <agent> <team> [lead]` | sets the agent's `team` and `team_role` (lead or member) |
| `clawtilla team tag list [--well-known]` / `tag add <team> <tag>` / `tag remove <team> <tag>` | team tags (e.g. `ephemeral`, `office-custodial`) |
| `clawtilla project list` | projects and this month's spend |
| `clawtilla project create <id> [KEY=VALUE...]` | keys: name, directory, goal, review_by, agents, monthly_budget_usd, budget_warn_percent, budget_hard_stop |
| `clawtilla project set <id> <key> <value>` / `rm <id>` | change (`""` removes a field) / remove |
| `clawtilla account list` / `login <id>` | separate CLI logins; `login` runs the CLI's sign-in for that account |
| `clawtilla account add <id> <provider> <dir> [DESCRIPTION]` / `rm <id>` | add or remove a login |
| `clawtilla history [list] [config\|persona\|skill] [SUBJECT]` | versions of config, persona files (`<agent>/<file>`), skills |
| `clawtilla history show <id>` / `diff <id> [<newer>]` / `restore <id>` | inspect, compare (one id = against now), restore (a new save) |
| `clawtilla fleet export <dir>` | write a pack: agents, teams, routines, triggers, projects, personas, skills; secrets and paths listed to fill |
| `clawtilla fleet import <dir> [--preview] [--skip K:ID] [--replace K:ID] [--rename K:ID=NEW] [--allow-dangerous]` | import; agents arrive held, skills disabled |

See [teams-and-ephemeral.md](teams-and-ephemeral.md), [tasks-and-projects.md](tasks-and-projects.md).

## Tasks, decisions, waits

| Command | What it does |
|---|---|
| `clawtilla task list [--history] [--state S] [--agent A] [--before UNIX] [--limit N]` | tasks (default verb) |
| `clawtilla task show <task> [--journal]` | one task, and what happened to it |
| `clawtilla task create [--agent A] [--title T] [--after T1,T2] [--review lead\|delegator\|operator\|AGENT\|none] [--repo DIR [--base REF]] [--project ID] "<what to do>"` | without `--agent` it goes on the backlog; `--agent AGENT@FLEET` hands it to a linked fleet (a proxy task here) |
| `clawtilla task assign <task> <agent>` | hand a backlog task out |
| `clawtilla task comment <task> <text>` | add a note |
| `clawtilla task link <task> <waits-for>` / `unlink <task> <waits-for>` | dependencies |
| `clawtilla task review <task> accept` / `review <task> changes "<what>"` | verdict |
| `clawtilla task diff <task>` | what its checkout changed |
| `clawtilla task checkout-remove <task> [--force]` | remove its checkout (keeps the branch) |
| `clawtilla task venture-retry <task>` | resend to VENTURE |
| `clawtilla task cancel <task>` | cancel it and what it spawned |
| `clawtilla task retry <task\|inc-id> [reason]` | send a failed run round again |
| `clawtilla task incidents` | broken runs, and who was told |
| `clawtilla decision [list [--all]]` | what agents asked you, with each one's `options:` when it has a fixed set |
| `clawtilla decision answer <id> "<free text>"` / `dismiss <id>` / `delete <id>` | answer; dismiss = agent uses its default; delete = remove for good |
| `clawtilla wait list [agent]` / `wait cancel <id>` | what agents are waiting for |
