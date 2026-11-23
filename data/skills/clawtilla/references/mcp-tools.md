# clawtilla MCP tools (1/2): how tools reach an agent, messaging, tasks, decisions

Every `clawtilla_*` orchestration tool registered in the `tools[]` table of
src/mcp/clawt-mcp-tools.c (98 names). This file covers the mechanics, the
permission gates, messaging, rooms, mailbox, delegation and tasks, and
decisions. [mcp-tools-2.md](mcp-tools-2.md) covers fleet management,
ephemeral helpers, routines and waits, memories, skills, computers, the
browser, web research, connectors, Grok and the office.

`scripts/list-mcp-tools.sh [REPO_ROOT]` prints the registered names, sorted;
diff it against these two files when a tool is added.

## How a tool reaches an agent

- The daemon writes `<workspace>/.mcp.json` with a `clawtilla` server entry:
  `clawtilla-mcp-server --agent ID --socket SOCK --token-file STATE/token`
  (src/agent/clawt-workspace.c). The agent's CLI discovers it the way it finds
  `CLAUDE.md`; libreclaw translates it for codex, grok and opencode.
  clawtilla owns the `clawtilla` key and every `clawtilla-*` key in that file;
  other entries are the operator's.
- `clawtilla-mcp-server` forwards each call to the daemon as a `tool.rpc`
  frame. The daemon decides the list (`clawt_mcp_tools_is_permitted()`) and
  runs the call. A tool the agent cannot use is **not offered at all** rather
  than offered and refused.
- The managed region `# BEGIN clawtilla tools` in the agent's `TOOLS.org` is
  generated from the same gate (`clawt_mcp_tools_describe_for_agent()`) and is
  the authoritative list for that agent. Anything else in TOOLS.org that
  names tools is advice, not an enumeration. `clawtilla_skill_list` and
  `tools/list` reflect live state.
- Plugins can add tools of their own (see [plugins.md](plugins.md)); they need
  no capability but still pass `tools.allow` / `tools.deny`, and are classed
  `external` for action rules.

## The gates

Checked in this order: capability (below), then the hop gate, then
`tools.deny` (always wins), then `tools.allow` (unset = everything that
passed). Agent-relative keys are written as an agent sees them; the schema
spells them `agents.<key>` (see [config-keys.md](config-keys.md)).

| Gate | Offered when |
|---|---|
| none | always (the daemon is present) |
| peer | the agent has peer comms -- every fleet agent does |
| assign | `chief_of_staff` is true, or `team_role` is lead (plus peer) |
| handoff | assign, and the daemon's handoff queue exists |
| retry | assign, and the incident desk exists |
| decision-answer | assign, and the daemon can take an answer |
| decision-inbox | the daemon has a decision inbox |
| fleet-admin | `tools.manage_fleet` true (default false, DANGEROUS) |
| ephemeral-spawn | `tools.ephemeral` true and the agent is not itself a helper |
| ephemeral-parent | `tools.ephemeral` true, or the agent still has helpers |
| office | `tools.office` true and not a helper |
| schedule | `tools.schedule` true (default true) and the routine store exists |
| waits | the wait manager exists |
| memory | the agent has a memory store (`memories.enabled`) |
| recall | `memories.recall` true and the transcript index exists |
| skills | at least one skill binding is active for the agent |
| skill-proposal | a skill library exists and `skills.max_proposals_per_day` > 0 |
| computer | the agent's computer object is not `none` (`computer.type`) |
| display | `computer.type` is `display` |
| display-input | display, and `computer.display.allow_input` |
| display-spawn | display, and `computer.display.allow_spawn` |
| screen | a screen: `computer.type: display` or `computer.desktop.enabled` |
| rebuild | `computer.type` is a machine clawtilla builds (container, vm, ...) |
| browser | `browser.mode` is not `none` (needs a `GSURF=1` build) |
| browser-scripts | browser, and `browser.allow_scripts` |
| browser-secrets | browser, and `browser.secrets` names at least one entry |
| web | a `libresearch` integration resolves for the agent |
| grok | a `grokbot` integration is shared with the agent |
| connector | a `connector` integration is bound to the agent |
| never | never offered to an agent (the Grok bot's side) |

**Hop tools** (marked *hop* below) are also withheld when the turn's hop depth
is already `>= orchestration.max_hops`, or the root task's
`orchestration.tree_message_budget` is spent. The router still refuses a call
that arrives anyway.

**Action class** (`read`, `write`, `delete`, `message`, `exec`, `external`,
`desktop_input`, `fleet_admin`) is what `policy.rules` (`MATCH=MODE`, first
match wins; defaults from `defaults.policy.rules`) can name instead of a tool.
See [security.md](security.md).

## Per-turn rules

- **One message, one answer.** Between agents it is a request and a single
  reply; the exchange ends there. Say everything in one message. Use
  `reply: none` for a notice so the recipient's sign-off does not wake you.
  `orchestration.pair_message_budget` caps rounds per pair. See
  [orchestration.md](orchestration.md).
- **`clawtilla_message_user` is refused during a peer-started turn.** Work
  handed down a chain comes back up it; answer whoever asked.
- **Consultations withhold acting tools.** In a turn started by
  `clawtilla_ask_agent consult: true` or `clawtilla_delegate mode: consult`,
  every tool whose class is not `read` is denied, except
  `clawtilla_task_complete` (how the answer is reported).
- **Read-only routine runs** deny every non-`read` tool; report with
  `clawtilla_ask_decision` instead.
- **`turn_room`**: an agent can have a turn running in several rooms at once.
  Tools that depend on turn state accept `turn_room`, copied from the
  `[clawtilla]` line of the message being handled. Only needed with more than
  one conversation going; it is not where the tool acts, and is believed only
  while a turn is running in that room.
- Outward calls (`clawtilla_message_user`, `exec` on a host or distrobox,
  `external`, `desktop_input`) mark the turn, so a restart does not rerun it.

## Granting a permission

```bash
clawtilla agent set ID tools.manage_fleet true
clawtilla agent set ID tools.office true
clawtilla agent restart ID
```

- `agent.set` re-renders TOOLS.org at once and replies `restart_required`
  for any `tools.*` key (also `chief_of_staff`, `team_role`) when the agent is
  running: the CLI reads its tool list at session start, so restart it.
- A `computer.*` change marks the kept computer stale; it is rebuilt at the
  next start, never while the machine runs. Until then the computer gates
  answer from the old one; see [computers.md](computers.md). Integrations (web, grok, connector) are
  granted by scoping the integration to the agent; see
  [integrations.md](integrations.md).
- `tools.deny` / `tools.allow` hold tool names. Check the result in the
  agent's TOOLS.org region, not in the config.

## Messaging and rooms

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_message_agent` *hop* | queue a message to an agent (works if stopped) | agent_id*, body*, reply (answer/none), priority (low..urgent), turn_room | peer | message |
| `clawtilla_ask_agent` *hop* | ask a question; answer arrives as a later message | agent_id*, message*, consult, turn_room | peer | message |
| `clawtilla_message_user` | say something to the human operator | body*, attachments, turn_room | none | message |
| `clawtilla_post_room` | post to a room; delivered only to @named members | room_id*, body*, reply, turn_room | peer | message |
| `clawtilla_create_room` | create a room you are a member of | room_id*, members* (comma-separated), name | peer | fleet_admin |
| `clawtilla_room_history` | recent messages of a room, or your DM with an agent id | room_id*, limit | none | read |
| `clawtilla_list_teams` | teams, leads, members, running counts | -- | peer | read |
| `clawtilla_mailbox_list` | messages waiting (empty while running: delivery acks) | limit | none | read |
| `clawtilla_mailbox_read` | read one mailbox message | message_id* | none | read |
| `clawtilla_mailbox_ack` | mark a message dealt with | message_id* | none | write |
| `clawtilla_mailbox_reply` | reply and acknowledge in one step | message_id*, body*, turn_room | peer | message |

See [mailboxes-and-rooms.md](mailboxes-and-rooms.md).

## Delegation and tasks

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_delegate` *hop* | hand work to an agent, get a task id | agent_id*, task*, reason, model, thread (standing/new), mode (work/consult), after, review, repo, base, project, turn_room | assign | message |
| `clawtilla_handoff` *hop* | give a task you own to another agent, at turn end | task_id*, agent_id*, reason*, turn_room | handoff | message |
| `clawtilla_task_retry` *hop* | re-queue a failed or stalled run (two retries per incident) | incident_id or task_id, reason, turn_room | retry | message |
| `clawtilla_task_link` | change what a waiting task is held `after` | task_id*, after*, remove | assign | message |
| `clawtilla_task_review` | accept a result or send it back with notes | task_id*, accept*, notes | none | message |
| `clawtilla_task_list` | every task you delegated or hold, and their subtrees | agent_id, include_finished | none | read |
| `clawtilla_task_status` | how one delegated task is going | task_id* | none | read |
| `clawtilla_task_result` | result of a finished task | task_id* | none | read |
| `clawtilla_task_cancel` | cancel a task you delegated and all it spawned | task_id* | none | delete |
| `clawtilla_task_complete` | report your assigned task done, with its result | task_id*, result* | none | message |
| `clawtilla_task_progress` | keep your task open one more turn, with a note | task_id*, note* | none | message |
| `clawtilla_fleet_cost` | spend per agent and in total, and the per-task cap | -- | none | read |

- Ending a turn is otherwise taken as finishing the task; call
  `clawtilla_task_progress` when stopping for now.
- `delegate` assigns within your own team; cross-team work goes to the chief.
- See [tasks-and-projects.md](tasks-and-projects.md) and
  [spending-and-usage.md](spending-and-usage.md).

## Decisions

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_ask_decision` | file a choice for the operator without waiting | question*, options, default*, default_reason, reversible_hours, task_id | decision-inbox | read |
| `clawtilla_decision_answer` | answer a decision routed to you as delegator | decision_id*, answer* | decision-answer | message |

The asker states its default and carries on; the answer arrives later as a
message. File one only when the branches give materially different work or
the choice is not cheaply reversible.
