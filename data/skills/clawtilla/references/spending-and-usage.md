# Spending, budgets and provider plan limits

Source docs: docs/spending.org, docs/provider-usage.org, docs/tasks.org (project budgets), src/core/clawt-daemon.c (`charge_turn_to_task()`). Key defaults: [config-keys.md](config-keys.md). Two different questions: *what did it cost* (spend, below) and *how long can it keep going* (plan allowances, further down). On a subscription CLI the second is what stops the fleet.

## Where the cost figure comes from

- libreclaw writes one `token_usage` row per AI turn into `<state_dir>/agents/<id>/sessions/libreclaw.db` (inside `sessions/`: libreclaw ignores `database.path`). Path helper: `clawt_usage_database_path()`.
- Cost is the provider's own per-turn figure (`total_cost_usd` from Claude Code, claude-tmux, Grok build), stored as integer micro-dollars. Only a value >= 0 is believed.
- `-1` = unknown, never free. A turn that moved tokens and recorded `0` (libreclaw had no price table) is *unpriced*: counted in TURNS and UNPRICED, kept out of every cost sum, shown as `unpriced` (empty cell in CSV).
- IN counts only NEW input tokens; cached context is billed but not reported as tokens. Trust the cost column, not the token columns.
- Origin tags live beside it in `sessions/clawtilla-ledger.db` (`turn_origin`). `/reset` (`clawtilla agent reset`) moves `sessions/` aside as `sessions.reset-<timestamp>`, so per-agent history appears to reset (rows still on disk).
- A never-run agent costs nothing and no database is created to say so.

## `clawtilla cost`

```
clawtilla cost                       # all time, per agent, most expensive first, LAST TURN, IDLE
clawtilla cost --today               # since local midnight
clawtilla cost --days 7
clawtilla cost --agent chief         # one agent, fleet line still printed for scale
clawtilla cost --by-day              # + fleet cost for each of the last 14 days (or --days N)
clawtilla cost --by agent|model|origin|day [--csv] [--today | --days N] [--agent ID]
clawtilla cost --by day --csv > spending.csv
```

- Origins (`clawt_usage_origin_classify()`): `routine:<id>` and `trigger:<id>` (including work they delegated on), `operator`, `agent:<id>`, `task:<id>`, `channel:<name>` (the agent's own Matrix/email channel), `other:<sender>` (e.g. a Grok bot), `untagged`. Run-to-task mapping survives restarts in `<state_dir>/task-sources.tsv`.
- CSV: RFC 4180, CRLF, exact six-place dollars, no total line, formula-looking cells prefixed with `'`.
- Sub-cent non-zero costs keep four decimals.
- IDLE uses `orchestration.idle_hours` (72; a label only). `agents.runtime.idle_stop_hours` (0 = off) actually stops an agent with no turn for that long; the next queued message wakes it (`agent.idle_stopped` event).
- IPC: `usage.summary`, `usage.ledger`. Clients: Settings → Spending (period, Group by, Export CSV) and Work → Fleet (ranked cost, 14-day strip, idle agents with Stop, the fleet cap beside today's spend).
- Agents: `clawtilla_fleet_cost` (per agent + the task budget). ALL-TIME only; will not match a windowed CLI figure. A chief should check it before a wide fan-out.
- Hand query: `sqlite3 <state_dir>/agents/<id>/sessions/libreclaw.db "SELECT date(recorded_at, 'unixepoch') AS day, count(*), sum(cost_usd_micros)/1000000.0 FROM token_usage GROUP BY day ORDER BY day DESC;"` (single quotes inside, double outside). Read-only; never write to it.

## Budgets: three independent guards

| Guard | Key | Default | Unit | Scope | At the limit |
|---|---|---|---|---|---|
| task budget | `orchestration.task_budget_usd` | 5.0 | USD (double) | one task id | messages carrying that task id refused (`CLAWT_ERROR_LOOP_LIMIT`): "this task has spent $X, which is its budget of $Y. Stop and report what you have." |
| daily fleet cap | `orchestration.daily_fleet_budget_micros` | 0 (off) | integer micro-USD (1000000 = $1) | configured fleet, since local midnight | router refuses to queue/lease new deliveries until midnight or a raised cap |
| daily agent cap | `orchestration.daily_agent_budget_micros`; per agent `daily_agent_budget_micros` (0 disables that agent's cap) | 0 (off) | integer micro-USD | one agent | same, for that agent; the fleet cap still applies |
| project budget | `projects[].monthly_budget_usd` + `budget_warn_percent` (80) + `budget_hard_stop` (false) | 0 (no budget) | USD | tasks made this calendar month in the project | warn once a month (`budget` notification); with hard stop, NEW work in the project refused `LOOP_LIMIT` ([tasks-and-projects.md](tasks-and-projects.md)) |

Task budget facts:
- Test is spend >= budget (fires AT the figure). 0 disables it. `config show` prints the file as written: no `task_budget_usd` line means the $5 default applies.
- Charged flat on the task id when the agent answers (each reply drains what that agent spent since its previous turn). No walk to the parent: a chief's ten subtasks each have their own full budget, so it bounds one agent's work on one assignment, not a fan-out in dollars (the schema text "and everything it spawns" overstates it). `orchestration.tree_message_budget` bounds fan-out in messages ([orchestration.md](orchestration.md)).
- Ordinary chat has no task id and is outside the task budget; the daily caps cover it.
- First drain on an agent after daemon start sets a watermark and charges nothing; restored tasks' spend is reseeded from the ledgers, so the budget keeps counting across restarts. Row ids prevent double charging.

Daily cap facts:
- Admission limits, not billing ceilings: no reservation for in-flight work, a running turn can overshoot, internal model/tool iterations are not interrupted. Checked when work is queued and again before leasing each pending delivery; queued work waits and drains after midnight or a raised cap. Transcript-only notes still work.
- Uses persisted usage since local midnight (DST-aware), including `sessions.reset-*` archives, so resetting a conversation does not restore today's allowance. Restart does not reset it. Unreadable existing databases refuse admission; missing ones count as zero. Removed agents drop out of the fleet total.
- First block per scope per day publishes `spending.limit-reached` (subject `fleet` or `agent:<id>`, `cost_micros`, `limit_micros`): an alerts-drawer notice in every client and a `budget` event for notify integrations. See `clawtilla event list`.
- Do not add a second daily budget key: `daily_fleet_budget_micros` is the cap (AGENTS.md "Grep the schema for the concept").

Set them:
```
$EDITOR clawtilla.yaml   # or: clawtilla config edit (reloads itself)
#   orchestration: { task_budget_usd: 2.0, daily_fleet_budget_micros: 20000000, daily_agent_budget_micros: 5000000 }
clawtilla config reload
clawtilla agent set researcher daily_agent_budget_micros 8000000
clawtilla project set shop monthly_budget_usd 40
clawtilla project set shop budget_hard_stop true
```

## Plan allowances: `clawtilla limits`

A *login* is one CLI signed in one way: each `accounts:` entry, plus the default login (`<cli>:default`, under the daemon's `$HOME`) of every CLI some agent uses without `agents.model.account`. Agents sharing a login share one allowance, so rows are logins (`claude-code:work`), never agents. Do not add rows together.

| CLI | Read through | Windows |
|---|---|---|
| Claude Code (`claude-code`, `claude-tmux`) | stream-json `get_usage` (older CLIs fall back to the `/usage` panel, needs `tmux` and a trusted workspace) | Session (5h), Weekly, per-model weekly caps |
| Codex (`codex-cli`) | app-server `account/rateLimits/read` | every named pool |
| Grok CLI (`grok-build`) | ACP `_x.ai/billing` | allowance %, dollar amounts/balances when present |
| Antigravity | its `/usage` panel in a private tmux; marked *partial* | as shown |
| OpenCode, Cursor, ACP | nothing | "Reports no plan limits" |

```
clawtilla limits             # asks for a refresh, waits up to a minute, prints meters
clawtilla limits --cached    # what the daemon already knows, no new read
clawtilla limits --json      # the usage.providers reply verbatim (has `source`, `partial`)
```

- Reads send no prompt and spend no allowance, but spawn a CLI: never at daemon start, at most once a minute per login (`usage.providers` with `refresh: true`), results announced as `usage.providers_changed`. Last good reading persisted in `<state_dir>/usage/providers.json` (0600) and restored on restart with its original time; failures are not stored.
- Meters always show what is LEFT (full bar = good). Used-percent sources are converted (`clawt_provider_usage_normalize()`); a percentage with no stated direction is shown as "40% (direction not reported)" with no meter and no colour.
- Levels (`clawt_usage_window_level()`, colour via `clawt_allowance_level_tone()`): >25% left = plenty (green); <=25% = low (amber, login badged "running low"); <=10% = critical (red, "nearly out"); unknown = neutral, never green. A login's badge is its worst window. Unknown is never shown as zero.
- Status lines: Not read yet / Asking the provider… / Read 3m ago[, refreshing] / partial / "Reports no plan limits: …" (an answer: API-key, Bedrock or Vertex login) / "Could not be read: …" (last good figures kept).
- Separate subscription, own row: `clawtilla account add work claude-code ~/.claude-work`, `clawtilla account login work`, `clawtilla agent set <agent> model.account work`. Failover when a plan runs out is libreclaw's (`agents.model.fallbacks`, see [agents.md](agents.md)).

## Tones and labels clients share

Never re-derive these in a client: `clawt_usage_window_level()` / `clawt_allowance_level_tone()` (meter colour), `clawt_usage_login_level()` (badge), `clawt_usage_window_value_label()` ("62% left"), `clawt_usage_window_reset_label()` ("resets in 2h 41m", from the client's clock), `clawt_usage_meter_text()` (CLI bar), `clawt_usage_idle_label()` (IDLE column), `clawt_usage_turn_is_priced()` (unpriced rule).

## Troubleshooting

| Symptom | Check |
|---|---|
| every agent reports $0 | `ls <state_dir>/agents/*/sessions/libreclaw.db`; tool-only turns are not recorded; a locked/unreadable DB counts as zero (`G_MESSAGES_DEBUG=Clawtilla` on the daemon shows it) |
| numbers reset after `/reset` | expected; old rows in `sessions.reset-<timestamp>` |
| task budget never fires | is it 0 in the file? is the work actually a task (chat has no task id)? |
| fleet quietly stopped answering | `clawtilla event list` for `spending.limit-reached`; raise the cap or wait for midnight |
| a login missing from limits | no agent uses it and it is not under `accounts:` |
| Claude login "permission denied" | old CLI fell back to the panel behind a trust prompt: update Claude Code or trust the daemon's working directory once |
| "Reports no plan limits" on a subscription | CLI signed in with an API key/Bedrock/Vertex; check its `/status` as the daemon's user |
