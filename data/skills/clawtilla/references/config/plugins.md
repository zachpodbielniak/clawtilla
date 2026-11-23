# Configuration: `plugins`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `plugins`

Loadable plugins: event handlers, extra tools, computer backends and
integrations.

A plugin that fails to load disables itself and nothing else.

### `plugins.enabled`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Whether plugins are scanned for at all.

### `plugins.dirs`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Extra directories to scan, searched before the XDG and system paths.

### `plugins.disabled`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Plugin ids to skip, by the <id> in libclawt-plugin-<id>.so.

This is a plain list rather than a per-plugin enabled flag so that a
third-party plugin the core has never heard of can still be turned off.
