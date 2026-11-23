# Configuration: `projects`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `projects`

Projects: the pieces of work the fleet is pointed at.

A task belongs to at most one project: the one its delegation or the
operator named, else its parent's, else the one whose directory its
checkout came from (by path, or by a shared git remote). Its assignee
is told the project's name and goal; the project's spend is summed
over its tasks each month, against an optional budget; and every
agent working in it finds it in PROJECTS.org. Optional: a fleet with
none works exactly as before.

### `projects.id`

- Type: string
- Default: unset
- Required: yes
- Since: 0.40.0

Identifier used to address the project: `project` on a delegation or
a new task, and the task lists' filter.

### `projects.name`

- Type: string
- Default: unset
- Since: 0.40.0

Display name; the id when unset.

### `projects.directory`

- Type: path
- Default: unset
- Since: 0.40.0

The project's repository or directory, as a host path.

A task whose checkout is of this directory -- or of any repository
sharing one of its git remotes, compared as host/owner/repo -- belongs
to this project without being told.

### `projects.goal`

- Type: string
- Default: unset
- Since: 0.40.0

What the project is for, in a sentence or two.

Every assignment in the project starts with it, so the agent doing a
piece of work knows what the piece is for.

### `projects.review_by`

- Type: string
- Default: unset
- Since: 0.40.0

Who reviews work in this project, as agents.review_by spells it.

Between a delegation's own `review` and the assignee's
agents.review_by: the project's rule beats the agent's habit.

### `projects.agents`

- Type: list of strings
- Default: unset
- Since: 0.40.0

The agents that work in this project, by id.

Only decides whose PROJECTS.org lists it; empty lists it for every
agent. Anyone may still be given its work.

### `projects.monthly_budget_usd`

- Type: number
- Default: `0`
- Since: 0.40.0

What the project may spend in a calendar month, in US dollars.

Summed from every agent's usage ledger over the project's tasks
made this month. 0 means no budget: spend is still shown.

### `projects.budget_warn_percent`

- Type: integer
- Default: `80`
- Since: 0.40.0

At what share of monthly_budget_usd the operator is warned, once a
month.

### `projects.budget_hard_stop`

- Type: boolean
- Default: `false`
- Since: 0.40.0

Refuse new work in the project once its monthly budget is spent.

A delegation, a new task and a held task's release are refused, as
the spending caps refuse, with the reason. Work already handed over
is not stopped.

### `projects.venture_repo_id`

- Type: integer
- Default: `0`
- Since: 0.44.0

The VENTURE repository (a `forge_repo` id) this project's tasks are
mirrored against.

Only with a venture connector's `mirror_tasks` on. Without it, a
task's checkout remotes are matched against VENTURE's repositories;
with it, the project's tasks file there whatever their remotes say.
0 for none. See docs/venture.org.

### `projects.venture_milestone_id`

- Type: integer
- Default: `0`
- Since: 0.46.0

The VENTURE milestone this project's mirrored tickets are filed under.

Its name, due date and state are read once a day and shown in the
project's agents' PROJECTS.org and each task's assignment. 0 for none.
See docs/venture.org.

### `projects.venture_goal_id`

- Type: integer
- Default: `0`
- Since: 0.46.0

The VENTURE goal this project's mirrored tickets count toward.

Its name and progress (current against target) are read once a day
and shown with the milestone. 0 for none. See docs/venture.org.
