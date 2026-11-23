# Tasks, projects, history and fleet packs

Source docs: docs/tasks.org, docs/orchestration.org ("Tasks", "The chief-of-staff pattern"), docs/maintenance.org ("Version history", "A fleet pack instead"), clients/cli/main.c. Key details: [config-keys.md](config-keys.md). How delegation and settle notices behave: [orchestration.md](orchestration.md).

## What a task is

One unit of work: origin (who asked), assignee, prompt, title, state, result, `parent_id` and depth. Made by `clawtilla_delegate` (agents), `clawtilla task create` / any client (operator), routines, triggers, and VENTURE tickets ([grok-and-venture.md](grok-and-venture.md)).

## States

| State | Means | Finished | Cancellable | Retryable |
|---|---|---|---|---|
| `unassigned` | operator's backlog, nobody has it | no | yes | no |
| `pending` | with its assignee, no turn yet | no | yes | no |
| `running` | assignee started a turn on it (set at `clawt_agent_begin_turn()`, not at creation) | no | yes | no |
| `blocked` | waiting for `after` tasks to complete | no | yes | no |
| `review` | finished by assignee, awaiting reviewer | no | yes | no |
| `completed` | done (accepted, if reviewed) | yes | no | no |
| `failed` | broke, or the agent/daemon went away under it | yes | no | yes |
| `stalled` | a limit stopped it (watchdog, room budget, loop guard, progress guard) | yes | no | yes |
| `cancelled` | somebody cancelled it | yes | no | no |

- Code asks predicates, never compares states: `clawt_task_state_is_finished()`, `_is_active()` (pending, running), `_is_cancellable()`, `_is_retryable()`. Waiting states (`unassigned`, `blocked`, `review`) are not active: turn timeouts, crash handling and restart reconcile ignore them.
- Badge colour: `clawt_task_state_tone()` (completed green, failed/cancelled red, stalled amber, running blue, pending grey).
- There is no `orphaned`: stopping an agent fails its tasks ("the agent handling this stopped before finishing").
- `pending` does NOT mean nobody picked it up yet in the sense of "delegate again": check `clawtilla_task_list` before re-delegating.

## How a task finishes (agent side)

- Assignee: `clawtilla_task_complete` (`task_id`, `result`), or just answer: the last thing it wrote is taken as the result (marked *inferred*; status/result say so).
- Not done yet: `clawtilla_task_progress` (`task_id`, `note`) holds it open past THIS turn only (repeat each turn) and marks it `running`.
- Turn-end completion is vetoed when: the turn was not the assignee's; the assignee is still typing; it called `clawtilla_task_progress` this turn; it has children still running.
- Settle notice: on any terminal state clawtilla (not the assignee) tells the delegator, in the room the delegation was made in, closed exchange. Act with tools: `clawtilla_message_user` for a root task, `clawtilla_task_progress`/`_complete` on the parent mid-chain. Never poll; results for a busy room are held and merged.
- Cancel: `clawtilla_task_cancel` (delegator or assignee only) takes the whole subtree. Depth limit is `orchestration.max_hops` measured from the parent.
- Follow: `clawtilla_task_list`, `clawtilla_task_status` (last five comments), `clawtilla_task_result`. All task verbs check the caller is origin or assignee.
- `thread: "new"` on delegate gives the task its own room `task:<id>` and a fresh session; default `standing` uses the pair's `dm:` room (tasks between a pair share context and queue). Unknown values refused.
- `model` on delegate applies to the assignment's turn only (libreclaw >= 0.28.0).

## Store and restart (`tasks.db`, since 0.35.0)

- Every transition written by `ClawtTaskManager` before `task-changed` is emitted. Fields set outside it need `clawt_task_manager_save()` (AGENTS.md).
- Journal per task: `clawtilla task show <id> --journal`. Thread (notes: progress, result, comments, review verdicts) emits `task.noted`, never `task.changed`.
- Live set = unfinished + finished in the last day (`task.list`, clients). Older: `clawtilla task list --history [--state S] [--agent A] [--before UNIX] [--limit N]`. Still answerable by id.
- Retention `orchestration.task_retention_days` (180; 0 keeps all). Unfinished tasks are never pruned.
- Restart: tasks restored silently (no notices, no incidents). An unfinished task with nothing carrying it (no progress hold, no unfinished child, no mailbox item, no wait, no queued handoff) is `failed` with "the daemon restarted while this ran ... Retry it". Retry with `clawtilla task retry <id>`.
- Task spend is reseeded from usage ledgers so `task_budget_usd` keeps counting.
- If `tasks.db` will not open the daemon runs in memory and lists say "held in memory" (`clawt_task_list_caption()`).

## Operator tasks and the backlog (since 0.36.0)

```
clawtilla task create --title "Release notes" "Write the 2.4 release notes."   # unassigned backlog
clawtilla task list --state unassigned
clawtilla task assign <task-id> scout       # only off the backlog; otherwise it is a handoff
clawtilla task comment <task-id> "Leave out internal refactors."
```

An assigned operator task is sent as `user` with the task contract. A comment on a pending/running task is also sent to the assignee (steered into a running turn or queued, never its own turn); on backlog/blocked/finished tasks it is only kept.

## Blockers (`after`, since 0.37.0)

- `clawtilla_delegate` `after: "task-a,task-b"`; `clawtilla task create --after A,B`; `clawtilla task link|unlink <task> <blocker>`; agent `clawtilla_task_link` (`task_id`, `after`, `remove`) on a task it delegated that is still `blocked`.
- Only `completed` releases. A failed/stalled/cancelled blocker keeps holding and the waiter is told once: retry it, cancel the waiter, or unlink. Removing the last blocker releases now. Held deliveries persist in `tasks.db` (`held_deliveries`).
- Refused: unknown id, self-link, cycles, a task already handed over or ended, no task store.

## Review (since 0.38.0)

- Reviewer resolved once at creation (`clawt_task_resolve_reviewer()`), in precedence: the delegation's/task's `review`, then the project's `review_by`, then the assignee's `agents.review_by`. Values: `lead` (team lead, or chief if none), `delegator`, `operator`, an agent id, empty/`none`. Never the assignee (self-landing means no review; naming the assignee outright refuses the task).
- Finishing moves it to `review`; `review-requested` fires once per result. Agent reviewer is notified; the operator gets inbox decision `review-<task id>`.
- Verdicts: agent `clawtilla_task_review` (`task_id`, `accept`, `notes` required when not accepting); operator `clawtilla task review <id> accept` or `clawtilla task review <id> changes "<what>"`. Changes send it back to `pending` and bump `review_round`. After `orchestration.review_rounds` (3; 0 = unlimited) the operator decides. Each verdict renews the tree budget.
- Set per agent: `clawtilla agent set builder review_by lead`.

## Checkouts (`repo`, since 0.39.0)

- `clawtilla task create --agent builder --repo ~/src/shop [--base REF] "..."`; delegate `repo` (absolute host path) and `base`; routines/triggers `worktree: true`.
- Checkout at `<workspace>/tasks/<task-without-task->` on branch `clawtilla/<task>`, base pinned to a commit. `git worktree` for unconfined host agents; hard-linked clone for bwrap/container/VM/distrobox. Refused for ssh hosts and remote podman connections, a bad path/base, an existing destination, or a backlog task with no assignee.
- `clawtilla task diff <id>` (committed + uncommitted + untracked names; 512 KiB cap); still works from the branch after removal.
- `clawtilla task checkout-remove <id> [--force]` (refused while pending/running or dirty without `--force`; branch always kept). Auto-removed `orchestration.checkout_keep_days` (14) after finishing unless dirty.

## Projects (since 0.40.0)

```yaml
projects:
  - id: shop
    name: Shop
    directory: /home/me/src/shop      # host path; checkouts of it (or a shared git remote) belong here
    goal: "Ship the new checkout flow"
    review_by: lead                   # beats agents.review_by
    agents: [builder, tester]         # only whose PROJECTS.org lists it; empty = everyone
    monthly_budget_usd: 40            # 0 = no budget (spend still shown)
    budget_warn_percent: 80           # one `budget` notification a month
    budget_hard_stop: true            # refuse NEW work once spent
```

- Membership decided once at creation: explicit `project` (unknown id refuses), else parent's, else by directory/remote match (`clawt_git_remote_canonical()`), else none.
- Assignee is told name + goal (stored as `project_brief`). `PROJECTS.org` gets a managed region (no live spend).
- Month = tasks MADE this calendar month (local time), summed from usage ledgers, read at most once a minute.
- Hard stop refuses delegation, new tasks, worktree routine/trigger runs and held releases with `LOOP_LIMIT`; running work continues.
- Manage: `clawtilla project list` (month spend, `(stopped)`), `clawtilla project create <id> key=value ...` (name, directory, goal, review_by, agents, monthly_budget_usd, budget_warn_percent, budget_hard_stop), `clawtilla project set <id> <key> <value>` (`""` removes), `clawtilla project rm <id>`. IPC `project.list/create/set/remove`. VENTURE keys: `venture_repo_id`, `venture_milestone_id`, `venture_goal_id`.

## Task CLI summary

```
clawtilla task list [--history] [--state S] [--agent A] [--before UNIX] [--limit N]
clawtilla task show <id> [--journal]
clawtilla task create [--agent A] [--title T] [--after T1,T2] [--review lead|delegator|operator|AGENT|none] \
        [--repo DIR [--base REF]] [--project ID] "<prompt>"
clawtilla task assign|comment|link|unlink|review|diff|checkout-remove|cancel ...
clawtilla task retry <task-id|incident-id> [reason]
clawtilla task incidents
clawtilla task venture-retry <id>      # requeue given-up VENTURE writes (task.venture_retry)
```

## Version history (`history`, since 0.41.0)

`revisions.db` keeps every saved version of the config, each persona `.org` file, and every skill's `SKILL.md` (newest 200 per subject; >2 MiB refused; secrets redacted).
```
clawtilla history [list] [config|persona|skill] [SUBJECT]   # persona subject: scout/SOUL.org
clawtilla history show <id>
clawtilla history diff <id> [<newer id>]                    # one id = compare with now
clawtilla history restore <id>                              # goes through the edit path; becomes newest
```

## Fleet packs (`fleet`, since 0.42.0)

- `clawtilla fleet export <dir>` (dir absent or empty): `FLEET.yaml` (agents, teams, routines, triggers, projects), `agents/<id>/*.org`, library skills. Never exported: secret values/refs (schema SECRET type or credential-shaped names), workspaces, the `daemon` section, credential files, ephemeral helpers. Each omission listed in `to_fill`.
- `clawtilla fleet import <dir> --preview` shows each item, `exists`/`new`, dangerous settings it turns on, and `to_fill`.
- `clawtilla fleet import <dir> [--skip KIND:ID] [--replace KIND:ID] [--rename KIND:ID=NEWID] [--allow-dangerous]`. Existing items skipped unless told. Any item turning on a `CLAWT_SCHEMA_FLAG_DANGEROUS` setting (e.g. `tools.manage_fleet`, `tools.ephemeral`, a `host` computer) refuses the WHOLE import without `--allow-dangerous`.
- Agents arrive held (`runtime.autostart: false`), skills disabled, persona files never overwritten; import is a history version (author `import`).
- After: fill `to_fill`, read and enable skills ([skills-and-teach.md](skills-and-teach.md)), start agents.

## Troubleshooting

| Symptom | Do |
|---|---|
| failed "the daemon restarted while this ran" | `clawtilla task retry <id>` |
| `blocked` never starts | `clawtilla task show <blocker> --journal`; retry or `task unlink` |
| stuck in `review` | reviewer silent or rounds exhausted: `clawtilla task review <id> accept|changes "..."` |
| `repo` refused | ssh/remote-podman assignee, or bad path/base; refusal quotes git |
| project work refused `LOOP_LIMIT` | hard stop hit: raise `monthly_budget_usd` or wait for the month |
| delegator never heard back | check `clawtilla task show --journal`; settle notice goes to the delegating room |
