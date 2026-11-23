# Configuration: `federation`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `federation`

Federation: this fleet linked with other clawtilla fleets -- yours on
another machine, or other people's.

Each fleet has a key; every request between fleets is signed by it and
answered signed, whatever carries it: a Cloudflare tunnel (https://),
a tailnet or LAN address (http://, or clawtilla:// for the daemon's
own IPC port), or a socket on this machine. A linked fleet has a tier
-- owned, trusted, limited, untrusted -- that presets what it may do
here, with tags and rules to adjust it. Peers themselves are kept in
<state_dir>/federation/peers.yaml and managed with `clawtilla
federation`, not edited here. See docs/federation.org.

### `federation.enabled`

- Type: boolean
- Default: `false`
- Since: 0.66.0

Whether this daemon links with other clawtilla fleets -- yours on
another machine, or a colleague's. On, it keeps a fleet key
(<secrets.dir>/federation/fleet.ed25519, whose fingerprint becomes the
fleet id), answers on federation.listen and, with federation.ipc, on its
own IPC port, and dials every linked fleet that has an endpoint. Every
request between fleets is signed, whatever carries it. Off, none of
that exists. Takes effect at the next daemon start. See
docs/federation.org.

### `federation.name`

- Type: string
- Default: unset
- Since: 0.66.0

What other fleets call this one: shown on their sidebar section and in
invites. Empty is the machine's host name.

### `federation.owner`

- Type: string
- Default: unset
- Since: 0.66.0

Who runs this fleet, as other fleets see it: a name, not an account.
Their `owner:` filters and rules match it.

### `federation.public_url`

- Type: string
- Default: unset
- Since: 0.66.0

The endpoint invites tell other fleets to reach this one at:
`https://fleet.example.com` behind a Cloudflare tunnel or a TLS proxy,
`http://100.64.0.7:7711` for the federation listener on a tailnet, or
`clawtilla://100.64.0.7:8792` for this daemon's own IPC port. Empty,
an invite names the tailnet IPC address when federation.ipc is on and
the daemon is on a tailnet, and otherwise asks for this to be set.
`clawtilla federation expose --cloudflare` prints a tunnel config and
the value to put here.

### `federation.listen`

- Type: list of strings
- Default: unset
- Since: 0.66.0

Where the HTTPS/WebSocket federation listener answers, each `unix:PATH`
or `HOST:PORT`. Empty is `unix:<state_dir>/federation.sock`, which
cloudflared reaches directly (`service: unix:...`) and nothing else on
the network can. A HOST:PORT must be loopback, a tailnet address, or
-- with federation.allow_lan_plaintext -- a LAN address: the listener
speaks plain HTTP and leaves TLS to whatever stands in front of it. A
listener that cannot bind is a warning, not a failed start.

### `federation.ipc`

- Type: boolean
- Default: `true`
- Since: 0.66.0

Whether other fleets may also speak to this daemon over its own IPC
port (daemon.tcp_address and the tailnet address) -- the simple
daemon connection: `clawtilla://HOST:PORT`, no extra port, no HTTP. A
connection that says `fed.hello` is a peer and may only send
federation frames; it never sees the event stream or any other
request.

### `federation.allow_lan_plaintext`

- Type: boolean
- Default: `false`
- Since: 0.66.0

Whether a link may use a local-network address (RFC 1918, link-local,
a name that is not localhost or *.ts.net) without TLS. Signed
envelopes keep it authentic either way; federation.seal keeps it
private. Plain text to a public address is refused whatever this says.

### `federation.seal`

- Type: enum
- Values: auto, always, never
- Default: `auto`
- Since: 0.66.0

Whether bodies between fleets are encrypted with a key the two agree
(X25519 + ChaCha20-Poly1305) on top of whatever carries them: `auto`
on plain-text LAN links only, `always` on every link (so a tunnel's edge
sees only ciphertext), `never`.

### `federation.inbox_agent`

- Type: string
- Default: unset
- Since: 0.66.0

The agent a peer reaches when its grant allows only the inbox
(untrusted fleets, by default). Empty is the chief of staff.

### `federation.invite_ttl_hours`

- Type: integer
- Default: `72`
- Since: 0.66.0

How long an invite can be used, in hours, unless the invite says
otherwise. An invite is single-use.

### `federation.clock_skew_seconds`

- Type: integer
- Default: `300`
- Since: 0.66.0

How far another fleet's clock may be from this one's before its
signed requests are refused as stale. Each nonce is remembered for
this long, across restarts.

### `federation.max_body_bytes`

- Type: integer
- Default: `1048576`
- Since: 0.66.0

The largest request another fleet may send, in bytes. Refused before
it is read whole.

### `federation.ping_seconds`

- Type: integer
- Default: `30`
- Since: 0.66.0

How often each linked fleet is pinged to keep its link health
current. Three missed answers mark it offline.

### `federation.reconnect_max_seconds`

- Type: integer
- Default: `300`
- Since: 0.66.0

The longest wait between attempts to reach a fleet that is not
answering; the wait doubles from one second up to this.

### `federation.outbox_hours`

- Type: integer
- Default: `72`
- Since: 0.68.0

How long a message to a linked fleet that cannot be reached waits in
the outbox (<state_dir>/federation/federation.db) before it is given up,
with a note in its conversation saying so. It is sent the moment the
fleet answers again; nothing is dropped silently.

### `federation.audit_retention_days`

- Type: integer
- Default: `90`
- Since: 0.66.0

How long the record of every request between fleets
(<state_dir>/federation/federation.db) is kept.

### `federation.rules`

- Type: list of strings
- Default: unset
- Since: 0.66.0

Grant overrides for every linked fleet a selector matches, applied in
order after the fleet's tier and before its own overrides, each
`SELECTOR: field=value; field=value`. A selector names fleets by
`tier:`, `tag:`, `owner:`, `name:`, `online`, with `!`, `+` (and) and
`,` (or): `tag:client-acme: reach=listed; agents=team:frontend;
cap.exec=deny`. `clawtilla federation explain ALIAS` shows which rule
set what. See docs/federation.org#rules.
