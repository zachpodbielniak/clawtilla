# Teams, team tags and ephemeral helpers

Source docs: docs/orchestration.org ("Teams", "Incidents"), docs/team-tags.org, docs/ephemeral-agents.org, src/agent/clawt-team.c, src/config/clawt-config-schema.c. Key defaults: [config-keys.md](config-keys.md). Delegation rules: [orchestration.md](orchestration.md).

## Teams

Optional. A fleet with no `teams:` is one group where only the chief of staff assigns.

```yaml
teams:
  - id: research
    name: Research
    description: Reads sources and answers questions about them. Not for anything that writes code.
    color: "#7aa2f7"
    order: 10                       # lowest first; ties keep file order
    tags: [night-shift]             # see below
    skills: [lit-review]            # every member gets these
    broker_server: research-gw      # only with broker.enabled
agents:
  - id: chief
    chief_of_staff: true
  - id: lead-r
    team: research
    team_role: lead
  - id: reader
    team: research                  # team_role defaults to member
```

- Membership lives on the AGENT (`agents.team`, `agents.team_role`: `lead` | `member`), not in the team entry.
- `description` is what the chief reads (`clawtilla_list_teams` leads with it): say what the team handles and what it does not, not who is on it.
- `teams.skills` is additive with `defaults.skills` and the agent's own `skills` ([skills-and-teach.md](skills-and-teach.md)).
- Integrations can be scoped to a team ([integrations.md](integrations.md)). Incidents route to the failing agent's team lead ([orchestration.md](orchestration.md)).
- Being a lead or chief makes `session.routing_mode` default to `agent`.

### Who may assign

`clawt_team_may_assign()` is the one rule:

| From | May assign to |
|---|---|
| chief of staff | anybody (it leads every team) |
| lead | members of its own team only; a lead with no team assigns to nobody |
| member | nobody |
| an ephemeral helper's parent | that helper (whatever either one's team role) |
| anyone | never itself ("do the work rather than delegating it to yourself") |

Talking is never restricted: everyone may `clawtilla_message_agent`, `clawtilla_ask_agent`, share rooms. `clawtilla_delegate` is offered only to agents that can assign to somebody; the target is checked at call time; a cross-team refusal says to send it to the chief.

### Fleet warnings (not errors)

`clawtilla team list` prints them to stderr; the Teams page shows them:
- an agent on a team nobody declared (usually a typo);
- a team with two leads ("Only one may assign work; make the other a member");
- a team with members and no lead (only the chief can assign into it; not reported for the `ephemeral`-tagged team);
- a malformed tag, or a unique tag on two teams.

### Team CLI

```
clawtilla team list                              # leads, running/total, then warnings on stderr
clawtilla team create research "Reads sources ..."
clawtilla team set research description "..."    # keys: name, description, color, order
clawtilla team assign reader research            # member
clawtilla team assign lead-r research lead       # lead (sets agents.team and agents.team_role)
clawtilla team rm research                       # its agents become teamless; reassign them
clawtilla team tag list [--well-known]
clawtilla team tag add|remove <team> <tag>
```

IPC: `team.list` (with `tags`, `warnings`), `team.create` (takes `tags`), `team.set` (`key`, `value`, or `values` array for lists; dispatches on schema type, so `tags` and `skills` are lists), `team.remove`. A role change needs `clawtilla agent restart <id>` (tool list read at session start).

## Team tags

A tag gives a team a job in the software, independent of its id/name: features look for the team *tagged* X, never *called* X.

- Syntax: lowercase letters, digits, hyphens; starts with a letter, does not end in a hyphen, at most 64 chars. Plugin tags: one `vendor:` prefix (`acme:night-shift`).
- Malformed tags in the file are kept and warned about; clients and `team.set` refuse them before writing.
- Unique tags: if two teams carry one, the FIRST in team order (`teams.order`, then file order) keeps it and the warning names both (`clawt_team_find_by_tag()`). Fix by removing it from the other.
- Any other valid tag is yours (shown as "(yours)"), acted on by nothing.

| Well-known tag | Unique | Used by |
|---|---|---|
| `ephemeral` | yes | where ephemeral helpers join. If no team has it, the first spawn adds `id: ephemeral`, name *Ephemeral*, to `clawtilla.yaml` |
| `office-custodial` | yes | the team that keeps the drawn office; never auto-created (`clawtilla office custodians install`, the Office page, or the chief). See [office-custodial.md](office-custodial.md) |

To put helpers on your own team: `clawtilla team tag add interns ephemeral` (and remove it from the auto-made team).

## Ephemeral helpers (since 0.55.0)

A temporary agent an agent makes for one separable piece of work, then reaps. For anything ongoing, create a real agent (`clawtilla_create_agent`, needs `tools.manage_fleet`).

### Lifecycle (agent side)

1. `clawtilla_ephemeral_spawn`: `description` (required, one line), optional `purpose` (its instructions), `name`, `provider`, `model`, `computer`, `container_image`. Returns its id; it is started.
2. `clawtilla_delegate` to it as to any agent. Its task is a child of the parent's turn task, so cancel cascades and tree budgets count it.
3. It answers; the task settles back as usual.
4. `clawtilla_ephemeral_reap` (`agent_id`, optional `force`): stopped, workspace and state removed, removed from rooms, `agent.removed` published.
`clawtilla_ephemeral_list`: each helper's purpose, running, open work.

### Gates and limits

| Key | Default | Effect |
|---|---|---|
| `agents.tools.ephemeral` | on for the chief (either spelling of the role), off for others; explicit `false` turns the chief off | may spawn; DANGEROUS flag (fleet import needs `allow_dangerous`) |
| `orchestration.ephemeral_max_per_parent` | 5 | helpers one agent may have (refusal lists them); 0 = none |
| `orchestration.ephemeral_max_fleet` | 20 | helpers fleet-wide; 0 = none |
| `orchestration.ephemeral_reap_reminder_hours` | 24 | one reminder to the parent after work is finished and nothing open; 0 = never |

- List and reap need only having helpers, so a parent whose `tools.ephemeral` was turned off can still clean up. Spawn and reap are action class `fleet_admin` (can be put behind approval: [security.md](security.md)).
- Enable for a lead: `clawtilla agent set research-lead tools.ephemeral true` then `clawtilla agent restart research-lead`.

### What a helper gets

- Copied from the parent: `runtime` (incl. `confine`, `network`), `model`, `tools`, `skills`, limits, a computer per the table below.
- Not copied: id, name, description, workspace, team/role, `chief_of_staff`, reviewer, `integrations`, `channels`, routines, face/colour, persona, projects. It still gets integrations scoped to its team.
- Forced off: `tools.ephemeral` (no helpers of helpers), `tools.manage_fleet`; checked at offer and at call.

| Parent's computer | Helper default | Helper may ask for |
|---|---|---|
| none | none | none |
| host | host (same confinement) | none, host, container |
| display | none | none, container |
| container, distrobox | none | none, container |
| vm | none | none (a VM is never spawned) |

- Names: random `adjective-noun` (`turbo-narwhal`) unless `name` given (must be a valid, unused agent id; taken = refused).
- Team: the `ephemeral`-tagged team; for assignment its parent counts as its lead (the chief may also assign to it).

### Reaping rules

- Only the parent or the operator may reap. Open work (any unfinished assigned task) refuses the reap, naming the tasks, unless `force` (drops it).
- Nothing reaps automatically (not on a timer, not when a task finishes). One reminder notice per idle stretch (from the daemon, invites no reply); clients badge the helper amber ("done").
- Operator: `clawtilla agent reap <id> [--force]`, the inspector's *Reap this helper*; `clawtilla agent rm <id>` reaps with open work dropped.
- A removed parent's helpers are re-parented to the chief of staff, who is told.

### Persistence

Never written to `clawtilla.yaml` (no save, revision, export or fleet pack). Stored in `<state_dir>/ephemeral.yaml` (0600), restored at start and after every reload; an unrestorable entry is skipped with a warning. `clawtilla agent list` marks them `[ephemeral, <parent>'s]`, plus `done` when awaiting reap.

### Troubleshooting

| Symptom | Cause / fix |
|---|---|
| chief lacks `clawtilla_ephemeral_spawn` | restart it after upgrade; check `tools.ephemeral` not false and `tools.deny` |
| "you already have 5 ephemeral agents" | reap one, or raise `ephemeral_max_per_parent` |
| "a helper cannot have a 'host' computer when you have 'none'" | by design: give the parent the computer or create a real agent |
| helper still there after restart | by design until reaped |
| two teams tagged `ephemeral` | remove the tag from one; the first keeps it meanwhile |
