# The office

The fleet drawn as people in a building. The daemon folds what it already saw (turn edges, tool steps, messages, decisions, incidents) into one activity per participant; a director seats everyone in a world pack; renderers draw it. Nothing is invented. Keeping the office (packs, characters) by agents is in [office-custodial.md](office-custodial.md). Every key below: [config-keys.md](config-keys.md).

Source docs: docs/office.org, docs/office-world-packs.org, docs/office-choreography.org, docs/office-adapters.org.

## What is built where

| Piece | Build |
|---|---|
| presence fold, `office.*` IPC, director (`office.layout`), roster (`clawtilla office who`) | always |
| `clawtilla-office` (2D/3D renderer on libregnum) | `make OFFICE=1` |
| lrg client Office page (same scene in-process) | `make OFFICE=1` (implies `LRG_BACKEND=1`) |
| GTK Office page | always; runs `clawtilla-office --embed` as a child, so needs it at run time |
| web Office page | always; its own canvas (`data/web/office.js`), `?simulate=1` |

`OFFICE=1` needs libregnum >= 0.11.1 at `LRG_DIR` (the build refuses older; 0.11.1 is the first whose raylib decodes JPEG, which Meshy models carry as textures) (default `../libregnum`), plus `gobject-introspection-devel` and `gjs-devel` (3D levels reach libregnum scripting) on top of the lrg client's packages (see clients.md). Gotcha: `make OFFICE=1 office` does not rebuild `clawtillad`; run plain `make` too before restarting a daemon, or it answers with old JSON. The GTK page finds the renderer beside its own binary, then `<prefix>/bin`, then `PATH`.

## Turning it on

```bash
clawtilla office on                 # office.set enabled=true; applied at once, no reload
clawtilla office use cozy-town      # office.world (default modern-office); unknown world refused
clawtilla office renderer 3d        # office.renderer: auto | 2d | 3d (auto = 3d when the pack has a level)
clawtilla office off
```

`office.set` writes `office.enabled` / `office.world` / `office.renderer` to clawtilla.yaml and publishes `office.changed`. Editing by hand needs `clawtilla config reload`. Other tunables: `office.stale_seconds` (600: a turn never seen to end is believed this long), `office.meeting_min_members` (3, values below 3 read as 3), `office.world_dirs`, `office.choreography`.

## CLI (`clawtilla office --help`)

| Verb | Does |
|---|---|
| `who` | roster (`office.snapshot`) |
| `worlds` | packs available, `*` = current, and refused packs with reasons (`office.worlds`) |
| `layout` | the director's seat for each participant (`office.layout`) |
| `choreography` | prints the built-in rules (also the override format) |
| `on` / `off` / `use WORLD` / `renderer R` | `office.set` |
| `custodians` / `custodians install [--team NAME]` | see office-custodial.md |
| `character AGENT [--direct]` | commission a character (office-custodial.md) |
| `api-key SERVICE` | `higgsfield`, `comfyui`, `pixellab` or `meshy`, read from stdin, echo off |
| `world restyle BASE NAME --style TEXT [--no-2d] [--3d]` | a new world with BASE's layout: tiles redrawn by PixelLab, furniture (with `--3d`) made by Meshy; one priced decision first |
| `peers` / `peer-pair NAME CODE` | federation, below |
| `open [ARGS...]` | execs `clawtilla-office` (beside the CLI, else `PATH`) with the remaining args |

`who`, `worlds` and `layout` take `--json` as the last argument (raw reply).

## Activities (loudest wins)

| Activity | When | Default drawing |
|---|---|---|
| `waiting_on_you` | an open decision it asked | own desk, `?` overhead |
| `error` | last turn timed out / repeated, or a run broke; cleared by next turn | bug corner, `!` |
| `in_meeting` | group room with >= `office.meeting_min_members` members, message in last 3 min | smallest `meeting` zone that fits; `town_hall` when chief/you speak and every lead is in |
| `on_call` | it and one agent exchanged a message in a room of two in the last minute | `phone_booth` (desk if none) |
| `delivering` | just assigned a task / queued a handoff | beside the assignee's desk, folder |
| `working` | turn running, a tool used in the last 45 s | the tool's station |
| `thinking` | turn running, no tool | own desk |
| `arriving` / `leaving` | ephemeral helper spawned < 15 s ago / being reaped | `entrance` |
| `idle` | running, nothing to do | `lounge` |
| `offline` | not running | not drawn |

Stations by tool name (`clawt_office_station_for_tool()`): library = read, grep, glob, search, fetch, research, recall, memory; lab = bash, shell, exec, test, computer; whiteboard = todo, plan, task, delegate, ephemeral, decision; terminal = browser, display, screen; desk = anything else.

Participant kinds: `agent`, `ephemeral`, `grok_bot` (visitor desk while it has a job), `external` (visitors from other fleets). Each carries `fleet` (id in `<state_dir>/fleet-id`).

## World packs

A directory with an `office.yaml`; its id is the directory name. Search order (first id wins): `office.world_dirs` -> `<state_dir>/office/worlds/` (where agents write) -> shipped `data/office/worlds/` (beside a checkout's binary, then the install data dir). Shipped: `modern-office`, `cozy-town` (CC0, drawn by `tools/clawt-office-genworld.c`; `make office-worlds` redraws, needs `cairo-devel`).

```
my-office/
  office.yaml        manifest (required)
  choreography.yaml  pack-level rule overrides (optional)
  map.tmj + tiles.png   Tiled map for 2D (32x32 tiles in the shipped ones)
  office.rlevel      libregnum level for 3D
```

```yaml
name: My Office
description: "..."
license: CC0-1.0                  # required, or the pack is refused
renderers:                        # at least a map or a level (or an adapter that loads)
  2d: { map: map.tmj }            # relative paths only, inside the pack
  3d: { level: office.rlevel }
  adapter: { module: tiled, source: map.tmj }   # optional, see Adapters
zones:
  - id: floor
    name: "Open floor"
    tags: [desk]
    team: core                    # optional: this team's desks first
    capacity: 12                  # optional: default = anchor count
    anchors_2d: [[400, 112], [496, 112]]          # seats, map pixels (middle of the tile)
    anchors_3d: [[12.5, 0.0, 3.5], [15.5, 0.0, 3.5]]  # x,y,z; shipped levels: 2D / tile size, map y -> z
  - id: boardroom
    tags: ["meeting:10"]
    anchors_2d: [[1120, 352], [1152, 352]]
districts: [north-wing]           # optional, for a future merged office
assets:
  - { path: tiles.png, sha256: <hex>, source: "drawn by hand", license: CC0-1.0 }
```

`rect_2d: [x, y, w, h]` (map pixels, optional) is a zone's area; the loader keeps it (`ClawtOfficeZone.rect_2d`, `office.world` JSON `rect_2d`) and the `usage_board` is fitted to it (five tiles round the anchor without one).

Load rules (`clawt_office_world_load()`): refused (listed under `refused` in `office worlds`) when missing/not YAML, no `license`, no map or level, or a path absolute / containing `..`. Warned but kept: zone without id (ignored), unknown tag, zone with no anchors, no `lounge` zone. `clawt_office_world_verify_assets()` checks every `assets` entry exists and matches its `sha256` (the check for a pack synced from another fleet). Licensing: one pack `license`; an asset with another says so in its own `license`; `source` records URL/author or generator+model+terms. Never ship non-redistributable art (LimeZu interiors = private pack).

### Zone tags (`clawt_office_zone_tag_count()`/`_nth()`)

`desk`, `team_area` (`team_area:<team>`), `lead_office`, `chief_office`, `lounge` (also the fallback), `breakroom` (lounge overflow), `meeting` (`meeting:<capacity>`), `town_hall`, `phone_booth`, `bug_corner`, `library`, `lab`, `whiteboard`, `terminal`, `server_room` (decor), `visitor` (Grok bots, other fleets), `entrance` (helpers), `usage_board` (wall showing `usage.providers` allowances, never forcing a fresh read; see Usage board below). A zone may carry several tags.

### How seats are given (`clawt_office_direct()`)

1. Homes in id order: chief -> `chief_office`, leads -> `lead_office`, Grok bot -> `visitor`, others -> `desk` (own team's zone first).
2. Meetings seated together in the smallest fitting `meeting` (or `town_hall`).
3. Everyone else where the choreography says (`desk` = own home; for `delivering` the assignee's).
4. Full zone -> another zone with the tag -> standing room at its last anchor. Missing tag -> desk (call/work) or lounge, with `fallback` set on the placement. Capacity is real: one anchor per chair.

Authoring: 2D in Tiled (copy `data/office/worlds/modern-office` into a dir in `office.world_dirs`, edit `map.tmj` layers `floor`/`furniture`/`zones`, write zones+anchors into office.yaml, check with `office worlds` and `office layout`). 3D: a `.rlevel` (YAML) edited in lrgeditor or by hand; nodes with `transform` and `visual` (`primitive` cube/plane/sphere/cylinder/capsule/cone/torus, or `mesh_asset` glTF/GLB/OBJ/VOX inside the pack). Agents do not write your `office.world_dirs` packs: see office-custodial.md.

## Choreography

Three layers, each replacing only the rules it names (by activity + station): built-in -> the pack's `choreography.yaml` -> the file `office.choreography` names.

```yaml
choreography:
  - { activity: idle,  zone: breakroom,  pose: sit,   prop: coffee }
  - { activity: error, zone: bug_corner, pose: stand, prop: wrench, emote: alert }
  - { activity: working, station: lab, zone: lab, pose: type, prop: terminal }
```

Fields: `activity` (required), `station` (working only: desk/library/lab/whiteboard/terminal; beats plain working), `zone` (tag, required), `pose` (stand, sit, walk, phone, present, read, type; default stand), `prop` (phone, folder, book, laptop, marker, terminal, screen, coffee, anything a renderer draws), `emote` (question, alert, thought, zzz, wave). Your file is checked whole: an unknown activity/station or a rule without zone refuses it out loud in `office.world`/`office.layout`; a pack's broken file is only a log warning. After editing: `clawtilla config reload && clawtilla office layout`.

## Adapters

`renderers.adapter.module` is a name, never a path. `tiled` = libregnum `LrgTiledWorld`: walls from layers with a true `collision`/`solid` property or named `collision`/`walls`, plus `obstacle` objects; people path around them, drawn as plain discs (no faces, characters, usage board; Tab still reaches the 3D level). `source` is a pack file if one has that name, else passed verbatim. Own module: `libclawt-office-adapter-<name>.so` exporting `clawt_office_adapter_new(const gchar *pack_dir, GError **error)` returning an `LrgWorldAdapter`; searched in each `--adapter-dir`, `$XDG_DATA_HOME/clawtilla/office-adapters/`, `<libdir>/clawtilla/office-adapters/`. Adapters run on the renderer's machine; the web client never runs them.

## Opening it

| Client | How |
|---|---|
| CLI | `clawtilla office open [clawtilla-office args]` |
| renderer | `clawtilla-office [--socket F | --profile NAME | --host H --daemon-port P --token-file F] [--world ID] [--2d|--3d] [--focus ID] [--zoom fit|1x|2x] [--ui-scale N] [--usage world|corner|off]` |
| GTK / lrg | `clawtilla-gtk --office`, `clawtilla-lrg --office` (open on the Office page) |
| GTK / lrg, own window | **Open office**: a button beside 2D / 3D on the Office page (that mode), and main menu -> Open office -> 2D / 3D (GTK `win.open-office` with `"2d"`/`"3d"`; lrg `win.office-2d`/`win.office-3d`) |
| web | `clawtilla-web --office` sends `/` to the Office page (wall display); no Open office (a server would open it on its own display) |

**Open office** (`clawt_office_renderer_open_window()`) starts `clawtilla-office` as a separate program -- not waited for, not killed with the client, one more per press -- on the client's own connection, named outright by `clawt_office_renderer_window_argv()` (no `--embed`, always `--2d` or `--3d`): local -> `--socket PATH` (default socket spelled out); saved -> `--profile NAME`; typed `--host` -> `--host H --daemon-port P` plus, if it has a token, `--token-file /dev/fd/3`, the token written into a pipe whose read end is the child's fd 3. Never `--token`. Typed + TLS is refused (TLS only via a saved profile). Each launch logs (`g_message`) and toasts the pid, mode and daemon -- "the local daemon (PATH)", "the saved connection “NAME” (HOST:PORT)", "HOST:PORT, its token through a pipe" -- never the token; a failure is a toast "The office did not open: ...".

`clawtilla-office` options: `--simulate` (invents calls/meetings/questions/failures for the real participants, on this screen only, labelled "Simulation"), `--screenshot FILE` (offscreen, settles `--frames N` default 90, writes PNG, exits; 30 s without world or layout is a failure), `--width`/`--height` (64-8192, default 1280x720), `--embed FD` (LFRM v1 frame stream for the GTK host), `--adapter-dir DIR` (repeatable), `--zoom fit|1x|2x` (2D map zoom: fit = largest whole-number scale that fits, 1x/2x = logical px per map px; in 3D the camera distance), `--ui-scale N` (1-4: text, plates and marks N times larger, for HiDPI frames), `--usage world|corner|off` (where provider usage goes), `--usage-open` (open the usage details panel), `--usage-report FILE` (with `--screenshot`: JSON of where board and panel were drawn), `--pack DIR --layout-file F [--usage-file F]` (no daemon: pack from disk, `office.layout` and `usage.providers` answers from files; the usage file's `now` is the board's clock). Window keys: Tab 2D/3D, arrows or right-drag orbit, wheel zoom, U cycles usage placement, Escape closes the usage panel or else quits; a click prints `{"event":"open_agent","agent":"<id>"}` on stdout.

Embedded (`--embed`): left click (press+release within 4 logical px) opens (the usage board takes its clicks first), left drag orbits 3D or pans a zoomed 2D map, wheel zooms 3D, an Escape key message closes the usage panel. stdin takes control lines `{"mode":"2d|3d","focus":"<id>|","zoom":"fit|1x|2x","ui_scale":N,"usage":"world|corner|off"}` (every member optional; unknown members ignored; a bad member refuses the whole line, reason on stderr) -- `clawt_office_view_control_line()` / `_parse()`. stdout adds `{"event":"view","mode":"2d","has_3d":true}` when what is drawn changes (a world without a level stays 2D).

Name plates never overlap (`clawt_office_labels_arrange()`): hovered and followed plates stay put; others keep their place if free, else are pushed just below the plate in the way (at most twice), else are hidden until hovered. Same in 2D, 3D, adapters, and the lrg client.

### GTK Office page

Toolbar over the drawing: **2D / 3D** (shows what is drawn; 3D greyed out for a world with no level), **Open office** (separate `clawtilla-office` window in that mode, on this connection), **Fit / 1x / 2x**, **Usage** (On the wall / In the corner / Off; remembered as `office_usage`), **Follow** (nobody or a roster participant; not remembered; reset when they leave), **Pop out** (own window, 1280x800; page shows **Bring back**; closing it brings it back; the child is not restarted). Hint: "Click somebody to open their conversation; drag or scroll to turn the 3D view." Mode and zoom are remembered in `appearance.yaml` as `office_view: 2d|3d` and `office_zoom: fit|1x|2x` (unknown spelling -> default, warned).

Size: page width x 3/4 of the page's visible height, min 320 logical px (`clawt_office_view_height_for_page()`); popped out, the whole window. Frames are requested in device px (`clawt_office_view_device_size()`, max 8192 a side) and resized with a RESIZE message, never a restart; the scale factor goes to the child as `ui_scale` so 4K at scale 2 reads like 1080p. Frames are drawn nearest-neighbour (`GSK_SCALING_FILTER_NEAREST`). The child runs while the office is on and (the page is on screen or the drawing is popped out). Pack files are fetched via `office.file` into `$XDG_CACHE_HOME/clawtilla/office/<world>/`, so a laptop draws a remote daemon's office. Verify a change visually: `clawtilla-office --simulate --screenshot /tmp/o.png --world my-office`.

### Usage board

Model: `clawt_office_board_new(usage, now)` -- every login (title, initial, status, tone, `tightest` index via `clawt_usage_login_tightest_window()`) and every window (label, value, reset, tone, `balance`, left); web gets it as `clawt_office_board_to_json()`. Never re-derive words or levels in a client. On the wall (`clawt_office_board_layout_world()`): fitted inside the `usage_board` zone's `rect_2d` (or 5x5 tiles round the anchor), never outside; drawn after the floor, before people and plates. Detail by the zone's on-screen size: rows (all logins+windows fit at 20 or 10 px x ui_scale) > gauges (badge with initial in tightest window's tone + meter, badge >= 14 px x ui_scale, "+N" for the rest) > one mark in the worst tone. Shipped worlds: gauges at Fit, rows at 2x. Hover opens the details panel (`clawt_office_board_layout_panel()`: heading, per login title+status, per window label | meter (none for a balance) | value | reset; columns content-sized, shrink reset first; text ellipsized; overflow rows -> "+N more"; right of board, else left, else over, clamped into the view); click pins it; second click / click elsewhere / Escape closes. Corner placement docks the same panel top right, always shown; off draws nothing. 3D: a camera-facing screen on the zone's 3D anchor. Tests: `tests/test-office-board.c` (layout), `tests/test-office-board-shots.sh` (both worlds, Fit and 2x, `--pack` fixture, report + pixel checks; references in `tests/fixtures/office/board-shots/`).

IPC (see mcp-tools.md / architecture.md for the wire): `office.snapshot`, `office.worlds`, `office.world`, `office.layout`, `office.file`, `office.set`, `office.characters`, `office.character_file`, `office.custodians`, `office.custodians_install`, `office.character`, `office.api_key`, `office.peers`, `office.peer_pair`; events `office.changed`, `office.presence` (ephemeral, not replayed: read `office.snapshot` on connect).

## Federation (read-only visiting)

Linking fleets for real -- keys, invites, tiers, grants -- is [federation.md](federation.md). Since 0.67.0 a linked fleet's running agents visit from the link's roster with no device token (federation.md, "Sidebar and office"); prefer `clawtilla federation link` to `office.peers`; `clawtilla federation migrate-office [NAME]` moves an entry onto a link (0.71.0). What follows is the older 0.64.0 visiting, still read: a fleet both linked and in `office.peers` is drawn once, from the link.

```yaml
office:
  peers:                              # NAME=WHERE
    - studio=/run/user/1000/studio/d.sock   # another daemon's socket on this machine
    - lab=100.64.0.7:8792                   # tailnet address of another machine
  peer_poll_seconds: 5
```

```bash
clawtilla device pair --name home-office   # on the peer machine: prints a code
clawtilla office peer-pair lab K7Q-4MZ     # here: token -> secrets.dir/office/peers/lab.token (0600); reply names the file only
clawtilla office peers                     # name, where, state asking|ok|quiet|error, fleet, visitors
```

- Polling starts at the first `office.layout` (someone drawing); a daemon nobody watches never dials peers.
- Visitors are `kind: external`, id `<peer>:<id>`, member `visiting_from` (not `peer`, which means call peer). Always seated at the `visitor` zone whatever they do, in their own colour; hover shows `Name · fleet`.
- Sanitised (`clawt_daemon_office_peer_sanitize()`): only the peer's own people, max 200, bounded strings, plain ids, unknown activity -> idle, colour only `#rrggbb`, a snapshot carrying this fleet's id refused.
- No echo: `office.snapshot` never includes visitors; they are merged into `office.layout` only.
- Three missed polls -> drawn empty, `peers` says `quiet` or the error.
- Nothing crosses fleets but looking: no messages, delegation or tasks.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Page says office is off | `clawtilla office on` |
| Drawn office empty | nobody running (offline is not drawn); `office who`; try `--simulate` |
| `did not load within 30 seconds` | daemon unreachable or `office.enabled` false |
| No 3D view | pack has no `.rlevel` or it failed (daemon log); 2D used |
| Everybody in the lounge | nobody in a turn |
| Agent stuck at desk | turn edge never closed (process died); decays after `office.stale_seconds` |
| Pack missing from `office worlds` | listed under `refused` with reason |
| Someone "at the lounge instead" | pack lacks the zone tag; `office layout` shows `fallback`; add zone or override rule |
| GTK: clawtilla-office not found | build `make OFFICE=1` in the same checkout, or install |
| GTK: "stopped drawing" | banner has exit status + last stderr line; rerun the command without `--embed`; `G_MESSAGES_DEBUG=Clawtilla clawtilla-gtk` |
| GTK with `--host`: cannot follow | save the connection as a profile and switch to it (a token cannot go in argv) |
| Open office: "The office did not open" | renderer not found (build `make OFFICE=1`), not startable, or a typed `--host` connection with TLS (save it) |
| GTK: 3D button greyed out | the world has no 3D level (`office worlds` shows `has_3d`); the saved 3D choice is kept for the next world |
| Some names missing | crowded or zoomed out: plates with no room are hidden; hover shows them, 1x/2x shows more |
| Character not drawn | sheet sha256 mismatch with its manifest, or no layout yet (office-custodial.md) |

## Restyling a world (0.77.0)

`clawtilla office world restyle BASE NAME --style "..." [--no-2d] [--3d]` (IPC `office.world.restyle`; agent tool `clawtilla_office_world_restyle`, gate `tools.office`). Keeps BASE's map, zones and anchors; PixelLab redraws each tile kind the map uses into its own slot of a new `tiles.png`, starting from the base tile itself (`init_image`; drawn from the name alone it painted whole little offices into floor slots) (needs a cairo build, `POPPLER=1`); with `--3d` Meshy makes each furniture kind the level draws as one `T<x>-<y>` node (`kind: mesh-asset`, `models/<kind>.glb`) -- walls, glass, doors and multi-node kinds (booths) stay primitives. Needs the base manifest's `tiles:` list (`make office-worlds` writes it). One card (id `office-generate-restyle-<name>:...`); parts kept in `<state_dir>/office/restyle/<name>/parts/`, so asking again buys only what is missing. The pack appears in `<state_dir>/office/worlds/<name>/` only once it loads and every checksum holds; licence names the services, never CC0. Switch to it with `clawtilla office use NAME`. Keys as for characters (`office api-key pixellab|meshy`, or any agent's integration).
