# Configuration: `integrations`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `integrations`

Named integrations, each handed to one agent, some agents or all of them.

An integration is a connection to something outside the fleet, kept in
one place and pointed at whichever agents should have it. That is the
difference from the per-agent `integrations:` block inside an agent,
which still works and is still the right thing for a connection only
that one agent will ever use: this one is configured once and scoped.

Which keys below apply depends on `type`. Each names the type it
belongs to; a key belonging to another type is ignored rather than
rejected, so changing a draft's type does not mean deleting fields
first.

### `integrations.name`

- Type: string
- Default: unset
- Required: yes
- Since: 0.2.0

What this instance is called.

Unique across the file, and used everywhere the instance is referred
to -- the CLI, the UI, and the key it gets in an agent's .mcp.json. Two
instances with the same name is an error rather than a merge, because
the second silently winning is how a credential ends up pointing at the
wrong account.

### `integrations.type`

- Type: string
- Default: unset
- Required: yes
- Since: 0.2.0

Which kind: matrix, email, webhook, local, cmacs or mcp.

An unknown type disables that instance and nothing else, with the
reason recorded -- the same treatment a shadow agent gets, and for the
same reason: a config written by a newer build must survive an older
one.

### `integrations.enabled`

- Type: boolean
- Default: `true`
- Since: 0.2.0

Whether this instance is live at all.

Independent of `scope`: this is the switch, scope is the audience.

### `integrations.scope`

- Type: enum
- Values: none, selected, all
- Default: `selected`
- Since: 0.2.0

Who gets it: all, selected or none.

`all` includes agents created later, which is the point of it -- a tool
server the fleet shares should not need revisiting every time the fleet
grows. `selected` uses the `agents:` list below. `none` keeps the
instance and its credentials without handing it to anybody.

### `integrations.teams`

- Type: list of strings
- Default: unset
- Since: 0.2.0

Team ids, when `scope: selected`.

Beside `agents:` rather than instead of it, and either can match.
Naming a team is how an instance covers a group without being
rewritten every time somebody joins it.

Worth thinking about for a *channel*: two agents sharing one Matrix
account answer as the same person, so a team is rarely what you want
there. For a tools integration it is the ordinary case.

### `integrations.agents`

- Type: list of strings
- Default: unset
- Since: 0.2.0

Agent ids, when `scope: selected`.

An id that names no agent is a warning rather than an error: an agent
removed for the afternoon should not stop the daemon starting.

### `integrations.per_agent`

- Type: mapping
- Default: unset
- Since: 0.2.0

Per-agent overrides, keyed by agent id.

This is how one instance covers several agents that cannot share an
identity. A Matrix account is one login: two agents on the same one
both receive every message and both answer as the same user. So the
homeserver and the room list live at the top and each agent's own
user_id and access_token go here.

Any key from this section may be overridden, not only the identity
ones -- a room list that differs per agent is an ordinary thing to
want.

### `integrations.description`

- Type: string
- Default: unset
- Since: 0.2.0

What this is for, in your words.

Written into the agent's TOOLS.org beside the integration, so it is
the sentence the model reads when deciding whether this is the thing
to reach for. Worth filling in for anything whose name is not
self-explanatory.

### `integrations.homeserver`

- Type: string
- Default: unset
- Since: 0.2.0

matrix: the homeserver base URL, e.g. https://matrix.example.org.

### `integrations.user_id`

- Type: string
- Default: unset
- Since: 0.2.0

matrix: the full user id, e.g. @agent:example.org.

Must differ per agent. Put it under `per_agent` when the instance
covers more than one.

### `integrations.access_token`

- Type: secret reference
- Default: unset
- Since: 0.2.0

matrix: the access token, as a secret reference.

`clawtilla integration matrix-login` turns a password into one of
these without the password ever being written down.

### `integrations.rooms`

- Type: list of strings
- Default: unset
- Since: 0.2.0

matrix: room ids to listen in.

Empty means every room the account is joined to. Bridged rooms --
Discord, Signal, anything else with a Matrix bridge -- are just rooms,
so an agent reaches them without knowing they are bridged.

### `integrations.require_mention`

- Type: boolean
- Default: `true`
- Since: 0.2.0

matrix: only answer messages that name the agent.

Defaults on, unlike a clawtilla room. A Matrix room usually has people
in it talking to each other, and an agent that takes a turn on every
line of it is both expensive and unbearable.

### `integrations.imap_host`

- Type: string
- Default: unset
- Since: 0.2.0

email: IMAP server hostname.

### `integrations.imap_port`

- Type: integer
- Default: `993`
- Since: 0.2.0

email: IMAP port.

### `integrations.smtp_host`

- Type: string
- Default: unset
- Since: 0.2.0

email: SMTP server hostname.

### `integrations.smtp_port`

- Type: integer
- Default: `587`
- Since: 0.2.0

email: SMTP port.

### `integrations.username`

- Type: string
- Default: unset
- Since: 0.2.0

email: the mailbox to log in as. Must differ per agent.

### `integrations.password`

- Type: secret reference
- Default: unset
- Since: 0.2.0

email: the password, as a secret reference.

### `integrations.folders`

- Type: list of strings
- Default: unset
- Since: 0.2.0

email: folders to watch. Empty means INBOX.

### `integrations.port`

- Type: integer
- Default: unset
- Since: 0.2.0

webhook: the port to listen on. Must differ per agent -- two agents
cannot bind the same one, and the second to start simply fails.

### `integrations.backend`

- Type: enum
- Values: desktop, ntfy, gotify, matrix, command, apprise
- Default: `desktop`
- Since: 0.2.0

notify: how the notification reaches you.

`desktop` raises one on the machine the daemon runs on, which is the
wrong answer when the daemon is on a workstation and you are not.
`ntfy` and `gotify` reach a phone. `matrix` posts into a room, which
on a bridged homeserver means it reaches you wherever you already
read messages. `command` runs a program and hands it the text.
`apprise` (since 0.22.0) hands it to the apprise CLI, which reaches a
hundred-odd services -- Telegram, Signal, email, Pushover, Gotify, SMS
-- through one URL scheme; the URL is `apprise_url`.

### `integrations.token`

- Type: secret reference
- Default: unset
- Since: 0.2.0

notify: the credential for ntfy, gotify or Matrix, as a secret
reference. libresearch: the server's token. grokbot: the routine's
webhook key (`crsr_...`), which is the whole of what lets anybody
start that bot.

Resolved once when the configuration loads rather than on every
notification: a `command` reference to a locked password manager
blocks until it times out, and doing that each time something needed
saying would make the notifier the slowest thing in the daemon.

### `integrations.apprise_url`

- Type: secret reference
- Default: unset
- Since: 0.23.0

notify: with `backend: apprise`, the Apprise URL -- or several, one per
line -- as a secret reference: `{file: ~/.config/clawtilla/apprise.url}`.

A secret because an Apprise URL nearly always carries a credential
(`tgram://BOT_TOKEN/CHAT_ID`, `mailtos://user:password@host`). It
reaches apprise in a 0600 file passed with `--config`, deleted when
apprise exits, and never on its command line, which any user on the
machine can read. `command` names the apprise binary when it is not on
PATH.

### `integrations.room`

- Type: string
- Default: unset
- Since: 0.2.0

notify: with `backend: matrix`, the room id to post into.

A room of your own with nobody else in it works well: it keeps the
fleet's noise out of a room people are talking in, and still arrives
on every device you read Matrix on.

### `integrations.events`

- Type: list of strings
- Default: `question,error`
- Since: 0.2.0

notify: what is worth interrupting you for.

`question` -- an agent said something to you and is waiting.
`done` -- a task finished.
`error` -- an agent stopped in a way nobody asked for.
`routine` -- a scheduled run failed.
`update` -- a newer clawtilla exists.
`budget` -- a daily spending cap refused work
(`orchestration.daily_fleet_budget_micros`, or an agent's
`orchestration.daily_agent_budget_micros`).

The default is question and error, which is the whole rule: blocked
on you, or broken. A notifier that fires on every turn is one people
turn off, and then it is not there for the two that mattered.

### `integrations.priority`

- Type: string
- Default: `normal`
- Since: 0.2.0

notify: low, normal, high or urgent.

Mapped onto whatever the backend understands -- ntfy's five levels,
gotify's ten, the two urgency hints a desktop has.

### `integrations.quiet_hours`

- Type: string
- Default: unset
- Since: 0.2.0

notify: a local-time range to stay silent in, such as `23:00-07:00`.

Silences this instance completely. If you want to be woken for a
broken agent but not for a question, that is two instances -- one
with quiet hours and one without, each with its own `events` -- which
is why instances have names.

### `integrations.title`

- Type: string
- Default: unset
- Since: 0.2.0

notify: a title to use instead of the agent's name.

Worth setting when several fleets notify the same phone, so a buzz
says which machine it came from.

### `integrations.command`

- Type: string
- Default: unset
- Since: 0.2.0

mcp: the program to run, spoken to over stdio.

This is the general way to give the fleet anything with an MCP server:
one entry here reaches every agent in scope, instead of the same block
pasted into each agent's .mcp.json by hand.

notify: with `backend: command`, the program to run; with
`backend: apprise`, the apprise binary when it is not on PATH.

### `integrations.args`

- Type: list of strings
- Default: unset
- Since: 0.2.0

mcp: arguments for `command`.

### `integrations.url`

- Type: string
- Default: unset
- Since: 0.2.0

mcp: an HTTP or SSE endpoint, instead of `command`.

One or the other, never both: an entry naming a command and a URL is
two different servers and there is no way to tell which was meant.

libresearch: where its server answers. grokbot: the Grok routine's
webhook address, as Grok shows it under "POST to".

connector: a remote MCP server, instead of the catalogue's. With
`provider: remote` -- or no provider at all -- this is the only thing
a connector needs: the server says how to sign in to it (see
connectors.discovery).

### `integrations.env`

- Type: mapping
- Default: unset
- Since: 0.2.0

mcp: environment for the server process.

A value may be a secret reference, which is resolved when the agent
starts and written into its .mcp.json -- a file that is already 0600
and already holds the agent's own token.

### `integrations.provider`

- Type: string
- Default: unset
- Since: 0.2.0

connector: which service this is an account on.

One of the ids from `clawtilla connector catalog`. The catalogue says
how to authenticate and, where there is a well-known one, which MCP
server fronts it -- so a connector integration is usually a provider,
a client id and nothing else.

### `integrations.account`

- Type: string
- Default: unset
- Since: 0.2.0

connector: which account on that service, in your own words.

Two connectors for the same provider are the ordinary case -- work
and personal, or one GitLab org and another -- and the account label
is what tells them apart everywhere they are shown.

Required, and unique per provider, for every account after the first:
adding a second one without a label, or with one another account on
that service already uses, is refused. An agent granted several
accounts on one service sees each account's tools prefixed with its
label -- `work_create_issue`, `personal_create_issue` -- and a call to
the bare name is refused rather than run as whichever account it
happened to reach.

### `integrations.instance`

- Type: string
- Default: unset
- Since: 0.2.0

connector: the host, for a service you run yourself.

Only meaningful for a self-hostable provider such as gitlab or
forgejo, where the endpoints are paths joined onto this. Left unset
it is the provider's flagship host.

### `integrations.client_id`

- Type: string
- Default: unset
- Since: 0.2.0

connector: the OAuth application you registered with the provider.

clawtilla ships none. It has no application of its own to lend, and
borrowing another project's would mean every clawtilla identifying
itself as something it is not. Registering one takes a few minutes
and `clawtilla connector catalog` says where for each provider.

### `integrations.client_secret`

- Type: secret reference
- Default: unset
- Since: 0.2.0

connector: the application's secret, if the provider insists on one.

Many do not for the device flow, and one that does not must not be
sent an empty string -- some providers authenticate that and fail.
Leave it unset rather than blank.

### `integrations.scopes`

- Type: string
- Default: unset
- Since: 0.2.0

connector: what to ask the provider for, space separated.

Defaults to the catalogue's suggestion. Ask for less than that when
the agent needs less: a scope granted is a scope every agent sharing
this connector has, for as long as the token lives.

### `integrations.token_file`

- Type: string
- Default: unset
- Since: 0.2.0

connector: where the credential is kept, mode 0600.

Written by the daemon when the flow completes, and named here rather
than in the config so that nothing has to put the value itself in a
file people keep in git. Set automatically; there is rarely a reason
to write it by hand.

### `integrations.credential_name`

- Type: string
- Default: unset
- Since: 0.2.0

connector: the environment variable or header the credential goes in.

Overrides the catalogue, which is what makes the generic `api-key`
and `bearer` providers cover a service that has no entry of its own.

### `integrations.tools`

- Type: list of strings
- Default: unset
- Since: 0.2.0

connector: the only tools the agent may use, if you want fewer.

Empty means every tool the server offers. Naming some is enforced in
the relay: the others are removed from the agent's tool list rather
than refused when called, because a tool that is advertised and then
refused is worse than one that was never there -- the agent plans
around it and has to work out from an error that it never had it.

### `integrations.directory`

- Type: enum
- Values: auto, always, never
- Default: `auto`
- Since: 0.18.0

connector: whether its tools are listed one by one or searched.

A server with hundreds of tools puts every description into every
model call. As a directory it is three tools instead --
<name>_search_tools, <name>_describe_tool and <name>_call_tool -- and
the agent looks up what it needs. The search, the description and the
call all work from the tools this agent may use, so narrowing `tools:`
narrows what can be found as well as what can be called.

`auto` makes a directory of a server past
connectors.directory_min_tools or connectors.directory_min_chars, for
an agent whose CLI does not search tools itself; Claude Code does, so
its agents get every tool as before. `always` makes one whatever the
size or the CLI, and `never` lists every tool however many there
are. See docs/connectors.org, "Servers with many tools".

### `integrations.max_result_bytes`

- Type: integer
- Default: `-1`
- Since: 0.27.0

connector: the most of one tool result the agent is shown, in bytes.

A result bigger than this is trimmed before the agent reads it -- the
text cut on a character boundary, a JSON array cut to the records that
fit, structuredContent trimmed the same way -- with a [clawtilla] note
at the start and the end saying what was cut. The whole result is kept
under the agent's state directory and the agent reads the rest with
clawtilla_mcp_result_query. -1 (the default) means the fleet's
connectors.max_result_bytes, 0 means no budget for this connector,
and anything below 1024 is read as 1024. See docs/connectors.org,
"Results bigger than the budget".

### `integrations.show_apps`

- Type: boolean
- Default: `false`
- Since: 0.27.0

connector: show its apps in chat.

An MCP server can ship an interactive view (an MCP App, a ui://
resource) for a tool. With this on, the relay tells the server it can
show them, reads the view when the tool is called, and the call
appears in the conversation as an app: the web client draws it in a
sandboxed frame when somebody opens it, and the GTK and lrg clients
offer it in the browser. Off by default, because the view is the
server's own code running in your browser. See docs/connectors.org,
"Apps in chat".

### `integrations.confirm_writes`

- Type: boolean
- Default: `true`
- Since: 0.2.0

For a connector that can stage its writes, whether it does.

A staged write becomes a decision in the operator's own inbox rather
than something to be found later in the other system's web
interface. Turning this off means the agent's writes land
immediately and unreviewed.

Read by the `venture` connector, which is the one built-in that
stages. It does not switch staging on -- `venturectl mcp` decides
that for itself and stages by default -- it decides whether
clawtilla polls the queue and raises what is waiting as a decision.
Turning it off leaves the changes in VENTURE for somebody to find in
its own web interface.

### `integrations.apply_writes`

- Type: enum
- Values: off, all, low_risk
- Default: `off`
- Since: 0.36.1

connector (venture): whether the agent's writes wait for approval.

`off` keeps every create, update and delete the agent makes in
VENTURE staged: VENTURE holds it, and with confirm_writes on it
reaches the operator's decision inbox to approve or reject. `all`
starts `venturectl mcp --apply-writes`, so every write lands at once,
unreviewed, as the agent's own VENTURE account -- opt in only for an
account whose memberships already limit what it can reach. With one
connector per agent this is per agent.

`low_risk` (since 0.43.0) starts `venturectl mcp
--apply-writes=low-risk`: ordinary writes -- tickets, comments,
worklogs, links -- land at once, and VENTURE still stages anything
touching money, people, access or credentials, anything sent outward,
and every delete. It needs VENTURE 0.10.0 or later; against an older
venturectl it falls back to `off` with a warning, never to `all`.

Ignored when `args` is set on the integration: those are the
server's arguments, as written. See docs/venture-setup.org, step 2.

### `integrations.mirror_tasks`

- Type: boolean
- Default: `false`
- Since: 0.44.0

connector (venture): copy the covered agents' tasks into VENTURE
tickets.

A task is mirrored only when this is on for an agent it involves and
its repository matches a VENTURE `forge_repo` -- a checkout remote
that /api/v1/forge_repo/resolve knows, or its project's
`venture_repo_id`. The delegator's account creates the ticket and
the assignee's keeps it current: status, assignee, results and
comments. Nothing on an agent's path waits on VENTURE: changes are
queued in tasks.db and sent behind. Needs VENTURE 0.10.0 or later.
See docs/venture.org.

### `integrations.service_token_file`

- Type: path
- Default: unset
- Since: 0.46.0

connector (venture), with mirror_tasks on: the operator's own VENTURE
token, for the mirror's writes that no agent makes.

Files the operator's own tasks, and tasks whose delegator and assignee
have no VENTURE account, as this account. Keep it outside
secrets.dir/connectors/ so no agent's box ever mounts it. See
docs/venture.org.

### `integrations.mirror_intake`

- Type: boolean
- Default: `true`
- Since: 0.45.0

connector (venture), with mirror_tasks on: a ticket assigned to this
agent's VENTURE user starts a task for it.

Only an internal ticket still `todo` and carrying no other system's
reference, at most ten an hour per agent; its text reaches the agent
fenced, as data. Anyone who can edit tickets in that VENTURE
organization can then hand this agent work: turn it off where that is
more than you want. See docs/venture.org.

### `integrations.poll_seconds`

- Type: integer
- Default: `60`
- Since: 0.2.0

How often a connector that has something to report is asked.

Polling, because the other end is another process with its own
event loop. It runs on a timer attached to the daemon's context --
never from a request handler, which would make a client wait on
somebody else's network.

One timer covers every connector, so the shortest interval anybody
asked for is the one that runs. The first poll is one interval after
the daemon starts, never during it.

### `integrations.push_replies`

- Type: boolean
- Default: `true`
- Since: 0.11.0

grokbot: hand each agent reply to the bot as a new run of its routine.

When a Grok bot starts a conversation with an agent, the agent's
reply is POSTed to the bot's routine, so the bot answers on its own.
Each reply is a Grok run -- 200 an hour per routine, and each one
billed. Off, replies wait in clawtilla until the bot reads them with
clawtilla_grok_replies. A reply Grok refuses waits the same way.
