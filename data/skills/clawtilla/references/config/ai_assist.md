# Configuration: `ai_assist`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `ai_assist`

The model that helps design new agents.

`clawtilla agent new --ai` describes what you want in prose and the
model fills in a draft spec through tool calls, shows you the YAML and
the workspace files it would create, and only then commits -- through
the same code path as manual creation, so there is no second
implementation to drift.

### `ai_assist.enabled`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Whether AI-assisted agent creation is offered.

### `ai_assist.provider`

- Type: string
- Default: `claude`
- Since: 0.1.0

Provider used for the designer. Must be one that takes tool
definitions -- claude, openai, gemini, grok or ollama.

Not one of the CLI backends an agent runs on. The designer works
entirely through tool calls, and ai-glib's command-line clients drop
the tool list rather than passing it on, so claude-code and friends
refuse this job however capable the model behind them is.

### `ai_assist.model`

- Type: string
- Default: `claude-opus-5`
- Since: 0.1.0

Model used for the designer. Worth a capable one: it is a short
conversation and the output is a file you live with.

### `ai_assist.max_turns`

- Type: integer
- Default: `20`
- Since: 0.1.0

Turn cap for the design conversation, so a model that never commits
stops rather than looping.
