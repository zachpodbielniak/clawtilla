# Skills and teach mode

A skill is a procedure written once in the library and **symlinked** into each assigned agent's workspace, at whatever path that agent's CLI reads. Editing it edits it for everyone. Teach mode records a task being done and has a model write the skill. Sources: docs/skills.org, docs/teach.org, src/skill/. Keys: [config-keys.md](config-keys.md).

## Layout and front matter

```
<skills.dir>/release/SKILL.md      # plus sibling .md files, tests/<name>.yaml
```

```markdown
---
name: release
description: Use this when cutting a release of a C project here.
---
First run `make clean all` and read the output, not the exit status.
```

| Rule | Consequence |
|---|---|
| `name` == directory name | mismatch refused |
| name: lowercase ASCII, digits, single interior hyphens, no leading/trailing hyphen, <= 64 chars | the traversal gate; percent-decoded before validation |
| `description` required, <= 1024 chars | it is in every assigned agent's context every turn |
| no `<` or `>` in front matter (body is free) | skill reported and not loaded |
| other keys (`allowed-tools`, ...) kept verbatim, enforced by nothing | not a permission boundary |
| `description: >` / `|` blocks read as YAML | since 0.9.2 |

## Where skills come from

| Root | Order | Written by clawtilla? |
|---|---|---|
| `skills.dir` (default `~/.clawtilla/skills`, not created for you) | 1st, wins on name clash | yes (new, import, edit, proposals) |
| `skills.extra_dirs` (default `~/.agents/skills`) | 2nd, earlier dir wins | **never**; rm refused; on/off state under `<state_dir>/skill-settings/<name>/` |
| `<state_dir>/bundled-skills` (embedded from `data/skills/`) | last | rewritten when the bundled digest (`.clawtilla-bundled` marker) changes |

- All paths are **on the daemon's machine**: a remote daemon reads that host's `~/.agents/skills`, not the client's.
- `skills.dir` and extra dirs are watched; no reload needed (`clawtilla skill reload` forces a rescan).
- `skills.enabled: false`: nothing scanned, watched or linked (existing links left).
- A `skills.dir` copy carrying a `.clawtilla-bundled` marker is an old clawtilla's leftover and loses to the bundled root.

### Bundled skills and implied assignment

- `web-research`: implied (origin `integration`) for every agent with a `libresearch` integration, via `clawt_skill_resolve_for_agent()` after the agent/team/fleet lists.
- `clawtilla` (this skill; being added): implied for agents that are chief of staff (`agents.chief_of_staff`, or named by `orchestration.chief_of_staff`) or have `tools.manage_fleet` or `tools.office`. Naming a bundled skill in an explicit list attributes it there instead.

## Linked paths per provider (from ai-glib's registry)

| Provider | Skill link | Command file |
|---|---|---|
| claude-code, claude-tmux | `<ws>/.claude/skills/<name>` | `<ws>/.claude/commands/<name>.md` |
| opencode | `<ws>/.opencode/skill/<name>` | `<ws>/.opencode/command/<name>.md` |
| grok-build | `<ws>/.grok/skills/<name>` | none |
| cursor | `<ws>/.cursor/skills/<name>` | `<ws>/.cursor/commands/<name>.md` |
| antigravity | `<ws>/.agents/skills.json` entries | none |
| codex-cli | nothing written | none |

clawtilla only removes links pointing into its own library; a real directory at a link path is left and reported; dangling links are repaired. Not related to libreclaw's own `skills.dir` (flat `.md`/`.org` files read by libreclaw).

## Assigning

```yaml
defaults:
  skills: [release]          # fleet
teams:
  - id: engineering
    skills: [code-review]
agents:
  - id: builder
    skills: [ci-triage]
```

```bash
clawtilla skill assign release builder          # agent
clawtilla skill assign release team:engineering
clawtilla skill assign release fleet
clawtilla skill unassign release team:engineering
```

Additive; a duplicate counts once, attributed to the most specific list. A name matching no skill is a warning (shown by `skill list`), never silent. The skill must also be **enabled**.

## CLI

```bash
clawtilla skill list                     # every skill, on/off, warnings count
clawtilla skill show <name>              # full text, provenance, scan warnings
clawtilla skill new <name> "<description>"   # writes and opens it; enabled
clawtilla skill import <dir> [origin]    # copied DISABLED, markdown only
clawtilla skill edit <name>              # $EDITOR on SKILL.md
clawtilla skill enable|disable <name>
clawtilla skill rm <name>
clawtilla skill reload
clawtilla skill proposals                # pending agent proposals, in full
clawtilla skill approve <proposal-id>
clawtilla skill reject <proposal-id> [why]
clawtilla skill tests <name>             # saved tests and last result
clawtilla skill test <name> [test] [--agent AGENT]
```

## Security posture

- **Imported and taught/proposed skills land disabled**; skills written with `skill new` are enabled (writing was the review).
- Import copies markdown only; sibling scripts are listed, not copied.
- Provenance: source + SHA-256 pinned at import, compared on every read (warning on drift).
- The scan **warns, never rejects**: 120+ char base64-looking runs, download-piped-into-shell, invisible Unicode (zero-width, bidi overrides), over the whole file including front matter.
- `/name` in a client composer is expanded in the daemon and sends `Use skill: <name>` plus arguments; nothing starts a process (ai-glib `shell: true` pinned off). A hand-written command file is expanded with arguments substituted.

## What the agent sees

`TOOLS.org` gets a clawtilla-owned skills region. Tools, offered only to an agent that has a skill: `clawtilla_skill_list` (current list) and `clawtilla_skill_read` (only skills resolved for it). Writing a skill is not a tool; proposing one is.

## Proposals (`clawtilla_skill_propose`)

Agent supplies `name`, `description`, `body`, `reason`; for a change to an existing skill it passes `skill` instead of `name` (reason required).

- New skill: written via `clawt_skill_library_create_taught()`, **disabled**, `source: taught`, `origin: propose:<id>`.
- Change: staged under `<state_dir>/skill-proposals/<id>/`; live skill untouched. Only skills in `skills.dir` can be changed (extra-dir and bundled refused).
- A decision is filed (id = proposal id `skill-proposal-<uuid>`), options `approve`, `reject`, `edit`; the agent carries on and is messaged when settled.

| Answer | New skill | Change |
|---|---|---|
| approve | enabled | written over live skill (keeps enabled state, other front matter) |
| edit (clients only) | your text, enabled | your text written |
| reject | draft removed (unless you enabled it meanwhile) | discarded |

CLI: `skill approve|reject`, or `clawtilla decision answer <id> approve` (any other answer = reject with your words; `edit` refused). Dismissing the decision rejects. Approval is refused if the draft or live skill changed since proposing (SHA-256 check). Limits: body <= 64 KiB, description <= 1024, reason <= 2000 bytes, UTF-8, one pending draft per skill, `skills.max_proposals_per_day` (default 3 per agent per 24 h, rejected count; 0 = tool withheld). All agent text is escaped (`[clawtilla]` forgeries neutralised) and fenced.

## Skill tests

`tests/<name>.yaml` beside SKILL.md:

```yaml
input: "Write the release notes for 1.2 from this changelog: ..."
agent: scout          # optional
expect: ["## Fixes"]  # each must appear (case-insensitive)
reject: [TODO]        # none may appear
```

Each run uses a **scratch copy** of the named agent (`skilltest-<hex>`, same runtime/model/computer/limits, the skill under test as its only skill), in room `skill-test:<skill>`, removed after. Refused for nonexistent agents, VM agents, and disabled skills. Verdict `passed|failed|error`, recorded against the skill revision. It is a real, billed turn; the check is textual.

## Teach mode

```bash
clawtilla teach list
clawtilla teach start <agent> [kind] [goal]   # kind: agent | host-demo | guest-demo
clawtilla teach stop <id>
clawtilla teach show <id>                     # steps + caveats
clawtilla teach draft <id>                    # model writes SKILL.md (alias: synthesize)
clawtilla teach commit <id>                   # into library, DISABLED, origin teach:<id>
clawtilla teach rm <id>                       # delete recording and frames
```

| Kind | Captures | Needs |
|---|---|---|
| `agent` | the agent's clawtilla tool calls, `clawtilla_computer_exec` commands, desktop actions, frames | nothing (works on `computer.type: none`, no frames) |
| `host-demo` | your real desktop via gowl | `computer.desktop.allow_recording` + gowl `input-recording: true` |
| `guest-demo` | inside the agent's VM via gnome-desktop-mcp | `computer.desktop.allow_recording` + extension `recording-consent` |

- An `agent` trace does **not** include the agent's own CLI bash/read/write (they never reach clawtilla).
- **A demo is a keylogger.** gowl pauses only on lock and on deny-listed app-ids/titles (add patterns with gowl's `input-recording-deny-apps`); a password typed into an ordinary window **is recorded**. The guest pauses on shell password entries but not inside app windows. Review traces as credential material; `teach rm` when done.
- Bounds: `skills.teach_max_seconds` (900, also passed to the compositor), `skills.teach_max_events` (20000, earliest kept, drops counted). Pointer motion is never a step.
- `draft` uses the `ai_assist` provider with only `set_name`, `set_description`, `write_body`, `preview`, `commit` tools (empty executor); the draft is validated before commit and gets the same checks as an import.

## Troubleshooting

Agent lacks a skill: check `enabled`, then that `skill list` shows the assignment reaching it, then the link (`ls -l <workspace>/.claude/skills` for Claude Code). A skill missing from every listing failed to parse (`skill list` prints unreadable directories). A dangling link is skipped silently by harnesses; `skill reload` repairs it. See [agents.md](agents.md), [teams-and-ephemeral.md](teams-and-ephemeral.md), [security.md](security.md).
