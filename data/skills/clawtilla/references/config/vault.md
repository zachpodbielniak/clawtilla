# Configuration: `vault`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `vault`

Secrets an agent can have typed into a web page without ever seeing
them: passwords, and TOTP seeds the daemon turns into one-time codes.

  vault:
    - name: github
      secret: {command: "pass show web/github"}
      sites: [github.com]
    - name: github-2fa
      kind: totp
      secret: {file: github-totp}
      sites: [github.com]

An agent may ask only for the names its agents.browser.secrets lists.
It calls clawtilla_secret_ref for a one-use reference bound to it and
to the conversation it is in, then clawtilla_browser_fill_secret with
that reference and a field. The daemon resolves the value, types it
into the field as key input, and the agent hears only that it was
filled. Screenshots and clawtilla_browser_eval are refused on that
page until the browser navigates away. Needs a GSURF=1 build.

### `vault.name`

- Type: string
- Default: unset
- Required: yes
- Since: 0.25.0

What an agent asks for, and what agents.browser.secrets lists. The
name is all an agent or a client ever sees of the entry's secret.

### `vault.description`

- Type: string
- Default: unset
- Since: 0.25.0

Whose login this is, or which field it belongs in.

### `vault.kind`

- Type: enum
- Values: password, totp
- Default: `password`
- Since: 0.25.0

`password` types the value itself. `totp` reads the value as a TOTP
seed -- base32, or an otpauth://totp/ URI with its algorithm, digits
and period -- and types the RFC 6238 code for the current time.

### `vault.secret`

- Type: secret reference
- Default: unset
- Required: yes
- Since: 0.25.0

A secret reference, never the value: {file: ...}, {env: ...} or
{command: ...}, resolved the way secrets: says, at the moment of the
fill and not before.

### `vault.sites`

- Type: list of strings
- Default: unset
- Required: yes
- Since: 0.25.0

The hosts the secret may be typed into: a page on any other host is
refused, so an agent steered to a look-alike site cannot be made to
fill it there. `example.com` also matches its subdomains
(login.example.com); a page whose address cannot be read is refused.
An entry without sites is never filled.
