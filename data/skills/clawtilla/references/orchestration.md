# Orchestration: how agents work together

Source docs: docs/orchestration.org (the long version), AGENTS.md "Orchestration", src/config/clawt-config-schema.c. Every key's type and default is in [config-keys.md](config-keys.md). Sections: roles, choosing the verb, one message one answer, reaching the operator, sessions, rooms, merges, loop safety, turn hygiene, steering, consultations, ack check, cross-talk summaries, decisions, waits, handoffs, incidents, changing settings. Tasks themselves: [tasks-and-projects.md](tasks-and-projects.md). Teams and helpers: [teams-and-ephemeral.md](teams-and-ephemeral.md).

## Roles and who may assign

| Role | Set by | May assign (`clawtilla_delegate`, `_handoff`, `_task_retry`) | Default `session.routing_mode` |
|---|---|---|---|
| chief of staff | `agents.chief_of_staff: true` or `orchestration.chief_of_staff: <id>` (at most one; two claims refuse daemon start) | anybody | `agent` |
| lead | `agents.team: <team>` + `agents.team_role: lead` | members of its own team only | `agent` |
| member | `agents.team_role: member` (default) | nobody (may still message, ask, share rooms with anyone) | `sender-room` (`room` in a group room) |
| helper's parent | spawned it with `clawtilla_ephemeral_spawn` | its own helpers | unchanged |

- Assigning is gated; talking is not. A lead delegating outside its team is refused with "send it to the chief of staff".
- No `tools.delegate` key exists. To stop one agent delegating: `agents.tools.deny: [clawtilla_delegate]`.
- `chief_of_staff` does not grant agent creation; that is `agents.tools.manage_fleet` (dangerous, off). `agents.tools.ephemeral` is on for the chief by default.
- Tools an agent cannot use are not offered at all. Tool list is read at session start: after changing `tools.*` or a role, `clawtilla agent restart <id>`.

## Choosing the verb (for a chief or lead)

| Need | Tool | Notes |
|---|---|---|
| tell somebody something, no answer wanted | `clawtilla_message_agent` with `reply: "none"` | receiver's closing text goes nowhere; you are not woken |
| one answer to a question | `clawtilla_message_agent` (`agent_id`, `body`) or `clawtilla_ask_agent` (`agent_id`, `message`) | answer arrives as a new turn; END YOUR TURN, do not sleep/poll the room |
| an opinion that must not turn into work | `clawtilla_ask_agent` `consult: true`, or `clawtilla_delegate` `mode: "consult"` | consulted turn gets no tool that acts |
| work that takes a while | `clawtilla_delegate` (`agent_id`, `task`, optional `reason`, `model`, `thread`, `mode`, `after`, `review`, `repo`, `base`, `project`) | returns a task id now; settle notice comes back to the room you delegated from |
| work you own that belongs to someone else | `clawtilla_handoff` (`task_id`, `agent_id`, `reason` required) | runs at your turn end; ownership moves |
| separable work, nobody suitable/free | `clawtilla_ephemeral_spawn` then `clawtilla_delegate` | reap when done |
| a human choice that changes the work | `clawtilla_ask_decision` (with a `default`) | keep going on the default |
| "tell me when X" | `clawtilla_wait_until` | costs no turns while pending |
| reach the operator | `clawtilla_message_user` | replying in a peer room does not reach them |
| who is here / free | `clawtilla_list_agents` (shows mid-turn), `clawtilla_list_teams` (read descriptions), `clawtilla_get_agent` | `running` is not `available` |
| what is my work doing | `clawtilla_task_list` (Delegated by you / Assigned to you / Handed on further; `include_finished`) | `agent_id` narrows to counterparties, does not search the fleet |
| is fan-out affordable | `clawtilla_fleet_cost` | all-time only |

Rules for delegation text: the assignee shares none of your context, under any routing mode. Put every constraint, scope limit and "do not" into `task`. Write team descriptions (`teams[].description`) and agent descriptions for the chief: what a team handles and does not.

## One message, one answer

An AI CLI cannot end a turn without writing text, so routing decides whether that closing text goes anywhere:

| Turn started by | Its closing text |
|---|---|
| a person, routine, webhook/trigger | delivered |
| a peer's `clawtilla_message_agent` / `ask_agent` | delivered once, closes the exchange (unless the turn already messaged them: that message *was* the answer) |
| a message sent with `reply: none` | sent nowhere |
| `clawtilla_delegate` | the assignee's text becomes the task result |
| another agent's answer, a settle notice, a wait/decision notice | sent nowhere; act with tools |

- `invites_reply` rides the stored mailbox item, so it survives a restart. Anything past one answer must be a new deliberate tool call (counted by `max_hops` and the budgets).
- The first message an agent sends the asker from the turn their request started IS the answer; its closing text is then dropped. Sending more afterwards is a new request.
- `orchestration.answer_in_request_room` (default true): the answer wakes the asker in the conversation it asked FROM (your chat, a group, its Matrix room with libreclaw >= 0.30.4), as a clawtilla notice; the answer itself is recorded in the pair's `dm:` room. Off: the answer wakes the asker in the pair room (separate session, none of your context).
- When woken in your chat by an answer: the closing text reaches the operator; do not also call `clawtilla_message_user` (one of the two is dropped).
- Operator's room is exempt: a reply there is always delivered.

## Reaching the operator

- `clawtilla_message_user` posts into the agent's room with the user (supports `attachments`, paths; bytes copied at send time). Agent-to-agent verbs have no attachments: put files in the exchange and name the path.
- Refused during a turn a PEER started (work came down a chain: report to whoever asked, the refusal names them). Allowed after an answer to the chief's own question to a worker, and in any turn a clawtilla notice started (settle notice, wait end) — that is how a chief relays a finished task.
- Text other than the daemon's is escaped: a line beginning `[clawtilla]` from a peer/webhook/tool result reads `[not from clawtilla]`. Only trust genuine notices.

## Sessions (routing mode)

`agents.session.routing_mode` (libreclaw's values): `sender-room` = one context per (room, sender); `room` = one per room; `agent` = one context for everything (serialised turns, nothing lost). Orchestrators default to `agent` so an instruction given in the operator's room is known when a peer's answer arrives. Changing it re-keys every session (fresh contexts once). An explicit `sender-room` on a group-room member is reported and replaced with `room`. Set: `clawtilla agent set <id> session.routing_mode agent`.

## Rooms

- Direct rooms: `dm:<a>:<b>` (pair sorted), made on demand, re-created at start from `<state_dir>/transcripts/*.ndjson`. Read a pair's talk with `clawtilla room history dm:a:b`, or as an agent `clawtilla_room_history` with an agent id. Naming a room is not permission: room history/post/recall/message all check membership.
- An agent's mailbox is EMPTY while it runs (delivery acks at the socket). Never conclude "no reply" from `clawtilla_mailbox_list`; use `clawtilla_room_history`.
- Group rooms (more than two members): `rooms.require_mention` defaults on. A message is always recorded, delivered only to members named (`@id`, case-insensitive; bare word exact; single-word display name counts). `@all` works only from a person/routine/trigger/daemon; from an agent it reaches nobody (refused loudly via `clawtilla_post_room`). Naming nobody is the normal way to take part.
- Rounds: a round starts at a message from a person, routine or trigger (depth reset to 0). Each answering member starts a thread; an `@mention` carries it on. A member answers once per thread; `rooms.max_chain` (default 3, 0 = no cap) caps thread length. Withheld mentions are recorded with `skip_reason` ("Not delivered to @x ..."). `rooms.max_hops` overrides the fleet hop limit for that room (may loosen) and is the backstop behind `max_chain`.
- `rooms.catchup_messages` (20): each delivery carries what the member missed. Pass `turn_room` on clawtilla tool calls when the preamble asks.
- Turn ownership per room: `rooms.turn_timeout_seconds` (0 = off, floor 60, counted in work, pauses on an open decision). Stalls a two-member room; in a group only yields that member.
- One `@all` in a five-member room is four model turns.

## Bursts, merges and held results

- libreclaw runs one turn per delivered message; five messages = five turns. Turn state (depth, origin, replies, task) is per delivery, per room.
- Results (peer answers, settle notices) waiting in one room are merged into one sectioned item (up to 16) and one turn; folded safely (deepest depth, replies if any, task only if all agree, any peer origin). Never merged: a person's message, a question, a one-way notice, a mention room's items, a per-turn model.
- Results for a room where the agent is mid-turn are HELD and handed over together when that room's turn ends ("one wake per parent"). Different rooms are never folded together.

## Loop safety (all `orchestration.*`, 0 = disabled)

| Limit | Default | Counts | Renews | At the limit |
|---|---|---|---|---|
| `max_hops` | 8 | depth per turn (a turn is one depth; drops at a turn no delivery preceded) | new chain | hop tools (`message_agent`, `ask_agent`, `delegate`, `handoff`) NOT OFFERED; room/post/history tools stay; also caps task depth |
| `rate_limit_per_minute` | 30 | messages per sender, sliding 60 s | time | send refused |
| `task_budget_usd` | 5.0 | provider-reported spend booked on the task and every ancestor, so one budget covers a fan-out | never | messages on that task, or on any task in its tree once the root is over, refused: "Stop and report what you have" |
| `tree_message_budget` | 60 | deliberate requests per root task tree (one per call) | any task in the tree settling (before the settle notice) | hop tools withheld from that tree; alert `task.tree_budget` |
| `pair_message_budget` | 12 | rounds of requests between two agents (one per sender turn); groups not counted | a person/routine/trigger writing to either, or `pair_quiet_minutes` (30) of silence | request refused (not withheld; may ask others); alert `crosstalk.pair_budget`; in memory, restart resets |
| `cycle_window` / `cycle_seconds` | 10 / 300 | byte-identical (sender, room, body); also the 6th near-identical (numbers stripped) | window expiry | refused; an agent's exact repeat STALLS the room until a person speaks in it |
| `daily_fleet_budget_micros` / `daily_agent_budget_micros` | 0 / 0 | spend since local midnight | midnight or raised cap | new deliveries wait; see [spending-and-usage.md](spending-and-usage.md) |

- Answers are never counted and never refused by tree or pair budgets. Settle notices and daemon notices pass unmeasured.
- Every refusal publishes `message.refused`. Check order: hops, budget, tree, rate, cycle; nothing counted for a refused message.
- `max_hops` refusal in a room says it is a room (long talk reaches it on its own); raise `rooms.max_hops` for that room rather than the fleet value.
- Change a fleet limit: edit `clawtilla.yaml` (`clawtilla config edit`, or by hand then `clawtilla config reload`); a reload reaches the loop guard. `orchestration.set` (IPC, clients' Orchestration page) sets `tree_message_budget` and `ack_check_model` live.

## Turn hygiene: what ends a stuck exchange

| Loop | What clawtilla does |
|---|---|
| two agents sending the same thing | room stalled, its tasks `stalled`, one alert. Only a person posting in that room clears it |
| a turn producing nothing | watchdog interrupts it (only if `agents.runtime.turn_timeout_seconds` > 0) |
| a member holding a room | yielded (`rooms.turn_timeout_seconds` > 0) |
| one turn calling the same tool repeatedly | lives inside the CLI: counted and noted in the thread at `orchestration.repeat_thresholds` (default `5,10,20`, exact hits only; key = tool name + args), interrupted at the highest; `repeat_max_keys` 256 bounds the table. Empty list disables |
| a task not moving across turns | progress guard: 2 continuations in a row with no successful tool call, or 3 ending in the same error, end the task `stalled` (fixed numbers) |

Watchdog details (`agents.runtime.*`, all off or permissive by default):
- `turn_timeout_seconds` (0 = off) counts ACTIVITY, not duration: a message, a `clawtilla_*` call, or a streamed step. A turn parked on an open decision is exempt. It is also rendered into libreclaw as `session.watchdog_timeout_seconds`, which caps total duration (budget + `busy_max_seconds` while the busy check is on).
- `busy_cpu_percent` (default 5, 0 = off): a process tree using that much of a core counts as alive (long builds/tests). `busy_max_seconds` (7200, 0 = no ceiling) bounds how long CPU alone keeps a silent turn alive. Does nothing for `runtime.type: embedded`.
- `process_timeout_ms` (0 = off): total CLI subprocess limit, rendered as libreclaw `ai.process_timeout_ms`.
- `first_reply_seconds` (20, 0 = never): not a stop. After that long with nothing shown, clients offer Stop or "Try <model> instead" (only `model.fallbacks` on the same provider and login). Needs `runtime.stream_steps` (default from `defaults.stream_steps`).
- A turn timeout fires: repeat counters dropped, turn interrupted, reason in the thread, waiting work `stalled`, grace timer force-releases.

Typing frames are a level re-sent every 25 s; the daemon derives turn edges from a per-room set (`clawt_agent_note_typing()`). Anything new that settles a turn must clear that set (see AGENTS.md).

## Steering into a running turn (`agents.steer`)

| Level | Steers into the running turn |
|---|---|
| `never` | nothing |
| `delegator` (default) | a message from whoever handed out the running turn's work |
| `peers` | as delegator + any other agent's message in the same room |
| `all` | as peers + the operator's own message in their chat with the agent |

Never steered, at any level: into another room's turn; anything in a group room; a held result or a daemon notice (exception: an answer handed back to an asker still mid-turn, at every level but `never`); a delivery naming another task or its own model; a consultation into an acting turn or vice versa (only the consulting agent may steer into a consultation). Engines: Claude Code, claude-tmux, codex on `ai.codex.transport: app-server`; libreclaw >= 0.30.0 advertising `steer`. Otherwise the message waits and becomes the next turn. A steered message is still a hop and passes the loop guard; it earns no separate answer. Without steering, operator messages typed mid-turn are held and delivered as one follow-up turn when the turn settles, and survive an interrupt (press stop after typing the correction).
Set: `clawtilla agent set <id> steer peers`.

## Consultations

`clawtilla_ask_agent` `consult: true` or `clawtilla_delegate` `mode: "consult"`. The consulted turn is offered only `read`-class tools plus `clawtilla_task_complete`; calls to acting tools (and the CLI's own Bash/edit via the `policy.check` hook, on claude-code/claude-tmux) are refused. Per turn, stored on the delivery, survives restart. Only the question is a consultation; the answer never makes the asker's next turn one. `consult` with `reply: none`, or an unknown `mode`, is refused.

## Acknowledgement check

`orchestration.ack_check_model` (empty = off; `provider/model`, an HTTP provider ai-glib reaches, e.g. a small Claude or ollama model). Considered only when the answering turn made no tool calls, its steps were streamed, the answer is <= 600 chars, it answers a peer's deliberate non-task message, not a mention room, not the operator's room. Verdict ACK skips waking the asker (answer recorded as "acknowledged (no wake)", noted in the asker's next delivery); NEEDS or any failure/timeout (8 s) wakes as normal. Set live: clients' Orchestration page (`orchestration.set`), or the file + reload. Worth turning on with `answer_in_request_room` and chatty agents.

## Cross-talk summaries (`orchestration.crosstalk_summary`)

- `chief` (default: only the chief's chat with you), `on`, `off`; per agent `crosstalk_summary`. `crosstalk_summary_to`: `source` (default: back where the request came from, incl. a Matrix room as `m.notice`) or `chat`.
- Covers pair `dm:` rooms, Grok-bot rooms, `task:` rooms, group rounds. Not routines, triggers, incident rooms, your own rooms.
- Ends when no turn runs in the room, nothing waits in a mailbox for it, no carried task is unfinished, and 15 s quiet; stale after an hour. Built from the record, no model call; recorded with `clawt_mailbox_router_record()` so it never reaches an agent, starts no turn, counts toward no limit. Optional rewrite: `orchestration.crosstalk_summary_model` (10 s, falls back).
- Not posted for an exchange whose answer was already handed back into your chat.
- Per-agent: `clawtilla agent set <id> crosstalk_summary on`.

## Decision inbox

- Agent: `clawtilla_ask_decision` (`question`, `options`, `default` required, `default_reason`, `reversible_hours`, `task_id`). The agent KEEPS WORKING on the default; an answer arrives later as a message and redirects. Bar: file only when branches produce materially different work or the choice is not cheaply reversible.
- Inside delegated work the decision is routed to the delegating agent first (`routed_to`); it answers with `clawtilla_decision_answer` (`decision_id`, `answer`). The operator sees it too and the first answer wins.
- Operator: `clawtilla decision list [--all]`, `clawtilla decision answer <id> "<text>"` (free text), `clawtilla decision dismiss <id>`, `clawtilla decision delete <id>`; Decisions page; inline in the chat it was asked from. Outcomes stay distinct: answered, defaulted, dismissed. Answering twice is refused.
- Policy asks (action rules, see [security.md](security.md)) and review verdicts (`review-<task>`) land in the same inbox.

## Waits (`clawtilla_wait_until` / `_list` / `_cancel`)

| `kind` | Condition | Polled |
|---|---|---|
| `url` | `url` answers `status` (2xx default), optional `contains`; public addresses only, no credentials; not offered to `runtime.network` gated/none agents | 60 s, backing off to 15 min |
| `file` | `path` in workspace exists, or `changed: true` | 15 s |
| `task` | `task_id` settles | 15 s |
| `computer` | the agent's computer runs `true` | 60 s |
| `command` | `command` on its computer exits `exit_status` (no shell syntax) | 60 s |

`label` in the operator's words, `hours` default 24, max 168, optional `note`. A pending wait costs nothing; the agent is woken once with a closed-exchange notice into the room it set it from. Limit `orchestration.max_waits_per_agent` (10; per agent `max_waits`). Files: `<state_dir>/waits/<id>.json`. Operator: `clawtilla wait list [agent]`, `clawtilla wait cancel <id>`. Prefer a wait over a routine that polls.

## Handoffs (`clawtilla_handoff`)

Moves an existing task and its responsibility; delegation creates new work and keeps you responsible. Offered like assignment. Queued; runs at the caller's turn end (`clawt_daemon_turn_settle()`), everything re-read first; an interrupt drops the queue (`dropped`). Receipts in `<state_dir>/handoffs.db`, read via `clawtilla_task_status`: `done`, `busy_gave_up` (still yours), `denied` (team rule), `dropped`, `failed` (usually hop limit), `error`. Limits: `orchestration.handoff_max_per_turn` (4 waiting), `handoff_busy_retries` (3 turn boundaries, not seconds), `handoff_receipt_days` (2).

## Incidents: broken runs reach the lead

- Opened for a task that `failed` or `stalled`, an `unstartable` agent with work waiting, a `routine-failed` run. One per run (repeat failures count up: "broke 3 times"); resolved when the task completes or is cancelled. An agent crash fails its `running` tasks (pending stay queued). Cancels and `agent stop` are not incidents (delegators still get settle notices).
- `orchestration.incidents.enabled` (true); `orchestration.incidents.to`: `lead` (default; chief if no/two leads or no team), `chief`, `none` (recorded only). Never the failing agent, never the delegator.
- Filed in room `incidents:<team>` (or `incidents:+fleet`), "Problems to look at". Report is from clawtilla, depth 0, invites no reply, no task id; held while the lead is mid-turn. Log lines fenced as untrusted data.
- Retry: agent `clawtilla_task_retry` (`incident_id` or `task_id`, optional `reason`), only for runs it could assign, at most twice per incident; then tell the operator. Operator: `clawtilla task incidents`, `clawtilla task retry <task-id|incident-id> [reason]` (not counted). Only `failed`/`stalled` are retryable; same task id is reopened.

## Changing orchestration settings: what each needs

| Change | How | Takes effect |
|---|---|---|
| fleet `orchestration.*` limits | edit `clawtilla.yaml` (`clawtilla config edit`) or the clients' Orchestration page | `clawtilla config reload` (edit reloads itself); `tree_message_budget` and `ack_check_model` via `orchestration.set` at once |
| per-agent override (`crosstalk_summary`, `crosstalk_summary_to`, `max_waits`, `daily_agent_budget_micros`, `mailbox.*`) | `clawtilla agent set <id> <key> <value>` | `agent set` replies `restart_required` when a restart is needed |
| `tools.*`, role (`chief_of_staff`, `team_role`), `tools.ephemeral` | `clawtilla agent set` | `clawtilla agent restart <id>` (tool list read at session start) |
| identity/persona text | edit persona files | `clawtilla agent reset <id>` (a resumed CLI is not re-handed its system prompt) |
| `session.routing_mode` | `clawtilla agent set <id> session.routing_mode agent` | re-keys sessions; each conversation starts fresh once |
| `rooms.max_chain`, `rooms.max_hops` | file or clients' Orchestration page | reload |

Diagnose with `clawtilla event list` (`message.refused`, `task.tree_budget`, `crosstalk.pair_budget`, `turn.timed_out`), `clawtilla room history <room>`, `clawtilla activity`, `clawtilla task list`.
