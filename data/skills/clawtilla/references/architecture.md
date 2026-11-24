# clawtilla architecture

What clawtilla is, how the pieces fit, where its state lives, and the rules anyone changing the code must follow. Source docs: `docs/architecture.org`, `docs/maintenance.org` ("What lives where"), `docs/development.org`, `AGENTS.md`.

## What it is

clawtilla is a GLib/GObject C daemon (`clawtillad`) that runs a fleet of AI agents. Each agent is a supervised `libreclaw` process (or an in-process `LcApp`) driving an AI CLI (Claude Code, opencode, grok-build, ...). clawtilla decides which agents exist, what each one is, how messages move between them and people, what each may touch (computers, mounts, network, connectors) and what it may spend. libreclaw owns one agent's model, session, tools and outside-world channels.

| Concern | Owner |
|---|---|
| One agent's model, session, tools, channels (Matrix, mail) | libreclaw |
| Which agents exist and what each is | clawtilla |
| Credentials and environment | clawtilla renders, libreclaw consumes |
| Messages between agents; hop, rate, cost limits | clawtilla |
| Containers and VMs | clawtilla, via podomation |

## The rule everything follows

**The daemon owns every transport, credential and agent process. Clients hold none.** A client sends typed requests and folds one event stream, which is why the CLI, GTK, web, lrg clients and an in-process cmacs embed are the same program with different front ends. Never let a client edit `config.yaml`, start a process or read a mailbox itself.

**One daemon per state directory.** `clawt_daemon_start()` takes an exclusive `flock` on `<state_dir>/daemon.lock` before touching anything. A second daemon is refused, naming the pid. There is no stale lock (the kernel drops it on exit, even SIGKILL); deleting the file does nothing. A second fleet on one machine must move **three** keys: `daemon.state_dir`, `daemon.socket`, `defaults.workspace_root` (the last defaults to the absolute `~/.clawtilla/agents` and does not follow `state_dir`).

## Two sockets

```
 clawtilla / clawtilla-gtk / clawtilla-web / clawtilla-lrg / cmacs (in-proc)
                  |  daemon.sock (unix 0600) + optional TCP/tailnet (token)
          ClawtIpcServer
                  |
             ClawtDaemon  -- config, agents, rooms, tasks, mailboxes, events, plugins
                  |
          ClawtLinkServer   <state_dir>/agents.sock (agents dial OUT to it)
                  |
         libreclaw processes (LcClawtillaChannel, libreclaw >= 0.26.0)
```

Clients get the whole surface; an agent gets exactly the tools its config grants (served by `clawtilla-mcp-server`, which its AI CLI starts from `<workspace>/.mcp.json`). Agents authenticate with a per-agent token file (`<state_dir>/agents/<id>/token`) on top of the 0600 socket.

## Object model

```
ClawtDaemon (owns everything)
├── ClawtConfig            config.yaml -> per-agent libreclaw config.yaml (rendered)
├── ClawtAgentManager
│   └── ClawtAgent
│       ├── ClawtAgentRuntime   process (default) | embedded (lc_app_new_embedded)
│       ├── ClawtComputer       none | host | container | distrobox | vm | display (+ desktop add-on)
│       ├── ClawtMailbox        durable SQLite queue
│       ├── ClawtMemoryStore    per-agent searchable memory (memories.*)
│       └── ClawtLink           bridge-protocol connection to the agent
├── ClawtRoomManager       rooms + append-only transcripts
├── ClawtTaskManager       tasks, written through to tasks.db before task-changed
├── ClawtMailboxRouter + ClawtLoopGuard   routing; hop/rate/budget/cycle limits
├── ClawtEventBus (ring of 1024, cursor) + ClawtEventLog (NDJSON per UTC day)
├── ClawtDecisionStore     questions waiting on a person (decisions.db)
├── ClawtUsage             spend, read from each agent's libreclaw database
├── ClawtMcpTools          orchestration tools served to agents
├── ClawtPluginManager, ClawtPodBridge (podomation), ClawtNotifier,
│   ClawtRoutineRunner, ClawtAutomation, ClawtExchange, ClawtVmImageStore
├── ClawtBroker            integration broker in podman (only if broker.enabled)
├── ClawtLinkServer        agents dial in
└── ClawtIpcServer         clients dial in
```

- Teams have **no object**: a config list plus two agent fields, read by pure functions (`clawt_team_may_assign()`, `clawt_team_validate_fleet()`).
- Shared computers: declared under top-level `computers:`, named by an agent's `computer.use`; the daemon pools built machines by id.
- Action-policy gate: `clawt_policy_mode_for()` (allow/deny from config) and `clawt_daemon_policy_request()` (ask/review/hand_back via the decision inbox). Every tool call route reaches it.
- `ClawtAgentCaps` is derived from what the agent *has*, not what its config asked for.
- Process runtime caps: `INTERRUPT | STREAMING`; embedded: `STREAMING` only, no own env/credentials, cannot be confined (`runtime.confine: bwrap` + embedded is a shadow).

## How a message moves

```
client msg.send / agent tool call / agent reply on its link
        -> ClawtMailboxRouter::send (every path converges here)
        -> resolve destination (agent id -> its direct room)
        -> ClawtLoopGuard: hops? rate? budget? cycle?   (refused -> message.refused)
        -> append to the room transcript
        -> for each member: post to its ClawtMailbox (durable)
        -> drain to whoever is connected -> ClawtLink -> libreclaw -> the agent
```

- Loop safety runs **before** enqueue; the mailbox is written **before** delivery, so messaging a stopped agent queues it and a killed daemon loses no queue.
- Delivery acks at the socket, so a **running** agent's mailbox is almost always empty. That is not an answer to anything.
- libreclaw runs **one turn per message** (except a steered message). Turn state is per room, per delivery. See [orchestration.md](orchestration.md) and [mailboxes-and-rooms.md](mailboxes-and-rooms.md).

## Threading

Single-threaded on one `GMainContext`. The only work moved to a worker is starting an agent's computer (`clawt_computer_lifecycle_async()`, `clawt_computer_exec_async()`). An IPC handler must never wait on the network; long work uses `clawt_ipc_server_defer()` + `clawt_ipc_pending_respond()`. Timers use `clawt_timeout_add_seconds()`; never `g_timeout_add*()`/`g_idle_add()` (they miss an embedded daemon's context).

## Files on disk

Config file: `-c PATH`, else `~/.clawtilla/config.yaml` (called `clawtilla.yaml` in docs; it is the only file written by hand). Defaults: `daemon.state_dir` = `~/.clawtilla`, `daemon.socket` = `$XDG_RUNTIME_DIR/clawtilla/daemon.sock`, `secrets.dir` = `~/.clawtilla/secrets`, `defaults.image_dir` = `$XDG_DATA_HOME/clawtilla/images`.

### `<state_dir>/` (daemon-wide)

| Path | What |
|---|---|
| `daemon.lock` | the one-daemon flock; never back up or delete |
| `agents.sock` | link socket agents dial |
| `tcp-token` | owner bearer token, generated on first network listener (0600) unless `daemon.token_file` is set |
| `devices.yaml` | paired devices: SHA-256 of each token, never the token |
| `events/YYYY-MM-DD.ndjson` | the event log: append-only, secrets scrubbed on write, swept after `daemon.event_log_days` (30). The record; transcripts are a projection |
| `transcripts/<room>.ndjson`, `transcripts.db` | what was said (append-only), and its index |
| `tasks.db` | tasks, journal, notes, links, incidents, VENTURE outbox |
| `decisions.db` | the decision inbox |
| `revisions.db` | every version of config, persona files, skills (newest 200 each) |
| `reads.db`, `notices.db` | read marks per conversation; notification list and mutes |
| `handoffs.db`, `triggers.db`, `grok-jobs.db`, `grok/` | task handoffs, trigger deliveries, Grok jobs |
| `routines.yaml` | routine run state (never in config.yaml) |
| `hold.yaml` | pause hold plus the set of agents to bring back |
| `ephemeral.yaml` | ephemeral agents (not in config.yaml); see [teams-and-ephemeral.md](teams-and-ephemeral.md) |
| `sentinel.yaml` | last sentinel report |
| `OPERATOR.org`, `memories/fleet.db` | operator profile; fleet-scope memories |
| `bundled-skills/`, `skill-settings/`, `skill-proposals/`, `teach/`, `saved-steps/`, `memory-review/` | skill machinery |
| `office/worlds/`, `office/characters/`, `fleet-id` | the office; see [office.md](office.md) |
| `egress/`, `broker/`, `browsers/`, `displays/<agent>/`, `frames/`, `attachments/` | gate sockets, broker state, browser sessions, virtual displays, screen frames, chat files |
| `.git/`, `.gitignore` | `daemon.git` (on): repository created, never committed by clawtilla |

### `<state_dir>/agents/<id>/` (also the workspace unless `agents.workspace` moves it)

| Path | What |
|---|---|
| `SOUL.org`, `IDENTITY.org`, `TOOLS.org`, ... | persona; yours to edit (clawtilla owns only `# BEGIN clawtilla` regions) |
| `.mcp.json` | yours except the `clawtilla` / `clawtilla-*` keys |
| `config.yaml` | GENERATED libreclaw config, rewritten every start and reload; never edit |
| `token` | link token, generated once, 0600 |
| `mailbox.db`, `memory.db` | durable queue; searchable memory |
| `credentials/` | GENERATED from secret references, 0600 |
| `sessions/libreclaw.db` | libreclaw's database (token_usage rows: spend comes from here) |

Outside state_dir: `$XDG_DATA_HOME/clawtilla/vms/<id>/` (overlay, seed, key, `ssh-port`), `.../boxes/<id>/` (distrobox home), `.../images/`, `.../exchange/`; connector credentials under `secrets.dir/connectors/<name>/` (the flat `connector-<name>.json` layout is migrated at start).

## Binaries

| Binary | Role |
|---|---|
| `libclawt-1.0.so` / `.a` | the library; everything else is a thin client (pkg-config `clawtilla-1.0`, header `<clawtilla.h>`, GIR `Clawt-1.0`) |
| `clawtillad` | the daemon (`clawtilla daemon` runs the same code in-process) |
| `clawtilla` | CLI; see [cli.md](cli.md) |
| `clawtilla-gtk` | GTK4/libadwaita client (skipped without libadwaita) |
| `clawtilla-web` | htmx web client, separate process, no login |
| `clawtilla-lrg` | libregnum client (`LRG_BACKEND=1`) |
| `clawtilla-office` | the drawn office (`OFFICE=1`) |
| `clawtilla-mcp-server` | MCP server an agent's CLI runs; tools in [mcp-tools.md](mcp-tools.md) |
| `clawtilla-relay` | runs inside a gated agent's bwrap box, bridges loopback to egress gate and broker |
| `plugins/libclawt-plugin-*.so` | bundled plugins; see [plugins.md](plugins.md) |

## Libraries

`deps/libreclaw` is the only submodule (pinned to a tag; currently 0.30.7) and brings ai-glib, yaml-glib, mcp-glib, podomation, htmx-glib. Always `git submodule update --init --recursive`. libregnum (for lrg/office) is a sibling checkout at `LRG_DIR` (default `../libregnum`), never a submodule; gsurf (browser sessions) at `GSURF_DIR` (`../gsurf`), driven over its control socket, never linked.

## Build flags

| Flag | Effect |
|---|---|
| `DEBUG=1` | `build/debug/` (`-O0 -g3`); default `build/release/` (`-O2`) |
| `ASAN=1`, `UBSAN=1` | sanitizers (GIR auto-off; `GIR=1` forces) |
| `LRG_BACKEND=1` | builds `clawtilla-lrg` against libregnum |
| `OFFICE=1` | builds `clawtilla-office` and the lrg Office view; implies `LRG_BACKEND=1`, needs libregnum >= 0.11.1 (its raylib decodes JPEG, a generated model's texture), `gjs-devel`, `gobject-introspection-devel` |
| `GSURF=1` | browser sessions; needs gsurf >= 0.5.14, `Xvfb` |
| `QRENCODE=0`, `POPPLER=0` | turn off auto-detected QR codes / PDF thumbnails |
| `PREFIX=` | install prefix (default `/usr/local`) |

Key targets: `make`, `make test`, `make tests` (build only: the zero-warning check), `make test-integration`, `make config-files`, `make docs-check`, `make parity`, `make clean` (keeps deps), `make clean-deps`, `make clean-all`, `make install-deps`, `make help`.

## Changing the code: rules that are not optional

- **Zero warnings** on `make clean all` and `make clean tests`. gnu89, gcc, `/* */` comments, `clawt_` prefix, SPDX AGPL header, GIR doc comments on all public API.
- **Config schema is the single source of truth**: edit `src/config/clawt-config-schema.c`, run `make config-files`, commit `data/example-config.yaml`, `data/default-config.yaml`, `docs/configuration-options.org`. Never hand-edit those.
- **Docs ship with the code** (`docs/*.org`, org-mode). `make docs-check` fails on undocumented exports, stale config keys, unregistered `clawtilla_*` tool names.
- **Parity**: a capability in one graphical client must exist in gtk, web and lrg (`make parity`).
- **Tests hermetic**: no network, podman, libvirt or compositor; real infra behind `CLAWT_TEST_INTEGRATION=1`.
- `make test` does not relink `clawtillad`; plain `make` does not relink tests. Rebuild the binary you are checking.
- **Versioning** (`config.mk` `VERSION_MAJOR/MINOR/MICRO`): micro +1 for any landing that changes the build (fix, perf, small feat, refactor); minor +1 (micro to 0) for a major feature or anything breaking an IPC frame/member, config key, C API/ABI, GIR or CLI output; major never without asking. docs/test/ci/chore-only changes do not bump. One bump per landing.
- **Every landing is released at once**: after `make clean all`, `make test`, `make DEBUG=1 ASAN=1 clean all test`, `make docs-check`, `make parity` pass, commit (message names the version), `git tag -a vX.Y.Z -m vX.Y.Z`, `git push origin master vX.Y.Z`. Never move or delete a pushed tag. Anything else still waits for the user to ask before pushing.
- Dependencies: `deps/libreclaw` bumped to a tagged release only (`build(deps): libreclaw vX.Y.Z -- why`); an upstream change lands and is tagged there first.

## Further reading in the source tree

When you have the checkout, these docs carry what no reference here repeats:

| Doc | For |
|---|---|
| `docs/use-cases.org` | seventeen worked scenarios end to end: one agent, a chief dividing work, teams, containers, VMs with desktops, the real machine, offline messaging, schedules, Matrix and mail, connectors, cost, a laptop driving a workstation, cmacs, a reviewed backlog, VENTURE, a GUI app on a virtual display. The recipes in recipes.md are their short form. |
| `docs/ui-usability.org` | how the desktop, browser and libregnum clients are navigated and how a UI change is verified on each |
| `docs/review-regressions.org` | the behavioural guarantees pinned by regression tests; read before changing anything they name |
| `docs/dependency-refresh.org` | the dependency baseline and how a refresh of the pinned libraries is validated |
