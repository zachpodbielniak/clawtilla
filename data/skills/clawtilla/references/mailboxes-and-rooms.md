# Mailboxes, rooms and messages

Sources: `docs/mailboxes.org`, `docs/orchestration.org` ("Rooms", "Group rooms", "Waits", "The decision inbox", "Sending a file"), `docs/clients.org` ("Notifications"), `clients/cli/main.c`. Loop limits, peer-messaging rules and consultations: [orchestration.md](orchestration.md). Tasks: [tasks-and-projects.md](tasks-and-projects.md). Keys: [config-keys.md](config-keys.md).

## Message flow in one line

client or agent -> `ClawtIpcServer` -> `ClawtMailboxRouter` (loop safety **before** enqueue) -> each recipient's `ClawtMailbox` -> drained into its `ClawtLink` when the agent is running -> libreclaw's clawtilla channel -> one turn per message.

## Mailboxes

Durable SQLite queue per agent at `<state_dir>/agents/<id>/mailbox.db` (WAL, 0600, daemon-only writer). You can message a stopped agent; a killed daemon loses at most the turn in flight.

**A running agent's mailbox is almost always empty.** Delivery acknowledges at the socket and arrives as a turn; the queue holds only what piled up while the agent was stopped, paused or backing off. Depth 0 is not "nothing was sent". What was said is in the room transcript (`clawtilla room history`, agent tool `clawtilla_room_history` with a room id *or* a peer agent id) and the event log (`clawtilla event list`). Agents must not look in `clawtilla_mailbox_list` for a peer's reply.

Item fields that matter: `from`, `to`, `room`, `task_id`, `reply_to`, `priority` (`low`/`normal`/`high`/`urgent`; urgent jumps everything; FIFO within a band; an unknown nickname is refused, not defaulted), `depth` (hops), `not_before` (set only by retry backoff), `expires_at`, `state`, `attempts`, `last_error`, `invites_reply`, `one_way`, `room_round`, `chain`, `model`, `consult`. States actually reached: `pending` -> `leased` -> `acked` or `dead` (`delivered`/`failed` are never written; seeing one is a bug).

| Behaviour | Rule |
|---|---|
| Lease | `attempts` counts at lease time, so a crash-looping message dead-letters instead of looping. Lease expiry returns it to `pending`. |
| Retry | backoff `backoff_seconds` << attempts (cap 6 shifts, ~32 min at default 30) + up to 25% jitter. `backoff_seconds: 0` means zero. |
| Dead letters | after `max_attempts` (5; 0 = never dead-letter). Never swept. |
| TTL | `default_ttl_seconds` (604800 = a week) applied to every item; 0 = live until delivered/dead. Leased items never expire mid-turn. |
| Sweep | once a minute: expire, reclaim abandoned leases, reap `acked` rows older than 24 h. |
| Backpressure | depth = pending + leased vs `max_depth` (1000). `overflow`: `reject` (`mailbox-full`, do not retry), `drop-oldest` (lowest band first, logged), `block-sender` (`mailbox-busy`, retry later; never blocks a thread). A full member does not fail a room post to the others. |
| Idempotency | supported by the store, unused by the router: duplicate sends are duplicate items. |
| Corruption | an unopenable `mailbox.db` is renamed `mailbox.db.corrupt-<unix-time>` and recreated (its messages are lost to the queue, not the agent). If it still cannot be opened the agent becomes a shadow ("its mailbox could not be opened"). |
| Paused account | an agent out of session allowance is deliberately not drained (draining would consume the queue unanswered). |

Fleet keys `orchestration.mailbox.{max_depth,overflow,max_attempts,lease_seconds,default_ttl_seconds,backoff_seconds}`; per agent the same without `orchestration.` (`mailbox.max_depth: 50` inside the agent).

```bash
clawtilla mailbox list AGENT          # pending items + queued follow-ups, "N waiting for delivery"
clawtilla mailbox dead AGENT          # dead letters with last_error
clawtilla mailbox requeue AGENT ITEM  # resets attempts, not_before, last_error
clawtilla mailbox ack AGENT ITEM      # refused unless the item is leased
clawtilla mailbox purge AGENT         # drop expired items now
clawtilla pause [AGENT] ; clawtilla resume [AGENT]   # hold delivery fleet-wide or per agent; turns in flight finish
```

Stuck-queue checklist (in order): anything queued? (`mailbox list`; 0 usually means delivered) -> connected? (`agent list` LINK up; else `config render AGENT`, `agent logs AGENT`) -> account out of allowance? (`agent logs`) -> deferred? (future `not_before`, read `attempts`/`last_error`) -> leased forever? (sweep returns it after `lease_seconds`, 300) -> dead? (`mailbox dead`) -> full? (symptom is at the sender). A stopped agent with QUEUE > 0 under `restart: on-failure` is stalled, not idle: start it, or set `runtime.restart: always` / `runtime.idle_stop_hours`.

**Queued follow-ups** are different: a message you send to an agent that is mid-turn in your conversation is held in daemon memory and delivered as one follow-up turn when that turn settles (unless `agents.steer: all` lets it join the running turn). `send` prints `Held: ... is mid-turn`. They appear in `mailbox list` (`follow_ups`), survive an interrupt, **not** a daemon restart, and have no ack/requeue.

**Turn records** (`mailbox.db`): every delivery is recorded until its turn ends. After a crash or daemon restart, `clawt_daemon_turns_recover()` reruns a cut turn **once** if nothing outward is known of it (prefixed with a `[clawtilla]` rerun line), otherwise posts an *interrupted* notice. Outward = `clawtilla_message_user`, exec on host/distrobox, external/desktop-input/exec-class tools, connector writes. Peer messages are inward. `agent stop` (and an approved rebuild) ends turns as interrupted with no rerun; `agent restart` reruns.

## Rooms

Agents are managed as chats; every conversation is a room. Transcripts persist per message to `<state_dir>/transcripts/<room>.ndjson` and are kept when a room is removed.

| Kind | Id | Notes |
|---|---|---|
| direct | `dm:<a>:<b>`, pair sorted (`dm:chief:user` is the operator's chat with `chief`) | made on demand, recreated at start from transcript files. Agent-to-agent chats do not appear in your chat with either; read them in Flow or `clawtilla room history dm:a:b`. Naming a room is not permission: tools refuse non-members. |
| group | configured `rooms:` or `room create` | >2 members; mention rules and rounds below |

```yaml
rooms:
  - id: standup
    name: Standup
    members: [chief-of-staff, researcher, writer]   # the human is always implicitly a member
    require_mention: true      # default by shape: off for 2 members, on for 3+
    max_hops: 12               # overrides orchestration.max_hops (8) for this room, up or down
    max_chain: 3               # replies per thread per round; 0 = no cap
    turn_timeout_seconds: 0    # per-member budget; 0 = off
    catchup_messages: 20       # missed lines included when a member is named
    team: research             # sidebar placement only
    order: 10
```

```bash
clawtilla room list                     # id, message count, members
clawtilla room create ROOM [--members a,b]
clawtilla room add ROOM AGENT
clawtilla room history ROOM             # or dm:a:b
clawtilla room read AGENT|ROOM ; clawtilla room read --all     # clear unread in every client
```

Group rules (`docs/orchestration.org`, "Group rooms"):
- Every message is recorded; it is **delivered only to the members it names**. Naming nobody is the ordinary case and costs nothing.
- `@id` (case-insensitive, at a word boundary) or the exact bare id; a one-word display name counts. `zach@bob.com` does not mention `bob`. No markdown awareness (code fences still match).
- `@all` from a person, routine, trigger or the daemon reaches everyone; from an agent it reaches nobody (`clawtilla_post_room` refuses it). One `@all` in a 5-member room = 4 model turns.
- A **round** starts at each message from a person/routine/trigger (depth reset to 0, "Round N" divider). Within a thread a member answers once; a thread runs at most `max_chain` replies. A withheld mention is recorded with `skipped`/`skip_reason`, drawn under the message. `max_hops` remains the backstop.
- Group members default to `session.routing_mode: room` (an explicit `sender-room` is replaced); orchestrators keep `agent`.
- `turn_timeout_seconds` stops a member's turn and notes it; only a two-member room is *stalled* (needs a person to speak to clear it).
- Each delivery's preamble names the room, members and round, and asks the agent to pass `turn_room` on clawtilla tool calls.

## Sending

```bash
clawtilla send AGENT-OR-ROOM message words...     # IPC msg.send; quoting optional
clawtilla chat AGENT                              # interactive; Ctrl-D leaves
```

`send` reports where it went: `Queued for N recipient(s)`; `Queued: X is stopped, so it is held in the mailbox until the agent starts`; `Posted, and nobody was interrupted` (room message naming no member -- write `@id` or `@all`); `Held: X is mid-turn` (follow-up). Agents send with `clawtilla_message_agent` (optional `priority`, `reply: none` for one-way), `clawtilla_ask_agent`, `clawtilla_post_room`, `clawtilla_mailbox_reply`, and reach you with `clawtilla_message_user` (refused during a peer-started turn). A deliberate message earns one answer; the answer earns none (`invites_reply`). Details and budgets: [orchestration.md](orchestration.md).

## Attachments

- Agent -> person: `clawtilla_message_user` takes `attachments` (paths the agent can read). The daemon copies the bytes at send time into its state dir (not the exchange); an unreadable file is reported, not dropped.
- Agent -> agent: **no attachments argument**. Put the file in the exchange (`/mnt/clawtilla/exchange/shared/` or the sender's own dir) and name the path. See [computers.md](computers.md).
- Clients fetch with IPC `attachment.get`; the CLI:

```bash
clawtilla attachment list AGENT|ROOM
clawtilla attachment get ID [-o FILE|-]
```

## Notices (the bell) and mute

The daemon turns error/notice-tier events into rows in `<state_dir>/notices.db`, worded once for every client. Bell = unread conversations + decisions waiting + Grok jobs needing attention + unread alerts, minus muted agents. Pruned by `notices.keep` (500) and `notices.keep_days` (30).

```bash
clawtilla notices [--agent A] [--all]       # --all includes dismissed
clawtilla notices read ID|--all ; clawtilla notices dismiss ID ; clawtilla notices clear [--agent A]
clawtilla mute AGENT [--for 90s|30m|2h|3d] ; clawtilla unmute AGENT
```

Mute: alerts arrive already read, the agent leaves every count, push/desktop notifiers skip it. It does not stop the agent or hold its messages. Stored in `notices.db` (survives restart).

## Daemon notices in a room

Messages from sender `clawtilla` (settled tasks, wait endings, interrupted turns, rebuild outcomes, incident reports) are the daemon's voice: depth 0, `invites_reply` false, quoted text escaped with `clawt_preamble_escape()` so nothing quoted can pose as `[clawtilla]`. A turn started by one sends no closing text anywhere.

## Decisions and waits (overview)

- **Decisions**: `clawtilla_ask_decision` files a choice with a required default and optional reversibility window (hours) and the agent keeps going. Inside delegated work it goes to the delegating agent first (`clawtilla_decision_answer`). Lapsed ones settle as *defaulted* every 15 s (`decision.settled`). Policy asks have ids `policy-...`; rebuild requests `rebuild-...`.

```bash
clawtilla decision list [--all] ; clawtilla decision answer ID ANSWER
clawtilla decision dismiss ID ; clawtilla decision delete ID
```

- **Waits**: `clawtilla_wait_until` (kinds `url`, `file`, `task`, `computer`, `command`; `hours` default 24, max 168), `clawtilla_wait_list`, `clawtilla_wait_cancel`. The daemon looks, not the model; the ending is exactly one mailbox item into the room it was set from. Limit `orchestration.max_waits_per_agent` (10; `max_waits` per agent). URL waits refuse private addresses and are not offered to `gated`/`none` agents. Stored in `<state_dir>/waits/<id>.json`.

```bash
clawtilla wait list [AGENT] ; clawtilla wait cancel ID
```
