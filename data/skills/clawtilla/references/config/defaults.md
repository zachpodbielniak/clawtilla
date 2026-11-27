# Configuration: `defaults`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `defaults`

What a new agent gets when its own block does not say.

Every key here can be overridden per agent.

### `defaults.provider`

- Type: string
- Default: `claude-code`
- Inside an agent: `model.provider`
- Since: 0.1.0

Default AI provider, naming an entry in the agent's libreclaw providers.

### `defaults.model`

- Type: string
- Default: `sonnet`
- Inside an agent: `model.model`
- Since: 0.1.0

Default model for new agents.

### `defaults.computer`

- Type: enum
- Values: none, host, container, vm, distrobox, ssh, display
- Default: `none`
- Inside an agent: `computer.type`
- Since: 0.1.0

Default computer type for new agents. The permitted values are
listed above, from the schema itself rather than written out again
here -- the copy that was written out here had already gone stale.

none is the default on purpose. An agent that can run commands is a
bigger grant than an agent that can only talk, and it should be asked
for rather than inherited.

### `defaults.container_image`

- Type: string
- Default: `registry.fedoraproject.org/fedora:45`
- Inside an agent: `computer.container.image`
- Since: 0.1.0

Image a container agent gets when it does not name one.

Fedora because that is what clawtilla is developed and tested on, and
because a container whose package manager you already know is one you
can debug. Change it once here rather than per agent.

### `defaults.container_images`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Extra images to offer in a client's image list.

Added to the built-in suggestions rather than replacing them, and
shown first: these are yours, and a list where your own images are
below a dozen you will never pick is one you scroll past.

An entry is a reference, optionally followed by ' -- ' and a note:

  container_images:
    - "registry.example.com/team/devbox:latest -- ours, has the toolchain"
    - "docker.io/library/haskell:9.10"

Nothing is validated here. A reference podman cannot pull fails at start with podman's own error.

## `defaults.container`

Container limits every container and distrobox agent gets unless its
team or the agent itself says otherwise.

The same six keys as agents.computer.container -- cpus, cpu_weight,
cpuset, memory, memory_swap, pids_limit -- with the same meaning. Most
specific wins, key by key: the agent's own value, then its team's
(teams[].computer.container), then this. `clawtilla agent show` says
which one each value came from.

Written here rather than under defaults.computer, which is already
the computer type.

### `defaults.container.cpus`

- Type: string
- Default: unset
- Since: 0.78.0

The fleet's agents.computer.container.cpus.

### `defaults.container.cpu_weight`

- Type: integer
- Default: unset
- Since: 0.78.0

The fleet's agents.computer.container.cpu_weight.

### `defaults.container.cpuset`

- Type: string
- Default: unset
- Since: 0.78.0

The fleet's agents.computer.container.cpuset.

### `defaults.container.memory`

- Type: string
- Default: unset
- Since: 0.78.0

The fleet's agents.computer.container.memory.

### `defaults.container.memory_swap`

- Type: string
- Default: unset
- Since: 0.78.0

The fleet's agents.computer.container.memory_swap. Checked against the
memory an agent ends up with, wherever that came from: a team's 8g
memory under a fleet swap total of 6g is refused.

### `defaults.container.pids_limit`

- Type: integer
- Default: unset
- Since: 0.78.0

The fleet's agents.computer.container.pids_limit.

### `defaults.workspace_root`

- Type: path
- Default: `~/.clawtilla/agents`
- Since: 0.1.0

Where per-agent workspaces are scaffolded.

Each agent gets a directory here holding its libreclaw config, its
identity files and its MEMORY.md. It is the agent's desk, not the whole
house.

### `defaults.exchange_dir`

- Type: path
- Default: `$XDG_DATA_HOME/clawtilla/exchange`
- Since: 0.1.0

Shared drop-box mounted into every computer at /mnt/clawtilla/exchange.

shared/ is writable by every agent; <agent-id>/ is writable by that
agent and readable by the others. It exists so agents can hand each
other files without anybody wiring up mounts by hand.

## `defaults.mounts`

Host paths shared into *every* agent's computer.

  defaults:
    mounts:
      - source: "~/source"
        target: "/work/source"
        mode: rw

The directory you want every agent to see -- your projects tree,
your notes -- written once instead of copied into every agent block
and then forgotten on the next agent you create.

Applied to container, distrobox and VM agents. Not to host agents:
there a mount is the confinement allowlist rather than a kernel
mount, and quietly widening what a host agent may reach is not
something a convenience should do.

An agent's own computer.mounts wins on a matching target, so one
agent can point the same path somewhere else without turning the
default off. agents.computer.default_mounts: false declines them all.

### `defaults.mounts.source`

- Type: path
- Default: unset
- Since: 0.1.0

Host path to share. Not needed for tmpfs.

### `defaults.mounts.target`

- Type: path
- Default: unset
- Required: yes
- Since: 0.1.0

Absolute path inside the computer. Must not overlap another mount.

### `defaults.mounts.mode`

- Type: enum
- Values: ro, rw
- Default: `ro`
- Since: 0.1.0

ro or rw. Read-only is the default for the same reason it is on an
agent's own mounts, and more so: an entry here reaches the whole
fleet, so a mount that says nothing is one every agent may read and
none may rewrite.

### `defaults.mounts.type`

- Type: enum
- Values: bind, volume, virtiofs, 9p, tmpfs
- Default: `bind`
- Since: 0.1.0

bind, volume, virtiofs, 9p or tmpfs. Chosen from the backend when
unset, so the same entry is a bind mount in a container and a
virtiofs share in a VM.

### `defaults.mounts.relabel`

- Type: enum
- Values: none, shared, private
- Default: `shared`
- Since: 0.1.0

SELinux relabelling: none, shared (:z) or private (:Z).

shared when unset, which matters on Silverblue and friends: an
unlabelled bind mount appears inside the container with every access
denied, which reads as a permissions bug rather than a labelling one.

### `defaults.mounts.scope`

- Type: enum
- Values: none, selected, all
- Default: `all`
- Since: 0.1.0

Who gets this folder: all, selected or none.

`all` includes agents created later, which is the point of a default.
`selected` uses the `agents:` and `teams:` lists below. `none` keeps
the entry without giving it to anybody, which is how you park one
without losing what it said.

Naming agents or teams without saying `scope` means `selected` --
writing a list and having it ignored would be a rule that reads
correctly and does the opposite.

A scope that is written and cannot be read reaches nobody, the same
way an unrecognised integration scope does: handing somebody's home
directory to the whole fleet by typo is far worse than handing it to
nothing and saying so.

### `defaults.mounts.agents`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Agent ids that get this folder, when `scope: selected`.

An id that names no agent is ignored rather than refused: an agent
removed for the afternoon should not stop the fleet starting.

### `defaults.mounts.teams`

- Type: list of strings
- Default: unset
- Since: 0.1.0

Team ids that get this folder, when `scope: selected`.

Naming a team is how a folder covers a group without being rewritten
every time somebody joins it, which is the whole reason teams exist.
An agent on no team matches no team entry -- including one spelled
with an empty string.

### `defaults.mounts.create`

- Type: boolean
- Default: `false`
- Since: 0.1.0

Create the source directory if it is missing.

### `defaults.mounts.size`

- Type: string
- Default: unset
- Since: 0.1.0

Size for a tmpfs mount, such as 512M.

### `defaults.mounts.required`

- Type: boolean
- Default: `true`
- Since: 0.1.0

Whether a missing source stops the agent starting.

Worth turning off for a fleet default that not every machine has --
a laptop without your projects tree should still start its agents.

### `defaults.image_dir`

- Type: path
- Default: `$XDG_DATA_HOME/clawtilla/images`
- Since: 0.1.0

Where downloaded cloud images are kept.

One copy serves the whole fleet: a VM agent writes to a qcow2 overlay
backed by the image here, never to the image itself. Manage them with
`clawtilla image vm`, or from Settings in the GTK client.

### `defaults.exchange_max_bytes`

- Type: integer
- Default: `1073741824`
- Since: 0.1.0

Size cap for the exchange directory. 0 disables the limit.

Applied by deleting the oldest files until the total is under the
cap: whenever the exchange is opened for use -- when an agent starts,
and before a file is put into it -- and again on the daemon's sweep,
which is what catches an agent writing through the mount from inside
its computer, where nothing on the host sees the write happen.

### `defaults.libreclaw_binary`

- Type: path
- Default: unset
- Since: 0.1.0

Which libreclaw to run for process-runtime agents.

Unset means: beside clawtillad, then the deps/libreclaw build this
checkout produced, then PATH. A normal build and a normal install
both work without setting this.

Set it when the one to run is somewhere else -- a second checkout, or
an installed copy you want in preference to the build tree. Naming it
here wins over all three.

### `defaults.restart`

- Type: enum
- Values: never, on-failure, always
- Default: `on-failure`
- Inside an agent: `runtime.restart`
- Since: 0.1.0

Default restart policy: never, on-failure or always.

### `defaults.autostart`

- Type: boolean
- Default: `false`
- Inside an agent: `runtime.autostart`
- Since: 0.1.0

Whether new agents start with the daemon.

### `defaults.stream_steps`

- Type: boolean
- Default: `true`
- Inside an agent: `runtime.stream_steps`
- Since: 0.2.0

Whether agents report what they are doing while a turn runs.

With this on, the tools an agent reaches for and the prose it writes
between them appear in the conversation as they happen, so a turn
that takes ten minutes is something to watch rather than a typing
dot. With it off, a turn shows the indicator and then its answer.

A step is not a message. It is never delivered, never queued, never
written to the transcript and never answered -- so an agent watching
a peer work does not take a turn over it, which is the failure the
old five-minute progress notes had.

Tool arguments are previewed, one line each, with secrets redacted.
Turn it off for a fleet where even a redacted command line should
not reach whoever can open the chat.

### `defaults.confine`

- Type: enum
- Values: none, bwrap
- Default: `none`
- Inside an agent: `runtime.confine`
- Since: 0.14.0

Whether agents' own CLIs run inside a bubblewrap sandbox: none or
bwrap.

The fleet-wide answer for agents.runtime.confine, which an agent may
override either way. Off by default, so turning it on is a decision
rather than something an upgrade did to agents that were running.

Set to bwrap, every process-runtime agent that says nothing itself is
confined. An embedded agent cannot be -- it is the daemon -- and is
disabled with that reason rather than run unconfined; give it
runtime.confine: none of its own to keep it embedded.

### `defaults.avatar_max_bytes`

- Type: integer
- Default: `4194304`
- Since: 0.2.0

Largest profile picture the daemon will read and serve.

A picture crosses IPC as bytes rather than as a path, because a
client may be on another machine entirely -- so an unbounded
picture is an unbounded frame. Over the limit is a refusal naming
the limit, never a truncated image, which would surface as a broken
file a long way from the cause.

### `defaults.skills`

- Type: list of strings
- Default: unset
- Inside an agent: `skills`
- Since: 0.2.0

Skills every agent gets unless it says otherwise.

Resolved together with the team's list and the agent's own through
one function, the way integrations already are. Two resolvers would
be two behaviours, and the one nobody tested would be the one that
ran.

Names, not paths. A name that matches no skill is a warning rather
than an error -- a fleet is edited by hand -- but it is never
silent, because an agent quietly missing the procedure it was
configured with looks exactly like one that has it and ignored it.

## `defaults.policy`

What agents may do without asking. See agents.policy.

### `defaults.policy.rules`

- Type: list of strings
- Default: unset
- Inside an agent: `policy.rules`
- Since: 0.3.0

Action rules every agent follows unless it has its own list.

Each is MATCH=MODE. MATCH is an action class -- read, write, delete,
message, exec, external, desktop_input, fleet_admin -- or a tool-name
glob such as clawtilla_create_* or Bash. MODE is allow, review, ask,
hand_back or deny. The first rule that matches decides; a call no
rule matches is allowed. An agent's own agents.policy.rules replaces
this list rather than adding to it, so the order of one list is the
whole answer.

### `defaults.policy.ask_timeout_seconds`

- Type: integer
- Default: `600`
- Inside an agent: `policy.ask_timeout_seconds`
- Since: 0.3.0

How long an `ask` waits for a person before it is refused. The
decision is then settled as defaulted, so the inbox says nobody
chose rather than that somebody said no.

### `defaults.policy.reviewer`

- Type: string
- Default: unset
- Since: 0.3.0

The decision model `review` asks: typesafe, ollama or laya (ai-glib's
deciders). Unset, `review` behaves as `ask`. A reviewer that is
unsure, or that fails, also becomes `ask` -- never `allow`.
