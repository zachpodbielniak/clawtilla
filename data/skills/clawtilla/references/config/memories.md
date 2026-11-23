# Configuration: `memories`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `memories`

What an agent remembers between conversations.

Not agents.memory, which is the MEMORY.md size budget and a different
thing entirely. This is the searchable store.

Each agent gets its own SQLite database beside its mailbox, holding
what it chose to write down: decisions, preferences, things that
turned out to be true. It searches its own with clawtilla_memory_*.

One database per agent rather than one table with an agent column, so
isolation is a property of the filesystem: a query that forgot to
filter still cannot reach another agent's memories, because they are
not in the file being read.

### `memories.enabled`

- Type: boolean
- Default: `true`
- Inside an agent: `memories.enabled`
- Since: 0.1.0

Whether agents get a memory store at all.

Off means the clawtilla_memory_* tools are not offered, rather than
offered and failing. An agent that can see a tool will try it.

### `memories.max_results`

- Type: integer
- Default: `20`
- Inside an agent: `memories.max_results`
- Since: 0.1.0

How many memories one search or listing returns at most.

This lands in the agent's context on every call, so the limit is a
budget rather than a formality.

### `memories.readers`

- Type: string
- Default: unset
- Inside an agent: `memories.readers`
- Since: 0.1.0

Comma-separated ids of agents allowed to read this agent's memories.

Empty by default, which is the whole point: an agent's memories are
its own unless somebody says otherwise. Reading only -- there is no
setting that lets one agent write into another's memory, because a
memory you did not form is not a memory.

A comma-separated string, not a YAML list: `readers: chief, deputy`.
Written as `readers: [chief]` it reads back as unset and grants
nothing, so the loader warns about that spelling rather than leaving
a permission somebody granted silently denied.

### `memories.scope`

- Type: enum
- Values: agent, team, fleet
- Default: `agent`
- Inside an agent: `memories.scope`
- Since: 0.2.0

Where this agent's own memories are written: agent, team or fleet.

Reading always fans out across every scope the agent is entitled to;
this says only where a new memory lands. A fact the whole fleet
needs is worth writing once rather than being relearned by each
agent in turn.

Each scope is a separate database, so what an agent may read is
decided by which file is opened rather than by a WHERE clause. A
permission that is structural cannot be lost to a missing
condition.

`team` needs the agent to be on one. An agent with no `team:` that
asks for team scope is refused rather than quietly written to its own
database -- a memory whose whole purpose was to reach the team is worse
in the wrong place than not written at all.

### `memories.recall`

- Type: boolean
- Default: `true`
- Inside an agent: `memories.recall`
- Since: 0.2.0

Whether the agent may search past conversations.

Separate from `memories.enabled`, which is about facts the agent
chose to record. This is about the transcript itself, which it never
chose and which is much larger.

Off means `clawtilla_recall` is not offered, rather than offered and
refused. Only rooms the agent is a member of are ever searched, so this
is about the agent reading its own past rather than anybody else's.

### `memories.summarise`

- Type: boolean
- Default: `false`
- Inside an agent: `memories.summarise`
- Since: 0.2.0

Whether finished work is summarised into memories automatically.

Off by default because it is a model call nobody asked for, billed
to whoever set it. On, a completed task or a closed routine run is
distilled into memories tagged with the transcript they came from.

It uses the `ai_assist` provider, so that has to be configured too,
and the memories land in whichever scope `memories.scope` names. A
summary that produced nothing is the common answer and not a failure.

### `memories.nudge_turns`

- Type: integer
- Default: `0`
- Inside an agent: `memories.nudge_turns`
- Since: 0.2.0

The cadence the reminder to record what was learned asks for. 0 is
off.

The reminder rides the per-agent prompt suffix rather than being a
mechanism of its own, so it is visible in the same place as every
other per-turn instruction -- `clawtilla config render` shows it
beside the computer directive.

A prompt suffix reaches every turn, so this number is what the
reminder asks the agent for rather than something clawtilla counts.
Counting turns from the daemon would need a per-turn channel to the
agent, and the only one there is is the delivery preamble -- which not
every turn has.

### `memories.journal_keep`

- Type: integer
- Default: `1000`
- Since: 0.26.0

How many changes each memory database's journal keeps. 0 keeps every
one.

Every remember, forget, pin and unpin is written to a journal beside
the memories it changed, with who made it, and the Memory review page
in each client lists it with an Undo per row. The oldest rows go first
once there are more than this, and a change whose row has gone can no
longer be undone -- the page says so rather than offering a button
that cannot work. One journal per database: an agent's own, each
team's and the fleet's.

### `memories.operator_profile`

- Type: boolean
- Default: `false`
- Since: 0.2.0

Whether the fleet keeps a profile of the person it works for.

A fleet-scope memory a newly created agent inherits, so it starts
knowing how you work instead of learning it again. Plain text and
editable: a model of a person that the person cannot read is not
something to build.

Two halves: `<state_dir>/OPERATOR.org`, which you write, and
fleet-scope memories in the `operator` category, which agents record.
Both are written into a marked region of every agent's USER.org, so a
change reaches the whole fleet rather than only agents made afterwards.
Off removes the region again.

## `memories.review`

A periodic review of each agent's memories by a small model, which
proposes merges, forgets and pins for a person to accept one at a
time. Off unless a model is named.

### `memories.review.model`

- Type: string
- Default: ``
- Since: 0.29.0

The model that reviews memories, as provider/model --
claude/claude-haiku-4-5-20251001, ollama/llama3.2. Empty turns the
review off, which is the default.

Off by default because every pass is a model call nobody asked for,
billed to whoever set this. On, each agent's own memories are read
by the model at most once per interval, fenced as data, and what it
proposes -- merge two memories into one, forget one, pin or unpin
one -- is never applied. It is listed as a proposal in the Memory
review group of every client, and only a person accepting it changes
anything, through the same journaled store as every other change, so
an accepted proposal can be undone like any other.

A provider ai-glib reaches over HTTP, with its key configured the way
ai_assist's is. The model has a hard limit of 60 seconds per pass; a
timeout, a failure and a reply that cannot be read propose nothing.

### `memories.review.interval_hours`

- Type: integer
- Default: `24`
- Since: 0.29.0

How often each agent's memories are reviewed, in hours (1 to 720).

A pass is skipped while the agent still has proposals nobody has
answered, so an unread list never grows by itself.

### `memories.review.max_memories`

- Type: integer
- Default: `60`
- Since: 0.29.0

How many of an agent's memories one pass reads at most, newest first
(5 to 500). This is the size of the request, so it is the cost of
one pass.
