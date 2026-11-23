# Configuration: `browsers`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `browsers`

Web browser sessions several agents share.

Each entry takes an id and the same keys as an agent's browser:
block -- home, allow_scripts, idle_minutes, width, height. An agent
joins one with agents.browser.mode: shared and agents.browser.use.
Agents on one entry share one profile -- its cookies, its logins, its
open page -- and drive it one at a time: a call while another agent
drives is refused, naming that agent. Needs a GSURF=1 build.

### `browsers.id`

- Type: string
- Default: unset
- Required: yes
- Since: 0.9.0

Identifier, and what agents.browser.use names.

### `browsers.description`

- Type: string
- Default: unset
- Since: 0.9.0

What this session is for -- say which logins it holds.
