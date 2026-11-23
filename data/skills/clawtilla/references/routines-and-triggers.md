# Routines and triggers

A **routine** is a clock: a prompt, an agent, a schedule. A **trigger** is an event: a webhook delivery builds a prompt and hands it to the same runner. Both become a *delegated task* (visible in `clawtilla task list`, with a result). Podomation pods (reacting to fleet events) are in [routines-and-triggers-2.md](routines-and-triggers-2.md). Every key: [config-keys.md](config-keys.md). Sources: docs/routines.org, docs/triggers.org.

## Routines

```bash
clawtilla routine list                         # NEXT / LAST columns, problems
clawtilla routine add <id> <agent> [key=value ...]
clawtilla routine set <id> [key=value ...]     # keys are routines.* schema keys
clawtilla routine rm <id>
clawtilla routine run <id>                     # now; ignores schedule AND enabled
```

```yaml
routines:
  - id: morning-brief
    agent: scribe                # never the chief of staff unless isolate: true
    description: "Yesterday's commits"
    instructions: "Summarise the last 24 hours of commits; flag risky ones."
    schedule: weekdays           # manual|hourly|daily|weekdays|weekly|custom
    at: "09:00"                  # quoted or not, same value
    # weekday: monday            # for weekly
    # cron: "0 */6 * * *"        # for custom
    model: haiku                 # this run's turn only; not checked against the catalogue
    isolate: true                # room routine:<id> + own sender = own session
    catch_up: false
    jitter_seconds: 0            # random 0..N s delay, clamped at 3600
    max_start_failures: 3        # 0 = never turn off
    locked: false                # true = agent cannot change it
    directory: /work/repo        # a SENTENCE in the prompt, no chdir
    worktree: true               # machinery: per-run checkout (see below)
    read_only: false
    proactive: false
```

### Schedules

| Preset | Cron | Note |
|---|---|---|
| `manual` | none | only `routine run` |
| `hourly` | `0 * * * *` | `at: "00:20"` gives minute 20 |
| `daily` | `0 9 * * *` | `at` |
| `weekdays` | `0 9 * * 1-5` | |
| `weekly` | `0 9 * * 1` | `weekday` |
| `custom` | `cron` | five fields |

- Five fields only (min hour dom month dow); 4 or 6 fields refused at `add`/`set`. Ranges, lists, steps, names (`jan`, `mon`), Sunday = `0` and `7`, wrapping ranges (`22-2`, `fri-mon`) all work. A wrap with a step restarts at the field bottom: `23-4/2` = 23,0,2,4.
- **Day-of-month OR day-of-week**: when *both* are restricted either matches. `0 0 13 * 5` = the 13th *and* every Friday, not Friday the 13th. A day field starting with `*` stays a wildcard even with a step (`0 9 */2 * mon` = odd-dated Mondays).
- Local time: spring-gap minutes are skipped, autumn-fold minutes fire twice.
- "Next" searches 1464 days, so `0 0 29 2 *` works; `-` under NEXT for a `custom` schedule means it can never fire (`0 0 30 2 *`).
- A hand-edited bad schedule is **not refused or deleted**: one fleet warning at load (`clawtilla config validate` shows it), NEXT shows `never` with the reason, `routine.list` carries `problem`.

### What a run shares

Without `isolate`, the prompt goes from `user` into the operator's room: same libreclaw session (shared context), queued behind the conversation, visible in the operator's chat (libreclaw keys sessions on channel+room+sender, never thread). Point routines at an agent nobody chats with, or set `isolate: true` (output then lives in the task result / Flow tab). If the `routine:<id>` room cannot be made, the run silently falls back to the shared path; check `clawtilla room list`.

The agent's prompt is the instructions plus a note that nobody is waiting: do the work, and use `clawtilla_message_user` only if blocked. Notify integrations' `quiet_hours` still hold those messages; there is no per-routine exemption (use a second notify instance). See [integrations.md](integrations.md).

### Read-only and proactive

- `read_only: true`: always runs in `routine:<id>`, which offers only `read`-class tools (listing, search, recall, `clawtilla_web_*`, `clawtilla_ask_decision`); the CLI's own Write/Bash refused by the Claude Code PreToolUse hook. Only on `claude-code` / `claude-tmux`; on other backends the run fails with that reason.
- `proactive: true` implies read-only and, with no instructions, sweeps integrations, connectors, mailbox and tasks, filing each finding as a decision. See [security.md](security.md).

### catch_up and run state

- Routines fire only while the daemon runs. A missed slot shows `missed: due ..., but the daemon was not running`; missed is not failed.
- `catch_up: true` runs **once** at daemon start however many were missed. A routine that never ran has missed nothing. Off by default.
- Run state: `<state_dir>/routines.yaml` (0600), never `clawtilla.yaml`: `last_run`, outcome (`ok|failed|missed|never`), detail, task id, start-failure count/error, edit fingerprint. Unparseable file = warning + empty slate (nothing catches up).

### Model, worktree, jitter

- `model`: rides the mailbox item, applied by libreclaw when the turn starts, agent's own model restored after. Unknown names fail the run, not the save.
- `worktree: true` (0.39.0+): `directory` must be a git repo on the host; each run gets `tasks/<task>` in the agent workspace on branch `clawtilla/<task>` (git worktree when unconfined on host, hard-linked clone for bwrap/container/VM/distrobox). Checkout failure fails the run. Not for ssh hosts or remote podman. Removed after `orchestration.checkout_keep_days` unless dirty; branch kept. `clawtilla task diff` shows the changes. See [tasks-and-projects.md](tasks-and-projects.md).
- `jitter_seconds`: delays scheduled runs only; `routine run` and catch-up start at once; survives a reload (holds the id).

### Agents scheduling themselves

Tools: `clawtilla_routine_list` (always offered), `clawtilla_routine_add`, `clawtilla_routine_update`, `clawtilla_routine_remove` (unless `tools.schedule: false`). Enforced: only routines whose `agent` is itself; not `locked` ones; settable fields are instructions, description, schedule/at/weekday/cron, enabled, isolate, read_only, catch_up, jitter_seconds (also `model`) -- never agent, locked, directory, worktree. Caps: `tools.schedule_limit` (8 routines on the agent, operator's included) and `tools.schedule_min_minutes` (15, checked over the next 200 runs). Agents cannot create triggers.

### Failure handling

**Unstartable computer**: a run to an agent in `error` because its computer refused retries the start in the background; after `max_start_failures` consecutive refusals the routine sets `enabled: false` (written to `clawtilla.yaml`), raises an alert, publishes `routine.turned_off`. Re-enable: `clawtilla routine set <id> enabled=true` (any edit, including a hand edit, resets the count).

**Incidents** (`orchestration.incidents.enabled`, `orchestration.incidents.to`: `lead|chief|none`): one per *routine*, reported to the team lead (or chief of staff) in the team's "Problems to look at" room:

| Cause | Kind |
|---|---|
| refused before becoming a task (agent gone, read-only on ungated CLI, room failed) | `routine-failed` |
| task failed (delivery refused, agent crashed) | `failed` |
| stopped by turn watchdog or room budget | `stalled` |
| computer would not start | `unstartable` |

Retry with `clawtilla task retry <incident-id>`, the `clawtilla_task_retry` tool, or Retry in a client (same path as Run now; refused while the last run is going; refused for `unstartable` -- fix the computer). A completed run resolves it. Run now is marked by hand, so the loop guard's cycle detector (`orchestration.cycle_seconds`) skips it; a refused Run now is returned as an error and its task cancelled, not an incident. See [orchestration.md](orchestration.md).

Events: `routine.ran` (a run *started*; never a failure), `routine.changed`, `routine.turned_off`, `task.changed` (success/failure shows here).

## Triggers

Receiver is off by default and binds loopback + tailnet address only (never `0.0.0.0`). Paths: `GET /health`, `POST /hooks/<endpoint>`. Listener binds at daemon start: restart after enabling.

```yaml
daemon:
  webhook_enabled: true
  webhook_port: 8788
```

```bash
clawtilla trigger list                       # state: NO KEY > unverified > off > on
clawtilla trigger add <id> <agent> [key=value ...]   # prints endpoint + secret ONCE
clawtilla trigger set <id> [key=value ...]
clawtilla trigger rm <id>
clawtilla trigger rotate <id>                # new secret AND new endpoint; back to pending
clawtilla trigger capture <id>               # the handshake body
clawtilla trigger test <id> [--run]          # preview expanded prompt / send synthetic
clawtilla trigger check <id> [path=value ...]  # try a match filter on newest body, list fields
clawtilla trigger deliveries [id]            # receipts
clawtilla trigger replay <id> [receipt] [--run]  # explain; --run needs explicit receipt
```

### Setup sequence (every trigger)

1. `trigger add` (secret cannot be supplied: `secret=` is refused). 2. Register `http://<host>:8788/hooks/<endpoint>` (endpoint, **not** id) with the secret at the sender. 3. Send a test delivery; first authenticated one is `captured`, not run. 4. `trigger capture <id>`, read it. 5. `trigger set <id> enabled=true` (refused before a capture). A hand-written entry with no `secret:` block shows `NO KEY` and refuses everything: run `trigger rotate <id>` to mint one into `secrets.dir`.

### Providers (`triggers.provider`)

| Provider | Auth |
|---|---|
| `forgejo` | `X-Forgejo-Signature` HMAC-SHA256 hex, no prefix (also accepts Gitea/GitHub headers) |
| `gitea` | `X-Gitea-Signature` |
| `github` | `X-Hub-Signature-256: sha256=<hex>` |
| `gitlab` | `X-Gitlab-Token` verbatim (or Standard Webhooks headers) |
| `venture` | `X-Venture-Signature: sha256=<hex>`; unsigned refused |
| `generic` | `Authorization: Bearer`, `X-Clawtilla-Token`, `X-Clawtilla-Signature`, or `?token=<secret>` (generic only) |

Key absent = header sniffing (Forgejo, Gitea, GitHub, GitLab order). `provider: generic` is taken literally: a forge delivery to it gets 401. Generic reads only `repo ref actor title url number`; `triggers.header` names an extra event header.

### Filters (all 200 + `ignored` when they decline)

- `events: [push, pull_request]`: empty = all; case and `- _ space` folded, vocabularies are not (GitLab sends `Merge Request Hook`).
- `repo`, `branch` (`refs/heads/` stripped). A body naming no repo fails a repo filter.
- `ignore_actors`: break agent-writes-back loops (bot accounts).
- `match`: up to 10 `path`/`value` pairs, all must hold, case-folded *containment*; dotted paths, numeric array index, a name across an array matches any element. CLI: repeat `match=path=value`; `match=` clears. Invalid filter = trigger answers 503 to everything.

### Prompt, mode, placement

- Template placeholders: `{{event}} {{repo}} {{ref}} {{actor}} {{title}} {{url}} {{number}} {{kind}}`; known-but-empty expands to nothing, unknown (`{{repoo}}`) left as typed. Expanded by hand, never printf.
- The payload follows in a nine-backtick `untrusted-event-payload` fence telling the agent it is data and not to memorise it. It is a prompt, not a control: point triggers at agents whose confinement you would trust with attacker input ([security.md](security.md)).
- `mode: post` + `room: <room-or-agent-id>`: records the delivery into the room, runs nothing (no task, no cost); post without a room is refused.
- `directory` / `worktree` / `isolate` as routines (room `trigger:<id>`); `room` overrides. `agents: [a, b]` fans out to extra recipients (`agent` still required); the batch must fit `max_pending` (default 4, 1..50).
- VENTURE: `replay_connector` + `organization_id` catch up the business-event feed; see [grok-and-venture.md](grok-and-venture.md).

### Receipts and limits

`<state_dir>/triggers.db`, 7-day retention (also the dedup window). Outcomes: `ran`, `duplicate`, `ignored`, `captured`, `refused` (auth or 60/min rate), `failed` (503 config problem), `dropped` (429, over `max_pending`; replayable), `posted`. Body cap 1 MiB (413, no receipt). Unknown endpoint and disabled-and-verified both 404 (no receipt). Agents see only `clawtilla_trigger_list` (their own; no endpoint, no secret).

Troubleshooting: 404 = wrong URL (id instead of endpoint) or disabled; 401 = wrong secret or wrong provider; 200 and nothing ran = read the receipt (`captured`, `ignored`, `posted`, `duplicate`); stuck after N deliveries = unfinished tasks holding `max_pending` slots.

Events: `trigger.fired`, `trigger.posted`, `trigger.changed`, `trigger.refused` (worth alerting on), `trigger.verified`.
