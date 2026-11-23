# Configuration: `broker`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `broker`

An integration broker clawtilla runs for you, in podman.

Ready-made app integrations -- hundreds of SaaS and business systems
-- behind one MCP gateway on this machine. clawtilla pulls the
pinned image, keeps the gateway's database, keys and token under
<daemon.state_dir>/broker/, starts it with the daemon and keeps it
running. Each agent or team names its own gateway server in
agents.broker_server or teams.broker_server and reaches it as a
connector, through the same relay as every other connector.

Not the provider-key broker of runtime.network: gated, which is a
different thing with a similar name. See docs/connectors.org,
"An integration broker".

### `broker.enabled`

- Type: boolean
- Default: `false`
- Since: 0.24.0

Run the broker.

Off by default, because turning it on pulls an image of several
hundred megabytes and opens ports on this machine. Started in the
background when the daemon starts or this is turned on, never in the
way of either: broker.status says starting while the image pulls.

### `broker.type`

- Type: enum
- Values: anythingmcp, jentic
- Default: `anythingmcp`
- Since: 0.24.0

Which broker: anythingmcp or jentic.

anythingmcp is an MCP gateway (AGPL-3.0) whose adapters turn REST,
SOAP, GraphQL and SQL services into tools, with OAuth through your
own client. jentic is Jentic One (Apache-2.0, a public beta whose
schemas may change), a credential broker that checks a per-agent
rule before it injects a stored key. Jentic One's own caveat is
that an agent running as the same OS user can read its state, so it
is refused unless every agent using it has runtime.confine: bwrap.
Its telemetry and release check are always switched off.

### `broker.image`

- Type: string
- Default: unset
- Since: 0.24.0

The gateway's image, pinned to a tag.

Empty means the image this release was checked against:
docker.io/helpcodeai/anythingmcp:v0.54.0 for anythingmcp and
ghcr.io/jentic/jentic-one-app:0.44.0 for jentic. An image with no
tag, or tagged latest, is refused: an upgrade should be a change
somebody made. AnythingMCP publishes amd64 only, so an aarch64
machine names a locally built one here -- docs/connectors.org says
how.

### `broker.database_image`

- Type: string
- Default: unset
- Since: 0.24.0

The database image beside an AnythingMCP gateway, pinned to a tag.

Empty means docker.io/library/postgres:17.12-alpine. The database
publishes no port: the two containers share a unix socket under
<daemon.state_dir>/broker/run. Jentic One keeps SQLite files and
runs no database container, so this is unused for jentic.

### `broker.bind`

- Type: string
- Default: `127.0.0.1`
- Since: 0.24.0

The address the gateway's ports are published on.

Loopback by default: the relay that reaches it runs on this machine.
Anything wider puts the gateway's dashboard and MCP endpoint on that
network, and the dashboard's own sign-in is all that guards it.

### `broker.port`

- Type: integer
- Default: `47400`
- Since: 0.24.0

The host port of the gateway's MCP endpoint and API.

For jentic this is also its dashboard, which shares the port.

### `broker.dashboard_port`

- Type: integer
- Default: `47300`
- Since: 0.24.0

The host port of AnythingMCP's dashboard.

Where you register the first (admin) account, sign in to SaaS apps
with your own OAuth clients and make a gateway server for each agent
or team. Unused for jentic, whose dashboard is on broker.port.

### `broker.connection`

- Type: string
- Default: unset
- Since: 0.24.0

Which podman runs the broker, as computer.container.connection says.

Empty is this user's rootless podman. A remote podman runs the
containers there while broker.bind and the relay still dial this
machine, so a remote one also needs a tunnel to its ports.

### `broker.restart`

- Type: enum
- Values: never, on-failure, always
- Default: `always`
- Since: 0.24.0

What to do when a broker container stops without being asked to.

always starts it again however it ended. on-failure starts it again
only when it exited non-zero or was killed for running out of
memory; a clean exit, a container that never ran and one removed
from podman are left down. never leaves it down whatever the exit.
A container left down leaves the broker failed, and broker status
says how it ended and why it was not restarted. on-failure needs
podomation 1.0.3 or later, whose inspect reports the exit code;
with an older one it cannot tell a failure and restarts nothing.
Five restarts within ten minutes stop the retries, so a gateway
that cannot start is reported rather than restarted for ever.
