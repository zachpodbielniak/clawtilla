# Configuration: `agents`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `agents`

The fleet.

Each entry is one libreclaw agent: its persona, its model, its
credentials, its integrations and its computer. This file is the source
of truth -- the daemon renders each agent's libreclaw config.yaml into
its workspace from what is here, and regenerates it when this changes.

An agent whose block cannot be understood becomes a shadow: it is
listed, it says why, and it refuses to run. That is what lets a config
written by a newer clawtilla round-trip through an older one without
losing anything.

### `agents.id`

- Type: string
- Default: unset
- Required: yes
- Since: 0.1.0

Identifier used everywhere else to address this agent. Must be unique.

### `agents.name`

- Type: string
- Default: unset
- Since: 0.1.0

Display name shown in the clients and used as the agent's own name.

### `agents.description`

- Type: string
- Default: unset
- Since: 0.1.0

One line about what this agent is for.

Other agents see it when they list the fleet, so it is how a
chief-of-staff decides who to delegate to. Worth writing properly.

### `agents.enabled`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Whether this agent may run at all.

### `agents.chief_of_staff`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Marks the agent that receives work addressed to the fleet.
At most one agent may hold this.

### `agents.color`

- Type: string
- Default: unset
- Since: 0.1.0

Accent colour in the clients, as a hex string.

### `agents.face`

- Type: string
- Default: unset
- Since: 0.53.0

The generated face the agent wears when it has no profile picture.

Unset, the agent gets a shape picked from its id -- the same one in
every client, every time. Set it to choose: pebble, cloud, cushion,
clover, star, bean, flower, drop, gem, petal, puff, squircle, wedge
or bloom. initials keeps the letter disc instead. The face is
painted in agents.color, and a profile picture always takes its
place. An unknown name is a warning and the automatic face is used.

### `agents.order`

- Type: integer
- Default: `0`
- Since: 0.1.0

Where this agent sits in the list, lowest first.

Set by dragging a row in the clients rather than written by hand,
though there is nothing wrong with writing it. Agents sharing a
number keep the order they appear in this file, so leaving them all
at 0 changes nothing.

### `agents.review_by`

- Type: string
- Default: unset
- Since: 0.38.0

Who checks this agent's work before it counts as done.

`lead` (its team's lead, or the chief of staff when the team has
none), `delegator` (whoever handed it the task), `operator` (you,
from the decision inbox or any client's task page), an agent id, or
empty for no review. When the agent finishes a task it goes to
`review` with the result instead of `completed`, and only an accept
completes it; asking for changes sends it back with the notes, and
after orchestration.review_rounds of that the operator decides. A
delegation's own `review:` and an operator task's `review` override
this. The reviewer is never the agent itself: a setting that would
make it so means no review.

### `agents.team`

- Type: string
- Default: unset
- Since: 0.1.0

Which team this agent belongs to, by teams.id.

Unset means no team: the agent still messages and is messaged, it is
simply not grouped and no team lead may assign to it.

### `agents.broker_server`

- Type: string
- Default: unset
- Since: 0.24.0

This agent's own gateway server on the broker.

For anythingmcp, the id of an MCP server made in its dashboard (the
last part of the server's MCP URL); the connectors assigned to that
server are this agent's permission, and clawtilla reaches it as the
connector broker-agent-<id>. For jentic, the Jentic agent this one
acts as, whose API key `clawtilla broker key <id>` stores. Unset
means the team's teams.broker_server, or no broker at all. An agent
with runtime.network gated or none cannot reach the gateway on
loopback and is not given it.

### `agents.team_role`

- Type: enum
- Values: member, lead
- Default: `member`
- Since: 0.1.0

Standing within that team.

A lead may hand work to the members of its own team and to nobody
else. A member may message, ask and share a room with anyone --
handing something over in conversation is not assigning it -- but may
not delegate at all.

At most one lead per team; a second is refused rather than picked
between. The chief of staff outranks every lead and needs no team.

### `agents.avatar`

- Type: path
- Default: unset
- Since: 0.1.0

The agent's profile picture. Optional, and usually unnecessary.

A file called profile-picture.png, .jpg, .jpeg or .webp in the
agent's own directory is found without being configured, which is
the expected way to set one: drop the file in and it appears. This
key is for pointing somewhere else, and is resolved against the
workspace when it is relative.

A path named here that does not exist warns and falls back to the
initials, rather than falling back silently -- a mistyped path and
no picture at all otherwise look identical.

The picture reaches a client as bytes, never as a path, because a
client may be running on another machine.

### `agents.skills`

- Type: list of strings
- Default: unset
- Since: 0.2.0

Skills this agent gets, on top of its team's and the fleet's.

Named, not paths: a skill lives once under `skills.dir` and is
linked into the workspace at whatever path this agent's provider
reads. A selector naming a skill that does not exist warns rather
than silently reaching nobody.

An imported skill arrives disabled and stays that way until somebody
has read it, so assigning one is not the same as the agent having it.
Both states are reported.

### `agents.workspace`

- Type: path
- Default: unset
- Since: 0.1.0

Workspace directory. Defaults to defaults.workspace_root/<id>.
Scaffolded on first start if it does not exist.

## `agents.persona`

Who the agent is.

### `agents.persona.system_prompt`

- Type: string
- Default: unset
- Since: 0.1.0

System prompt, inline. Mutually exclusive with identity_files.

### `agents.persona.identity_files`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Files in the workspace concatenated into the system prompt, in order.

Leave it unset to get the standard set, which is what every workspace
is scaffolded with: SOUL.org for character, IDENTITY.org for role,
USER.org for who you are, AGENTS.org for how to work, TOOLS.org for
what the agent has, TOOL_GOTCHAS.org for what has already bitten it,
PROJECTS.org for the work. Splitting them means one can be edited
without rewriting the rest -- `clawtilla agent edit <id>` opens them.

Set it to take control of the list: to reorder, to drop one, or to add
a file of your own. An inline system_prompt replaces the lot.

### `agents.prompt_suffix`

- Type: string
- Default: unset
- Since: 0.1.0

Text appended to every turn this agent runs.

For a rule that has to hold on turn 200 as much as turn 1. A resumed
session never re-receives the system prompt, and a long conversation
drifts away from anything said only once at the start.

clawtilla already adds one of its own for an agent with a computer,
naming the container or VM and telling it to run commands there
rather than on the host. Yours is appended to that, not instead of
it.

Costs tokens on every turn, so keep it to rules that genuinely have
to be repeated -- anything else belongs in the agent's org files.

## `agents.memory`

The agent's long-term memory file.

MEMORY.md in the workspace is loaded into the system prompt every turn,
under a budget. memory/<topic>.md files are read on demand by the agent
instead, so a large memory does not cost every turn.

### `agents.memory.enabled`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Whether MEMORY.md is loaded into the prompt.

Not implemented in this build. No memory block is rendered into the
agent's config.yaml at all, so MEMORY.md is loaded on libreclaw's own
defaults and this cannot turn it off.

### `agents.memory.max_lines`

- Type: integer
- Default: `200`
- Since: 0.1.0

Line budget for MEMORY.md. Beyond it the file is truncated with a note.

Not implemented in this build, for the same reason as memory.enabled:
the budget never reaches libreclaw, which applies its own.

### `agents.memory.max_bytes`

- Type: integer
- Default: `24000`
- Since: 0.1.0

Byte budget for MEMORY.md, applied alongside max_lines.

Not implemented in this build, for the same reason as memory.enabled:
the budget never reaches libreclaw, which applies its own.

## `agents.model`

Which model this agent thinks with.

### `agents.model.provider`

- Type: string
- Default: unset
- Since: 0.1.0

Provider name. Defaults to defaults.provider.

### `agents.model.model`

- Type: string
- Default: unset
- Since: 0.1.0

Model name. Defaults to defaults.model.

### `agents.model.effort`

- Type: string
- Default: unset
- Since: 0.1.0

Reasoning effort: low, medium, high, xhigh or max, when the model has one.

### `agents.model.command`

- Type: string
- Default: unset
- Since: 0.28.0

The command line an acp agent is started from, e.g.
"gemini --experimental-acp". Only read when model.provider is acp.

acp runs any program that speaks the Agent Client Protocol (v1,
JSON-RPC over stdio) as the agent's engine, so which program is this
setting. It is split like a shell would split it, with no shell: no
pipes, no variables. Left empty, libreclaw falls back to
$AI_ACP_COMMAND in the agent's environment, and a turn with neither
fails saying so.

The model is the agent's own default: ACP has no request that picks
one, so model.model is not sent and the program's own configuration
(or its command line) chooses.

### `agents.model.account`

- Type: string
- Default: unset
- Since: 0.2.0

Which entry of accounts this agent's CLI logs in as. Unset is the CLI's
default login. The account's provider must be this agent's provider.

### `agents.model.routing_profile`

- Type: string
- Default: unset
- Since: 0.1.0

Reserved. Not implemented in this build: nothing reads it, and
nothing will until it can mean something here.

libreclaw's smart routing (off, eco, auto, premium, free) picks a
model per message by scoring the prompt against a price table of
named providers. An agent here runs one CLI backend with one model,
so there is nothing for a profile to choose between -- rendering it
would switch on a router with a single candidate. Moving off a
backend that refuses is model.fallbacks; a cheaper model for one
scheduled job is routines.model.

## `agents.model.fallbacks`

Where this agent's conversations continue, in order, when its own
backend refuses a turn: a usage limit, a rate limit, a sign-out or
an outage.

  fallbacks:
    - provider: codex-cli
      model: gpt-5.5
    - provider: claude-code
      model: sonnet

libreclaw does the failing over: the next turn runs on the first
entry nothing is holding, and the agent returns to its own backend
at the reset the provider named, or after a backoff when it named
none. A move to another CLI or another login starts a fresh
conversation there; the agent's own conversation resumes when it
comes back. Each switch raises an agent.failover event and alert,
and agent status shows the fallback until the agent is back.

Another sign-in to the same subscription is refused: an entry on
this agent's CLI (claude-code and claude-tmux are one Claude login)
with a different account, unless that account has pay_per_use set.
Rotating accounts to get past a usage limit breaks the provider's
terms. The agent is a shadow with the reason until the list is
fixed, and agent set refuses such a list where it arrives.

On the command line the list is one string:
`clawtilla agent set ID model.fallbacks "codex-cli:gpt-5.5,
claude-code:sonnet@backup"` -- provider:model, then @account.

### `agents.model.fallbacks.provider`

- Type: string
- Default: unset
- Required: yes
- Since: 0.16.0

The agent CLI: one of the providers model.provider takes.

### `agents.model.fallbacks.model`

- Type: string
- Default: unset
- Required: yes
- Since: 0.16.0

The model that CLI is asked for.

### `agents.model.fallbacks.account`

- Type: string
- Default: unset
- Since: 0.16.0

Which entry of accounts this fallback's CLI logs in as. Unset is the
CLI's default login. The account's provider must be this entry's
provider.

### `agents.steer`

- Type: enum
- Values: never, delegator, peers, all
- Default: `delegator`
- Since: 0.28.0

Which messages may be handed to the turn this agent is already
running, instead of waiting to become a turn of their own.

Without steering, a message for a busy agent waits and becomes its
next turn: a correction ("stop, use the staging DB") arrives after
the work it was meant to change has finished, and a follow-up costs a
whole extra turn that rereads everything. A steered message reaches
the running turn at the next point its engine accepts input -- before
the next model call -- and the turn's closing answer answers it too.
Nothing is killed or restarted.

never: nothing steers. delegator, the default: a message steers when
its sender handed out the work the running turn is on -- the agent
that turn is answering, or the delegator of the task it is working.
peers: as delegator, and any other agent's message in the same room.
all: as peers, and your own message in your conversation with this
agent (otherwise held and sent when the turn ends).

No level lifts these: a message never steers into another room's
turn; never into a consultation unless the consulting agent is asking
again, and a consultation never steers into a turn that can act; a
result held for the end of a turn, a message naming another task or
a model of its own, and anything in a group room take turns of their
own. A steered message is still a hop and still passes the loop guard
when it is sent.

Only some engines can be steered: Claude Code, claude-tmux and codex
on libreclaw's ai.codex.transport app-server. On any other the agent
says no and the message waits as before. Needs libreclaw 0.30.0 or
later; an older agent is never asked.

## `agents.session`

How this agent's conversations map to AI sessions.

### `agents.session.routing_mode`

- Type: enum
- Values: sender-room, room, agent
- Default: `sender-room`
- Since: 0.2.0

How many context windows this agent is.

sender-room, the default, gives every (room, sender) pair a session
of its own: the conversation with the operator, the conversation
with each peer, each a separate AI context that shares nothing with
the others. room drops the sender, so everybody in one room shares
a session. agent is one session for everything -- every room, every
sender, one conversation.

The default suits an agent that serves several people and should
keep them apart. It is the wrong shape for an orchestrator: a chief
of staff told "only use oryx" in the operator's room has never
heard that constraint when a peer's reply wakes the sibling session,
and what looks like one agent contradicting itself is three contexts
that have never met. Give such an agent agent mode. The cost is
concurrency -- one session runs one turn at a time, so three busy
rooms take turns -- and the mode changes every session key, so each
conversation starts a fresh context once when it is switched.

The values are libreclaw's own (its session.routing_mode); the
daemon renders this straight through.

The default is role-dependent: an agent marked chief_of_staff --
on the agent or through orchestration.chief_of_staff -- or given
team_role: lead defaults to agent mode, because an orchestrator's
job assumes the shape. An explicit value here always wins, and
dropping the role restores the ordinary default; toggling a role
never writes this key on your behalf.

A member of a room with more than two members defaults to room
mode instead, and sender-room is refused for one. That mode gives
an agent a session per speaker in the same room, and every piece
of the daemon's per-room turn state is keyed on the room alone --
the typing indicator carries the room and not the session, so two
such turns cannot be told apart: the second to start would run
holding the first one's depth and origin, and the first to finish
would settle both. It is reported and replaced with room rather
than refused, because refusing would take the agent out of the
fleet over a setting.

### `agents.session.timeout_minutes`

- Type: integer
- Default: `30`
- Since: 0.7.5

Minutes of inactivity before libreclaw drops this agent's session.

The next message after the timeout starts a fresh context rather
than continuing the old one. -1 means never expire: the session
lives as long as the agent does, and the tradeoff is one context
that only ever grows until the provider compacts it or somebody
runs /reset. 0 and anything below -1 are meaningless to libreclaw's
own session manager, which takes only a positive number of minutes
or -1: `clawtilla config validate` rejects such a value, and if one
reaches the daemon anyway it is reported with a warning and
replaced with 30 in the rendered config.

This is libreclaw's own value (its session.timeout_minutes); the
daemon renders this straight through.

## `agents.runtime`

How this agent's libreclaw instance is hosted.

### `agents.runtime.type`

- Type: enum
- Values: process, embedded
- Default: `process`
- Since: 0.1.0

process or embedded.

process supervises a real libreclaw child: crashes stay contained, the
environment and credentials are genuinely separate, and the agent can
live inside its own container. embedded runs it inside the daemon,
which is cheaper and is what an in-process host such as cmacs wants, at
the cost of sharing a fate with every other embedded agent.

### `agents.runtime.confine`

- Type: enum
- Values: none, bwrap
- Default: unset
- Since: 0.14.0

Run this agent's own CLI inside a bubblewrap sandbox: none or bwrap.
Defaults to defaults.confine.

computer.host.confine governs clawtilla_computer_exec and nothing
else. The agent's own Bash, Read and Write belong to the AI CLI that
libreclaw spawns, and without this they reach the whole filesystem as
the daemon's user. bwrap wraps the libreclaw child -- and so the CLI,
every command it runs and its MCP servers -- in one sandbox that
binds the workspace and the agent's state directory read-write, the
CLI's login directory read-write (it refreshes its OAuth tokens there),
the agent's mounts as declared, the programs its .mcp.json names
read-only, and the daemon's sockets. /usr and /etc are read-only, /tmp
and $HOME are private and empty apart from those binds, and TMPDIR,
HOME and XDG_* are set by the sandbox -- an agent's env: cannot point
them somewhere else. The network is left alone.

Only runtime.type: process can be confined; bwrap with embedded
disables the agent. bubblewrap missing disables it too, with the
reason, and it is never started unconfined instead. Takes effect at
the agent's next start.

### `agents.runtime.confine_paths`

- Type: list of strings
- Default: unset
- Since: 0.14.0

More paths to bind read-only into a confined agent's sandbox.

The sandbox finds libreclaw, the AI CLI and the programs in .mcp.json
for itself. A CLI installed somewhere it cannot follow -- a node
under ~/.nvm, a tool a skill shells out to -- is named here. Read-only
on purpose: something the agent should write is a mount, which says
so in its mode. Ignored unless runtime.confine is bwrap.

### `agents.runtime.network`

- Type: enum
- Values: open, gated, none
- Default: `open`
- Since: 0.17.0

Where this agent's own process may connect: open, gated or none.

open shares the machine's network, LAN and tailnet included. gated
gives the sandbox no network of its own: its only route is
clawtilla's egress gate, an HTTP proxy reaching public addresses only
(plus egress.allow), handed to the CLI as HTTPS_PROXY and HTTP_PROXY.
none gives it no route at all.

Under gated or none, provider keys whose CLI can be pointed elsewhere
(ANTHROPIC_API_KEY for Claude Code, OPENAI_API_KEY for Codex) are
brokered: the agent holds a token for its own process, and the daemon
swaps it for the real key on the way to a fixed upstream. Others are
passed as before and listed as unbrokered. An OAuth login is
unaffected.

Anything but open needs runtime.confine: bwrap -- outside the sandbox
a process can ignore a proxy -- and without it the agent is disabled
with that reason, never run on an open network instead. Takes effect
at the agent's next start.

### `agents.runtime.autostart`

- Type: boolean
- Default: unset
- Since: 0.1.0

Start with the daemon. Defaults to defaults.autostart.

### `agents.runtime.restart`

- Type: enum
- Values: never, on-failure, always
- Default: unset
- Since: 0.1.0

never, on-failure or always. Defaults to defaults.restart.

### `agents.runtime.backoff_seconds`

- Type: integer
- Default: `5`
- Since: 0.1.0

Base delay before a restart, doubling on each consecutive failure.

### `agents.runtime.max_restarts`

- Type: integer
- Default: `10`
- Since: 0.1.0

Consecutive failures before the agent is left in error rather than
restarted again. 0 means never give up.

### `agents.runtime.stream_steps`

- Type: boolean
- Default: unset
- Since: 0.2.0

Report this agent's steps while its turn runs. Defaults to
defaults.stream_steps.

### `agents.runtime.process_timeout_ms`

- Type: integer
- Default: `0`
- Since: 0.2.0

Maximum elapsed milliseconds for one AI CLI subprocess, including
streaming turns. 0, the default, disables the limit so an agent can
work for several hours. Set a positive integer to opt into a deadline.
Rendered explicitly as libreclaw ai.process_timeout_ms for every agent.
The tmux provider uses its own command and inactivity bounds instead.
This is a duration limit; runtime.turn_timeout_seconds separately
controls the turn watchdog. Values must be between 0 and 2147483647.

### `agents.runtime.idle_stop_hours`

- Type: integer
- Default: `0`
- Since: 0.8.0

Stop this agent after this many hours without a turn. 0 never does.

An idle agent still holds its process, its ports and its computer.
Stopped this way it is woken again by the next message queued for
it -- from you, a routine, a trigger or another agent -- so sleeping
loses no conversation; the message waits in its mailbox while it
starts. An agent that has never taken a turn is measured from when
it started. Stopping it by hand is not this: it stays stopped.

### `agents.runtime.turn_timeout_seconds`

- Type: integer
- Default: `0`
- Since: 0.2.0

How long a turn may go without producing anything before it is
cancelled. 0, the default, disables it.

Off by default because the number was always a guess, and the guess
was wrong in the expensive direction: a twenty-minute ceiling killed
real work -- a long refactor, a slow test suite -- and reported it
as a wedged turn. Every step an agent takes now counts as activity,
so a turn that is working says so continuously and a person can see
a stalled one rather than having a timer guess for them.

Set a number to bring the watchdog back. It counts activity, not
duration: a turn may legitimately run for an hour while steps keep
arriving, and one that has produced nothing at all for this long is
wedged. A turn parked on a decision is exempt, because waiting for a
person is not a stall.

Watched in two places, on purpose. The daemon watches activity and
publishes turn.timed_out; the same number is rendered into the
agent's own session.watchdog_timeout_seconds. That inner watchdog
measures total duration, even while output arrives or a decision is
pending; a positive value therefore also caps the whole turn -- at
this number plus runtime.busy_max_seconds while the busy check
(runtime.busy_cpu_percent) is on, and not at all when that ceiling
is 0.

### `agents.runtime.first_reply_seconds`

- Type: integer
- Default: `20`
- Since: 0.26.0

How long a turn may show nothing at all -- no step, no thought, no
reply -- before the clients offer to stop it or to run the same
message on another model. 0 never offers.

Nothing is stopped by this; it is an offer, shown beside the
conversation and withdrawn the moment the turn shows anything. It
needs runtime.stream_steps, since a turn whose steps are not
streamed is silent until it answers whether or not it is working.

Silence here is what the daemon can see, which is less than what the
CLI is doing: libreclaw does not forward the CLI's own status lines,
and holds prose until a tool call or the end of the turn. So the
offer says the turn has shown nothing, never that it is stuck. The
models offered are the model.fallbacks entries on the agent's own
CLI and login, since one turn's model is a name on the CLI it is
already running.

### `agents.runtime.busy_cpu_percent`

- Type: integer
- Default: `5`
- Since: 0.26.0

How much CPU the agent's processes must use, in percent of one core,
for a silent turn to count as working. 0 turns the check off.

A turn running one long command -- a test suite, a build -- reports
nothing between the tool starting and the tool finishing, so
runtime.turn_timeout_seconds would read forty minutes of compiling as
a wedged turn. Each watchdog sweep (every five seconds) reads the CPU
time of the agent's process tree from /proc; a tree that used at
least this much since the last sweep counts as a sign of life. A
command waiting on the network uses none and is still stopped.

Only for runtime.type: process. An embedded agent shares the daemon's
process, so its tree cannot be told apart from the fleet's and the
check does nothing there. Nothing is read while
runtime.turn_timeout_seconds is 0.

### `agents.runtime.busy_max_seconds`

- Type: integer
- Default: `7200`
- Since: 0.26.0

The longest a busy process tree may keep a silent turn alive, in
seconds since the turn last reported anything. 0 is no ceiling.

A command stuck in a loop uses CPU too, and without a ceiling it would
hold its turn for ever. Past this many seconds with no step, message
or tool call the CPU stops counting and runtime.turn_timeout_seconds
applies as it would to any silent turn.

## `agents.computer`

What the agent can run commands on.

### `agents.computer.use`

- Type: string
- Default: unset
- Since: 0.5.0

Share a machine declared under computers: instead of having one of
this agent's own. Set, every other computer.* key comes from that
entry and this agent's own computer block is ignored, so several
agents naming one entry run on exactly one machine.

### `agents.computer.type`

- Type: enum
- Values: none, host, container, vm, distrobox, ssh, display
- Default: unset
- Since: 0.1.0

Which kind of computer this agent gets. Defaults to
defaults.computer. The permitted values are listed above, from the
schema itself -- the copy that used to be written out here had
already gone stale by omitting distrobox.

Desktop control is not a type -- it is the desktop block below, and
works alongside any of these.

### `agents.computer.start`

- Type: enum
- Values: on_agent_start, on_first_use
- Default: `on_agent_start`
- Since: 0.19.0

When the machine is started: with the agent, or the first time
something runs in it.

on_agent_start brings the computer up before the agent's first turn,
so a broken image or a dead libvirtd stops the agent at once, with the
reason, instead of a turn later. That is the default, and what every
agent did before this key existed.

on_first_use leaves it down until the first clawtilla_computer_exec,
`computer exec`, wait condition or desktop call. The agent's own bash,
read and write run on the host either way, so an agent that mostly
talks, plans or delegates never pays for a VM it does not touch.
Callers that arrive while it is coming up share the one start, and
the time it takes is not counted against the command's timeout.

Applies to container, distrobox, vm and ssh computers; a host computer
is a local check and always starts with the agent. A computer stopped
by hand under a running agent is started again by its next use.

An agent with a VM desktop is the exception in practice: its MCP
client asks for the desktop when the agent starts, and that request
is a use.

### `agents.computer.exchange`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Whether the shared exchange directory is visible to this agent.

On by default, because otherwise every pair of agents that wants to
hand a file across needs its own hand-wired mount. Turn it off for an
agent that should not see what the others are passing around.

### `agents.computer.workspace`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Whether the agent's own workspace is mounted into its computer at
/mnt/clawtilla/workspace.

On by default. The workspace holds the agent's persona, its notes and
its MEMORY.md, and without this it exists only on the host -- so the
agent reads the files that describe it with one set of tools and
works in a machine that cannot see them, and anything it writes there
from inside goes somewhere the host never looks.

Turn it off for an agent whose computer should hold nothing of
clawtilla's.

### `agents.computer.default_mounts`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Whether this agent gets the fleet's shared folders from
defaults.mounts.

On by default -- that is what makes them defaults. Turn it off for an
agent that should see only what it declares itself, which is the
point of giving one a container in the first place.

Declining all of them is different from replacing one: an agent that
declares its own mount at the same target already wins there, with
no need to turn the rest off.

## `agents.computer.mounts`

Host paths shared into the computer.

  mounts:
    - source: "~/src/projects"
      target: "/work/projects"
      mode: rw
      relabel: shared

For a container these become bind mounts; for a VM, virtiofs shares;
for a host computer they are the confinement allowlist itself.

### `agents.computer.mounts.source`

- Type: path
- Default: unset
- Since: 0.1.0

Host path to share. Not needed for tmpfs.

### `agents.computer.mounts.target`

- Type: path
- Default: unset
- Required: yes
- Since: 0.1.0

Absolute path inside the computer. Must not overlap another mount.

### `agents.computer.mounts.mode`

- Type: enum
- Values: ro, rw
- Default: `ro`
- Since: 0.1.0

ro or rw. Read-only is the default: an agent that only needs to read
your notes should not be able to rewrite them.

### `agents.computer.mounts.type`

- Type: enum
- Values: bind, volume, virtiofs, 9p, tmpfs
- Default: `bind`
- Since: 0.1.0

bind, volume, virtiofs, 9p or tmpfs.

Usually leave this alone: bind for containers and virtiofs for VMs are
chosen automatically from the computer type.

### `agents.computer.mounts.relabel`

- Type: enum
- Values: none, shared, private
- Default: `shared`
- Since: 0.1.0

SELinux relabelling for a container bind mount: none, shared or private.

This matters on Fedora Silverblue and friends, where an unlabelled bind
mount is visible in the container but every access is denied -- which
reads like a permissions bug rather than a labelling one.

shared (:z) is the default because it is what makes a shared folder
work. It used to default to none, which meant every mount anyone
declared failed with permission denied on an SELinux system while
this very text said shared was usually what you wanted. On a machine
without SELinux it does nothing.

private (:Z) rewrites the host directory's labels exclusively for this
container, which will break anything else using that directory. none
leaves the labels alone, which is right when the source is already
labelled for container access.

### `agents.computer.mounts.create`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Create the source directory if it is missing, rather than failing.

### `agents.computer.mounts.size`

- Type: string
- Default: unset
- Since: 0.1.0

Size for a tmpfs mount, e.g. "512M".

### `agents.computer.mounts.required`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Fail to start when the source is missing, rather than warning and
carrying on without it.

## `agents.computer.host`

Settings for computer.type: host -- the real machine clawtilla runs on.

This is the most useful and the most dangerous backend. Read
docs/security.org before enabling it: what each confinement mode
actually prevents, and what none of them do, is spelled out there.

### `agents.computer.host.confirm_host_control`

- Type: boolean
- Default: `false`
- Warning: hands over real authority; read security.md first
- Since: 0.1.0

Required before a host computer will start.

There is no default that makes this safe, so there is no default that
skips it. The daemon refuses to start the agent without it.

### `agents.computer.host.confine`

- Type: enum
- Values: none, workspace, allowlist, bwrap
- Default: `workspace`
- Since: 0.1.0

How much of the machine the agent may touch.

  workspace  cwd pinned to root; every path in the command line is
             canonicalised and rejected if it escapes
  allowlist  root plus allow_paths, minus deny_paths
  bwrap      a real kernel sandbox via bubblewrap
  none       no restriction at all

workspace and allowlist are enforced by inspecting the command before
running it, which closes '..' and symlink escapes together. They do not
stop a program that opens paths itself once it is running. Only bwrap
does, because only bwrap involves the kernel.

All of it applies to clawtilla_computer_exec. The agent's own bash,
read and write tools are its CLI's, and that CLI is not wrapped: they
reach the whole filesystem as the user running the daemon whatever
this is set to. That is true of every computer type -- only exec
enters the computer -- but host is the type where the setting reads
like a boundary around the agent rather than around one tool.

If bwrap is asked for and not installed, the agent becomes a shadow with
that as its reason. It is never silently downgraded.

### `agents.computer.host.root`

- Type: path
- Default: unset
- Since: 0.1.0

Directory the agent works in. Defaults to its workspace.

### `agents.computer.host.allow_paths`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Additional paths reachable under confine: allowlist or bwrap.

### `agents.computer.host.deny_paths`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Paths refused even when they fall inside an allowed one.

Checked after allow_paths, so ~/.ssh stays out even when all of ~ is in.
Under bwrap a denied directory is covered with an empty tmpfs and a
denied file with /dev/null, so the kernel enforces it rather than the
argument scan; a path outside every bind needs nothing, since bwrap
starts from nothing.

### `agents.computer.host.allow_network`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Whether the agent's commands can reach the network.
Only enforceable under confine: bwrap, which unshares the namespace.

### `agents.computer.host.allow_sudo`

- Type: boolean
- Default: `false`
- Warning: hands over real authority; read security.md first
- Since: 0.1.0

Permit privilege escalation.

With this false, sudo, pkexec, doas, run0 and machinectl shell are
refused at the exec layer, including through a shell one-liner, and the
agent is told so rather than left guessing why its command failed.

### `agents.computer.host.shell`

- Type: path
- Default: `/bin/sh`
- Since: 0.1.0

Shell used for interactive sessions and shell-form commands.

Not implemented in this build. Nothing reads it, and there is no
shell-form command for it to apply to: clawtilla_computer_exec takes
an argv, every element of which is quoted before it is run, so a host
computer never invokes a shell at all.

### `agents.computer.host.nice`

- Type: integer
- Default: `0`
- Since: 0.1.0

Scheduling niceness for the agent's commands, so a runaway build does
not make the desktop unusable.

## `agents.computer.display`

Settings for computer.type: display -- GUI apps on a virtual screen of
their own, which the agent drives and a person can watch and take.

Commands run on the host exactly as computer.type: host runs them, so
computer.host.confirm_host_control and computer.host.confine apply
here too. See docs/computers.org, "display".

### `agents.computer.display.backend`

- Type: enum
- Values: auto, gowl, xvfb, xvnc
- Default: `auto`
- Since: 0.51.0

What draws the screen: auto, gowl, xvfb or xvnc.

  gowl  gowl run headless, in a runtime directory of its own; Wayland
        apps draw on it directly, X apps through its XWayland, and the
        agent gets gowl's own window tools as well
  xvnc  TigerVNC's X server: the Computer tab's viewer button opens it
        live in a VNC viewer
  xvfb  an X server drawing into memory; the lightest of the three

auto takes the first that runs, in that order. Each candidate is run
before it is chosen, so a broken binary first on PATH is skipped, and
the Computer tab says what was skipped and why.

### `agents.computer.display.resolution`

- Type: string
- Default: `1280x800`
- Since: 0.51.0

The screen's size, WIDTHxHEIGHT, each between 64 and 8192.

The agent's clicks are in these pixels. Larger screens cost more to
picture, and every picture the agent asks for is part of its turn.

### `agents.computer.display.depth`

- Type: integer
- Default: `24`
- Since: 0.51.0

Colour depth for an X server: 16, 24 or 32. gowl ignores it.

### `agents.computer.display.startup`

- Type: list of strings
- Default: unset
- Since: 0.51.0

Apps started on the screen each time it comes up, as shell command
lines, under the agent's confinement: e.g. [firefox, "foot -e htop"].
One that does not start is logged; the others still are.

### `agents.computer.display.vnc_port`

- Type: integer
- Default: `0`
- Since: 0.51.0

Xvnc's VNC port on 127.0.0.1. 0 picks a free one at each start.

The VNC server listens on loopback only and asks for no password: it
is reachable from this machine alone, which is the trust the daemon's
own socket gives.

### `agents.computer.display.allow_input`

- Type: boolean
- Default: `true`
- Since: 0.51.0

Whether the agent may click, type and scroll on its display, not only
look at it.

On by default, unlike computer.desktop.allow_input: that one is about
the operator's own screen, and this is a private one nobody is sitting
at. A person who takes the screen still refuses the agent's input for
as long as they hold it.

### `agents.computer.display.allow_spawn`

- Type: boolean
- Default: `true`
- Since: 0.51.0

Whether the agent may start apps on its display with display_launch.

Off leaves it the apps in computer.display.startup and whatever it
starts through clawtilla_computer_exec, which is its own setting.

### `agents.computer.display.gowl_binary`

- Type: path
- Default: unset
- Since: 0.51.0

A gowl to run instead of searching /usr/bin, /usr/sbin and PATH.

For when the first gowl found is not the one wanted -- an old build in
/usr/local/bin that cannot load its wlroots, say. It must answer
--version or it is not used.

## `agents.computer.container`

Settings for computer.type: container.

### `agents.computer.container.image`

- Type: string
- Default: `registry.fedoraproject.org/fedora:45`
- Since: 0.1.0

Image the container is created from.

Any reference podman can pull. Unset means defaults.container_image.

Name the registry: a bare 'fedora:45' is resolved through podman's
unqualified-search list, which is per-machine, so the same config can
pull a different image on two hosts.

### `agents.computer.container.name`

- Type: string
- Default: unset
- Since: 0.1.0

Container name. Defaults to clawt-<agent-id>.

### `agents.computer.container.connection`

- Type: string
- Default: unset
- Since: 0.1.0

Podman connection: a socket path, ssh://user@host/socket, tcp://host:port,
or a named connection. Defaults to the user's podman socket.

### `agents.computer.container.command`

- Type: string
- Default: `sleep infinity`
- Since: 0.1.0

What the container runs.

The default keeps the container alive, because a container computer
exists to be exec'd into: a plain base image's entrypoint exits the
moment it starts, and an agent whose container has already gone is an
agent whose every command fails for no visible reason.

Set it when the image has an entrypoint worth running. A JSON array is
used as-is; anything else is split on spaces.

### `agents.computer.container.network`

- Type: string
- Default: unset
- Since: 0.1.0

Network to attach to. Leave unset for podman's default.

Not implemented in this build. The value reaches the container object
through clawt_container_computer_set_network() and stops there: the
podman create request is built without it, so the container is always
on podman's default network however this is set.

### `agents.computer.container.keep`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Keep the container when the agent stops, instead of removing it.

Stopping an agent stops its machine, and with this off the container
is removed as well: everything the agent installed goes with it and
the next start builds a fresh one from the image.

On by default. It reads as the smaller of the two settings and it is
not -- a bare image has no toolchain, so the first thing an agent does
in a new container is install one, and discarding that on every stop
means paying for it again on every start. The distrobox backend has
defaulted to keeping for exactly this reason since it was written.

Turned off, `computer stop` and `computer restart` refuse without
`--remove`, and both graphical clients ask first: the contents are
gone rather than offline, which is not what the word stop suggests.

### `agents.computer.container.pids_limit`

- Type: integer
- Default: `0`
- Since: 0.19.0

Most processes and threads the machine may hold at once.

What stops a fork bomb, or a build with -j0, taking the host down with
it. 0 leaves podman's own default (2048 on Fedora, from
containers.conf); -1 removes the limit. Anything else is the limit.

Read by the distrobox backend too, which is a podman container
underneath: computer.container.* limits reach either. Passed to podman
when the machine is created; a changed value is applied to the
existing one with `podman update` at the next start, without a
rebuild and without losing what the agent installed.

### `agents.computer.container.memory`

- Type: string
- Default: unset
- Since: 0.19.0

Most memory the machine may use, e.g. 512m or 4g.

A whole number with an optional unit -- b, k, m or g, in powers of
1024 -- and at least 6m, which is podman's floor. Unset means no limit
beyond the host's. Past it the kernel's OOM killer picks a process
inside the machine, which is the point: the agent's container dies
rather than your desktop.

Applies to container and distrobox computers, at creation and then
with `podman update` whenever it changes.

### `agents.computer.container.memory_swap`

- Type: string
- Default: unset
- Since: 0.19.0

Memory and swap together, e.g. 8g; -1 for unlimited swap.

podman's own meaning, and easy to misread: this is the total, not the
swap alone, so memory: 4g with memory_swap: 6g allows 2g of swap. Equal
to memory means no swap at all. Unset lets podman allow as much swap
again as memory. It needs memory to be set and must not be smaller
than it; both are refused when they arrive rather than when podman
reads them.

### `agents.computer.container.selinux_level`

- Type: string
- Default: `auto`
- Since: 0.78.0

The SELinux MCS level the container is created at.

podman gives every container a random pair of categories, and a file
created inside a container is labelled with that pair. On a folder
several containers share -- ~/source, mounted rw with relabel: shared
-- a file one agent writes from inside its container is then
unreadable inside every other one, even at mode 0644. It reads as a
permissions bug and is a labelling one. Files written on the host
(an agent's own tools run there) are not affected.

auto (the default) puts an agent on a team at its team's level: a
pair clawtilla picks the first time the team needs one, distinct
from every other team's and from every explicit level in the config,
and keeps in tasks.db under the state directory, so it survives a
daemon restart and a rebuild. An agent on no team gets podman's
random pair. random always gets podman's random pair. A level such as
s0:c100,c200 (or one category, s0:c5) is used as written: categories
c0..c1023, ascending, no leading zeros. disable is refused -- it
would switch SELinux off for the container, not choose its level.

Applied only when the container is created. Changing it, or the
agent's team under auto, takes effect when the container is next
made: stop the agent and start it, or run `computer rebuild`.
`config validate` warns when two containers share a writable folder
at different levels.

## `agents.computer.ssh`

A machine reached over ssh.

The only backend clawtilla does not own. A container, a distrobox and
a VM are things it creates and can destroy; this one was running
before the fleet existed. So clawtilla will not start, stop or take
apart an ssh computer, and both clients leave those buttons out
rather than offering them and failing.

Nothing is mounted into it either -- there is no mount to make across
an ssh connection. That means the agent does NOT get the workspace at
/mnt/clawtilla/workspace or the shared exchange, which every other
backend does get, and fleet-wide shared folders do not apply. What
the mount list does instead is described under workspace below.

### `agents.computer.ssh.host`

- Type: string
- Default: unset
- Since: 0.2.0

The ssh config alias to connect to.

An alias from ~/.ssh/config, not a hostname -- so the identity file,
the jump host, the port and the user stay where ssh already keeps
them, and clawtilla never reimplements ssh's own configuration.

Letters, digits, '.', '_' and '-' only, and never a leading '-'. ssh
reads an argument beginning with '-' as an option, so an alias called
-oProxyCommand=... would be a command rather than a destination.

Run `ssh <alias> true` once by hand first. The host key is accepted
on your terms, never automatically: an unknown or changed key is a
refusal that names this as the remedy. clawtilla does not pass
StrictHostKeyChecking=no, because accepting a key is your decision.

### `agents.computer.ssh.workspace`

- Type: path
- Default: unset
- Since: 0.2.0

Where the agent works on that machine.

A path as that machine spells it. It is not expanded here: '~' and
$XDG_* would be resolved against this machine's home and produce a
path that exists here and means nothing over there.

There is no kernel mount to make over ssh, so the mount list becomes
the allowlist instead -- the same shape `confine: allowlist` has on
the host backend. Each mount's *target* is the grant, because that is
the path inside the computer and the computer is the other machine;
the source names a directory on this one and is not used.

That check reads the paths in a command and resolves '.' and '..' by
text. It cannot follow a symlink, because the symlink is on the other
machine -- so it is a boundary against mistakes and against paths an
agent constructs, not against a filesystem laid out to defeat it. The
directory must already exist; clawtilla creates nothing on a machine
it does not own, and a mount marked required that is not there makes
the agent a SHADOW with that reason rather than starting it anyway.

### `agents.computer.ssh.shell`

- Type: string
- Default: `/bin/sh`
- Since: 0.2.0

Which shell runs the command over there.

ssh hands the remote login shell one string whatever we do, so this
is not a way to avoid a shell -- it is a way to name one that behaves
the same everywhere. A login shell that is fish or csh reads the
`cd <dir> && ...` prologue differently, and naming /bin/sh is what
makes an agent behave identically on every host in a fleet.

### `agents.computer.ssh.allow_sudo`

- Type: boolean
- Default: `false`
- Since: 0.2.0

Whether sudo and its neighbours may be run there.

Off refuses sudo, pkexec, doas, run0, su, setpriv and machinectl at
the point the command is built, including inside a shell one-liner,
and the agent is told so rather than discovering it a turn later.

It is an argument check, not a kernel boundary: it stops a command
that names one of those, and cannot stop a program that escalates
once it is already running. Nothing on this backend can -- the kernel
doing the enforcing is on the other machine.

### `agents.computer.ssh.connect_timeout`

- Type: integer
- Default: `10`
- Since: 0.2.0

Seconds to wait for the connection before failing.

Bounds the one failure that otherwise has none. A machine that has
dropped off the network accepts nothing and refuses nothing, and ssh
will sit out the kernel's TCP timeout waiting -- turning a turn that
should have failed in ten seconds into one that hangs. Zero is
treated as one second rather than as no limit.

### `agents.computer.ssh.control_persist`

- Type: integer
- Default: `600`
- Since: 0.2.0

Seconds an idle multiplexed connection is kept open.

Without multiplexing every single command pays a full TCP connect,
key exchange and authentication, which over a WAN is most of the time
an agent spends running anything. With it the first command opens a
master and every later one is a channel on the connection already
there.

Zero turns it off. It also turns itself off, with a warning, when the
control socket path would exceed the 108 bytes the kernel allows in a
unix address -- an over-long path does not fail at bind time, it just
means the master is never created and every command silently pays a
fresh handshake.

### `agents.computer.ssh.wake_command`

- Type: list of strings
- Default: unset
- Since: 0.19.0

A program to run when the machine does not answer, to wake it.

For a machine that sleeps: a homelab box that suspends when idle, a
cloud server that is paused. Wake-on-LAN is the usual answer --
[wakeonlan, aa:bb:cc:dd:ee:ff] -- or a provider's own resume command.
It should return once the machine is on its way up; clawtilla then
connects as usual, and ssh's connect_timeout covers the rest of the
boot.

A program and its arguments, as a list. It is never run through a
shell, so a pipe or a redirect would be text; written as one line in
a client it is split into words and refused if it uses one. Nothing
is substituted into it.

Run only when a connection attempt finds nothing answering, never for
a refused key or login. Commands that arrive while it runs wait for
that one run rather than starting their own, and each is told how
long it waited. It gets at most 120 seconds, after which it is killed
along with everything it started, and the time is not taken out of
the command's own timeout. A wake that fails is reported with the
connection failure, saying which.

## `agents.computer.distrobox`

Settings for computer.type: distrobox.

A distrobox is a podman container deliberately wired into the machine
around it: the agent runs as the same user as you, sees your sockets
and displays, and can reach the host itself with distrobox-host-exec.
That makes it a good place to build things and a poor place to put
something that should be contained -- use computer.type: container for
that, which shares none of it.

### `agents.computer.distrobox.image`

- Type: string
- Default: unset
- Since: 0.1.0

Image the box is created from. Unset uses distrobox's own default,
which is a Fedora toolbox image.

Name the registry, for the reason the container image does: a bare
'fedora:45' is resolved through podman's unqualified-search list,
which is per-machine.

### `agents.computer.distrobox.name`

- Type: string
- Default: unset
- Since: 0.1.0

Box name. Defaults to clawt-<agent-id>.

Prefixed on purpose. These machines already have boxes called 'dev'
and 'util', and an agent adopting one of those would be a fleet
operation reaching into somebody's development environment.

### `agents.computer.distrobox.home`

- Type: string
- Default: unset
- Since: 0.1.0

HOME inside the box.

Unset gives the agent a directory of its own under
$XDG_DATA_HOME/clawtilla/boxes/<agent-id>/home, which is NOT what
distrobox does on its own -- see share_home.

### `agents.computer.distrobox.share_home`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Give the box your real home directory.

This is distrobox's own default, and clawtilla's is the opposite,
because the two look identical from inside the box and are completely
different in what they touch: an agent sharing your home can read your
ssh keys, your shell configuration and any credential you keep there.

Turn it on when the point is to work in your environment -- an agent
maintaining your dotfiles, say. Leave it off otherwise. It is fixed
when the box is created, so changing it means removing the box.

### `agents.computer.distrobox.packages`

- Type: string
- Default: `gcc make git`
- Since: 0.1.0

Packages installed when the box is first created, space separated.

Read once, at creation, the way a VM's cloud-init reads its seed --
so adding a name here and restarting the agent does nothing visible.
The agent can install more itself; this is for what it needs before
it can.

### `agents.computer.distrobox.flags`

- Type: string
- Default: unset
- Since: 0.1.0

Extra flags passed through to podman, space separated.
Quote anything containing a space.

### `agents.computer.distrobox.init`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Run an init system (systemd) inside the box.
Needed by anything expecting systemctl or a user unit.

### `agents.computer.distrobox.keep`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Keep the box when the agent stops.

On by default, unlike the plain container: making one pulls an image
and then runs a package install inside it, and a distrobox is the kind
of computer somebody chose so that what the agent installs survives.

## `agents.computer.vm`

Settings for computer.type: vm.

### `agents.computer.vm.backend`

- Type: enum
- Values: libvirt, qemu
- Default: `libvirt`
- Since: 0.1.0

libvirt or qemu.

libvirt is the default and brings snapshots, migration and device
hotplug with it. qemu drives qemu-system-* directly over QMP and needs
no libvirtd, at the cost of that surface.

### `agents.computer.vm.domain`

- Type: string
- Default: unset
- Since: 0.1.0

libvirt domain name. Defaults to clawt-<agent-id>.

### `agents.computer.vm.uri`

- Type: string
- Default: `qemu:///session`
- Since: 0.1.0

libvirt connection URI. qemu:///system for system-wide VMs.

### `agents.computer.vm.emulator`

- Type: path
- Default: unset
- Since: 0.1.0

The QEMU binary to run the guest with.

Left unset, clawtilla looks in /usr/bin and then on PATH, and writes
what it finds into the domain.

Naming it matters because libvirt otherwise picks one itself, by
searching the session daemon's PATH -- so a host with another package
manager ahead of /usr/bin gets a domain pointing into a home
directory. SELinux refuses that: svirt_t cannot start a binary
labelled user_home_t, and the guest never boots. Set this only for a
host whose QEMU is somewhere clawtilla does not look.

### `agents.computer.vm.image`

- Type: path
- Default: unset
- Since: 0.1.0

Base disk image. A qcow2 overlay is created on top, so the base is
never written to and several agents can share one.

clawtilla ships no image and downloads none: point this at a qcow2 you
already have. A distribution's cloud image is the easy answer, because
cloud_init below can give it a login without the image being touched.

### `agents.computer.vm.cpus`

- Type: integer
- Default: `2`
- Since: 0.1.0

Virtual CPUs.

### `agents.computer.vm.memory_mb`

- Type: integer
- Default: `8192`
- Since: 0.1.0

Memory in megabytes.

Sized for a guest running a GNOME session, because that is what
computer.desktop.enabled installs into one. A headless VM is happy in
a quarter of it.

### `agents.computer.vm.disk_gb`

- Type: integer
- Default: `128`
- Since: 0.1.0

Virtual disk size in gigabytes.

A cloud image's own disk is only a few gigabytes, which a desktop and
a toolchain fill immediately. The overlay is created at this size
instead and cloud-init grows the guest's filesystem to match on first
boot.

qcow2 is sparse, so this costs what is written and not what is asked
for. Raising it grows the disk on the next provision; lowering it is
refused, because shrinking a disk destroys whatever was past the new
end.

### `agents.computer.vm.resolution`

- Type: string
- Default: `1280x800`
- Since: 0.1.0

The size of the VM's screen, as WIDTHxHEIGHT.

1280x800 is the hypervisor's own default, which is why an agent's
screenshots come back that size unless this says otherwise. The
virtual GPU reports it as the preferred mode and GNOME takes it, so
nothing inside the guest has to be configured -- and unlike anything
in the cloud-init seed, changing it applies at the VM's next boot
rather than needing the machine rebuilt.

It costs guest memory: the framebuffer is width x height x 4 bytes.

### `agents.computer.vm.ssh_user`

- Type: string
- Default: `root`
- Since: 0.1.0

User commands are run as inside the guest, over SSH.

With cloud_init on, this account is created in the guest rather than
having to exist already, and a non-root one gets passwordless sudo.

root, because of how a shared directory is mapped. An unprivileged
libvirt session runs virtiofsd in a user namespace where the *guest's*
root is the host user who started the daemon, and every other guest
id lands in that user's subuid range. So a share of your own files
appears inside the guest as root:root 0755, and an agent running as
any other account can read it and cannot write to it -- including its
own workspace, which arrives as 0700 root and is not even readable.

This was `clawt` for a while, on the reasoning that the guest's first
account is uid 1000 and so lines up with the host user. That is true
of the numbers and false of the mapping, and booting one is the only
way to find out: guest uid 1000 maps to a subuid around 525287, which
owns nothing. Naming an <idmap> on the share does not help either --
virtiofsd's sandbox cannot set one up for a session daemon at all, and
the domain then fails to start rather than starting without it.

The cost is that GDM will not log root in, so a VM with a desktop gets
a second account for the screen -- see computer.vm.desktop.user, which
is created for exactly this. That account is a normal guest user, so
it cannot write to a share either; anything the session produces for
you goes through a path root writes.

### `agents.computer.vm.ssh_key`

- Type: path
- Default: unset
- Since: 0.1.0

Private key used to reach the guest.

Left unset, an ed25519 key is generated per agent under the VM's state
directory and authorised through cloud-init. It cannot be encrypted:
the daemon uses it with nobody present to type a passphrase.

### `agents.computer.vm.ssh_host`

- Type: string
- Default: unset
- Since: 0.1.0

Address that reaches the guest.

Leave it unset and clawtilla forwards a port on 127.0.0.1 to the
guest's SSH and fills this in itself -- the port is chosen once and
remembered, so it survives a restart. Set it when the VM is somewhere
clawtilla did not put it: a bridged network, another host, or a VM that
existed before this agent did.

The libvirt backend can only forward a port through passt, because
libvirt has no port forwarding for the SLIRP backend. Without passt
installed the VM still runs, and this key is the way to reach it.

### `agents.computer.vm.ssh_port`

- Type: integer
- Default: `22`
- Since: 0.1.0

Port SSH is reached on. Ignored when clawtilla is forwarding a port of
its own choosing, which is the default.

### `agents.computer.vm.packages`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Packages the guest installs at first boot.

  packages: [git, inetutils]

A cloud image is deliberately small, and what is missing differs by
distribution -- Arch's has no `hostname`, which agents reach for
reflexively. This is where to put what yours needs, rather than
having every agent install it again on every fresh machine.

Names are the guest's, not yours: the same tool is `inetutils` on
Arch and `hostname` on Fedora. cloud-init treats a package it cannot
find as a failure of the *whole* install, so one wrong name costs
every other package here -- and on a guest with a desktop, the
desktop with them.

Read once, by cloud-init, from a seed written before the guest ever
ran. Adding one to a VM that already exists needs computer.rebuild,
like everything else in that seed.

### `agents.computer.vm.cloud_init`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Hand the guest a cloud-init seed on first boot.

A cloud image has no account, no password and no authorised key: it
expects to be given them. Without this it boots perfectly and nothing
can get in, which looks exactly like a VM that failed to boot.

The seed is a small ISO labelled cidata, attached as a CD-ROM, holding
the login and public key. Building it needs xorriso (Fedora: xorriso).
Turn this off for an image that already has a login and a key.

### `agents.computer.vm.snapshot_on_start`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Take a snapshot each time the VM starts, so a session can be rolled back.
libvirt backend only.

## `agents.computer.vm.desktop`

How the guest's graphical session is built.

None of this happens unless computer.desktop.enabled is also true --
that key is the grant, and these are the details of what it installs.
A cloud image has no desktop at all, so an agent given one on a VM has
to have it put there first.

### `agents.computer.vm.desktop.user`

- Type: string
- Default: unset
- Since: 0.1.0

The account the graphical session runs as.

Left unset this is computer.vm.ssh_user, unless that is root: GDM
refuses to log root in, so a VM whose ssh_user is root -- the default
-- gets a separate `clawt` account for the session and keeps running
commands as root. Both are created by cloud-init and both authorise
the same key.

cloud-init acts on first boot only, so changing this later means
deleting the overlay rather than restarting.

### `agents.computer.vm.desktop.autologin`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Log that account straight in at boot, with no password prompt.

There is nobody at the VM's console to type one, and until a session
exists there is no desktop to control: the shell extension the agent
talks to runs inside GNOME Shell. Turn this off and something else has
to start the session.

### `agents.computer.vm.desktop.flavour`

- Type: enum
- Values: auto, fedora, enterprise, debian, ubuntu, arch
- Default: `auto`
- Since: 0.2.0

Which family the guest belongs to, for installing things into it.

`auto` reads it off `computer.vm.image` -- a catalog id says outright,
and an image you downloaded yourself keeps the distribution in its
filename. Set it when neither does: clawtilla says which family it
assumed rather than guessing quietly.

`enterprise` covers CentOS Stream, RHEL and the rebuilds; `arch` is
Arch Linux, whose names drop the 3 everyone else puts in their Python
packages and which is upgraded fully before anything is installed --
refreshing pacman's index without upgrading is the partial upgrade it
warns about. Ubuntu is
separate from Debian for one reason: Debian stable has no `firefox`
package, only `firefox-esr`, and Ubuntu has no `firefox-esr` -- so a
single family would be wrong on half of it, and a package that does
not exist fails the whole install rather than merely missing.

It decides more than package names -- the display
manager is gdm on Fedora and gdm3 on Debian, PyGObject is
python3-gobject there and python3-gi here, and a Debian cloud image
has neither the dconf binary nor glib-compile-schemas until asked.

### `agents.computer.vm.desktop.packages`

- Type: list of strings
- Default: unset
- Since: 0.1.0

What to install to get a desktop, if not the usual set.

Left unset this is the list for the guest's `flavour`, which is
almost always what you want: the names differ per distribution and
nobody should have to know that to change image. Each family's list
is deliberately small rather than the full workstation group -- the
agent needs a session to drive, not an office suite to download.

A list here replaces that one rather than adding to it, so it is also
how you take something out. It does not reach the packages the MCP
server needs: those are not the desktop, and trimming this list must
not silently switch automation off.

### `agents.computer.vm.desktop.mcp`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Install gnome-desktop-mcp into the guest and switch it on.

This is the part the agent actually talks to: a GNOME Shell extension
exposing screenshots, window management and input injection over
D-Bus, and a stdio MCP server in front of it. GNOME on Wayland refuses
those to any process outside the compositor, which is why an extension
is involved at all.

Without this the guest has a desktop and the agent has no way to see
it.

### `agents.computer.vm.desktop.mcp_repo`

- Type: string
- Default: `https://git.podbielniak.com/zachpodbielniak/gnome-desktop-mcp.git`
- Since: 0.1.0

Where the guest clones gnome-desktop-mcp from.

Cloned rather than installed from PyPI because the extension is only
in the repository, and the two halves have to be the same version --
the server calls D-Bus methods the extension has to already export.

## `agents.computer.desktop`

Let the agent see and drive a desktop session.

An add-on rather than a computer type, so it works alongside whichever
one the agent has. An agent with this can read anything on your screen
and click anything on it.

### `agents.computer.desktop.enabled`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Whether desktop tools are offered to the agent.

### `agents.computer.desktop.backend`

- Type: enum
- Values: auto, gowl, gnome, guest
- Default: `auto`
- Since: 0.1.0

auto, gowl, gnome or guest.

guest is the agent's own VM, and auto picks it whenever the agent has
one -- an agent with its own machine should drive that screen rather
than yours. It is also the only backend where a misjudged click lands
somewhere recoverable.

Otherwise auto tries gowl's MCP socket and falls back to
gnome-desktop-mcp on the host. gowl is the better fit where it is
available: same tool vocabulary, native, and no Python or GNOME Shell
extension in the way.

### `agents.computer.desktop.socket`

- Type: path
- Default: `$XDG_RUNTIME_DIR/gowl-mcp.sock`
- Since: 0.1.0

gowl's MCP socket. Requires gowl built with MCP=1 and modules.mcp
enabled in its config.

### `agents.computer.desktop.allow_spawn`

- Type: boolean
- Default: `false`
- Warning: hands over real authority; read security.md first
- Since: 0.1.0

Whether the agent may launch and signal processes through the
compositor.

Separate from allow_input, and off by default. The compositor's spawn
tool starts a process through the compositor's own socket, so none of
clawtilla's confinement applies to it -- not the path checks, not the
sudo block, and not bwrap. Turning this on gives the agent a way to
run anything, whatever computer.host.confine says.

### `agents.computer.desktop.allow_input`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Permit key and pointer injection, not just observation.

With this false the agent can take screenshots and query windows but
cannot act, which is a useful amount of access on its own and a much
smaller grant.

### `agents.computer.desktop.observe_fps`

- Type: integer
- Default: `1`
- Since: 0.2.0

Frames a second while somebody is watching the screen.

A supervision view, not a video call. Grabbing shares the one
channel the agent is using for its own work, so every frame is
latency taken from the task being watched. Nothing is captured at
all unless a client is subscribed.

Clamped to 1-4. A zero means the default rather than never: a
client that subscribed and then received nothing would show an
empty panel with nothing to explain it.

### `agents.computer.desktop.takeover_lease_seconds`

- Type: integer
- Default: `900`
- Since: 0.2.0

How long a person may hold the screen before the hold lapses.

While it is held the agent's input is refused rather than queued: a
queued click lands after the person has moved on, on whatever
happens to be under it then. The lease expires so that a browser tab
closed mid-takeover cannot lock an agent out for good.

Taking the screen again while you already hold it extends the lease,
which is what a client does while somebody is still working.

### `agents.computer.desktop.driver_idle_seconds`

- Type: integer
- Default: `120`
- Since: 0.5.0

On a shared computer, how long one agent keeps the screen after its
last input before another may type or click.

A shared machine has one desktop. The first agent to act on it holds
it; another agent's input is refused, naming who is driving, until
the holder has been idle this long. Looking is never refused.

### `agents.computer.desktop.allow_recording`

- Type: boolean
- Default: `false`
- Warning: hands over real authority; read security.md first
- Since: 0.2.0

Whether a demonstration may be recorded on this screen.

Its own setting, and not part of `allow_input`, because capturing
what a person types is categorically different from injecting
keystrokes. An agent allowed to click must not thereby be allowed to
watch you type.

While a recording runs the desktop says so visibly and the
recording stops itself after `skills.teach_max_seconds`.

Capture pauses for a password field only as far as the compositor
can see one, and that is less far than it sounds. In a guest, GNOME
Shell recognises its own password entries -- the lock screen, its
polkit and keyring prompts -- and pauses. On the host, gowl cannot:
under Wayland a client's widget tree is private, so it suppresses
capture while the session is locked and while the focused window's
app-id or title matches its deny list of credential applications,
and a password typed into a form inside any other window IS
recorded. Every trace carries that sentence. Read a recording
before you turn it into a skill.

## `agents.browser`

A real web browser the agent can drive (needs a GSURF=1 build).

### `agents.browser.mode`

- Type: enum
- Values: none, own, shared
- Default: `none`
- Since: 0.9.0

Whether this agent can drive a real web browser.

`own` gives it a session of its own, `shared` one declared under
browsers: (agents.browser.use names it), `none` neither -- the agent
still has clawtilla_web_fetch. A session is gsurf on a private X
display, started on the agent's first browser call and stopped after
agents.browser.idle_minutes without one; it runs JavaScript, so it
reads pages clawtilla_web_fetch cannot, and the agent can click and
type. Watch it from the Computer page's Browser view.

Needs clawtilla built with GSURF=1: an agent asking for a browser
from a build without one is disabled with that reason rather than
run without the tools it was told it has.

### `agents.browser.use`

- Type: string
- Default: unset
- Since: 0.9.0

With agents.browser.mode: shared, the browsers: entry to join. Set,
every other browser.* key comes from that entry.

### `agents.browser.home`

- Type: string
- Default: `about:blank`
- Since: 0.9.0

The page a session opens on.

### `agents.browser.allow_scripts`

- Type: boolean
- Default: `false`
- Warning: hands over real authority; read security.md first
- Since: 0.9.0

Whether the agent is offered clawtilla_browser_eval, which runs its
own JavaScript in the page.

Off by default. Reading, clicking and typing cover what a person does;
a script can read any field and post anything as the logged-in
user without a single visible step.

### `agents.browser.idle_minutes`

- Type: integer
- Default: `15`
- Since: 0.9.0

Stop the session after this many minutes without a browser call. The
profile stays, so a login survives; the open page does not. 0 keeps
it running until its agent stops.

### `agents.browser.secrets`

- Type: list of strings
- Default: unset
- Since: 0.25.0

The vault: entries this agent may have filled into a page, by name.
Empty, the agent is offered no secret tools. Always the agent's own,
even on a shared browser: what an agent may ask for is about the
agent, not the session.

### `agents.browser.width`

- Type: integer
- Default: `1280`
- Since: 0.9.0

The session's window width, in pixels.

### `agents.browser.height`

- Type: integer
- Default: `800`
- Since: 0.9.0

The session's window height, in pixels.

### `agents.env`

- Type: mapping
- Default: unset
- Since: 0.1.0

Environment variables for this agent's process.

Only these and a small fixed set are passed through. The daemon's own
environment is not inherited: a stray ANTHROPIC_API_KEY leaking into a
subscription CLI would quietly move it onto pay-as-you-go billing
nobody agreed to.

### `agents.credentials`

- Type: mapping
- Default: unset
- Since: 0.1.0

Secret references for this agent, written to credential files at 0600.

  credentials:
    anthropic_api_key: {env: ANTHROPIC_API_KEY}
    matrix_token: {file: ~/.clawtilla/secrets/matrix-chief}

Values are never echoed back over IPC; clients see only which keys are
configured.

## `agents.integrations`

Channels this agent reaches the outside world through.

libreclaw implements all of these; clawtilla renders the configuration,
writes the credential files and health-checks them.

## `agents.integrations.matrix`

Matrix. Needs homeserver, user_id and an access token.

### `agents.integrations.matrix.enabled`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Whether the Matrix channel is active.

### `agents.integrations.matrix.homeserver`

- Type: string
- Default: unset
- Since: 0.1.0

Homeserver base URL.

### `agents.integrations.matrix.user_id`

- Type: string
- Default: unset
- Since: 0.1.0

Full Matrix user id, e.g. @agent:example.com.

### `agents.integrations.matrix.access_token`

- Type: secret reference
- Default: unset
- Since: 0.1.0

Access token, as a secret reference.

### `agents.integrations.matrix.rooms`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Room ids to join.

### `agents.integrations.matrix.require_mention`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Only respond when named, so the agent is not a participant in every
conversation in the room.

## `agents.integrations.email`

Email over IMAP and SMTP.

### `agents.integrations.email.enabled`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Whether the email channel is active.

### `agents.integrations.email.imap_host`

- Type: string
- Default: unset
- Since: 0.1.0

IMAP server hostname.

### `agents.integrations.email.imap_port`

- Type: integer
- Default: `993`
- Since: 0.1.0

IMAP port.

### `agents.integrations.email.smtp_host`

- Type: string
- Default: unset
- Since: 0.1.0

SMTP server hostname.

### `agents.integrations.email.smtp_port`

- Type: integer
- Default: `587`
- Since: 0.1.0

SMTP port.

### `agents.integrations.email.username`

- Type: string
- Default: unset
- Since: 0.1.0

Account username.

### `agents.integrations.email.password`

- Type: secret reference
- Default: unset
- Since: 0.1.0

Account password, as a secret reference.

### `agents.integrations.email.folders`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Folders to watch. Defaults to INBOX.

## `agents.integrations.webhook`

Inbound HTTP webhooks.

### `agents.integrations.webhook.enabled`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Whether the webhook channel is active.

### `agents.integrations.webhook.port`

- Type: integer
- Default: `0`
- Since: 0.1.0

Port to listen on. 0 asks the kernel for a free one, which is what you
want with several agents: a fixed port means the second agent to start
fails to bind.

### `agents.integrations.webhook.bind_address`

- Type: string
- Default: `127.0.0.1`
- Since: 0.1.0

Which address to listen on.

The loopback by default. An endpoint in shell_command mode runs a
command, and bearer tokens are otherwise the only thing between that
and whoever can reach the port -- so widening it is something you say
rather than something you get. Set it to 0.0.0.0 for every interface,
or to a private interface's address.

The routing itself is libreclaw's channels.webhook.endpoints, which
goes in the agent's `libreclaw:` block: it is a list of objects with
nested targets and has no sensible spelling as a flat key. clawtilla
merges that block into the channel it renders rather than refusing
it.

### `agents.integrations.local`

- Type: boolean
- Default: `false`
- Since: 0.1.0

libreclaw's stdin/stdout channel.

At most one agent may have this, and only with runtime.type: process --
it takes over file descriptors 0 and 1, which an embedded agent would be
taking from the daemon.

### `agents.integrations.cmacs`

- Type: boolean
- Default: `false`
- Since: 0.1.0

libreclaw's in-process cmacs channel.

### `agents.federation_reach`

- Type: list of strings
- Default: unset
- Since: 0.68.0

Which linked fleets may reach this agent, as peer selectors
(`tier:owned`, `tag:client-acme`, `name:lab`, `owner:Dana`, `*`; see
docs/federation.org#selectors). A fleet must match one of them *and* be
allowed by its own grant: this can only take reach away, never give it.
Unset, the grant alone decides. A conversation already under way is not
cut off by a change here; a new one is.

### `agents.federation_may_reach`

- Type: list of strings
- Default: unset
- Since: 0.72.0

Which linked fleets this agent may write to and hand work to, as peer
selectors (`tier:owned`, `tag:client-acme`, `name:lab`, `*`; see
docs/federation.org#selectors). Unset, an agent with tools.federation may
reach any linked fleet; set, only those that match -- and it sees only
their agents in clawtilla_list_agents. The other fleet's grant still
decides what lands there.

## `agents.tools`

Which orchestration tools this agent may call.

The clawtilla_* tools let an agent message its peers, delegate work,
read its mailbox and drive its computer. A worker usually wants fewer
of them than a chief-of-staff.

### `agents.tools.allow`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Tool names permitted. Unset means all of them except those needing a
capability this agent does not have.

### `agents.tools.deny`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Tool names refused. Applied after allow, so a deny always wins.

## `agents.policy`

What this agent may do without asking.

Enforced on clawtilla's own tools, connectors and the desktop relay
for every backend, and on the CLI's own tools (Bash, Write, ...) for
claude-code and claude-tmux through a PreToolUse hook. Other CLIs run
their own tools unchecked; docs/security.org says so in full.

### `agents.policy.rules`

- Type: list of strings
- Default: unset
- Since: 0.3.0

This agent's action rules, MATCH=MODE, first match wins. Defaults to
defaults.policy.rules.

### `agents.policy.ask_timeout_seconds`

- Type: integer
- Default: unset
- Since: 0.3.0

How long an `ask` waits for a person. Defaults to
defaults.policy.ask_timeout_seconds.

### `agents.tools.manage_fleet`

- Type: boolean
- Default: `false`
- Warning: hands over real authority; read security.md first
- Since: 0.1.0

Whether this agent may create other agents.

Off by default, and worth thinking about before turning on: an agent
that can create agents can give one a computer, and a container or a
VM is a machine that runs code. It cannot exceed what you could
create yourself -- every agent it makes goes through the same
validation -- but it can do it without being asked twice.

Intended for a chief-of-staff you talk to about what the fleet needs.

### `agents.tools.ephemeral`

- Type: boolean
- Default: `false`
- Warning: hands over real authority; read security.md first
- Since: 0.55.0

Whether this agent may spawn ephemeral agents: temporary helpers that
do one piece of work and are then reaped (clawtilla_ephemeral_spawn,
clawtilla_ephemeral_reap).

On for the chief of staff unless set otherwise, off for every other
agent. A helper copies how its parent runs -- runtime, confinement,
model, tools, skills -- and can never do more: it cannot spawn helpers
of its own, cannot create agents, and gets a computer no stronger than
its parent's. It stays, across restarts, until the agent that spawned
it reaps it; only that agent or you can. See docs/ephemeral-agents.org.

### `agents.tools.federation`

- Type: boolean
- Default: `false`
- Warning: hands over real authority; read security.md first
- Since: 0.68.0

Whether this agent may start conversations with agents in linked
fleets (clawtilla_message_agent and clawtilla_delegate with
`AGENT@ALIAS`) and sees them in clawtilla_list_agents.

Off by default: what this agent writes leaves this machine for
somebody else's fleet. Answering a message another fleet sent it needs
nothing -- the answer goes back where the question came from. What the
other fleet lets it reach is that fleet's grant, not this key. With
tools.manage_fleet as well it may also manage links
(clawtilla_federation_*). See docs/federation.org#messages.

### `agents.tools.office`

- Type: boolean
- Default: `false`
- Since: 0.58.0

Whether this agent may read, change and apply the office's world packs
and import assets into them (clawtilla_office_*).

On for the chief of staff unless set otherwise, so the chief can run
the office alone; on for the custodial team's agents, which are made
with it; off for every other agent, and never offered to an ephemeral
helper. Writes land only under the state directory, and a change that
would stop a pack loading is refused. See
docs/office-custodial-team.org.

### `agents.tools.schedule`

- Type: boolean
- Default: `true`
- Since: 0.7.1

Whether this agent may schedule its own routines.

With it on, the agent is offered clawtilla_routine_add, _update and
_remove: it can set up a check on something for itself, move one to a
better time, or split a long watchlist into several -- the way you
would ask a colleague to remind themselves rather than doing it for
them. Only ever routines that run on this agent, never another's,
and never one you marked `locked`.

On by default because a routine opens nothing -- unlike a trigger,
which is an address on your network and stays yours to create. What
it can cost is bounded by `tools.schedule_limit` and
`tools.schedule_min_minutes`.

### `agents.tools.schedule_limit`

- Type: integer
- Default: `8`
- Since: 0.7.1

How many routines may run on this agent before it can add no more
of its own.

Counts every routine whose agent is this one, including those you
wrote: the bound is on how often the agent is woken, not on who
arranged it. Routines you add are never refused for it.

### `agents.tools.schedule_min_minutes`

- Type: integer
- Default: `15`
- Since: 0.7.1

The shortest gap between two runs of a routine this agent schedules,
in minutes.

An agent that writes `*/1 * * * *` has written itself a turn a
minute, which is a bill rather than a schedule. Checked over the
schedule's next runs, so a cron expression whose runs bunch up --
`0,1,2 9 * * *` -- is refused as well as one that is simply too
frequent. Applies only to what an agent schedules for itself.

## `agents.mailbox`

Per-agent overrides of orchestration.mailbox.

### `agents.libreclaw`

- Type: mapping
- Default: unset
- Since: 0.1.0

Merged verbatim into this agent's generated libreclaw config.yaml.

An escape hatch for libreclaw settings clawtilla does not model. What is
here wins over what clawtilla generates, so it can also be used to
override something deliberately.
