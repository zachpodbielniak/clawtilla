# Configuration: `grok`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `grok`

Handing work to Grok bots, and hearing back.

Each bot is a `grokbot` integration named for it -- `chief`,
`research` -- whose url is a Grok routine's webhook. A job is POSTed
there with a callback address minted for it, and the bot reports
back to the daemon's webhook listener over the tailnet, by HTTP or
by the clawtilla MCP tools. These keys are about that return path.
See docs/grok.org.

### `grok.callback_host`

- Type: string
- Default: unset
- Since: 0.10.0

The name a Grok bot uses to reach this daemon.

Empty means the machine's own tailnet name, asked of tailscaled as
soon as the daemon starts -- the FQDN `tailscale status` shows for
it -- and a job sent before it answers waits for the answer. This is
an override, for a machine whose tailscaled the daemon's user cannot
read or a name other than its MagicDNS one. Either way the listener
(daemon.webhook_enabled) has to be bound on the tailnet
(daemon.tailscale): a job is refused rather than sent with an
address nothing answers on.

### `grok.job_ttl_minutes`

- Type: integer
- Default: `120`
- Since: 0.10.0

How long a job may go unanswered before it is given up.

Longer than the half hour an unattended Grok run's approval card
waits, because a run that stalls on one is the usual reason a
report is late. When it lapses the agent is told, with Grok's run
id, and a report arriving afterwards is refused. A job may ask for
less or more; this is the default.

### `grok.attention_days`

- Type: integer
- Default: `3`
- Since: 0.52.0

Days a job that needs somebody keeps counting on the Grok badge.

A job that failed, was refused, expired or asked for input needs
somebody. It counts on the badge for this many days after it
settled, then stops asking -- it stays listed until it is removed
or grok.retention_days passes. 0 means it counts until it is
removed. Clamped to 0..365.

### `grok.retention_days`

- Type: integer
- Default: `30`
- Since: 0.52.0

Days a settled job is kept for the Grok page and `grok jobs`.

Pruned at start and then hourly. A job still open is never pruned:
it expires through grok.job_ttl_minutes first, and its agent is
told. Clamped to 1..3650.
