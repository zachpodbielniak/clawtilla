# Configuration: `triggers`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `triggers`

Work started by something happening elsewhere.

A routine is a clock; a trigger is an event. Both end in the same
queued run against the same agent, so a trigger inherits the
routine's ordering behind a busy agent and its durable receipts
rather than growing a second set.

### `triggers.id`

- Type: string
- Default: unset
- Required: yes
- Since: 0.2.0

How the trigger is named on the command line and in the clients.

Not the address it is called on: that is a separate unguessable
endpoint, so renaming a trigger does not tell anybody where it
lives.

### `triggers.description`

- Type: string
- Default: unset
- Since: 0.2.0

What this trigger is for, in a line.

### `triggers.agent`

- Type: string
- Default: unset
- Required: yes
- Since: 0.2.0

Primary agent to run when it fires.

### `triggers.agents`

- Type: list of strings
- Default: unset
- Since: 0.2.0

Additional recipients for the same delivery. The primary agent runs first;
duplicates are removed in order. Every recipient has a separate durable
task and outcome. A batch must fit the trigger's max_pending slots in
full.

### `triggers.room`

- Type: string
- Default: unset
- Since: 0.2.0

Where the run reports, if not the agent's own conversation.

### `triggers.mode`

- Type: enum
- Values: run, post
- Default: `run`
- Since: 0.15.0

What a delivery that passed every filter turns into: run or post.

run starts the agent with the expanded instructions, which costs a
turn. post writes the instructions and the event into `room` as a
system message and runs nothing -- for the deploy that finished or
the monitor that fired, which somebody wants to see and no agent
needs to act on. A post wakes nobody and costs nothing.

post needs `room`: a trigger set to post with no room is refused at
load, and refuses every delivery until one is named.

### `triggers.max_pending`

- Type: integer
- Default: `4`
- Since: 0.15.0

How many runs this trigger may have unfinished at once, 1 to 50.

A push that fires a five-minute run, twenty times while the first is
still going, is twenty agents on one machine. A delivery past the cap
is answered 429, so the sender retries it later, and recorded as
dropped in the trigger's deliveries -- never silently. Every recipient
of a fan-out takes a slot. A value outside 1 to 50 written by hand is
read as the default, with a warning at load.

### `triggers.provider`

- Type: enum
- Values: generic, forgejo, gitea, github, gitlab, venture
- Default: `generic`
- Since: 0.2.0

Who is calling: forgejo, gitea, github, gitlab, venture or generic.

This decides how the delivery is authenticated, and the four forges
genuinely disagree. Forgejo and Gitea sign the body with HMAC-SHA256
and send the hex bare; GitHub sends the same digest behind a
`sha256=` prefix; GitLab sends the secret itself, verbatim, and
signs nothing.

Naming it matters because Forgejo also sends GitHub- and
Gitea-shaped headers for compatibility. Sniffing is a fallback for
when nobody said, and it can never widen what a configured trigger
accepts.

venture is VENTURE's own webhook: the same `sha256=` HMAC as GitHub
under X-Venture-Signature, so the webhook must be created with this
trigger's secret as its signing secret. Its record type fills
{{kind}}, the record's label {{title}}, its id {{number}} and the
user who made the change {{actor}}.

### `triggers.secret`

- Type: secret reference
- Default: unset
- Since: 0.2.0

The shared secret the caller proves it knows.

Shown once, when it is created or rotated, and never again -- not in
a listing, a log line, an event or a transcript. Rotating it stops
the old one working immediately.

### `triggers.events`

- Type: list of strings
- Default: unset
- Since: 0.2.0

Which event names are acted on. Empty means all of them.

An event outside the list is answered normally and recorded as
ignored, because a caller that gets an error for a delivery you
simply did not want will keep retrying it.

### `triggers.repo`

- Type: string
- Default: unset
- Since: 0.2.0

Only act when the delivery names this repository.

### `triggers.branch`

- Type: string
- Default: unset
- Since: 0.2.0

Only act when the delivery names this branch.

### `triggers.header`

- Type: string
- Default: unset
- Since: 0.2.0

For `provider: generic`, the header carrying the event name.

### `triggers.ignore_actors`

- Type: list of strings
- Default: unset
- Since: 0.9.3

Changes made by these users are recorded as ignored, not run.

The loop guard. An agent that writes what its own trigger watches --
a VENTURE ticket it updates, a branch it pushes -- is called back by
its own write, and the sender cannot tell: to it every write is an
ordinary user's change. Name the account the agent's credential
belongs to. Matched against {{actor}}, ignoring case.

## `triggers.match`

Fields the delivery's JSON body must contain, or it is ignored.

  match:
    - path: pull_request.title
      value: release
    - path: labels.name
      value: deploy

Up to 10 pairs, and all of them must match. The path is dotted; a
number indexes an array (`commits.0.message`), and a name on an array
looks in every element (`labels.name`). The value must be contained in
what the path holds, ignoring case. Checked before anything is queued:
a miss is recorded in the deliveries as ignored, naming the pair that
failed, and starts no run. Empty, or null, means no filter.

### `triggers.match.path`

- Type: string
- Default: unset
- Required: yes
- Since: 0.15.0

Where in the JSON body to look, dotted, such as `repository.name`.
No spaces, no `=` and no empty part; a bad path is refused at load.

### `triggers.match.value`

- Type: string
- Default: unset
- Required: yes
- Since: 0.15.0

Text the field must contain, ignoring case. An object or array is
searched as its JSON text.

### `triggers.replay_connector`

- Type: string
- Default: unset
- Since: 0.9.3

venture: the connector whose credential reads the business-event
feed, so deliveries missed while the daemon was down are caught up.

The name of a `venture` connector integration bound to this
trigger's agent. Its token must belong to an owner or administrator
of the organization, which is who VENTURE lets read the feed. The
feed is read every poll interval and replayed through the same
deduplication as a pushed delivery, on the event's stable id, so a
delivery that arrives both ways runs once.

The first read only finds where the feed ends: replaying an
organization's whole history into an agent is never what somebody
turning this on meant. Only the durable lane (`business_events` on
the VENTURE webhook) has a feed; ordinary record webhooks are never
retried by VENTURE and cannot be caught up.

### `triggers.organization_id`

- Type: integer
- Default: `0`
- Since: 0.9.3

venture: which organization's business events to replay.

The `organization_id` VENTURE scopes every request by. 0 sends none,
which VENTURE answers for the token's default organization.

### `triggers.instructions`

- Type: string
- Default: unset
- Required: yes
- Since: 0.2.0

What the agent is asked to do, with the event fenced beneath it.

Placeholders -- {{event}}, {{repo}}, {{ref}}, {{actor}}, {{title}},
{{url}}, {{number}}, {{kind}} -- are expanded by hand rather than by a format
function, because this string comes out of a configuration file and
a stray percent sign must be a percent sign.

The payload arrives marked as untrusted, and the agent is told to
read it as data: a webhook body is somebody else's text.

### `triggers.enabled`

- Type: boolean
- Default: `true`
- Since: 0.2.0

Whether it fires.

A new trigger starts unverified: the first authenticated delivery is
captured and shown to you rather than run, so you can see what the
caller actually sends before an agent acts on it.

### `triggers.directory`

- Type: path
- Default: unset
- Since: 0.2.0

Where the run should work, if somewhere particular.

### `triggers.worktree`

- Type: boolean
- Default: `false`
- Since: 0.2.0

Run in a fresh checkout of `directory` rather than in it.

Since 0.39.0 clawtilla makes the checkout for each run, as for
routines.worktree: in the agent's workspace at tasks/<task>, on the
branch clawtilla/<task>, and tells the agent where it is above the
trigger's payload. The run fails if it cannot be made.

### `triggers.isolate`

- Type: boolean
- Default: `false`
- Since: 0.2.0

Give the run a conversation of its own rather than the agent's.
