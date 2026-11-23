# Configuration: `rooms`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `rooms`

Standing rooms, created at startup if they do not exist.

A room is a conversation with members. Posting into one enqueues to
every member's mailbox. A direct chat is just a room with two members,
so there is one mechanism rather than two.

### `rooms.id`

- Type: string
- Default: unset
- Required: yes
- Since: 0.1.0

Identifier used to address the room.

### `rooms.name`

- Type: string
- Default: unset
- Since: 0.1.0

Display name.

### `rooms.members`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Agent ids in the room. The human is always implicitly a member.

### `rooms.require_mention`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Only deliver messages that name a member.

Defaults to the room's shape when it is not set: off for two members,
where everything said is for the other one, and on for three or more,
where delivering every remark to everybody costs a model turn each
and is never what anybody meant. Setting it explicitly wins either
way.

A member is named as @its-id or as a bare word; matching ignores
case, and an agent's display name counts when it is a single word.
@all reaches everybody and belongs to you -- an agent writing it
reaches nobody, because one broadcast per reply is how a room of five
turns one remark into a runaway.

A message that names nobody is still recorded and everyone can read
it. That is the ordinary way to take part, and each delivery says so.

### `rooms.max_hops`

- Type: integer
- Default: unset
- Since: 0.1.0

Overrides orchestration.max_hops for this room.

It may loosen the fleet limit as well as tighten it, which is the
whole reason it exists: in a conversation three agents each reply one
hop deeper, so an ordinary standup reaches the fleet ceiling on its
own, and the only other remedy is raising orchestration.max_hops for
every delegation chain in the fleet to fix one room.

It reaches the hop count and nothing else. The rate limit, the task
budget and the cycle detector are untouched, so a loop that costs
money still has three limits on it.

In a group room this is the backstop rather than the limit that
fires: rooms.max_chain ends a thread of replies first, and the depth
starts again at each round.

### `rooms.max_chain`

- Type: integer
- Default: `3`
- Since: 0.20.0

How many replies one thread may run to in a round of a group room.
0 removes the cap; the once-per-thread rule below still applies.

A round starts at a message from you, a routine or a trigger. Each
member that answers it starts a thread, and an @mention in a reply
carries the thread on. Within one thread a member answers once: a
mention of a member already in that thread is recorded in the room
and not delivered, with the reason shown beside it. A member that
spoke in another thread of the same round still answers.

Without this the hop count built up across a room's history, so an
ordinary standup reached max_hops by itself. The depth now starts
again at every round, and max_hops stays as the backstop.

### `rooms.turn_timeout_seconds`

- Type: integer
- Default: `0`
- Since: 0.2.0

How long one member may hold a room's turn before it is yielded.
0, the default, turns it off.

Off for the same reason as agents.runtime.turn_timeout_seconds, and
the five minutes it used to default to were worse: an agent doing
real work in the operator's own room was stopped mid-turn and the
room stalled, five minutes in, and the notice explaining it read as
though the agent had misbehaved.

Set a number for a busy shared room, where the point is that one
member must not hold the floor indefinitely. Counted in work, not
wall time: the clock holds while the turn is parked on an open
decision and resumes with what is left. Stopping a turn under a
question nobody has answered manufactures a stranded decision, which
the daemon then has to repair.

Different from the runtime's turn timeout: that one catches a wedged
worker, this one catches a wedged conversation.

Anything below 60 is raised to 60.

### `rooms.order`

- Type: integer
- Default: unset
- Since: 0.2.0

Where this room sits in the sidebar, among the agents.

The same scale as agents.order, because a client draws one list and
a room can sit beside the agents it concerns. Written by the clients
when a room is dragged or moved, numbered from one in steps of ten so
there is room to hand-place something between two.

### `rooms.team`

- Type: string
- Default: unset
- Since: 0.2.0

Which team's group this room appears under in the sidebar.

Presentation and nothing else: it does not change who is in the room
or who a message reaches. A room with no team sits with the agents
that have none.

### `rooms.catchup_messages`

- Type: integer
- Default: `20`
- Since: 0.2.0

How much of a room a member is caught up on when it is named.

In a room that requires mentions an agent only receives the messages
that named it, so without this it has no idea what was being
discussed -- the transcript holds the conversation and the model does
not. Each delivery carries the messages since that member last heard
from the room, capped here, with a count of anything dropped.

0 turns it off. Anything older is clawtilla_room_history, which the
delivery preamble names.
