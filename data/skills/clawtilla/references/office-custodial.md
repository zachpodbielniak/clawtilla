# The office custodial team

Agents that keep the office: its world packs, assets and per-agent characters. Optional, never made unasked; the chief can do all of it alone. The office itself (packs, zones, choreography, renderers) is in [office.md](office.md); keys in [config-keys.md](config-keys.md); approvals in security.md; connectors in connectors.md.

Source docs: docs/office-custodial-team.org, docs/higgsfield.org. Code: src/core/daemon-office-custodial.c, src/core/daemon-office-generate.c, src/office/clawt-office-pack.c.

## Who is on it

Found by the team tag `office-custodial`, never by name (rename it, or tag your own team; a second tagged team is a fleet warning and the first in the team list wins). Install makes team `<team>` (default `facilities`, name "Facilities") and three agents, each with `tools.office: true` and its mission in `SOUL.org`:

| Agent id | `team_role` | Title | Does |
|---|---|---|---|
| `<team>-lead` | lead | Facilities lead | takes requests from operator and chief, delegates, checks with `clawtilla_office_preview`, applies a world only when asked |
| `<team>-assets` | member | Asset steward | makes/imports tiles, props, characters with the configured generator; records provenance for every file |
| `<team>-designer` | member | Set designer | edits `office.yaml`, `choreography.yaml`, maps; read before write, preview after |

If `office.asset_generator` is set at install, the steward gets `policy.rules: ["mcp__clawtilla-<generator>__*=ask"]`: every generator call becomes a decision card. Set the generator before installing, or add the rule by hand afterwards.

## Installing

```bash
clawtilla office custodians                          # current custodial team + asset generator
clawtilla office custodians install                  # team "facilities"
clawtilla office custodians install --team town-council
```

Also: Office page "Create custodial team" in any client; IPC `office.custodians_install` (`team`, `start` default true); the chief's MCP tool `clawtilla_office_custodians_install` (needs `tools.office` **and** `tools.manage_fleet`). Refused, before anything is written, when a team already carries the tag (named), or the team id or any of the three agent ids is taken. Agents are started at once unless `start: false`.

## Permission: `tools.office`

`agents.tools.office` (bool, schema default false) gates every `clawtilla_office_*` tool. Resolved on for the chief of staff unless set otherwise; on for the custodians (made with it); off for others; never offered to an ephemeral helper. Turn it on for another agent: `clawtilla agent set <id> tools.office true` (see agents.md).

## The MCP tools

| Tool | Args (* = required) | Action class |
|---|---|---|
| `clawtilla_office_worlds` | none | read: packs, which is in use, refused + why, which were written here |
| `clawtilla_office_world_read` | `world` (default current), `path` (default `office.yaml`; text only) | read, with loader warnings |
| `clawtilla_office_world_write` | `world`*, `path`*, `content`* (whole file), `from` (for a new id: world to copy first) | write |
| `clawtilla_office_world_apply` | `world`* | write: draws the office there for everybody; only when operator/chief asked |
| `clawtilla_office_preview` | `world` (default current) | read: who sits where, pose, fallbacks |
| `clawtilla_office_asset_import` | `world` or `character` (exactly one), `source`* (file in your own workspace), `path`* (inside pack/character, e.g. `tiles/desk.png`, `sprites/sheet.png`), `license` (when not the pack's), `provenance`* | write: records sha256 + licence + provenance |
| `clawtilla_office_character_layout` | `agent`*, `sheet`* (imported PNG), `frame_width`*, `frame_height`*, `animations`* (object), `background` (`corners` or `#rrggbb`) | write |
| `clawtilla_office_character_mesh` | `agent`*, `mesh`* (imported `.glb`, `.obj`, `.vox`, `.iqm`, `.m3d`; `.gltf` refused, it references other files) | write |
| `clawtilla_office_custodians_install` | `team` (default `facilities`) | fleet admin; also needs `tools.manage_fleet` |

Full tool catalogue: mcp-tools.md.

## How writes stay safe (enforced in clawt-office-pack.c, for every caller)

- Only under `<state_dir>/office/worlds/<id>/`. A write to a shipped pack copies it there first; that copy then wins (state dir is searched before shipped).
- A pack in the operator's `office.world_dirs` is never written (it is searched first, so a copy would never be drawn). Make a new world instead: `world_write world=<new> from=<theirs>`, then apply `<new>`.
- Paths: relative, no `..`, no hidden names; the landing dir is resolved through symlinks and must stay inside the pack.
- A write to `office.yaml` or `choreography.yaml` is load-checked; a refusal restores the old file and returns the loader's reason. Read it, fix, rewrite.
- Assets come only from the importing agent's workspace and always with provenance; the sha256 goes into the manifest's `assets` (`source` holds the provenance) so another fleet can verify the pack.
- Characters live in `<state_dir>/office/characters/<agent>/` with a `character.yaml` (files, checksums, provenance, sheet layout, `mesh`, `background`). Renderers refuse a sheet whose sha256 does not match.

## Editing a world: the loop

1. `clawtilla_office_worlds` -> pick a base.
2. `clawtilla_office_world_read world=<base> path=office.yaml` (and `choreography.yaml` if present).
3. `clawtilla_office_world_write world=<new-id> from=<base> path=choreography.yaml content=<whole file>` (new id = a new pack; same shipped id = an edited copy).
4. `clawtilla_office_preview world=<new-id>`: everybody seated, no `fallback`, capacity enough (one anchor per chair).
5. Report and ask; on yes, `clawtilla_office_world_apply world=<new-id>`.

Zone tags, anchors, manifest schema and choreography fields: office.md. A rule naming a tag the pack lacks falls back (desk for call/work, lounge otherwise) and shows as `fallback` in preview: add the zone or change the rule.

## Characters

### Road 1: the custodial team, via a generator connector (`office.asset_generator`)

`office.asset_generator` = the name of a connector the steward holds (e.g. `hf`, a Higgsfield connector; or a self-hosted ComfyUI behind an MCP server added with `--provider remote --url ...` or `--command ...`). With it set, **creating a non-ephemeral agent** files a task *Office character for <agent>* to the custodial lead, else to the chief if its `tools.office` is on; else nothing (logged). Nothing about the new agent waits on it. Re-ask any time: `clawtilla office character <agent>` (IPC `office.character`; refused with what is missing: no agent, no generator, nobody keeps the office).

The daemon stages the reference picture in the maker's (and every custodian's) workspace under `office-characters/<agent>/`: `agents.avatar` or the client-set profile picture if any, else the agent's generated SVG face. The task asks for: a turnaround; clips for walk (4 directions), idle, sit+type, phone, present; a sprite sheet, 32x48 px frames, one named row per animation. Then:

```text
clawtilla_office_asset_import character=kudu source=office-characters/kudu/sheet.png
    path=sprites/sheet.png provenance="Higgsfield nano_banana_2 via connector hf; Higgsfield terms (docs/higgsfield.org)"
clawtilla_office_character_layout agent=kudu sheet=sprites/sheet.png frame_width=32 frame_height=48
    background=corners
    animations={"idle":{"row":0,"frames":1},"walk_down":{"row":1,"frames":4,"fps":8},
                "walk_up":{"row":2,"frames":4,"fps":8},"walk_left":{"row":3,"frames":4,"fps":8},
                "walk_right":{"row":4,"frames":4,"fps":8},"type":{"row":5,"frames":4,"fps":6}}
```

Row names the task asks for: idle, walk_down, walk_up, walk_left, walk_right, sit, type, phone, present. Every row/frame is checked against the image; past the edge is refused and the manifest left alone. The sheet must be imported first. `background`: `corners` (colour all four frame corners share) or `#rrggbb`; keying removes only backdrop connected to the frame edge (`clawt_office_character_key_background()`, so eye whites survive), frames whose corners disagree are left as is, and a keyed sheet is trimmed to the one opaque rectangle across all frames (`clawt_office_character_opaque_bounds()`). A new layout without `background` clears an old one. Grant the connector only to the steward: `clawtilla connector add hf --provider higgsfield --scope selected --agents facilities-assets --tools generate_image,estimate_image_cost,job_status,balance` then `clawtilla connector connect hf`.

3D models: Higgsfield makes no 3D. Get a model elsewhere (ComfyUI image-to-3D node, an artist, a CC0 pack; check licences), import it under `character`, then `clawtilla_office_character_mesh agent=kudu mesh=models/kudu.glb`. A rigged `.glb` with an animation named like *walk* walks; otherwise it glides. No model = the sheet's frame on a billboard.

### Road 2: the daemon itself (`office.asset_api`, `--direct`)

No model in the loop: one portrait from the agent's description and picture, imported as a one-frame character (`generated/portrait.png`, `background: corners` set for you). A one-frame character stands still in every pose.

| `office.asset_api` | Talks to | Auth | Pays from | Default model (`office.asset_model` overrides, road's own spelling) |
|---|---|---|---|---|
| `comfyui` | `office.comfyui_url` (default `http://127.0.0.1:8188`) with `office.comfyui_workflow` | optional proxy bearer: `clawtilla office api-key comfyui` | free (estimate 0, no card) | the workflow's |
| `higgsfield` | REST `api.higgsfield.ai` | `clawtilla office api-key higgsfield` (`KEY_ID:KEY_SECRET` from console.higgsfield.ai) | API balance (separate from app credits; empty -> `HTTP 403 not_enough_credits`) | `higgsfield-ai/soul/v2/standard` |
| `higgsfield-mcp` | MCP `mcp.higgsfield.ai` | connector SSO: `clawtilla connector connect hf` | app credits | `nano_banana_2` |

`higgsfield-mcp` uses the connector `office.asset_generator` names, else the fleet's only Higgsfield connector; two with none named is refused. An unsigned connector is refused up front with the sign-in command. Keys go to `secrets.dir/office/<service>.key` (0600); replies name the file, never the value.

ComfyUI workflow: export in **API format** (the editor save is refused). Placeholders in inputs: `{{prompt}}` (CLIPTextEncode text), optional `{{negative_prompt}}`, `{{seed}}` (whole input becomes a number), `{{image}}` in a LoadImage node (needs the agent to have a picture: `clawtilla agent avatar set <agent> <file>`, PNG/JPEG/WebP; refused at once otherwise). Results fetched only from that server's `/view`. Output licence = the models you run (e.g. CommonCanvas = CC BY-SA 4.0 -> record attribution).

Flow for a paid road:

1. `clawtilla office character kudu --direct` (IPC `office.character` with `direct: true`) answers `asked: true` and `service`.
2. The estimate arrives as a decision (id prefix `office-generate-`), options `approve` / `decline`, no deadline, default decline. It names price and money source: "about $0.0040, from your Higgsfield API balance" or "1.5 credits, from your Higgsfield app credits". A paid service that estimates zero, or gives no estimate, still asks.
3. Only the exact answer `approve` proceeds: `clawtilla decision list`, then `clawtilla decision answer <id> approve` (operator's call, or the card in a client). Decline/dismiss/delete spends nothing.
4. Upload picture (if any) -> submit one image -> wait -> download -> import with provenance (model, road, the quoted price, Higgsfield's terms).
5. Outcome (made or refused) is posted as a system note in the operator's conversation with that agent, starting no turn; also in the daemon log.

## Recipe: the chief bootstraps the office in one conversation

1. Operator: `clawtilla office on` (turning it on is the operator's; the chief has no tool for it).
2. Chief: `clawtilla_office_worlds` -> current world and alternatives.
3. Chief: `clawtilla_office_world_read world=cozy-town path=office.yaml`; then `clawtilla_office_world_write world=winter-lodge from=cozy-town path=choreography.yaml content="choreography:\n  - { activity: idle, zone: breakroom, pose: sit, prop: coffee }\n"`.
4. Chief: `clawtilla_office_preview world=winter-lodge`; fix any `fallback`; report "everybody seated, shall I apply?".
5. On yes: `clawtilla_office_world_apply world=winter-lodge`.
6. Optional, with operator consent and `tools.manage_fleet`: `clawtilla_office_custodians_install team=facilities` so later requests go to `facilities-lead`. Ask the operator to set `office.asset_generator` (and grant that connector to `facilities-assets`) **before** this step so the steward's `=ask` rule is written.

## Recipe: regenerate one agent's character

- Team road: `clawtilla office character kudu` -> task to the custodial lead -> lead hands to `<team>-assets` with the staged picture path (`office-characters/kudu/` in the maker's workspace) -> steward generates (each call a decision card), imports with provenance, sets layout with `background` -> lead checks `clawtilla_office_preview` -> report.
- Direct road: set `office.asset_api` (+ key or connector), give the agent a picture if the road needs one (`clawtilla agent avatar set kudu pic.png`), `clawtilla office character kudu --direct`, operator approves the card, watch for the system note in kudu's conversation. Re-importing overwrites `generated/portrait.png` in `<state_dir>/office/characters/kudu/`.

## Never

- Never spend without a person's approval: do not loosen the steward's `=ask` rule, never answer an `office-generate-*` decision yourself, never resubmit a Higgsfield job that is still running (charged twice; ask `job_status` again).
- Never write outside `<state_dir>/office/worlds/` (packs) or `<state_dir>/office/characters/` (via the tools); never edit shipped `data/office/worlds/` or the operator's `office.world_dirs` packs directly.
- Never import an asset without provenance or with a licence that forbids redistribution into a pack meant to travel.
- Never apply a world nobody asked for; never install the team unasked.
- Never put a key or token in a message, file or argv: `clawtilla office api-key` reads stdin.

## Troubleshooting

| Symptom | Cause |
|---|---|
| No character task on agent creation | office off, `office.asset_generator` empty, or nobody keeps the office (no custodial team and chief's `tools.office` off); `clawtilla office character <agent>` says which |
| A write is refused | the message is the loader's reason; the pack is unchanged |
| Steward cannot see the picture | it is staged in the maker's and custodians' workspaces; a lead handing on should pass the path |
| Every generator call waits on me | the install's `mcp__clawtilla-<generator>__*=ask` rule, by design |
| Direct road "refused": no picture | ComfyUI workflow with `{{image}}` and no avatar: `clawtilla agent avatar set` |
| REST road `403 not_enough_credits` | API balance empty (app credits do not pay it): fund in console or switch to `higgsfield-mcp` |
| Character drawn as a box | sheet on a backdrop without `background`; relayout with `background=corners` |
