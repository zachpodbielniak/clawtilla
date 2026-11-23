# clawtilla MCP tools (2/2): fleet, routines, memories, computers, browser, web, office

Continues [mcp-tools.md](mcp-tools.md), which explains how the tools reach an
agent (`clawtilla-mcp-server` in `<workspace>/.mcp.json`), the gate names
used in the **Gate** column, the hop gate, action classes, the per-turn
rules and how to grant a permission. Registered in the `tools[]` table of
src/mcp/clawt-mcp-tools.c. Config keys: [config-keys.md](config-keys.md).

## Fleet management

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_list_agents` | the fleet: what each agent is for, whether running | -- | none | read |
| `clawtilla_get_agent` | one agent's description, state and abilities | agent_id* | none | read |
| `clawtilla_agent_options` | providers and models, fetched disk images, settable keys | -- | fleet-admin | read |
| `clawtilla_create_agent` | add an agent and (by default) start it; not reversible here | agent_id*, description*, name, purpose, provider, model, computer (none/host/container/vm), container_image, vm_image, settings (key=value lines), start | fleet-admin | fleet_admin |

Call `clawtilla_agent_options` first; it reports what exists. See
[agents.md](agents.md).

## Ephemeral helpers

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_ephemeral_spawn` | spawn a temporary helper running the way you run | description*, purpose, name, provider, model, computer (none/host/container), container_image | ephemeral-spawn | fleet_admin |
| `clawtilla_ephemeral_list` | your helpers: purpose, running, open work | -- | ephemeral-parent | read |
| `clawtilla_ephemeral_reap` | stop a helper and remove its workspace | agent_id*, force | ephemeral-parent | fleet_admin |

`tools.ephemeral` defaults on for the chief of staff, off for others. A
helper never gets more than its parent (no VM, no helpers of its own, no
fleet admin). Hand it work with `clawtilla_delegate`; reap it when done. See
[teams-and-ephemeral.md](teams-and-ephemeral.md).

## Routines, triggers and waits

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_routine_list` | routines that run on you, next run, locked or not | -- | none | read |
| `clawtilla_routine_add` | schedule a check on yourself | id*, instructions*, description, schedule (manual/hourly/daily/weekdays/weekly/custom), at, weekday, cron, enabled, isolate, read_only, catch_up, jitter_seconds, model | schedule | write |
| `clawtilla_routine_update` | change one of your routines (only fields passed) | id*, then as routine_add | schedule | write |
| `clawtilla_routine_remove` | remove one of your routines | id* | schedule | write |
| `clawtilla_trigger_list` | triggers that can wake you (operator-made only) | -- | none | read |
| `clawtilla_wait_until` | be told when a condition holds; a pending wait costs nothing | label*, kind* (url/file/task/computer/command), url, status, contains, path, changed, task_id, command, exit_status, hours, note, turn_room | waits | write |
| `clawtilla_wait_list` | your pending waits and what the last check saw | -- | waits | read |
| `clawtilla_wait_cancel` | stop a pending wait (no notice follows) | wait_id* | waits | delete |

Limits: `tools.schedule_limit`, `tools.schedule_min_minutes`; a routine the
operator marked locked cannot be changed. A `command` wait also needs the
computer. See [routines-and-triggers.md](routines-and-triggers.md).

## Memories and recall

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_memory_add` | record a verified fact, decision or preference | content*, summary, category, importance (low..critical), tags | memory | write |
| `clawtilla_memory_search` | search your, your team's and the fleet's memories | query*, category, limit, agent (needs their `memories.readers`) | memory | read |
| `clawtilla_memory_list` | most recent memories, pinned first | category, pinned_only, limit | memory | read |
| `clawtilla_memory_get` | one memory in full | id* | memory | read |
| `clawtilla_memory_forget` | archive a wrong or stale memory | id* | memory | delete |
| `clawtilla_memory_pin` | keep a memory at the top of listings | id*, pinned | memory | write |
| `clawtilla_recall` | search the transcripts of rooms you are in | query* (a phrase, not syntax), agent, days, limit | recall | read |

Never record what arrived from another agent, a webhook or an import as a
memory. `memories.recall` is a separate grant from `memories.enabled`.

## Skills

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_skill_list` | your assigned skills, as they are now | -- | skills | read |
| `clawtilla_skill_read` | one skill in full; read before working from it | name* | skills | read |
| `clawtilla_skill_propose` | draft a new or changed skill for operator approval | name, skill, description*, body*, reason | skill-proposal | write |

A proposal reaches no prompt until approved; never write the skill file
yourself. See [skills-and-teach.md](skills-and-teach.md).

## Computers, displays and screens

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_computer_exec` | run one program on your computer -- not a shell | command*, working_dir, timeout | computer | exec |
| `clawtilla_computer_state` | what your computer is and what it can reach | -- | computer | read |
| `clawtilla_display_screenshot` | picture of your virtual display, in screen pixels | -- | display | read |
| `clawtilla_display_windows` | windows open on your virtual display | -- | display | read |
| `clawtilla_display_launch` | start a GUI app on the display, same confinement | command* | display-spawn | exec |
| `clawtilla_display_click` | click at a point | x*, y*, button | display-input | desktop_input |
| `clawtilla_display_move` | move the pointer without pressing | x*, y* | display-input | desktop_input |
| `clawtilla_display_type` | type into the focused window | text* | display-input | desktop_input |
| `clawtilla_display_key` | press one key or combo | key* | display-input | desktop_input |
| `clawtilla_display_scroll` | scroll at a point | x*, y*, dy, dx | display-input | desktop_input |
| `clawtilla_request_hands` | ask a person to take the screen; does not wait | reason* | screen | read |
| `clawtilla_request_rebuild` | file a decision asking the operator to rebuild your machine | reason* | rebuild | read |

- An agent's own `bash`, `read` and `write` run on the **host**; only
  `clawtilla_computer_exec` enters a container or VM.
- `computer_exec` takes a command, not a shell line: a line using `|`, `>`,
  `;`, `&&`, `$(...)` or `$VAR` is refused, naming the construct -- wrap it
  in `bash -c "..."`. Globs pass through unexpanded.
- See [computers.md](computers.md).

## Browser

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_browser_open` | open a page in a real browser and wait for it | url* | browser | external |
| `clawtilla_browser_read` | the page's text after scripts ran, with links | max_chars, links | browser | read |
| `clawtilla_browser_html` | HTML of the page or of a selector | selector, max_chars | browser | read |
| `clawtilla_browser_click` | click by CSS selector or at x,y | selector, x, y, button | browser | desktop_input |
| `clawtilla_browser_type` | type into a field, key by key | text*, selector, submit | browser | desktop_input |
| `clawtilla_browser_key` | press one key | key*, modifiers | browser | desktop_input |
| `clawtilla_browser_scroll` | scroll by screens | pages | browser | read |
| `clawtilla_browser_screenshot` | picture of the page, in CSS pixels | -- | browser | read |
| `clawtilla_browser_back` | go back one page | -- | browser | external |
| `clawtilla_browser_release` | let go of a shared browser (lapses after two minutes) | -- | browser | read |
| `clawtilla_browser_eval` | run JavaScript in the page, get JSON back | script* | browser-scripts | exec |
| `clawtilla_secret_ref` | one-use, ten-minute reference to a vault secret | name | browser-secrets | read |
| `clawtilla_browser_fill_secret` | type a secret into a field by its reference | secret*, selector*, submit | browser-secrets | desktop_input |

The agent never sees a secret's value; screenshots and `browser_eval` are
refused on that page until it navigates away. See [security.md](security.md).

## Web research (libresearch)

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_web_search` | metasearch, merged and ranked | query*, category (web/news/code/images/videos), time_range, site, count | web | read |
| `clawtilla_web_fetch` | a page's text, Wayback fallback optional | url*, max_chars, archive | web | read |
| `clawtilla_web_query` | LQL pipeline over search results | query*, explain, max_rows | web | read |
| `clawtilla_web_research` | cited answer (takes minutes) | question*, deep, pages | web | read |
| `clawtilla_web_query_environment` | LQL commands, sources, functions on this server | -- | web | read |

## Connectors

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_mcp_result_query` | read part of a connector result too big to show whole | result_id*, path, search, offset, limit | connector | read |

Connector tools themselves come from their own `clawtilla-*` servers in
`.mcp.json`, not from this table. See [connectors.md](connectors.md).

## Grok bots

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_grok_dispatch` | hand work to a Grok bot, get a job id | bot*, instructions*, context, timeout_minutes | grok | external |
| `clawtilla_grok_jobs` | your Grok jobs: open, answered, what they said | -- | grok | read |
| `clawtilla_grok_report` | bot reports a job done, failed or needing input | job_id*, job_token*, status*, result* | never | external |
| `clawtilla_grok_progress` | bot reports progress on a job | job_id*, job_token*, note* | never | external |
| `clawtilla_grok_assigned` | bot's open jobs | -- | never | read |
| `clawtilla_grok_agents` | agents the bot may talk to | -- | never | read |
| `clawtilla_grok_message` | bot messages an agent | agent*, message* | never | external |
| `clawtilla_grok_replies` | agents' replies not yet sent to the bot | agent | never | read |

The six `never` tools are served only to the Grok bot over its own endpoint
(src/core/daemon-grok.c). See [grok-and-venture.md](grok-and-venture.md).

## The office

| Tool | Does | Key args (* required) | Gate | Class |
|---|---|---|---|---|
| `clawtilla_office_worlds` | world packs: which exist, which is drawn, refusals | -- | office | read |
| `clawtilla_office_world_read` | one file of a pack, with loader warnings | world, path | office | read |
| `clawtilla_office_world_write` | write one pack file; a load-breaking write is undone | world*, path*, content*, from | office | write |
| `clawtilla_office_world_apply` | draw the office in another world, for everybody | world* | office | write |
| `clawtilla_office_preview` | who would sit where, in what pose | world | office | read |
| `clawtilla_office_asset_import` | copy a workspace file into a pack or a character | world or character, source*, path*, license, provenance* | office | write |
| `clawtilla_office_character_layout` | describe an imported sprite sheet's frames | agent*, sheet*, frame_width*, frame_height*, animations*, background | office | write |
| `clawtilla_office_character_mesh` | give a character an imported 3D model | agent*, mesh* | office | write |
| `clawtilla_office_custodians_install` | install the office-custodial team | team | office (+ `tools.manage_fleet`) | fleet_admin |

`tools.office` defaults on for the chief of staff and the custodial team,
off for others, never for a helper. See [office.md](office.md) and
[office-custodial.md](office-custodial.md).
