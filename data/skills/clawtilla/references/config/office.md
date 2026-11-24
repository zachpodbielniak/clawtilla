# Configuration: `office`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `office`

The office: the fleet drawn as people in a building.

The daemon folds what it sees -- turn edges, tool steps, messages,
decisions, incidents -- into an activity for every agent, and every
client draws who is where from it; with OFFICE=1 the office itself is
drawn in 2D or 3D. Nothing is invented: an agent at its desk is one
the daemon saw start a turn. See docs/office.org.

### `office.enabled`

- Type: boolean
- Default: `false`
- Since: 0.56.0

Whether the daemon keeps the office: the presence fold, the
office.presence events and the office.* requests. Off, nothing is
computed, and every client's Office page says how to turn it on.
Takes effect on the next reload.

### `office.world`

- Type: string
- Default: `modern-office`
- Since: 0.56.0

Which world pack to draw: a directory name under one of the world
directories (office.world_dirs, <state_dir>/office/worlds, then the
shipped data/office/worlds). Two ship: modern-office and cozy-town.
`clawtilla office worlds` lists what is available. See
docs/office-world-packs.org.

### `office.renderer`

- Type: string
- Default: `auto`
- Since: 0.56.0

How a renderer should draw the world: `2d`, `3d`, or `auto` -- 3D
when the pack has a level, otherwise 2D. A renderer's own option
(clawtilla-office --2d) wins over this.

### `office.world_dirs`

- Type: list of strings
- Default: unset
- Since: 0.56.0

Extra directories holding world packs, searched before the daemon's
own and the shipped ones, so your copy of a pack replaces theirs.

### `office.stale_seconds`

- Type: integer
- Default: `600`
- Since: 0.56.0

How long a turn that was never seen to end is believed. A process
that dies mid-turn never closes its turn edge; after this long its
agent goes back to the lounge rather than sitting at its desk for
ever.

### `office.meeting_min_members`

- Type: integer
- Default: `3`
- Since: 0.56.0

How many members a group room needs for a round in it to be drawn
as a meeting. Fewer than 3 is read as 3: two agents talking is a
call, not a meeting.

### `office.choreography`

- Type: path
- Default: unset
- Since: 0.56.0

A YAML file of choreography rules -- which zone, pose, prop and emote
each activity gets -- applied over the built-in rules and the world
pack's own. `clawtilla office choreography` prints the built-in rules,
which are also the format. See docs/office-choreography.org.

### `office.asset_api`

- Type: string
- Default: unset
- Since: 0.60.0

A service the daemon makes characters with itself, with no model
deciding what to ask: `comfyui` (self-hosted, free: office.comfyui_url
and office.comfyui_workflow); `higgsfield` (Higgsfield's REST API, a
key pair stored with `clawtilla office api-key higgsfield`, billed to
the API balance in Higgsfield's console); `higgsfield-mcp` (Higgsfield's
MCP server through a connector's sign-in -- office.asset_generator, or
the only higgsfield connector -- billed to the account's app credits,
the ones the website and the hf CLI spend); `pixellab` (a pixel-art
character in four directions with a walk cycle in each, at
office.pixel_size; a key stored with `clawtilla office api-key
pixellab`, or any agent's pixellab integration); `meshy` (a rigged 3D
model that walks, for the 3D view: see office.asset_api_3d); or empty
for none.
The two Higgsfield roads are billed separately: choose the one your
money is in.
`clawtilla office character AGENT --direct` makes one; a paid service's
estimate is put to you as a decision and nothing is spent until you
approve.
See docs/office-custodial-team.org.

### `office.asset_api_3d`

- Type: string
- Default: unset
- Since: 0.76.0

A service that makes each character a 3D model as well: `meshy`, or
empty for none.

Meshy makes a model from the agent's picture (or, with none, its
description) and rigs it; the rig comes with a walking clip, which the
3D view plays when the character walks. Beside office.asset_api, so
one request can make a PixelLab sprite for the 2D view and a Meshy
model for the 3D one. The key is `clawtilla office api-key meshy` or
any agent's meshy integration; the price, in Meshy credits, is put to
you before anything runs. Meshy deletes what it made after three days;
the office keeps its copy.

### `office.pixel_size`

- Type: integer
- Default: `48`
- Since: 0.76.0

The size, in pixels a side, of a character PixelLab draws for the
office (office.asset_api: pixellab): 16 to 128. The 2D office's tiles
are 32 pixels, so 48 stands a head taller than a desk.

### `office.mesh_polycount`

- Type: integer
- Default: `8000`
- Since: 0.76.0

How many polygons Meshy aims for in a character's model
(office.asset_api_3d: meshy): 100 to 300000. A fleet's worth of
characters is drawn at once, so the default is light.

### `office.comfyui_url`

- Type: string
- Default: `http://127.0.0.1:8188`
- Since: 0.61.0

The ComfyUI server office.asset_api: comfyui makes characters with --
your own, self-hosted. A token for an auth proxy in front of it is
stored with `clawtilla office api-key comfyui`.

### `office.comfyui_workflow`

- Type: path
- Default: unset
- Since: 0.61.0

The ComfyUI workflow characters are made with, exported in API format,
with {{prompt}} where the description goes and, if you like,
{{image}} (the agent's picture, uploaded), {{seed}} and
{{negative_prompt}}. See docs/office-custodial-team.org.

### `office.asset_model`

- Type: string
- Default: unset
- Since: 0.60.0

The model office.asset_api makes characters with, in the service's own
spelling. Empty is each road's own default: for `higgsfield` the REST
endpoint path higgsfield-ai/soul/v2/standard, for `higgsfield-mcp` the
MCP catalogue's nano_banana_2 -- the two name models differently, and
one road's name is refused by the other.

### `office.asset_generator`

- Type: string
- Default: unset
- Since: 0.58.0

The connector the office's assets and characters are made with: the
name of one you added (`clawtilla connector add hf --provider
higgsfield`, or a self-hosted ComfyUI). Set, creating an agent while
the office is on files a task to the custodial team (or the chief) to
make its character from its picture, and installing the custodial team
makes every call to it ask you first. Empty, nothing is generated.
See docs/office-custodial-team.org.

### `office.peers`

- Type: list of strings
- Default: unset
- Since: 0.64.0

Other fleets whose people visit this office, each `NAME=WHERE`:
WHERE is the other daemon's socket on this machine
(`work=/run/user/1000/work/d.sock`) or its tailnet address
(`studio=100.64.0.7:8792`), paired once with `clawtilla office
peer-pair NAME CODE` using a code `clawtilla device pair` printed on
that machine. Their people are drawn as visitors, read-only: nothing
they send can act here, and each fleet publishes only its own people,
so two fleets that peer with each other never echo. Asked only once a
client is drawing the office. See docs/office.org#federation.

Superseded by linking fleets (`clawtilla federation link`), whose people
visit without the owner-level device token this needs, as each fleet's
grant allows. A fleet both linked and listed here is drawn once, from
the link. Still read; it will go in a later release.

### `office.peer_poll_seconds`

- Type: integer
- Default: `5`
- Since: 0.64.0

How often, in seconds, each office.peers fleet is asked who is where.
A fleet that has not answered for three of these is drawn empty, not
frozen.
