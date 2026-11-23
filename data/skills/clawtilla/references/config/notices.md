# Configuration: `notices`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `notices`

The notification list the daemon keeps for every client.

Errors and notices -- a refused message, a turn that timed out, an
agent that stopped -- are recorded in <state_dir>/notices.db, read
and dismissed there, and drawn by every client from notice.list. Each
agent can be muted. These keys say how much of it is kept. See
docs/clients.org, "Notifications".

### `notices.keep`

- Type: integer
- Default: `500`
- Since: 0.52.0

How many notices are kept, newest first.

Read and dismissed ones count too: the list is a record, and a
dismissed row is still there with include_dismissed. Pruned at start
and then hourly. 0 keeps every one, until notices.keep_days.

### `notices.keep_days`

- Type: integer
- Default: `30`
- Since: 0.52.0

Days a notice is kept.

Pruned at start and then hourly, whichever of this and notices.keep
drops a notice first. 0 keeps them until notices.keep.
