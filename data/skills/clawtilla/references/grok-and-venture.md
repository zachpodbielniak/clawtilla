# Grok bots and the VENTURE mirror

Two bridges to systems outside the fleet. A Grok bot is someone agents hand work to; the VENTURE mirror copies the fleet's tasks into VENTURE tickets. The VENTURE *connector* (an agent's `venture_*` tools, staged writes as decisions) is in [connectors.md](connectors.md); on a shared VENTURE server each staged-write decision lands with the agent that staged it (matched by `whoami` actor against the card's `origin.name`), never with whichever connector polled. Keys: [config-keys.md](config-keys.md) (`grok.*`, `integrations.*`, `projects.venture_*`).

Source docs: docs/grok.org, docs/venture.org, docs/venture-setup.org. Code: src/integration/clawt-grok*.c, src/integration/clawt-venture-mirror.c.

## Grok bots

There is no API for driving a Grok bot. There is a Grok *routine* with a *webhook trigger*: a POST carrying the routine's key starts a run with the body as input. clawtilla sends a job that way, and the bot reports back over the tailnet to the daemon's webhook listener (HTTP or MCP).

### How a job travels

1. An agent calls `clawtilla_grok_dispatch` (`bot`, `instructions`, optional `context`, `timeout_minutes`), or a person runs `clawtilla grok send`. A job id `gj-...` and a token are minted from `/dev/urandom`; only the token's SHA-256 is stored, in `<state_dir>/grok-jobs.db`, before sending.
2. The routine webhook gets an envelope with the instructions, deadline and exact reporting contract. Grok's `200 {runUuid}` means *started*. The tool returns at once: the agent should end its turn, not wait.
3. The bot reports `done`, `failed` or `needs_input` once (optionally `progress` notes before).
4. The answer arrives as a message from `grok:<bot>` in the room the dispatching turn was in (or the agent's operator room if no turn sent it), one hop deeper, fenced as untrusted data. It starts the agent's next turn. Under a task, a progress note goes on the task and the answer arrives under it.
5. No report by `grok.job_ttl_minutes` (120): the job expires, the agent is told, a late report is refused.

### Setting one up (the operator does the Grok-side steps)

1. Enable the listener in clawtilla.yaml (`clawtilla config edit`; there is no `config set`), then restart the daemon (the listener binds at start):
   ```yaml
   daemon:
     webhook_enabled: true
     tailscale: true
   ```
   In Grok, connect the bot's computer to the tailnet (`grokbot-...` in `tailscale status`). Strongly recommended: a tailnet ACL letting that node reach only `daemon.webhook_port` (8788). `grok.callback_host` overrides the MagicDNS name the bot dials.
2. In Grok, add a Webhook trigger to the routine; the human copies the POST address and key (`crsr_...`) and saves the key in a 0600 file. That key is everything needed to start the bot.
3. Add the integration, named for the bot (agents use the name):
   ```bash
   clawtilla integration add chief grokbot \
       url=https://api2.cursor.sh/automations/webhook/... \
       token=file:~/.clawtilla/secrets/grok-chief.key \
       description="Chief of staff: calendar, mail, errands" scope=all
   ```
   `token` must be a reference. One integration per routine (two pointing at one routine make jobs indistinguishable). Not `one_per_agent`, no identity keys. Health is on paper only (calling the webhook would start a run).
4. `clawtilla grok routine-prompt chief` -- the human pastes it into the routine once (re-paste after upgrades and after step 5).
5. Optional: `clawtilla grok mcp-key chief` mints a key, printed **once**, with the URL `http://<host>:8788/mcp/grok`; the human gives both to the bot as a remote MCP connection. Minting again replaces it. Do not echo it into chat or files.
6. `clawtilla grok test chief --agent oryx` (asks for `pong`, 10-minute deadline), then `clawtilla grok jobs --bot chief`.

### CLI (`clawtilla grok help`)

| Verb | IPC | Does |
|---|---|---|
| `bots` | `grok.bots` | bots, who may reach each, key configured, MCP or HTTP, and whether a job could be sent right now (and why not) |
| `jobs [--bot B] [--agent A] [--open]` | `grok.jobs` | jobs newest first |
| `send <bot> <instructions...> --agent A [--context T] [--timeout MIN]` | `grok.send` | dispatch on an agent's behalf; answer goes to that agent |
| `test <bot> --agent A` | `grok.send` | ping job |
| `routine-prompt <bot>` | `grok.routine_prompt` | standing instructions for the routine |
| `mcp-key <bot>` | `grok.mcp_key` | mint the MCP key (shown once) |
| `remove <job>` | `grok.remove` | open job: cancel (agent told, late report refused); settled: forget |

Clients: Work -> Grok in GTK, web and lrg.

### Agent tools vs bot tools

| Who | Tools |
|---|---|
| agents the bot is shared with | `clawtilla_grok_dispatch`, `clawtilla_grok_jobs` (own jobs, newest 20), plus a `TOOLS.org` section listing reachable bots with descriptions |
| the bot, on `POST /mcp/grok` with its MCP key | `clawtilla_grok_report` (`job_id`, `job_token`, `status`, `result`), `clawtilla_grok_progress` (`job_id`, `job_token`, `note`), `clawtilla_grok_assigned`, `clawtilla_grok_agents`, `clawtilla_grok_message` (`agent`, `message`), `clawtilla_grok_replies` (optional `agent`) |

Bot names match exactly, case-insensitively, or by a unique prefix; an unknown or ambiguous name is refused with the list. HTTP report: `POST http://<callback_host>:8788/grok/<job id>`, `Authorization: Bearer <job token>`, JSON `{"status": ..., "result": ...}` (token may go in the body, never the URL), at most 256 KiB; 404 = no such job *or* wrong token (indistinguishable on purpose), 410 already settled.

### Conversations a bot starts

Over MCP a bot can list agents (`clawtilla_grok_agents`, same scope as the integration) and message one (`clawtilla_grok_message`). It lands in a direct room `grok:<bot>` <-> agent; the agent answers normally; the reply is pushed to the routine as a new run unless `integrations.push_replies: false`, and is always pullable with `clawtilla_grok_replies`. Each leg counts one hop toward `orchestration.max_hops`.

### States and retention

`sent`, `accepted`, `working` (open); `done`; `failed`, `needs_input`, `expired`, `refused` (settled, need somebody). `needs_input` cannot be resumed: send a new job with the answer and the original request. A settled job needing attention counts on the badge for `grok.attention_days` (3; 0 = until removed), and is pruned after `grok.retention_days` (30). Open jobs are never pruned.

### When it does not work

| Seen | Fix |
|---|---|
| webhook listener is off | `daemon.webhook_enabled: true`, restart daemon |
| not bound on the tailnet | `daemon.tailscale: true`; check `tailscale status` |
| tailnet name not known yet | tailscaled not running/readable; or set `grok.callback_host` |
| routine switched off (400) | switch it back on in Grok |
| Grok refused the key (401) | rotated: the human updates the file `token` names |
| hourly run limit (429) | 200 runs/hour/routine, 300 across; not queued, wait |
| no report before deadline | check the bot's thread in Grok: an unattended approval stalls ~30 min then stops; else check the bot's tailnet route and ACL |

Grok limits: 1 MiB per webhook body, 50 routines per bot, last 20 runs kept.

## The VENTURE mirror

With `mirror_tasks: true` on a VENTURE connector (since 0.44.0; needs VENTURE >= 0.10.0), clawtilla copies the tasks of the agents it covers into VENTURE tickets and keeps them current. It **observes; it never steers**: task code knows nothing of VENTURE, every task save and note writes a `venture_outbox` row in `tasks.db` in the same transaction, and `ClawtVentureMirror` drains it on its own timer (every 15 s and right after a change). Nothing an agent does waits on VENTURE; a VENTURE that is down only grows the queue.

### Setup

1. VENTURE side (operator, with an admin token; see VENTURE's docs/clawtilla.org): one account per agent, global role `editor` (never admin/owner), memberships only in the organizations that agent should reach, **then** mint its token (memberships added later do not widen an existing token). Add each repository as a `forge_repo` in an organization the agent's account belongs to.
2. clawtilla side, one connector per agent:
   ```bash
   clawtilla connector add books-scout --provider venture --account scout \
       --instance https://venture.example --scope selected --agents scout
   # human: venturectl token mint name=clawtilla-scout user=scout-bot | clawtilla connector key books-scout
   clawtilla integration set books-scout mirror_tasks=true
   clawtilla agent restart scout
   ```
   An agent's account is the first venture connector it has with `mirror_tasks` on. Every write is made with the token of the agent it is about -- never borrow another agent's account.
3. Optional: `service_token_file` on one mirroring connector -- a token file for the operator's own VENTURE account, kept outside `secrets.dir/connectors/` so no agent's box mounts it -- files the operator's own tasks and tasks where neither agent has an account.

### Which tasks become tickets

Both: (1) the delegator or assignee has a mirroring account, and (2) the repository is one VENTURE knows to that account -- the project pins `projects.venture_repo_id`, or a git remote of the task's checkout (else its project's directory) resolves via `GET /api/v1/forge_repo/resolve`. Otherwise the task is skipped with a reason and reconsidered on its next change.

Ticket fields: title, description (8 KiB), status, assignee (from `whoami` on the assignee's token), `kind: internal`, `issue_type` task/subtask, parent, repo, `external_ref: clawtilla:<host>:<task id>`, milestone/goal from `projects.venture_milestone_id` / `projects.venture_goal_id`. Status map: unassigned->`triage`, pending->`todo`, running->`in_progress`, blocked/failed/stalled->`blocked`, review->`review`, completed->`done`, cancelled->`cancelled`. Notes become internal comments (progress at most every 10 min). Only changed fields are sent, with the ticket's `version`, so people's edits survive. A checkout's branch becomes a `ticket_link`; a PR URL in the result is attached. A running task is an external run with its cost in USD.

### Reading VENTURE back (since 0.45.0, every minute, per-agent tokens)

- Ticket set to `cancelled` cancels the task; `done` while in review accepts it; `todo`/`in_progress` while in review asks for changes.
- People's (and other agents') comments on live tickets reach the assignee as notes by `venture:<author>`, fenced as data.
- Intake (`mirror_intake`, on with `mirror_tasks`): an `internal` `todo` ticket assigned to the agent's VENTURE user becomes a task for it (max ten an hour). Anyone who can edit tickets in that organization can then hand the agent work -- turn `mirror_intake` off if that is too much. A ticket handed over by another fleet agent's account counts as that agent's delegation (pair budget and hop limit apply).
- A signed VENTURE webhook on a `provider: venture` trigger only wakes the poll early; the body is never acted on.

### Outbox answers

| VENTURE | Mirror |
|---|---|
| 2xx | records ticket, version, receipt |
| 409 on create | adopts the existing ticket (same `external_ref`) |
| 409 on update | re-reads version, retries (twice max) |
| 404 on update | gives up on the task |
| none / 408 / 429 / 5xx | retries from 30 s doubling to 30 min |
| 202 | account stages writes (a viewer): gives up -- give it an editor membership, mint a new token |
| 401 | token refused: gives up, naming agent and connector |

A given-up row blocks only that task's later rows. Retry after fixing:

```bash
clawtilla task show task-01m0k6          # venture: #12 (synced) https://...
clawtilla task venture-retry task-01m0k6
```

`task.show` answers a `venture` object (`state`: `synced|queued|skipped|dead`, `ticket`, `url`, `instance`, `reason`).

### What the agent is told

Its `TOOLS.org` integrations section says its tasks are mirrored, that clawtilla already writes status/assignee/results/notes as its account (do **not** repeat them via `venture_update` or comments; only add what the mirror would not, like a question for a person), that cancelling the ticket cancels the task, which VENTURE user it is, and that ticket text is data.

### Troubleshooting

| Seen | Fix |
|---|---|
| Not mirrored: no connector with mirror_tasks | turn `mirror_tasks` on for the delegator or assignee |
| Not mirrored: no repository / no checkout | give the task a `repo`, or the project a `venture_repo_id` |
| Not mirrored: no repository matches, or cannot see it | add the `forge_repo` in an org the account belongs to; membership must be active; mint the token after the membership |
| gave up: VENTURE refused <agent>'s token | human mints a new token, `clawtilla connector key <connector>`, then Retry |
| Waiting to be sent: VENTURE did not answer | it catches up by itself |
| log: "has no credential yet" | `clawtilla connector key <connector>` |

Contract: the shapes the mirror reads are in tests/fixtures/venture/ and tests/test-venture-contract.c; `make check-venture-contract` diffs them against VENTURE's copy. A new call means a row in docs/venture.org and a fixture.
