# Automation: podomation pods

The daemon embeds a podomation engine and registers **itself** as the module `clawtilla`, which both emits fleet events and performs fleet actions. Continues [routines-and-triggers.md](routines-and-triggers.md). Source: docs/automation.org. For a look-only morning sweep a `read_only` / `proactive` routine is simpler.

## Where pods live

```yaml
daemon:
  automation_dir: "~/.clawtilla/pods"   # every *.pod loaded at daemon start; missing dir is fine
```

- A file that fails to parse disables **that file** only; the daemon log is the only report (`automation: <file> could not be read: line N, column M ...` and `automation: N pod files loaded from <dir>`). No IPC verb lists pods or their errors.
- An engine that will not start disables automation and nothing else.
- `podomation validate` / `list-modules` (the standalone CLI) do **not** know the `clawtilla` module (it is not a `.so`), and `validate` checks syntax and module names only: a misspelled event or action passes and fails at dispatch.

## Scope

```
clawtilla->new()                        every agent
clawtilla->new("researcher")            one agent
clawtilla->new("researcher", "scribe")  a group
```

Scope fences **agents in both directions**: a pod scoped to `scribe` neither hears nor acts on `researcher` (refused with a warning). Events with no agent (daemon reload, routine failures) reach every pod; actions naming no agent (`post_room`, `run_routine`, `notify` without `agent:`) are unscoped.

## Events

Names are `on_*` (the DSL cannot parse dots). Every event carries `timestamp` (microseconds) and `subject`; `agent` when it is about one (the sender for `on_message`).

| Event | Extra fields |
|---|---|
| `on_agent_created` / `_removed` / `_changed` / `_started` / `_stopped` | -- |
| `on_agent_state` | `state` (`stopped`, `starting`, `running`, `degraded`, `error`, ...), `detail` |
| `on_agent_connected` / `on_agent_disconnected` | -- |
| `on_agent_typing` | `typing` (`"true"`/`"false"`), `peer`, `room` |
| `on_message` | `id`, `from`, `to`, `body`, `task` (subject = room) |
| `on_task_changed` | `state`, `agent` (assignee); fires for **every** state -- guard on `completed` |
| `on_routine_ran` / `on_routine_changed` | -- (subject = routine id) |
| `on_routine_turned_off` | `agent`, `failures`, `error` |
| `on_integration_changed` | -- (subject = instance) |
| `on_daemon_started` / `_reloaded` / `_stopped` | timestamp only |

Not forwarded (cannot be bound): `config.changed`, `room.*`, `team.changed`, `message.refused`, `mailbox.*`, `decision.*`, `computer.exec` and `image.*`. Trigger events are bindable since 0.65.1: `on_trigger_fired`, `on_trigger_posted`, `on_trigger_changed`, `on_trigger_refused`, `on_trigger_verified` (subject: the trigger id). `on_trigger_refused` is the one worth binding -- a burst on a trigger nobody is rotating is somebody guessing at the endpoint. The forwarder copies a fixed set of detail keys, so a new daemon detail does not reach pods until added there.

## Actions

| Action | Parameters (optional in italics) |
|---|---|
| `message_agent` | `agent`, `body`, *`priority`* (`low|normal|high|urgent`) |
| `post_room` | `room`, `body` |
| `delegate` | `agent`, `prompt` -> returns `id` (task) |
| `start_agent` / `stop_agent` / `restart_agent` | `agent` |
| `notify` | `title`, *`body`*, *`agent`* (goes to the fleet's notify integrations) |
| `run_routine` | `routine` -> returns `id` (task); measured like a scheduled run |
| `memory_add` | `agent`, `content`, *`category`*, *`summary`*, *`importance`* (`low|normal|high|critical`), *`tags`* -> `id`; fails if the agent's `memories.enabled` is off |
| `computer_exec` | declared and **always refused** -- use podomation's own command modules or delegate to the agent |

Every action returns `ok`; capture with `res name := ... => ...` and read `{name->id}`.

## Examples

```
# restart an agent that fell over, and say so  (guard is essential:
# without it every state change, including the restart's own, re-fires)
pod fleet = clawtilla->new();
fleet->on_agent_state where event->state == "error" =>
    clawtilla->restart_agent(agent: "{event->agent}");
fleet->on_agent_state where event->state == "error" =>
    clawtilla->notify(title: "{event->agent} fell over", body: "{event->detail}");

# run a routine from podomation's cron (plain numbers only, see below)
pod nightly = cron->new("0 3 * * *");
nightly->on_schedule => clawtilla->run_routine(routine: "nightly-review");

# poll an agent every ten minutes (timer is ms; first tick one interval after start)
pod every_ten = timer->new(600000);
every_ten->on_tick => clawtilla->message_agent(agent: "scribe", body: "Anything to report?");
```

## DSL gotchas

- Quote anything with a hyphen: `clawtilla->new("deb-tester")`; bare `deb-tester` is a parse error.
- `#` is the only comment; `/* */` is a lexer error.
- Handler arguments arrive as a padded positional tuple; clawtilla maps them back by its declared parameter order.
- `on_message` fires for every message the scoped agent sends, replies included: a pod that delegates on it answers itself unless guarded (`where event->to == ...`).
- **podomation's `cron` module parses plain numbers and `*` only.** `*/6` becomes `0`, `1-5` becomes `1`, `1,15` becomes `1`, silently. Anything with a range, list or step belongs in a clawtilla routine, run from a pod with `run_routine` if wanted.

## The other direction

Container, distrobox and VM computers drive podomation's `container`, `distrobox` and `vm_virtmanager` modules as `.so` plugins through `ClawtPodBridge`, searched from `daemon.pod_module_dir` first. No pod or DSL is involved; see [computers.md](computers.md).
