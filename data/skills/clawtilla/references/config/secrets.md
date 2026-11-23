# Configuration: `secrets`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `secrets`

How secret references in this file are resolved.

There is deliberately no way to write a literal secret here. Credentials
are always a reference:

  api_key: {file: ~/.clawtilla/secrets/anthropic}
  api_key: {env: ANTHROPIC_API_KEY}
  api_key: {command: "pass show clawtilla/anthropic"}

Resolved values are written to per-agent credential files at mode 0600
and injected into that agent's environment. They never appear in an IPC
response, a log line or a transcript.

### `secrets.default_backend`

- Type: enum
- Values: file, env, command
- Default: `file`
- Since: 0.1.0

Backend assumed when a reference gives a bare string: file, env or command.

### `secrets.dir`

- Type: path
- Default: `~/.clawtilla/secrets`
- Since: 0.1.0

Directory a bare file reference is resolved against.

### `secrets.command_timeout_seconds`

- Type: integer
- Default: `10`
- Since: 0.1.0

How long a command-backend secret may take before it is an error.

A password manager that is locked will block forever rather than fail,
and without a timeout so will daemon startup.
