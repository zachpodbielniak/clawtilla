# Configuration: `accounts`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `accounts`

Separate logins for an agent CLI, so two agents on one machine can
spend two subscriptions.

Each account is a directory the CLI keeps its login and settings in;
an agent picks one with agents.model.account. Claude Code and the tmux
variant use CLAUDE_CONFIG_DIR, codex CODEX_HOME, grok GROK_HOME and
opencode its XDG directories. cursor and antigravity keep their login
under $HOME and cannot take an account: an agent that names one for
them is disabled with that reason rather than run on the default
login.

The directory is not created. Log in once with `clawtilla account
login ID`, which runs the CLI's own login against it.

### `accounts.id`

- Type: string
- Default: unset
- Required: yes
- Since: 0.2.0

Identifier, and what agents.model.account names.

### `accounts.provider`

- Type: string
- Default: unset
- Required: yes
- Since: 0.2.0

The agent CLI this login is for: claude-code, claude-tmux, codex-cli,
grok-build or opencode. An agent may use the account only when its own
provider is the same CLI.

### `accounts.config_dir`

- Type: path
- Default: unset
- Required: yes
- Since: 0.2.0

The directory the CLI keeps this login in. ~ is expanded.

### `accounts.description`

- Type: string
- Default: unset
- Since: 0.2.0

Whose login this is, or what it is for.

### `accounts.pay_per_use`

- Type: boolean
- Default: `false`
- Since: 0.16.0

This login bills each use -- an API key -- rather than drawing on a
subscription's limit.

Only agents.model.fallbacks reads it. Failing over to another sign-in
of the same subscription is refused, because rotating accounts to
get past a usage limit breaks the provider's terms; a pay-per-use
login on the same CLI pays for the work it does and is allowed.
