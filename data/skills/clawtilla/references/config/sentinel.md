# Configuration: `sentinel`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `sentinel`

What `clawtilla sentinel` looks for in the event log.

The sentinel is a read-only scan of <state_dir>/events/*.ndjson that
groups failing tool calls, long queue waits, spending and stalled turns
into issues and writes them as YAML. It runs out of process: the
clients' Issues view asks the daemon to start `clawtilla sentinel`
and then shows what it wrote. It never writes to the event log.

### `sentinel.days`

- Type: integer
- Default: `7`
- Since: 0.26.0

How many days of the event log one scan reads, counting today. 0
reads every file there is, which daemon.event_log_days bounds.

### `sentinel.min_failures`

- Type: integer
- Default: `3`
- Since: 0.26.0

How many failures of one agent's tool make an issue. A tool that
failed once is a turn's ordinary business; one that failed this many
times in the window is a pattern somebody should look at.

### `sentinel.queue_wait_seconds`

- Type: integer
- Default: `600`
- Since: 0.26.0

How long a message may wait in an agent's queue, from being queued to
being delivered, before the wait is an issue. Messages still queued
past it when the scan ends are reported too.

### `sentinel.daily_spend_micros`

- Type: integer
- Default: `10000000`
- Since: 0.26.0

One agent's spend in one day, in micro-dollars, that makes an issue.
0 reports only the days a cap was reached.

A line to report, not a limit: nothing is refused at it. The caps that
do refuse work are orchestration.daily_fleet_budget_micros and each
agent's own, and every time one is reached is an issue whatever this
says.
