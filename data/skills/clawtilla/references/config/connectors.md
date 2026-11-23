# Configuration: `connectors`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `connectors`

Connected accounts: how clawtilla obtains credentials and keeps them.

A connector integration gives an agent the tools of a service without
giving it the credential. The daemon runs the authorization, holds the
token, renews it, and injects it into the tool server -- so the value
is never in the agent's .mcp.json, its environment or its transcript.

What that is worth depends on where the agent runs. For a container
or VM agent the boundary is real: the token file and the relay are on
the host and the agent cannot reach either. For an unconfined host
agent it is protection against leaking a credential by accident, not
against one that goes looking -- see docs/security.org.

### `connectors.dir`

- Type: path
- Default: `$XDG_CONFIG_HOME/clawtilla/connectors.d`
- Since: 0.2.0

Directory of extra connector definitions.

Each `.yaml` file holds a `connectors:` list, and an entry whose id
matches a built-in one replaces it. These are somebody else's URLs:
a provider that moves an endpoint should be fixable here rather than
by waiting for a clawtilla release.

### `connectors.refresh_margin_seconds`

- Type: integer
- Default: `300`
- Since: 0.2.0

How long before expiry to renew a token.

An access token often lasts an hour, and one that expires while the
request carrying it is in flight fails exactly like a wrong one --
the agent sees only a refusal from a service it was using a minute
ago.

### `connectors.redirect_port`

- Type: integer
- Default: `8765`
- Since: 0.2.0

Loopback port for the authorization-code flow's redirect.

Only used by providers with no device flow. It has to be a fixed
number rather than whatever the kernel offers, because the redirect
URI is registered with the provider in advance and most match it
exactly: register `http://127.0.0.1:8765/callback`.

Bound on 127.0.0.1 only. A redirect carries an authorization code in
a URL, and a listener on every interface offers that code to whoever
on the network reaches the port first.

A client clawtilla registers for itself by dynamic client registration
is registered with this redirect, so changing the port means signing
in again.

### `connectors.discovery`

- Type: boolean
- Default: `true`
- Since: 0.20.0

Whether a remote connector finds out for itself how to sign in.

A connector reached by URL whose catalogue entry names no endpoints
asks the server: its RFC 9728 protected-resource document names an
authorization server, whose RFC 8414 metadata names the endpoints,
and RFC 7591 dynamic client registration supplies a client id when
the connector has none. Endpoints a catalogue entry names by hand
always win, and so does a configured `client_id`.

Only when somebody asks for a sign-in -- never at daemon start. No
request in it follows a redirect, and nothing it finds may be plain
http unless the server itself is on loopback. Off, such a connector
takes a pasted token only.

### `connectors.client_name`

- Type: string
- Default: `clawtilla`
- Since: 0.20.0

What clawtilla calls itself when it registers with a server.

Sent as `client_name` in a dynamic client registration, so it is what
the server's consent screen shows the person approving it -- worth
setting to something that says which machine is asking when several
fleets sign in to one service.

### `connectors.registry_enabled`

- Type: boolean
- Default: `false`
- Since: 0.2.0

Whether the open MCP registry is imported into the catalogue.

Off by default because it reaches the network. When on, the fetch
happens on a timer into a cache on disk -- never at daemon start and
never from a request handler, so a client pressing a button waits on
this machine and not on somebody else's. `connector.registry_refresh`
asks for one sooner than the timer would.

An imported entry only ever fills a gap: it never replaces a
built-in connector or one from connectors.dir, which are both a
choice somebody already made on purpose.

### `connectors.registry_url`

- Type: string
- Default: `https://registry.modelcontextprotocol.io`
- Since: 0.2.0

Base URL of the MCP registry to import from.

Somewhere else entirely is a legitimate answer: the registry is an
ordinary HTTP service and an internal mirror serves the same
shape.

### `connectors.registry_refresh_hours`

- Type: integer
- Default: `24`
- Since: 0.2.0

How often the cached registry listing is refreshed.

Checked against the cache's own `fetched_at`, not against when the
daemon last started -- a restart must not reset this clock, or a
fleet that bounces every hour would refresh every hour regardless of
what this key says.

### `connectors.directory_min_tools`

- Type: integer
- Default: `40`
- Since: 0.18.0

More tools than this makes a connector's server a directory.

Counted after the connector's `tools:` narrowing, because that is the
list the agent would otherwise read. Forty is where a tool list starts
to cost more than it helps -- and API-model engines refuse more than
128 tools a turn across every server they have. Only read when a
connector's `directory` is `auto`.

### `connectors.directory_min_chars`

- Type: integer
- Default: `100000`
- Since: 0.18.0

More characters of tool definitions than this makes a directory too.

The names, descriptions and input schemas together, which is what the
model actually reads on every call. A server with thirty tools can
still be a hundred thousand characters when each one carries a page of
documentation; about where Claude Code starts searching its own tools
rather than listing them. Only read when a connector's `directory` is
`auto`.

### `connectors.max_result_bytes`

- Type: integer
- Default: `65536`
- Since: 0.27.0

The most of one connector tool result an agent is shown, in bytes.

For every connector that does not set max_result_bytes itself. Past
it the result is trimmed, announced at both ends, and kept whole
under the agent's state directory for clawtilla_mcp_result_query, so
one listing of a big repository does not spend the whole context. 0
turns the budget off; anything below 1024 is read as 1024. Whatever
this says, the relay holds at most 64 MiB of one message in memory --
anything longer goes to disk as it arrives.

### `connectors.apps_web_url`

- Type: string
- Default: `http://127.0.0.1:8790`
- Since: 0.27.0

Where the web client is reached, for an MCP app's Open in browser.

The GTK and lrg clients cannot draw an app's HTML, so they link to the
web client's view of it: <this>/a/<agent>/app/<app>. The address
clawtilla-web listens on by default; change it when the web client
runs elsewhere or on another port.
