# Configuration: `skills`

GENERATED FILE -- do not edit.  Produced from
`src/config/clawt-config-schema.c` by `make config-files`.

The full documentation of every key in this section.  The index of
all of them is ../config-keys.md.

## `skills`

Reusable procedure, shared across the fleet.

A skill is a directory holding a SKILL.md, in the layout every
supported CLI understands. clawtilla links the ones an agent is
assigned into that agent's workspace at whichever path its provider
actually reads -- the paths differ per provider and are asked of the
library rather than written down here.

### `skills.enabled`

- Type: boolean
- Default: `true`
- Since: 0.2.0

Whether skills are scanned and linked at all.

Off means off rather than empty: nothing is scanned, no directory is
watched, and no link is written into any workspace. Existing links
clawtilla made are left where they are, so turning this off does not
quietly rewrite every agent -- turn it back on, or remove the
assignments, if that is what you meant.

### `skills.dir`

- Type: path
- Default: `~/.clawtilla/skills`
- Since: 0.2.0

Where the fleet's skills live.

One directory per skill, each holding a SKILL.md whose front matter
names it and says when to use it. The directory name and the `name`
must agree, because that name is the traversal gate: no dots, no
slashes, and no way to call a skill `..`.

A skill lives here once and is linked into whichever workspaces need
it, at whatever path that agent's own CLI reads -- which differs per
provider and is asked of the library rather than written down. The
directory is watched, so editing a SKILL.md takes effect without a
reload. It is not created for you: a read with a side effect is how a
typo in this path leaves an empty directory to puzzle over later.

### `skills.extra_dirs`

- Type: list of strings
- Default: `~/.agents/skills`
- Since: 0.9.2

Other directories of skills to read, on the daemon's machine.

~/.agents/skills by default: the directory ai-glib's harnesses, Claude
Code and others share, so a skill installed for them is a skill the
fleet has. Read after skills.dir -- a skill of the same name there
wins -- and never written: removing one is refused, and turning one
on or off is kept under the daemon's state directory. A directory
that does not exist is skipped without a word. An empty list reads
only skills.dir.

### `skills.teach_max_seconds`

- Type: integer
- Default: `900`
- Since: 0.2.0

How long a demonstration may record before it stops itself.

A recording that has been forgotten is worse than one that ended
early, because what it captures is whatever you did next.

The deadline is armed when the recording starts and is not a
request: it fires whether or not anything is still listening, and
it is passed to the compositor as well, so a clawtilla that dies
mid-demonstration does not leave one running. Zero means the
default rather than no limit.

### `skills.teach_max_events`

- Type: integer
- Default: `20000`
- Since: 0.2.0

How many recorded steps a demonstration keeps.

Bounded, and the count of what was dropped is reported rather than
hidden: a silently truncated demonstration teaches half a task.

The earliest steps are the ones kept, because the first part of a
procedure is a usable prefix of it while a slice out of the middle
is not. Pointer motion is never counted: a drag is hundreds of
events and would spend the whole budget on the mouse travelling,
so every click and scroll carries the position it happened at
instead.

### `skills.max_proposals_per_day`

- Type: integer
- Default: `3`
- Since: 0.19.0

How many skills one agent may propose in any 24 hours.

clawtilla_skill_propose lets an agent write down a technique it
worked out -- a new skill, or a change to one that exists -- as a
draft that reaches no prompt, with a decision in your inbox to
approve, edit or reject it. Every proposal spends your attention, so
this bounds how often one agent can ask for it; a rejected proposal
still counts. 0 turns proposing off, and the tool is not offered.
