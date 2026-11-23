# Configuration: `egress`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `egress`

Where an agent with runtime.network: gated may connect.

A gated agent's sandbox has no network of its own. Its one way out is
clawtilla's egress gate, an HTTP proxy in the daemon that resolves
names itself and connects only to public addresses: never this
machine, the LAN (RFC 1918), link-local, CGNAT and tailnets
(100.64.0.0/10), IPv6 unique-local, multicast or a cloud metadata
service. It judges the address, after resolution, so a public name
pointed at 127.0.0.1 is refused too.

### `egress.allow`

- Type: list of strings
- Default: unset
- Since: 0.17.0

Private addresses gated agents may reach anyway.

Each entry is an address (100.101.102.103), a network
(192.168.1.0/24, fd7a:115c:a1e0::/48) or a host name
(nas.example.ts.net), optionally with a port (10.0.0.5:8080,
[fd00::1]:443). A name matches the name the agent asked for and never
opens loopback; loopback needs an address. Cloud metadata, multicast,
the unspecified address and reserved space cannot be opened, and an
entry naming one is refused. One bad entry refuses the whole list, at
load and where it is added.
