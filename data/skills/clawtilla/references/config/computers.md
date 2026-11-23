# Configuration: `computers`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `computers`

Machines several agents share.

Each entry takes an id and the same keys as an agent's computer:
block -- type, container, vm, desktop, mounts, and the rest; every
one is documented under agents.computer. An agent joins one with
agents.computer.use. Agents A and B on `alpha` and C, D and E on
`beta` are two machines, not five.

Each agent keeps its own workspace, mounted at
/mnt/clawtilla/workspaces/<agent>, and its own exchange folder. The
machine stops when its last running agent stops, and is rebuilt only
when every agent on it is stopped. A desktop is one screen, driven by
one agent at a time.

### `computers.id`

- Type: string
- Default: unset
- Required: yes
- Since: 0.5.0

Identifier, and what agents.computer.use names. The machine is called
clawt-c.<id> in podman, distrobox and libvirt -- a dot no agent id can
hold, so it never shares a name with an agent's own machine.

### `computers.description`

- Type: string
- Default: unset
- Since: 0.5.0

What this machine is for.
