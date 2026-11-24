# Integrations

How an agent reaches, or is reached from, the world outside the fleet. Accounts whose credential clawtilla holds (`type: connector`) are in [connectors.md](connectors.md); Grok bots (`type: grokbot`) in [grok-and-venture.md](grok-and-venture.md). Every key: [config-keys.md](config-keys.md) (`integrations.*`, `connectors.*`, `broker.*`).

Source docs: docs/integrations.org, docs/research.org, docs/connectors.org (broker section). Code: src/integration/clawt-integration.c (the type table), src/integration/clawt-integration-field.c (the form both clients draw).

## Three directions

| Kind | Types | What it is |
|---|---|---|
| channel | `matrix`, `email`, `webhook`, `local`, `cmacs` | a person on the other end; a message arrives as a turn and costs a turn |
| tools | `mcp`, `connector`, `libresearch`, `grokbot`, `meshy`, `pixellab` | tools beside the agent's own; free until called (`meshy`/`pixellab` are paid per call: see [media-generation.md](media-generation.md)) |
| notify | `notify` | the daemon telling a person about an agent; the agent never sees it |

`clawtilla integration types` prints the table the build has. An unknown `type` disables that instance only (config from a newer clawtilla still loads).

## Where one lives

- **Inline**, inside one agent: `agents[].integrations.<type>: {enabled: true, ...}`. For a connection only that agent uses.
- **Named instance** at top level, `integrations: [{name, type, scope, ...}]`, for several agents or the fleet.

Both go through one resolver (`clawt_integration_resolve_for_agent()`); inline blocks resolve first, then instances in file order. Nothing downstream (rendered libreclaw config, `.mcp.json`, the `TOOLS.org` region, health) cares which form it came from.

### Scope

| `scope` | Reaches |
|---|---|
| `all` | every agent, including later ones |
| `selected` | ids in `agents:` plus everyone on a team in `teams:` |
| `none` | nobody; parked with settings and credentials intact |

- Omitted `scope:` means `selected` -- with no `agents:` that reaches nobody. `connector add` without `--scope` is `none`.
- An unparseable scope (`evryone`) reaches **nobody**, with a warning. Never assume a typo widened access; never assume it worked either -- read `integration list`.
- `agents:` survives switching to `all` and back.

### one_per_agent and identity_keys

Channel types are `one_per_agent`: libreclaw renders one `channels.<type>` block per agent. When an inline block and a shared instance of the same channel type both apply, the inline one wins and the instance is dropped with a warning. `libresearch`, `meshy` and `pixellab` are also one per agent. `mcp`, `connector`, `notify`, `grokbot` may repeat.

Identity keys must differ between agents sharing one instance; put them under `per_agent:`:

| Type | Must differ per agent |
|---|---|
| `matrix` | `user_id`, `access_token` |
| `email` | `username` |
| `webhook` | `port` |
| `connector` | whatever the catalogue entry declares (`venture` declares `token_file`) |
| `local` | only one agent in the whole fleet may have it |
| `mcp`, `notify`, `cmacs`, `grokbot` | nothing |

```yaml
integrations:
  - name: home
    type: matrix
    scope: selected
    agents: [researcher, scribe]
    homeserver: "https://matrix.example.org"
    per_agent:
      researcher: {user_id: "@researcher:example.org", access_token: {file: "home-researcher-matrix-token"}}
      scribe:     {user_id: "@scribe:example.org",     access_token: {file: "home-scribe-matrix-token"}}
```

`integration list` reports two agents sharing an identity key on a shared instance (empty values are not counted). Any key may be overridden per agent, not only identity ones.

## CLI (`clawtilla integration help`)

| Verb | IPC | Does |
|---|---|---|
| `types` | `integration.types` | the type table |
| `list [agent]` | `integration.list` | instances, warnings; with an agent, what it has (inline and shared) |
| `show <name>` | `integration.list` | one instance, every member |
| `add <name> <type> [key=value ...] [--agent ID]` | `integration.add` | create |
| `set <name> [--agent ID] key=value ...` | `integration.update` | change; `--agent` writes into that agent's `per_agent` |
| `scope <name> all\|none\|a,b` | `integration.update` | who gets it (a list means `selected`) |
| `rm <name>` | `integration.remove` | remove; credential files it wrote stay on disk |
| `test <name>` | `integration.notify_test` | send a test notification (ignores events and quiet hours) |
| `matrix-login <name> <homeserver> <user> [--agent ID]` | `integration.matrix_login` | sign in; password read echo-off |
| `matrix-rooms <name> [--agent ID]` | `integration.matrix_rooms` | the account's joined rooms, by id and name |
| `health <agent> [integration]` | `integration.health` | connectivity checks; non-zero exit on any failure |

`key=value` follows the schema (`integrations.<key>` must exist or it is refused): lists comma-separated, and a secret key takes a **reference only** -- `token=file:PATH`, `access_token=env:NAME`, `password=command:"pass show x"`. A literal is refused. An empty secret field in a client means "keep what is set".

Health is `integration health <agent>`.

## Matrix

```bash
clawtilla integration add home matrix
clawtilla integration matrix-login home matrix.example.org researcher   # prompts for the password
clawtilla integration matrix-rooms home
clawtilla integration set home rooms='!abc:example.org,!def:example.org'
clawtilla integration scope home researcher
```

- The daemon uses the password once and never stores it; there is no `--password` flag. The access token goes to a 0600 file under `secrets.dir`; the reply carries only its path. The device appears as `clawtilla (<agent>)` on the account; sign it out there to revoke.
- `user_id` written is the server's answer (`@researcher:example.org`), not what was typed.
- Empty `rooms` = every joined room, including future invites. `require_mention` defaults **on**.
- Bridged rooms (Discord, Signal) are just rooms.
- Loop safety: every agent's Matrix channel lists the fleet's other Matrix accounts as `bot_users`; libreclaw answers at most three bot messages in a row per room until a person writes, and never answers `m.notice`. Add outside bots via the agent's `libreclaw:` passthrough.

## email, webhook, local, cmacs

- `email`: `imap_host`, `imap_port`, `smtp_host`, `smtp_port`, `username`, `password` (secret ref), `folders`. Health checks IMAP then SMTP.
- `webhook`: `port` (give each agent its own). Inline `agents.integrations.webhook.bind_address` defaults to loopback; widen deliberately (`0.0.0.0` or an interface address); a non-IP is refused. Endpoints/routing go in the agent's `libreclaw: channels.webhook.endpoints`, merged key by key; keys clawtilla writes (`listen_port`) are refused by name, and the sections `agent`, `ai`, `session`, `database`, `skills` are refused whole. Health binds the port and closes it. This is libreclaw's per-agent listener, not clawtilla's trigger receiver (routines-and-triggers.md).
- `local`: stdin/stdout, only for an agent run by hand; one in the fleet.
- `cmacs`: libreclaw's in-process Emacs channel.

## mcp: any MCP server, no brokered credential

```yaml
integrations:
  - name: github
    type: mcp
    scope: all
    description: "GitHub issues and pull requests"
    command: npx
    args: ["-y", "@modelcontextprotocol/server-github"]
    env: {GITHUB_TOKEN: {env: GH_TOKEN}}
  - name: remote-tools
    type: mcp
    scope: all
    url: "https://mcp.example.com/v1"
```

- `command` or `url`, never both (refused). `env` values may be secret refs, resolved **into the agent's 0600 `.mcp.json`** -- so the agent can read them. Use a `connector` instead for any credential an agent must not read.
- The server appears as `clawtilla-<name>` in `.mcp.json`. clawtilla owns `clawtilla` and every `clawtilla-*` key there (rewrites and removes them each start); never name your own server `clawtilla-*`.
- Health: `command` on the daemon's PATH, or `url` accepts a connection. The server is never started by a check.

## libresearch: web search, LQL, cited research

```bash
clawtilla integration add search libresearch url=http://127.0.0.1:8087 scope=all
# only if the server sets api.require_token:
clawtilla integration set search token=file:~/.clawtilla/secrets/libresearch.token
```

- Gives agents in scope `clawtilla_web_search`, `clawtilla_web_fetch`, `clawtilla_web_query` (LQL), `clawtilla_web_research` (cited answer, minutes), `clawtilla_web_query_environment`, plus the bundled `web-research` skill (needs `skills.enabled` and a `skills.dir`; a `web-research` in `skills.dir` wins). Codex gets the LQL reference inline in `TOOLS.org`.
- Tools are clawtilla's own on the deferred path (`tool.rpc` via `clawtilla-mcp-server`), never an MCP server. The server is reached over REST, never linked.
- Health: `GET /api/v1/query/environment`; a 401 counts as reachable (token not resolved for health).
- Errors are the server's own: 401 needs `token`; 403 wrong token or `lql.enabled: false`; 404 from research = `modules.research` off; 429 wait; 400 read the caret; 503 research busy.
- Server limits: `lql.timeout_ms` (30 s), `lql.max_model_rows` (20), `rate_limit.query_per_minute` (10). `clawtilla_web_query` shows 50 rows unless `max_rows`.

## notify: tell a person, not an agent

```bash
clawtilla integration add phone notify scope=all backend=ntfy url=https://ntfy.sh/your-topic
clawtilla integration test phone
clawtilla integration add desk notify scope=all backend=command command=receipt-print
```

| Event (`events`) | Default |
|---|---|
| `question` (agent waiting on you) | on |
| `error` (stopped unasked) | on |
| `done` (task finished) | off |
| `routine` (run failed / routine turned off) | off |

| `backend` | Needs |
|---|---|
| `desktop` | nothing (session bus on the daemon's machine) |
| `ntfy` | `url`, optional `token` |
| `gotify` | `url`, `token` |
| `matrix` | `homeserver`, `room`, `token` |
| `command` | `command`, optional `args` with `{{title}}`, `{{body}}`, `{{agent}}` (none = title and body as two args) |
| `apprise` | `apprise_url` (a **secret ref**: the URL is the credential), optional `command` |

- `quiet_hours: "23:00-07:00"` (local, wraps midnight) silences that instance fully; an unparseable range silences nothing. Two instances with different `events`/quiet hours = buzz for errors at night but not questions.
- Muted agents (`clawtilla mute <agent> [--for 2h]`) are skipped by every notifier.
- Credentials resolve at config load; an unresolvable one disables that notifier with a warning.
- `integration health` says nothing useful about a notifier (always "ok"); `integration test` is the check.
- Apprise URL is never on argv: written to a fresh 0600 temp file per send.

## Health and validation

`clawtilla integration health <agent> [name]`: matrix TCP to homeserver; email IMAP+SMTP; webhook binds the port; mcp PATH/url; connector from disk (provider, token loads, unexpired or renewable); notify nothing. Checks are connectivity, not login; they run async and one at a time.

Load-time validation names: a missing required key, a credential with no secret reference, mcp with neither/both of command and url, shared identity keys, two `local`, `selected` naming a missing agent, an unreadable instance (`integration 'x' is disabled: <why>`).

## What the agent is told

A `# BEGIN clawtilla integrations` region in the agent's `TOOLS.org`, rewritten on start when changed; text outside the markers is never touched. Write a good `description:` on every integration: it is the sentence the model reads when choosing a tool.

## The integration broker (AnythingMCP / Jentic One)

Optional SaaS gateway clawtilla runs in rootless podman. Distinct from the provider-key broker of `runtime.network: gated` (security.md).

| `broker.type` | What | Licence |
|---|---|---|
| `anythingmcp` | MCP gateway: JSON adapters turn REST/SOAP/GraphQL/OData/SQL into tools, OAuth2 with your own clients | AGPL-3.0 |
| `jentic` | Jentic One (beta): credential broker with per-agent rules and call logs | Apache-2.0 |

Turn on: edit clawtilla.yaml (`broker: {enabled: true, type: anythingmcp, port: 47400, dashboard_port: 47300}`), then `clawtilla config reload`. There is **no** `clawtilla config set`: use `clawtilla config edit` or edit the file and reload.

| Verb | IPC | Does |
|---|---|---|
| `broker status` | `broker.status` | state, dashboard + MCP URLs, every gateway server and whether its key is stored |
| `broker start` / `stop` / `restart` | `broker.start` / `.stop` / `.restart` | start answers at once (state `starting` until healthy); stop keeps `<state_dir>/broker/` |
| `broker logs` | `broker.logs` | last 100 lines per container, credential-shaped text redacted |
| `broker key <agent\|team:ID>` | `broker.key` | store a gateway server's key from stdin, echo off |

AnythingMCP setup (the human does steps 1-4 in the dashboard; clawtilla never accepts terms or registers for them):
1. Open the dashboard, register (first account is admin, accepts AnythingMCP's terms).
2. Sign in to apps with your own OAuth clients, install their connectors.
3. Make an MCP server per agent or team, assign connectors (that assignment *is* the permission).
4. Make an MCP API key on that server's page (works for that server only, 403 elsewhere).
5. `clawtilla agent set scout broker_server <server-id>` (or `clawtilla team set research broker_server <id>`), then `clawtilla broker key scout` / `clawtilla broker key team:research`.

Each agent reaches one gateway server: its own `broker_server`, else its team's. clawtilla synthesises a connector `broker-agent-<id>` / `broker-team-<id>`; editing it with `integration set`/`connector key` is refused (change `broker.*` / `broker_server`).

Gotchas: not given to agents with `runtime.network` `gated` or `none` (its loopback is not the host's). Jentic One is refused while any agent naming a gateway server lacks `runtime.confine: bwrap`. Images must be pinned (no tag / `latest` refused). `broker.restart` `always` (default) / `on-failure` / `never`; five restarts in ten minutes leaves it `failed`. Back up `<state_dir>/broker/` whole -- it is a second credential store (gateway's own encrypted DB plus key); a new encryption key makes every stored SaaS token unreadable. AnythingMCP has no working arm64 image (Asahi: use `jentic`).
