# Connectors

A connector is an account (GitHub, a self-hosted GitLab, Notion, VENTURE, Higgsfield) whose credential clawtilla holds. Agents get the service's tools through a relay; they never get the token. It is the `connector` integration type, so scope, `per_agent` and `integration set` work as in [integrations.md](integrations.md). Keys: [config-keys.md](config-keys.md) (`connectors.*`, `integrations.*`). VENTURE's task mirror is in [grok-and-venture.md](grok-and-venture.md); approval rules in [security.md](security.md).

Source docs: docs/connectors.org, docs/higgsfield.org, docs/venture-setup.org; packs in data/connectors.d/.

## Hard rules for an agent doing this

- Never put a token, key, password or client secret in argv, clawtilla.yaml, an env var you export, a chat message or a file you write. There is deliberately no `--key` / `--password` flag.
- `clawtilla connector key NAME` and `clawtilla broker key ...` read stdin with echo off: **ask the human to run them and type/paste the secret themselves.** Piping from a minting tool (`venturectl token mint ... | clawtilla connector key books`) is fine when the human runs it.
- OAuth/device sign-ins need a person at a browser: run `connector connect`, hand them the URL/code, and wait.
- Paid services (Higgsfield, anything billed per call) need the operator's approval before any spending call; add an `ask` rule (below) rather than relying on good intentions.

## Where the credential lives

`secrets.dir/connectors/<name>/token.json` (dir 0700, file 0600); `secrets.dir` defaults to `~/.clawtilla/secrets`. Old flat `secrets.dir/connector-<name>.json` files are moved on daemon start (a hand-chosen `token_file` never is). Only the daemon writes or renews it; a relay asks the daemon over IPC (`connector.refresh`) and re-reads the file. Never in: the agent's `.mcp.json` (it names `clawtilla connector relay <name>` only), the agent's environment, any argv, any IPC reply.

What that buys depends on where the agent runs: container/VM agent -- a real boundary (relay on the host). Confined host agent (`runtime.confine: bwrap`) -- box binds only its own granted connectors' directories read-only; it can read those, nothing else in `secrets.dir`. Unconfined host agent -- prevents accidental leaks only; same user, `secrets.dir` is right there.

## CLI (`clawtilla connector --help`)

| Verb | Does |
|---|---|
| `catalog` | services clawtilla knows (`connector.catalog`) |
| `list` | your connectors: live?, time left, granted scopes, tool count/size, `sign in needed` |
| `add <name> --provider ID [opts]` | add (`integration.add`, type connector); connects at once only if `--client-id` given |
| `connect <name>` | OAuth/device/discovered sign-in (`connector.begin` then `connector.await`, long wait) |
| `discover <name>` | ask a remote server how to sign in, without signing in |
| `key <name>` | paste a token from stdin (echo off) |
| `refresh <name>` | renew now |
| `revoke <name>` | delete local copy always; call provider's revoke endpoint where one exists, else say it did not |
| `rm <name>` | remove the connector entirely |
| `registry-refresh` | import the open MCP registry now (needs `connectors.registry_enabled: true`) |

`add` options: `--provider`, `--account LABEL`, `--client-id`, `--instance URL`, `--scopes "a b"`, `--scope all|selected|none` (default **none**), `--agents a,b`, `--command CMD`, `--url URL`, `--tools a,b`, `--apply-writes off|all` (VENTURE). Any other `--some-key V` passes through as the schema key `some_key` (e.g. `--credential-name OPENWEATHER_KEY`). A `--url` with no `--provider` is the built-in `remote` entry.

```bash
clawtilla connector add gh --provider github --account work --command github-mcp-server --scope all
clawtilla connector key gh                                   # human pastes a PAT
clawtilla connector add notion --url https://mcp.notion.com/mcp --scope selected --agents scribe
clawtilla connector connect notion                           # human opens the printed link
clawtilla connector add work --provider gitlab --instance https://gitlab.example.com --client-id abc123
clawtilla connector add weather --provider api-key --command weather-mcp --credential-name OPENWEATHER_KEY
```

After granting a connector, the agent's CLI reads its tool list at session start: restart the agent.

## How a credential is obtained

| Way | When | Notes |
|---|---|---|
| token you have | `connector key` | often best; no app registration |
| device flow | provider supports it, `--client-id` set | prints code + URL; works from a phone over SSH; code lives ~15 min; a 400 `authorization_pending` is the normal poll answer |
| auth code + PKCE | provider lacks device grant | register redirect `http://127.0.0.1:8765/callback` (`connectors.redirect_port`); listener is loopback only; only the newest sign-in waits; 10 min |
| discovered (remote MCP) | `--url`, no endpoints in catalogue | RFC 9728 protected-resource -> RFC 8414 metadata -> RFC 7591 dynamic registration (as `connectors.client_name`), PKCE S256 + RFC 8707 `resource`; registration kept in `secrets.dir/connector-auth/<name>.json` |

clawtilla ships no OAuth client ids; `connector catalog` says where to register (`client_id_help`). Discovery refuses redirects on every leg, plain http except loopback-from-loopback, documents about another resource/issuer, no S256, documents over 64 KiB. Catalogue `auth_url`/`token_url`, a configured `client_id`, and `connectors.discovery: false` always win over what a server publishes. A remote server answering 401 gets the relay's own refusal ("needs a sign-in ... Do not retry"), the daemon records it (`connector.unauthorized`), and every client shows *Sign in*. Credentialed HTTP never follows a redirect: set `url` to where it points.

Renewal: the daemon renews within `connectors.refresh_margin_seconds` of expiry on its own timer. `expires_in` is stored as absolute `expires_at`. Requested `scopes` are never overwritten by granted ones (`granted_scopes` on `connector.list`); an empty refresh token never overwrites a good one.

## The catalogue

Built-ins plus YAML in `connectors.dir` (default `$XDG_CONFIG_HOME/clawtilla/connectors.d/`); an entry with a built-in's `id` replaces it whole; a bad file is a warning. Example packs to copy (not auto-installed): `data/connectors.d/{gitlab,gitea,matrix,home-assistant,grafana,nextcloud,jellyfin,immich,paperless-ngx,miniflux,vaultwarden}.yaml`. Most have no `server_command`: name your server with `--command`/`--url`. `matrix.yaml` is a tool connector, unrelated to `type: matrix` chat.

Pack fields: `id`, `name`, `summary`, `category`, `auth` (`none|device|pkce|api_key`), `auth_url`, `token_url`, `revoke_url`, `scopes`, `client_id_help`, `docs_url`, `default_instance`, `server_command`, `server_args`, `server_url`, `known_tools`, `placement` (`env|header`), `credential_name`, `credential_format` (one `%s`), and optionally `identity_keys` / `identity_note`. With `default_instance` set every URL field is a path joined to `instance:`; `server_url` needs `placement: header`. Generic entries `api-key` and `bearer` cover any key-in-env or bearer-on-HTTP server. The open MCP registry (off by default) only fills gaps, ids prefixed `registry:`.

## Grants and narrowing

- Scope as any integration: `--scope all`, `--scope selected --agents a,b`, or inline. Sharing one connector is fine (one account the fleet reads through) except where the entry declares identity keys (VENTURE: one token = one actor; give each agent its own).
- `--tools a,b` / `integration set NAME tools=a,b`: tools outside the list are **removed** from the agent's list (not refused at call). Empty/unset = all. A name not in the pack's `known_tools` warns.
- Several accounts on one provider: each is its own connector and needs a unique `--account` label once there is a second (refused otherwise). An agent holding two accounts sees tools prefixed with the folded label (`work_create_issue`, `personal_create_issue`); the bare name is refused. `tools:` and action rules keep naming the server's own names.

## Action rules and approval cards

Every connector call goes through `policy.check` from the relay (stdio relay and HTTP bridge share `ClawtMcpRelayPolicy`). Rules name the `.mcp.json` entry, `clawtilla-<name>`:

```yaml
agents:
  - id: chief
    policy:
      rules: ["mcp__clawtilla-hf__*=ask", "external=ask"]
```

`ask` posts an approve/deny decision (`clawtilla decision list`, `clawtilla decision answer <id> approve`); unanswered by `ask_timeout_seconds` = refused. Through a tool directory the card names the inner tool, never `<name>_call_tool`. Full rule grammar in security.md.

## Servers with many tools: the directory

`integrations.directory`: `auto` (default: a directory when big **and** the CLI does not search tools itself), `always`, `never`. Big = more than `connectors.directory_min_tools` (40) tools or `connectors.directory_min_chars` (100000) chars of definitions, counted after `tools:`. Claude Code, claude-tmux and grok-build search tools themselves and get the flat list under `auto`; opencode, codex, antigravity, cursor get the directory. The directory is three tools: `<name>_search_tools`, `<name>_describe_tool`, `<name>_call_tool`; it only finds what the agent may use ("No tool named ... here" for both missing and withheld). Size is reported by relays (`connector.tools_seen`) into `<state_dir>/connector-tools.yaml`; unknown until an agent has used it.

```bash
clawtilla integration set forge directory=always
```

## Result budgets

`connectors.max_result_bytes` (65536) fleet default; `integrations.max_result_bytes` per connector (-1 = fleet's, 0 = none; values under 1024 read as 1024). An over-budget result is trimmed structurally (JSON stays valid, UTF-8 safe), framed by `[clawtilla] TRIMMED` / `END OF TRIMMED RESULT` notes, and held whole at `<state_dir>/agents/<id>/mcp-results/<result_id>.json` (a day, 64 per agent, only that agent can read). The agent reads the rest with `clawtilla_mcp_result_query` (`result_id`, `path`, `search`, `offset`, `limit`) rather than calling the tool again. Hard ceiling 64 MiB per message whatever the budget.

## MCP apps

`integrations.show_apps` (off): a tool with a `ui://` view appears in chat as an app, drawn only by the web client in `sandbox="allow-scripts"` with a daemon-built CSP; GTK and lrg link to it via `connectors.apps_web_url` (default `http://127.0.0.1:8790`). A view may call its own server's tools only if the server marks them app-visible; the call goes through the same rules and approvals ("asked by an app in chat"). Refused for confined agents.

## Higgsfield (closed, paid)

```bash
clawtilla connector add hf --provider higgsfield --scope selected --agents designer \
    --tools generate_image,estimate_image_cost,job_status,balance
clawtilla connector connect hf        # human signs in with their Higgsfield account
```

- Server `https://mcp.higgsfield.ai/mcp`, sign-in discovered (nothing to register, no key in config). Credits spent on every generation; the server lists ~200+ tools, so always narrow with `--tools`.
- Put an `ask` rule on every agent except the one whose job it is: `mcp__clawtilla-hf__*=ask`.
- Generations are jobs: `estimate_image_cost` (free, returns `quote_ref`) -> `generate_image` with `{"params": {"ref": "quote:..."}}` -> `job_status` (`jobId`, camel case; `sync: true` waits) -> fetch `results.rawUrl` into the exchange dir or workspace. Never resubmit a slow job (charged twice). `balance` is free.
- Two billing roads for the office's own character generation (`office.asset_api`): `higgsfield-mcp` = this connector's SSO sign-in, billed to the account's app credits; `higgsfield` = the REST API with a console key pair stored by the human via `clawtilla office api-key higgsfield`, billed to a separate API balance (empty balance: `HTTP 403 not_enough_credits`). Choose the one the money is in. `office.asset_model` names models per road (they differ). Details: office-custodial.md.
- Terms: proprietary; outputs may train their models below enterprise; free tier watermarked.

## VENTURE (self-hosted ERP/CRM)

```bash
clawtilla connector add books-bookkeeper --provider venture --account bookkeeper \
    --instance http://127.0.0.1:8747 --scope selected --agents bookkeeper
# human: venturectl token mint name=clawtilla-bookkeeper user=books-bot | clawtilla connector key books-bookkeeper
```

Needs `venturectl` on the daemon's PATH (runs `venturectl mcp`); token injected as `VENTURE_TOKEN` with `VENTURE_URL`. Nineteen `venture_*` tools. One VENTURE account + token + connector per agent. Writes are staged by default and become decisions (`integrations.confirm_writes` true, `integrations.poll_seconds` 60; default answer reject; VENTURE drops unanswered cards after its TTL). Connectors on a shared VENTURE server: the server's queue is read once per poll, each connector learns its actor via `whoami`, and each decision lands with the agent whose token staged it (`origin.name`), answered with that agent's token only; a card nobody in the fleet staged (a person's, the operator's service token's) is not raised -- answer it in VENTURE. One entry with `per_agent` tokens = one source per agent (`<name>/<agent>`). `integrations.apply_writes`: `off` (default), `low_risk` (VENTURE's classification; money/people/access/credentials/outward/deletes still wait), `all` (unreviewed; only for a tightly scoped account). Unknown value = `off`. Task mirroring: grok-and-venture.md.

## The integration broker

AnythingMCP / Jentic One gateway connectors `broker-agent-<id>` / `broker-team-<id>` are synthesised from `broker.*` and `broker_server`; see integrations.md. Store their keys with `clawtilla broker key`, never `connector key`.

## Troubleshooting

| Seen | Fix |
|---|---|
| "not connected yet" | `connector key` or `connector connect` |
| "cannot read the credential for 'x'" (confined) | connector not granted to this agent, or `token_file` did not exist at start; fix and restart agent |
| egress gate refused (gated agent) | server is private: add its address (with port for loopback) to `egress.allow`; also opens it to the agent's own tools |
| "no connector called 'x'" | `provider` names no catalogue entry; add a pack to `connectors.dir` |
| has connector, no tools | catalogue entry knows no server: set `command` or `url` |
| three tools instead of the server's | it is a directory; search it, or `directory=never` |
| "needs a sign-in" | `connector connect NAME` |
| "offers no dynamic client registration" | register a client by hand (redirect `http://127.0.0.1:8765/callback`), set `client_id` |
| "this agent holds several accounts on this service" | call the prefixed name |
| "both ... accounts labelled" | give one a different `account` |
| `[clawtilla] TRIMMED` | read with `clawtilla_mcp_result_query`, or raise `max_result_bytes` |
| "is the integration broker's connector" | change `broker.*` / `broker_server`; `clawtilla broker key` |
