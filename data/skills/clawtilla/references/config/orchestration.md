# Configuration: `orchestration`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `orchestration`

How agents talk to each other, and what stops them doing it forever.

Bot-to-bot messaging is the point of clawtilla and also the thing most
able to run away: two agents that each reply politely will do so until
something stops them. Every limit here is that something.

### `orchestration.max_hops`

- Type: integer
- Default: `8`
- Since: 0.1.0

How far a message may travel agent-to-agent before it is refused.

Every message carries the depth it was sent at. A chief-of-staff
delegating to a worker that asks a specialist is three hops; anything
much beyond that is usually a loop rather than a plan.

### `orchestration.rate_limit_per_minute`

- Type: integer
- Default: `30`
- Since: 0.1.0

Messages one agent may send per minute. 0 disables the limit.

### `orchestration.daily_fleet_budget_micros`

- Type: integer
- Default: `0`
- Since: 0.1.0

Daily fleet admission cap in integer millionths of USD; 0 disables.
Uses recorded usage since local midnight, including reset archives.
Already running work can overshoot; new deliveries wait until reset.

### `orchestration.daily_agent_budget_micros`

- Type: integer
- Default: `0`
- Inside an agent: `daily_agent_budget_micros`
- Since: 0.1.0

Daily admission cap per agent in integer millionths of USD; 0 disables.
Override with daily_agent_budget_micros on an agent.
Uses local midnight and persisted usage; in-flight work can overshoot.

### `orchestration.meshy_credit_micros`

- Type: integer
- Default: `0`
- Since: 0.75.0

What one Meshy credit costs you, in integer millionths of USD (20000
for two cents); 0, the default, leaves it unknown.

Meshy prices its jobs in credits, not dollars. With a price here, an
agent's Meshy jobs count towards the daily caps
(daily_agent_budget_micros, daily_fleet_budget_micros) and the
Spending page's total; without one they are listed as credits and
leave both alone -- unknown, never free. PixelLab says its price in
dollars and needs nothing here.

### `orchestration.task_budget_usd`

- Type: number
- Default: `5.0`
- Since: 0.1.0

Spend cap for one delegated task and everything it spawns.

A turn's cost is booked on the task it was about and on every task
above it, so a task's figure is what its whole subtree cost.  A
message on any task is refused once that task, or the root of its
tree, has spent the cap -- one budget for a whole fan-out, not one
per subtask (per subtask until 0.65.2).  So the default bounds
everything a chief's assignment grows into; raise it for fleets that
do large delegated work.  orchestration.tree_message_budget bounds the
same tree in messages, a project's monthly_budget_usd its month.
0 disables the cap.

### `orchestration.idle_hours`

- Type: integer
- Default: `72`
- Since: 0.8.0

How long without a turn before the Fleet page calls an agent idle.

Only a label: an idle agent is still running, still holding its
process and its computer. `agents.runtime.idle_stop_hours` is what
stops one. 0 calls nothing idle.

### `orchestration.cycle_window`

- Type: integer
- Default: `10`
- Since: 0.1.0

How many recent messages per room are remembered for loop detection.

A message repeating one already in the window is refused. This catches
the case the hop limit does not: two agents alternating the same two
replies, each one a fresh chain.

This bounds the memory. How far back the check looks in time is
orchestration.cycle_seconds.

### `orchestration.cycle_seconds`

- Type: integer
- Default: `300`
- Since: 0.1.0

How long a repeated message counts as a loop. 0 disables the check.

Without this the cycle window was a count alone, so how far back it
looked was however long the room's last cycle_window messages had
taken -- ten messages in a quiet room is hours. An agent that hit a
spawn failure and reported the same error string every turn had its
first report delivered and every one after it refused, so a wedged
agent was indistinguishable from an idle one for ten hours.

The default suits the runaway this check exists for -- two agents
alternating the same two replies with nothing pacing them, where a
turn is seconds. Raise it for a fleet whose turns are minutes; a
repeat is only caught if it lands inside the window.

### `orchestration.ack_check_model`

- Type: string
- Default: ``
- Since: 0.17.0

A small model that decides whether an answer only acknowledges, as
provider/model -- claude/claude-haiku-4-5-20251001, ollama/llama3.2.
Empty turns the check off.

An answer to a deliberate message wakes the agent that sent it, and
that turn costs as much for "Thanks, noted." as for a page of
findings. With a model set, an answer is put to it only when the
answering turn made no tool calls (so ran no commands, changed no
files and sent nothing), its steps were streamed so that is known,
the answer is at most 600 characters, and the message it answers was
not a task. The model is asked one question, ACK or NEEDS, with both
texts fenced as data, and has 8 seconds.

Only a plain ACK skips the wake. NEEDS, a failure, a timeout or any
other reply wakes the sender as before. An acknowledged answer is
still recorded in the room, marked "acknowledged (no wake)", and
the sender's next turn opens with a one-line note of it, so nothing
is dropped.

A provider ai-glib reaches over HTTP, with its key configured the way
ai_assist's is. A command-line provider would spend the 8 seconds
starting. A message sent with reply: none never wakes its sender,
with or without this.

### `orchestration.tree_message_budget`

- Type: integer
- Default: `60`
- Since: 0.20.0

How many requests one task tree may send between settlements.
0 disables the budget.

A tree is a root task and everything delegated from it. max_hops
bounds how deep a chain goes and says nothing about how wide: a chief
that hands work to ten agents, each of which asks two more, is three
hops deep and can still talk all night. This counts the deliberate
messages sent from inside one tree -- clawtilla_message_agent,
clawtilla_ask_agent, clawtilla_delegate, clawtilla_handoff,
clawtilla_post_room and an @mention in a room -- one per call, however
many teammates the call reaches.

Answers are not counted and never refused, and neither is the
follow-up turn a delegator is owed when its work settles. The count
starts again whenever any task in the tree settles, because a
settlement is progress and chatter is not.

At the limit the messaging tools are withheld from that tree's turns,
exactly as max_hops withholds them, and one alert names the tree.

### `orchestration.pair_message_budget`

- Type: integer
- Default: `12`
- Since: 0.34.0

How many rounds of requests two agents may send each other with no
person in between. 0 disables the budget.

Every other limit here can be started again by the agents
themselves. The hop count begins at zero on a notice of the daemon's
(a task settling, a wait ending), and the tree budget renews when a
task settles -- so two agents trading one job at a time, or asking
each other something after every notice, could do it for as long as
both were willing and nothing was ever refused. This is the count
nothing an agent does renews.

A round is every request one agent sends the other from one turn --
clawtilla_message_agent, clawtilla_ask_agent, clawtilla_delegate, a
handoff -- so a chief handing a worker ten jobs at once has used one
round, and a pair passing one job back and forth has used a round
each time. Answers are not counted and never refused. Group rooms
are not counted: rounds and rooms.max_chain bound those.

The count starts again when a person, a routine or a trigger writes
to either agent, or when the two have sent each other nothing for
orchestration.pair_quiet_minutes. At the limit a request between
the two is refused, saying so, and one alert names the pair.

### `orchestration.pair_quiet_minutes`

- Type: integer
- Default: `30`
- Since: 0.34.0

How long two agents must send each other nothing before their pair
budget starts again. 0 means only a person renews it.

A fleet driven from somewhere the daemon does not see -- a Matrix
room, a webhook -- never shows it a person, so without this two
agents that had ever done pair_message_budget rounds of honest work
could not ask each other anything again until somebody opened a
chat. A loop is never quiet, so it does not earn this; once it has
been stopped nothing is left running to start it again.

### `orchestration.ephemeral_max_per_parent`

- Type: integer
- Default: `5`
- Since: 0.55.0

How many ephemeral agents one agent may have at once. A spawn past it
is refused with the list of the ones it has, so the answer to the
refusal is to reap one. 0 means none at all.

### `orchestration.ephemeral_max_fleet`

- Type: integer
- Default: `20`
- Since: 0.55.0

How many ephemeral agents the whole fleet may have at once, whoever
spawned them. Each one is a running CLI process, so this is a ceiling
on what a busy afternoon can start. 0 means none at all.

### `orchestration.ephemeral_reap_reminder_hours`

- Type: integer
- Default: `24`
- Since: 0.55.0

How long an ephemeral agent may sit with its work finished and nothing
assigned before its parent is reminded, once, to reap it. Nothing is
reaped automatically: the parent decides when the work is really
done. 0 means never remind.

### `orchestration.answer_in_request_room`

- Type: boolean
- Default: `true`
- Since: 0.34.0

Hand an agent's answer to the asker in the conversation it asked
from, rather than in the room the two agents share.

An agent that asks a teammate something from your chat, or from a
group, is woken with the answer there: it has the conversation
behind it, what it writes at the end of that turn reaches you as any
reply does, and anything more it wants from the teammate is a tool
call. The answer itself stays on the record in the two agents' own
room, where Flow shows the exchange.

Off, the answer wakes the asker in the two agents' room -- a
separate session that holds none of your conversation -- as it did
before 0.34.0. A request made while answering somewhere the daemon
does not see (a Matrix room) is always answered that way.

### `orchestration.crosstalk_summary`

- Type: enum
- Values: chief, on, off
- Default: `chief`
- Inside an agent: `crosstalk_summary`
- Since: 0.30.0

Tell you about agent-to-agent conversations: chief, on or off.

When an exchange between agents ends -- a message and its answer, a
delegated task settling, a Grok bot reaching out and being answered,
a group room's round -- a short summary of it is posted into your own
conversation with the agent that took part: who reached out, what
they asked, what the agent did (tool calls by kind, tasks it handed
on, decisions it raised) and how it ended, with a link that opens the
exchange in Flow. Without it, that conversation is only in Flow, and
you have to go and look.

chief, the default: only the chief of staff's conversation with you
gets them. on: every agent's. off: none. Set per agent as
`crosstalk_summary` to turn one agent on or off whatever this says.

The summary is the daemon's note, built from the record and costing
no model call: it is never delivered to the agent, never starts a
turn, invites no reply and counts towards no hop, budget or incident.
An exchange ends when nobody in it has a turn running or a message
waiting there, no task it carries is still running, and it has been
quiet for 15 seconds; exchanges ending within a few seconds of each
other are posted as one note. Work a trigger or routine started is
not cross-talk and is never summarised.

### `orchestration.crosstalk_summary_to`

- Type: enum
- Values: source, chat
- Default: `source`
- Inside an agent: `crosstalk_summary_to`
- Since: 0.33.0

Where a cross-talk summary is posted: source or chat.

source, the default: back where the request came from. An agent that
reached out to another agent or a Grok bot while answering somebody in
a Matrix room has the summary posted in that room, from its own Matrix
account, so the person who asked hears how it went. Asked in a
clawtilla room you are in -- a group -- it is recorded there. Anything
else comes to your chat with the agent, as does a Matrix post that
fails (with a line saying why).

chat: always your chat with the agent.

Only the agent whose turn began the exchange posts it at the source;
another agent in it that is told (`crosstalk_summary`) is told in your
chat with it. An exchange picked up again after a daemon restart has
lost where it came from and comes to your chat. Set per agent as
`crosstalk_summary_to`.

### `orchestration.config_notes`

- Type: enum
- Values: agents, all, off
- Default: `agents`
- Since: 0.74.0

Say in an agent's chat when its configuration was changed: agents,
all or off.

When an agent changes another agent's settings -- or its own -- or
adds, edits, turns on or off or removes a routine or trigger that runs
as it, a one-line note appears in your chat with the agent that was
changed: what changed and who changed it. Without it the only trace
is the acting agent's own tool step, in the acting agent's chat.

agents, the default: changes an agent or another fleet made. all:
your own changes from a client or the CLI as well. off: none. A
hand edit of this file names nobody and is never noted.

The note is the daemon's, recorded and never delivered: it starts no
turn and the changed agent is not told by it. A credential's value is
never shown, and nothing in an agent's env: block is.

### `orchestration.crosstalk_summary_model`

- Type: string
- Default: ``
- Since: 0.30.0

A small model that rewrites each cross-talk summary as one friendly
paragraph, as provider/model -- the same spelling as
orchestration.ack_check_model. Empty, the default, posts the summary
built from the record and asks no model anything.

The model is given the record-built summary, fenced as data, and 10
seconds. Nothing waits on it: the note is posted when it answers. A
failure, a timeout, an empty reply or one longer than 1200
characters posts the record-built summary instead, so turning this on
can delay a note and never lose one.

## `orchestration.mailbox`

Defaults for every agent's mailbox.

Each agent has a durable queue, so a message to a stopped agent waits
for it rather than failing. Any of these can be overridden per agent.

### `orchestration.mailbox.max_depth`

- Type: integer
- Default: `1000`
- Inside an agent: `mailbox.max_depth`
- Since: 0.1.0

How many undelivered items a mailbox holds before overflow applies.

### `orchestration.mailbox.overflow`

- Type: enum
- Values: reject, drop-oldest, block-sender
- Default: `reject`
- Inside an agent: `mailbox.overflow`
- Since: 0.1.0

What a full mailbox does: reject, drop-oldest or block-sender.

All three are noisy on purpose. A queue that silently discards is a
queue that loses work nobody notices until much later.

### `orchestration.mailbox.max_attempts`

- Type: integer
- Default: `5`
- Inside an agent: `mailbox.max_attempts`
- Since: 0.1.0

Delivery attempts before an item is dead-lettered rather than retried.

### `orchestration.mailbox.lease_seconds`

- Type: integer
- Default: `300`
- Inside an agent: `mailbox.lease_seconds`
- Since: 0.1.0

How long an agent has to acknowledge an item before it returns to the queue.

This is what makes delivery survive an agent dying mid-turn: the item
comes back rather than being lost or silently delivered twice.

### `orchestration.mailbox.default_ttl_seconds`

- Type: integer
- Default: `604800`
- Inside an agent: `mailbox.default_ttl_seconds`
- Since: 0.1.0

How long an undelivered item lives before it is swept. 0 means forever.

### `orchestration.mailbox.backoff_seconds`

- Type: integer
- Default: `30`
- Inside an agent: `mailbox.backoff_seconds`
- Since: 0.1.0

Base for the exponential retry backoff, doubling per attempt, with
jitter applied so a dependency that failed for every queued message does
not bring them all back at the same instant.

0 retries immediately.

### `orchestration.chief_of_staff`

- Type: string
- Default: unset
- Since: 0.1.0

Id of the agent that receives work addressed to the fleet.

At most one agent may hold this. Setting it here is equivalent to
chief_of_staff: true on that agent, and the daemon refuses to start if
two agents claim it.

### `orchestration.repeat_thresholds`

- Type: list of strings
- Default: `5,10,20`
- Since: 0.2.0

Repeat counts worth reporting when an agent keeps making the same
call.

A count lands on a threshold exactly once, so the fifth identical
call reports and the sixth does not. Reporting every repeat past a
floor turns a signal into noise, and noise is what gets skipped.

This one observes rather than intervenes. clawtilla does not own the
model's own tool-call loop, so it cannot steer a turn that is already
running: it counts the calls it serves, writes a note into the
thread, and at the highest threshold interrupts the turn.

An empty list turns the counting off.

### `orchestration.repeat_max_keys`

- Type: integer
- Default: `256`
- Since: 0.2.0

How many distinct calls one turn remembers while watching for
repeats.

Bounded on purpose: an unlimited set of unique arguments lets one
pathological turn grow the daemon for as long as it runs. The least
recently seen call is dropped first, and a dropped one starts
counting again from one.

### `orchestration.handoff_max_per_turn`

- Type: integer
- Default: `4`
- Since: 0.2.0

How many handoffs one turn may queue.

Small deliberately. A blocking ask gets backpressure for free
because the caller waits; an asynchronous handoff does not, so this
is the only thing standing between a confused chief of staff and a
fan-out of real turns that each cost money.

Counted as handoffs still *waiting*, not as a tally reset each turn.
Usually the same number, because a queue drains when the turn that
filled it ends -- and deliberately not the same when it does not: a
transfer still waiting for a busy recipient goes on counting, so an
agent with three stuck cannot queue four more on top of them.

The refusal says how many are waiting and suggests doing the piece
rather than queueing another. 0 removes the limit.

### `orchestration.handoff_busy_retries`

- Type: integer
- Default: `3`
- Since: 0.2.0

How many times a handoff waits for a busy assignee before giving up
and saying so.

Giving up is its own outcome rather than a failure: 'nobody was
free' and 'it went wrong' need different answers from whoever reads
the receipt.

These are tries, not seconds. A queued handoff is looked at every
time any turn in the fleet ends, so three tries means three turn
boundaries went by with the recipient still mid-turn. 0 waits
indefinitely, which is only sensible in a fleet where every turn
ends.

### `orchestration.handoff_receipt_days`

- Type: integer
- Default: `2`
- Since: 0.2.0

How long a finished handoff's receipt is kept.

A receipt says how one handoff went: queued, delivered, refused. The
task itself is kept for orchestration.task_retention_days; this is
the record of the move.

A count bounds the file as well as this age, because a fleet that
passes work around all day fills two days faster than a quiet one
fills two years. A handoff that has not run yet is never pruned
however old it is: it is undrained rather than finished. 0 keeps
receipts until the count alone drops them.

### `orchestration.review_rounds`

- Type: integer
- Default: `3`
- Since: 0.38.0

How many times a reviewer may send a task back for changes before the
operator decides.

Reviewed work goes back and forth between its assignee and its
reviewer (agents.review_by); past this many rounds of changes the task
stays in review and the verdict becomes the operator's, as a decision
in the inbox, rather than the two going round again. 0 removes the
limit.

### `orchestration.checkout_keep_days`

- Type: integer
- Default: `14`
- Since: 0.39.0

How long a finished task's checkout is kept, in days.

A task given a repository (`repo` on clawtilla_delegate or a new task)
works in a checkout of its own under its assignee's workspace, at
tasks/<task>, on the branch clawtilla/<task>. Once the task has been
finished this long the checkout is removed and its branch kept: a
worktree's branch is in the repository already, and a clone's is
fetched back into it first. A checkout with uncommitted changes is
never removed this way -- that is the operator's call, from the
task's Changes. 0 keeps every checkout.

### `orchestration.task_retention_days`

- Type: integer
- Default: `180`
- Since: 0.35.0

How long a finished task is kept, in days.

Every task is written to tasks.db in the state directory as it
changes, with a journal of what happened to it, so a daemon restart
keeps the list, a delegator can still ask what became of its work,
and a parent still knows its children. A task that has finished is
removed once it is older than this, with its journal; one that has
not finished is never removed however old it is, because somebody
may still be waiting on it. The clients' Tasks lists show the tasks
that have not finished and those that finished in the last day; the
rest are in the history (task.list with history: true, or
`clawtilla task list --history`). 0 keeps every task.

### `orchestration.max_waits_per_agent`

- Type: integer
- Default: `10`
- Inside an agent: `max_waits`
- Since: 0.15.0

How many pending waits one agent may have.

A wait (clawtilla_wait_until) is a condition clawtilla checks by code
-- a URL, a file, a task, the agent's computer, a command -- so the
model is woken once when it holds rather than on every look. A pending
wait costs no turns, but each one is a probe: a URL fetched once a
minute, a command run on the agent's computer. This bounds how many
of those one agent can leave running, and the refusal names its
pending waits so it can fold a new one into an old one.

Override with max_waits on an agent. 0 removes the limit.

## `orchestration.incidents`

Reports of runs that broke, sent to whoever leads the team.

A delegated task that failed or stalled, an agent that would not start
with work waiting for it, and a routine run that did not happen each
open an incident: one per run (the same task failing again updates it),
filed in a room per team called "Problems to look at", and delivered
as a report the recipient can act on with clawtilla_task_retry. A
recipient that is mid-turn has its report held until the turn ends.
See docs/orchestration.org, "Incidents".

### `orchestration.incidents.enabled`

- Type: boolean
- Default: `true`
- Since: 0.17.0

Whether broken runs open incidents at all.

Off, nothing is filed and nobody is told; tasks still fail and stall
exactly as before, and their delegators still get the settle notice.
Retrying a failed task works either way.

### `orchestration.incidents.to`

- Type: enum
- Values: lead, chief, none
- Default: `lead`
- Since: 0.17.0

Who an incident report goes to: lead, chief or none.

lead is the failing agent's team lead, or the chief of staff when the
team has none (or two). chief is always the chief of staff. none spends
nobody's turn: the report is recorded in the room for you to read.
Never the agent whose run broke, and never the agent that delegated
the task -- it already has the settle notice.
