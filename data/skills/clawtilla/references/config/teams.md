# Configuration: `teams`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `teams`

Teams, so a fleet larger than a handful has a shape.

An agent names its team in agents.team, and its standing there in
agents.team_role. A lead may hand work to the members of its own team;
a member may talk to anyone and assign to nobody. The chief of staff
sits above all of them and hands work to the leads.

Teams are optional. A fleet that declares none behaves exactly as it
did before there were any.

### `teams.id`

- Type: string
- Default: unset
- Required: yes
- Since: 0.1.0

Identifier used to address the team, and what agents.team names.

### `teams.name`

- Type: string
- Default: unset
- Since: 0.1.0

Display name. Defaults to the id.

### `teams.description`

- Type: string
- Default: unset
- Since: 0.1.0

What this team is for, and what to send it.

Written for a chief-of-staff deciding where a piece of work goes: it
reads these to choose a team, so say what the team handles and what it
does not, rather than naming the people on it.

### `teams.color`

- Type: string
- Default: unset
- Since: 0.1.0

Accent colour in the clients, as a hex string.

### `teams.order`

- Type: integer
- Default: `0`
- Since: 0.1.0

Where this team sits in the list, lowest first. Ties keep file order.

### `teams.tags`

- Type: list of strings
- Default: unset
- Since: 0.55.0

Labels that give this team a job in the software, by tag rather than
by name, so the team can be called whatever you like.

A well-known tag is one a feature looks for: `ephemeral` is the team
ephemeral agents join (clawtilla makes one called Ephemeral the first
time it needs it; tag a team of your own to use that instead).  Some
tags belong to one team only, and a second team carrying one is a
fleet warning -- the first in the team list keeps it.  Any other tag
is kept and shown, for your own use or a plugin's; a plugin's tags
are written `vendor:name`.  A tag is lowercase letters, digits and
hyphens.  `clawtilla team tag list --well-known` lists the tags this
clawtilla knows.  See docs/team-tags.org.

### `teams.skills`

- Type: list of strings
- Default: unset
- Since: 0.2.0

Skills every agent on this team gets.

Between the fleet default and the agent's own list, resolved by the
same function as both. The three are additive: a skill named in two
of them is one skill, and the most specific place it was asked for is
what gets reported.

### `teams.broker_server`

- Type: string
- Default: unset
- Since: 0.24.0

The broker's gateway server for this team's agents.

For anythingmcp, the id of an MCP server made in its dashboard (the
last part of the server's MCP URL); its connectors are what the
team's agents may use. For jentic, the Jentic agent the team acts
as, whose API key `clawtilla broker key team:<id>` stores. An agent
with its own agents.broker_server uses that one instead. Unused
unless broker.enabled is on.

## `teams.computer`

Defaults for the computers of this team's agents.

## `teams.computer.container`

Container limits for this team's container and distrobox agents.

Between the fleet's defaults.container and the agent's own
computer.container, key by key: a member's own cpus beats the team's,
and the team's memory still applies to it. Set from a client with
`clawtilla team set <team> computer.container.cpus 4`; running
members take the change in place.

### `teams.computer.container.cpus`

- Type: string
- Default: unset
- Since: 0.78.0

The team's agents.computer.container.cpus.

### `teams.computer.container.cpu_weight`

- Type: integer
- Default: unset
- Since: 0.78.0

The team's agents.computer.container.cpu_weight.

### `teams.computer.container.cpuset`

- Type: string
- Default: unset
- Since: 0.78.0

The team's agents.computer.container.cpuset.

### `teams.computer.container.memory`

- Type: string
- Default: unset
- Since: 0.78.0

The team's agents.computer.container.memory.

### `teams.computer.container.memory_swap`

- Type: string
- Default: unset
- Since: 0.78.0

The team's agents.computer.container.memory_swap.

### `teams.computer.container.pids_limit`

- Type: integer
- Default: unset
- Since: 0.78.0

The team's agents.computer.container.pids_limit.
