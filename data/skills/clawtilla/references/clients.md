# clawtilla clients and the IPC protocol

The front ends, their flags and pages, saved connections, and enough of the wire protocol to script against the daemon socket directly. Source docs: `docs/clients.org`, `docs/web-client.org`, `docs/lrg-client.org`, `docs/office.org`, `docs/ipc-protocol.org` (the full kind list).

All clients are thin: none holds state, a transport or a credential of its own; each sends typed requests and folds the daemon's event stream. Two clients open at once therefore agree. None of them needs a local daemon: each opens, shows a "not connected" banner, keeps retrying, and can switch to a saved connection.

## The five front ends

| Binary | What | Build |
|---|---|---|
| `clawtilla` | CLI, every verb; see [cli.md](cli.md) | always |
| `clawtilla-gtk` | GTK4 + libadwaita desktop app, single-instance | always (skipped, loudly, without libadwaita) |
| `clawtilla-web` | htmx web app over HTTP, separate process | always |
| `clawtilla-lrg` | same app drawn with libregnum on OpenGL | `make LRG_BACKEND=1` |
| `clawtilla-office` | the fleet drawn as people in a building | `make OFFICE=1` |

### Flags

| Client | Options |
|---|---|
| `clawtilla` | `-c/--config FILE`, `-s/--socket FILE`, `-p/--profile NAME`, `-H/--host HOST`, `--port PORT` (8792), `--token TOKEN`, `-V`, `--license`, `--generate-config`, `--generate-systemd-service`. Globals go before the verb |
| `clawtilla-gtk` | `-s/--socket`, `-p/--profile`, `-H/--host`, `--port`, `--token`, `-o/--open ROOM` (goes to the running window), `--office`, `-V`, `--license` |
| `clawtilla-lrg` | as gtk, plus `--self-test PAGE[:TARGET]` (lay out one page headless, print JSON); `--open` always opens a new window |
| `clawtilla-web` | `-p/--port` (8790), `-b/--bind ADDR` (repeatable, replaces defaults, must bind), `--no-tailscale`, `--office` (send `/` to the Office page), `-s/--socket`, `--profile`, `-H/--host`, `--daemon-port` (8792), `--token`, `-V`, `--license` |
| `clawtilla-office` | `-s`, `-p`, `-H`, `--daemon-port`, `--token-file FILE`, `--token` (deprecated), `-w/--world ID`, `--2d`, `--3d`, `--simulate`, `--screenshot FILE` (+ `--frames N`, default 90), `--embed FD`, `--width` (1280), `--height` (720), `-f/--focus ID`, `--adapter-dir DIR` |

`--open` accepts an agent id (its chat), a group room, a direct room `dm:<a>:<b>`, or `clawtilla:<room>` (what an org-mode link hands over). An unknown name is reported in the window, not opened.

GTK and lrg pick the daemon in this order: `--host`, `--profile`, `--socket`, the connection last switched to in Settings (remembered in `$XDG_STATE_HOME/clawtilla/last-connection`), then the local socket.

`clawtilla-office` run bare follows the clients instead: `--host`/`--profile`/`--socket` if given; else the daemon a running GTK or lrg client is on now; else the last-switched connection; else the local socket. It prints which and why on stderr. Running GTK/lrg processes publish their connection in `$XDG_RUNTIME_DIR/clawtilla/clients/current-connection-PID` (0600; removed on clean exit; a dead pid's file is ignored; newest wins). The file never holds a token: a remote daemon's token comes from the matching saved profile or the token file the client named, so a client started with `--host --token` on an unsaved daemon is not followed. `--token` on `clawtilla-office` warns (argv is world-readable); use `--token-file FILE` with `--host`, or `--profile`.

### `clawtilla-web` specifics

- Listens on loopback **and** the tailnet address when there is one; never falls back to all interfaces. **It has no login**: anything that reaches a bound address drives the whole fleet. Do not bind it on an unauthenticated network.
- Works without JavaScript (every action is a form post); htmx and SSE (`/events`, every daemon event republished as `fleet`) make it live. Scripts are vendored under `data/web`; nothing is fetched at page load.
- Routes: `/` and `/fleet`, `/a/<id>/<view>` (agent pages, nick from the page table), `/r/<room>` (group room; also the open-room link), `/settings/<page>`, `/alerts`, `/decisions`, `/tasks/<task>`, `/office/...`, `/new`, `/import`, `/design`. `/a/:id/:view` is registered last and must stay last.
- Check routing against a running instance: `make web-smoke WEB_SMOKE_URL=http://127.0.0.1:8790 WEB_SMOKE_AGENT=chief`.
- Installable as an app (manifest, no service worker). Pair a phone from its Settings > Devices page or `clawtilla device pair --qr`.

### `clawtilla-office`

Needs `office.enabled: true` (`clawtilla office on`). `clawtilla office open` starts it; GTK embeds it (`--embed`); web draws its own office at `/office`. GTK and lrg also have **Open office** (Office page button beside 2D / 3D, and main menu -> Open office -> 2D / 3D): a separate window on the client's own connection -- `--socket`, `--profile`, or for a typed `--host` `--host`/`--daemon-port` with the token via a pipe as `--token-file /dev/fd/3`, never `--token`; not waited for; logged and toasted with the daemon (never the token). Not in the web client (a server would open it on its own display). Window keys: Tab switches 2D/3D, arrows or right-drag turn the camera, wheel zooms, Escape quits. See [office.md](office.md).

## Pages and sections (gtk, web, lrg)

The page list is the library's `ClawtPage` / `ClawtSection` table (`clawt_section_count()`, `clawt_section_page_count()`), walked by every client; never hard-code it.

| Section | Pages (nick) |
|---|---|
| Chat | Chat (`chat`) |
| Agent | Overview (`agent`), Mailbox (`mailbox`) |
| Computer | Computer (`computer`): Shell / Screen / Mounts / Exchange / Browser sub-views |
| Automation | Routines (`routines`), Triggers (`triggers`) |
| Work | Tasks, Decisions, Flow, Activity, Fleet, Grok, Office |
| Library | Skills (`skills`), Memory (`memory`) |

A section's tab shows the sum of its pages' badges (Decisions waiting surface on Work). Settings (connections, appearance, integrations, connectors, teams, history, fleet hold, devices...) sit behind the app menu / `/settings`. Slash commands in the composer are the same set in every client.

Appearance is per client machine, not in the fleet config: `$XDG_CONFIG_HOME/clawtilla/appearance.yaml`, shared by gtk and lrg (lrg also reads its own `lrg_*` keys: motion, material, background, accent, scale).

## Parity rule

A capability added to one graphical client must exist in all three (gtk, web, lrg). `make parity` (also run by `make docs-check` and `make test`) compares five layers: IPC frame kinds sent, slash commands answered, library enumerations walked, library vocabulary spelled out, and declared affordances (a marker per client). It cannot see a frame kind built with `g_strconcat()` (write kinds as literals) nor a feature nobody declared. Deliberate exceptions live in the map in `tools/clawt-client-parity.sh`, each with a reason. Rules both clients apply belong in libclawt (e.g. `clawt_unread_should_count()`, `clawt_ipc_reply_refusal_text()`), not in a client.

## Saved connections

`$XDG_CONFIG_HOME/clawtilla/connections.yaml`, mode 0600, read and written by every client and by `clawtilla remote`. Top-level `connections:` list; each entry has `name` and either `socket` (local) or `host`, `port`, `token`, `tls`, `accept_unknown_certificate`. Manage with `clawtilla remote list|add|pair|rm`; see [operating.md](operating.md) for pairing. `remote list` never prints a token.

## IPC protocol primer

Same protocol on the unix socket (`daemon.socket`, default `$XDG_RUNTIME_DIR/clawtilla/daemon.sock`) and on the TCP/tailnet listener (port `daemon.tcp_port`, 8792).

**Framing.** Newline-delimited JSON, UTF-8, one object per line, at most 8 MiB per frame (larger closes the connection). A malformed frame gets an error reply and the connection stays open. A client that stops reading is disconnected once ~17 MiB is queued for it.

**Envelope.** Request: `{"v":1,"kind":"agent.list","id":"r1","payload":{}}`. `v` must equal `CLAWT_IPC_VERSION` (1); `id` is echoed back. Reply adds `ok`; on failure `code` (a `ClawtErrorCode`: 0 FAILED, 1 INVALID_ARGUMENT, 2 NOT_FOUND, 3 ALREADY_EXISTS, 4 NOT_SUPPORTED, 5 PERMISSION_DENIED, ...) and `error` (readable, secrets redacted). A reply with nothing to say carries an empty object, never a missing one. An unknown kind is refused naming the kind.

```json
{"v":1,"kind":"agent.list","id":"r1","ok":true,"payload":{"agents":[...]}}
{"v":1,"kind":"agent.start","id":"r2","ok":false,"code":2,"error":"there is no agent called 'ghost'"}
```

**Authentication.** Unix socket: none (0600 is the access control). TCP: the first frame must be `control.hello` with `payload.token` (owner token or a paired device's); anything else is refused with AUTH. The only exception is `device.redeem` (`code`, optional `name`), which is itself the handshake and returns the device's `token` once.

**Request/response.** Handlers run on the daemon's single main loop. Kinds that wait on the network or a command (`computer.exec`, `integration.health`, `integration.matrix_login`, `connector.begin`/`await`/`discover`/`refresh`/`revoke`, ...) are **deferred**: the reply arrives later with the same `id`, and other requests are answered meanwhile. Match replies by `id`, not by order. Handlers that re-render agents answer with `refused` (array of `{agent, message}`), present even when empty; `agent.set` also answers `restart_required`.

**Events.** Send `control.subscribe` with `payload.cursor` (the last cursor seen; 0 replays the whole ring). Reply: `cursor` (current) and `resumed` (false means the cursor fell off the 1024-event ring: re-fetch state, do not trust a partial replay). Then replayed and live frames:

```json
{"v":1,"kind":"event","payload":{"kind":"agent.started","subject":"researcher","ts":1755792000000000,"cursor":42,"detail":{"from":"chief-of-staff"}}}
```

`ts` is microseconds. `subject` is what the event is about: `message` and `message.refused` name the **room** (match transcripts on the room, deduplicate on `detail.id`); `task.changed` names the task; `agent.state` fires for every state (an agent that crashed raises only this). `turn.step` is live-only (not in the ring or the log); catch up with `room.steps`. To start from now: read `cursor` from `control.status`, subscribe with it. Event history beyond the ring: `event.list` (`subject`, `limit`) reads the on-disk log.

**Useful kinds** (all verified in the daemon; full payloads in `docs/ipc-protocol.org`):

| Kind | Payload -> reply |
|---|---|
| `control.status` | -> `version`, `commit`, `config`, `agents`, `connected`, `clients`, `cursor`, `hold` |
| `control.reload`, `control.pause`/`control.resume` (`agent`), `control.shutdown` | reload answers `refused`; pause/resume answer `hold` (`held`, `draining`, ...) |
| `agent.list`, `agent.show` (`agent`), `agent.start`/`stop`/`restart`/`interrupt`/`reset` | lifecycle |
| `agent.set` (`agent`, `key`, `value`) | `refused`, `restart_required`, `shadow` |
| `msg.send` (`target`, `body`, `from`) | `queued`, `target_state` |
| `room.list`, `room.history`, `room.steps`, `room.read` | rooms and transcripts |
| `task.list`, `decision.list`, `decision.answer`, `activity.list`, `notice.list`, `mailbox.list` | work in flight |
| `config.show`, `config.render` (`agent`), `config.validate` | YAML text, validation |
| `event.list`, `sentinel.scan`, `sentinel.report` | history and issues |

Over a raw socket (bash, `socat`):

```bash
sock="${XDG_RUNTIME_DIR}/clawtilla/daemon.sock"
printf '%s\n' '{"v":1,"kind":"control.status","id":"1","payload":{}}' \
	| socat -t 2 - "UNIX-CONNECT:${sock}"
```

Keep one connection open for a subscription and send requests on the same line-oriented stream; events (no `id`) interleave with replies. TCP connections are armed with keepalive (30 s idle, 10 s interval, 3 probes). Prefer the CLI for anything it covers; use the socket for event-driven automation.

From C, the library does all of this: `clawt_client_new()`, `clawt_client_connect()`, `clawt_client_set_auto_reconnect()`, `clawt_client_subscribe()`, the `event` signal, `clawt_client_request()` (sync; iterates the caller's context) or `clawt_client_request_async()`. Auto-reconnect re-subscribes from the last cursor but does not report `resumed`, so re-fetch on reconnect. The same API is reachable through GObject Introspection (`Clawt-1.0`), and a host with its own main loop can embed the whole daemon with `clawt_daemon_new(config_path, context)` instead of using a socket.

Adding a frame kind (developers): `docs/development.org` "Adding an IPC frame kind"; every graphical client must use it (parity), and a new optional member does not bump `CLAWT_IPC_VERSION`, a changed meaning does.
