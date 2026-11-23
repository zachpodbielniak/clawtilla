# Configuration: `routines`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `routines`

Standing work: a prompt, an agent, and when to run it.

A routine is the thing you would otherwise remember to ask for every
morning. It runs as a delegated task, so it has its own result.

It does not get a session of its own. libreclaw keys a session on
channel, room and sender and deliberately ignores the thread, so a
routine lands in the operator's session and queues behind whatever
the operator is doing -- and inherits that conversation's context.
Point a routine at an agent that is not also somewhere you chat, or
give it `isolate` so it runs in a room of its own.

Routines only fire while the daemon is running. A machine that was
asleep at nine o'clock has missed nine o'clock, and the routine says
so rather than pretending otherwise -- see `catch_up`.

### `routines.id`

- Type: string
- Default: unset
- Required: yes
- Since: 0.2.0

What this routine is called, and how it is referred to.

### `routines.description`

- Type: string
- Default: unset
- Since: 0.2.0

One line about what it is for, for a person reading the list.

### `routines.instructions`

- Type: string
- Default: unset
- Required: yes
- Since: 0.2.0

What the agent is asked to do, in full.

Sent as the whole of the prompt, so it should read as an instruction
rather than a reminder: the agent has no memory of the last run and
nobody is there to answer a question about what was meant.

### `routines.agent`

- Type: string
- Default: unset
- Required: yes
- Since: 0.2.0

Which agent runs it.

### `routines.model`

- Type: string
- Default: unset
- Since: 0.7.7

The model this routine's run uses, instead of the agent's own.

For that one turn only: the agent's next turn is back on its own
model. A scheduled check that reads a page and compares a number does
not need the model the agent plans with, and a run costs what its
model costs. A model name for the agent's provider, as in
`agents.model.model`; like that key it is not checked against the
catalogue, which goes stale, so a name the provider does not know
fails the run rather than the save. Empty is the agent's own model.

### `routines.enabled`

- Type: boolean
- Default: `true`
- Since: 0.2.0

Whether it runs on its schedule. A disabled routine can still be
started by hand, which is how you try one before trusting it.

### `routines.schedule`

- Type: enum
- Values: manual, hourly, daily, weekdays, weekly, custom
- Default: `daily`
- Since: 0.2.0

manual, hourly, daily, weekdays, weekly or custom.

All of them become a cron expression, including the presets -- so
there is one answer to "when does this next run" rather than two.

### `routines.at`

- Type: string
- Default: `09:00`
- Since: 0.2.0

The time of day, for daily, weekdays and weekly.

Local time, because somebody who wrote 09:00 means nine o'clock where
they are. With `hourly` only the minute is used.

### `routines.weekday`

- Type: string
- Default: `monday`
- Since: 0.2.0

Which day, for `weekly`.

### `routines.cron`

- Type: string
- Default: unset
- Since: 0.2.0

The expression, for `custom`: minute hour day-of-month month day-of-week.

Ranges, lists and steps all work, and months and weekdays may be
named. Note the oldest oddity in cron: when *both* day fields are
restricted the match is day-of-month OR day-of-week, so `0 0 13 * 5`
is the thirteenth and every Friday, not Friday the thirteenth.

### `routines.directory`

- Type: path
- Default: unset
- Since: 0.2.0

Where to run, if not the agent's workspace.

A path on the agent's computer, which for a container or a VM is a
path inside it and not on the host.

### `routines.worktree`

- Type: boolean
- Default: `false`
- Since: 0.2.0

Run in a fresh checkout of `directory` rather than in it.

For a routine that changes files: it keeps a scheduled run from
landing on top of whatever you had checked out at the time. Since
0.39.0 clawtilla makes the checkout itself, for each run, in the
agent's workspace at tasks/<task> on the branch clawtilla/<task> (a
git worktree, or a local clone for a confined or containerised agent),
and tells the agent where it is; the run fails if it cannot be made.
The run's Changes show what it did, and the checkout is removed after
orchestration.checkout_keep_days with its branch kept.

### `routines.isolate`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Give this routine a conversation of its own.

A run is sent from `user` to the agent by default, so it lands in the
operator's room from the operator's sender -- and libreclaw keys a
session on channel, room and sender. One session, one queue: Monday's
run is in Tuesday's context, a 09:00 brief starts whenever the agent
next goes idle, and both happen in the operator's transcript.

Set this and the run goes to a room of its own from a sender of its
own, which is a session of its own: no shared context, no waiting
behind a conversation. The cost is that the prompt and the answer are
no longer in the operator's chat -- they are in the task result and on
the Flow tab, which is where a delegated task's output already lives.

Off by default because moving somebody's output is not a thing to do
to them silently. Pointing the routine at an agent nobody talks to
solves the same problem and needs no setting.

### `routines.read_only`

- Type: boolean
- Default: `false`
- Since: 0.4.0

Run with read-only tools: the agent may look at anything it can reach
and change nothing -- no messages, no writes, no commands, no screen.

The run gets a room of its own (as routines.isolate does) and every
turn in that room is offered only clawtilla's read tools; its own
tools are refused through the PreToolUse hook. That hook exists only
for claude-code and claude-tmux, so a read-only routine on any other
backend is refused rather than run with its own tools unchecked.
What it finds it reports with clawtilla_ask_decision.

### `routines.proactive`

- Type: boolean
- Default: `false`
- Since: 0.4.0

Look for ways to help, unasked, and report them without acting.

Implies read_only. With no instructions, the agent is told to go
through what it is connected to -- its integrations and connectors --
for anything that needs attention, and to file each finding as a
decision for the operator rather than acting on it.

### `routines.catch_up`

- Type: boolean
- Default: `false`
- Since: 0.2.0

Run once at startup when its time passed while the daemon was down.

Off by default, and worth leaving off for anything that talks to
somebody: a laptop opened after a long weekend would otherwise deliver
a stack of good mornings at once. Only ever one run, however many
were missed.

### `routines.jitter_seconds`

- Type: integer
- Default: `0`
- Since: 0.2.0

Delay each run by up to this many seconds, chosen at random.

Zero here, unlike a hosted scheduler, because there is no shared
server to spread load across: this is your machine, and a routine set
for 09:00 should run at 09:00. Worth setting only when several
routines share one rate-limited service.

Only a scheduled run is delayed. `clawtilla routine run` starts at
once, because somebody waiting at a terminal is not load to spread;
so does a catch-up, because a run that was already missed does not
need to be later still.

### `routines.max_start_failures`

- Type: integer
- Default: `3`
- Since: 0.13.0

Turn the routine off after this many runs in a row whose computer
did not start. 0 never turns it off.

A dead VM or a missing image otherwise fails the routine on every
tick, and nobody hears about it. At the limit the routine is turned
off -- `enabled: false`, so it shows as off everywhere and is turned
back on the same way -- and an alert names the routine and the
computer's error. The count is run state, kept beside the config and
never in it. Any run whose computer starts sets it back to zero, and
so does editing the routine; a run that began before the edit is not
counted. A negative value reads as the default.

### `routines.locked`

- Type: boolean
- Default: `false`
- Since: 0.7.1

Keep the agent from changing or removing this routine.

An agent with tools.schedule may adjust the routines that run on it --
move a check, split one, drop one it no longer needs. Lock the ones
that are yours to decide: it can still read a locked routine, and it
is told why a change was refused rather than left guessing. Only a
client or the config can set or clear this; an agent cannot.
