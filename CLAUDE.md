# CLAUDE.md - clawtilla

## Build and Test Commands

```bash
# Release build
make

# Debug build
make DEBUG=1

# Clean current build type (leaves deps/libreclaw built -- it is slow)
make clean
make DEBUG=1 clean

# Clean everything including deps/libreclaw
make clean-all

# Run the hermetic test suite
make test
make DEBUG=1 test
make test-verbose

# Tests that need real podman / libvirt / gowl
make test-integration

# Regenerate the config files and docs tables from the schema
make config-files

# Fail on undocumented public API or stale doc references
make docs-check

# Full rebuild
make clean all
make DEBUG=1 ASAN=1 clean all
```

## Build Environment

Builds on the bare host. Every dependency is layered into the Hyacinth Macaw
image, so no container is needed.

`deps/libreclaw` is the only submodule, and it brings its own five
(ai-glib, yaml-glib, podomation, mcp-glib, htmx-glib). Check them out with:

```bash
git submodule update --init --recursive
```

Required Fedora packages. The first block is what libreclaw already needs;
the second is what clawtilla adds:

| pkg-config / command | Fedora package | Needed for |
|---|---|---|
| `glib-2.0`, `gobject-2.0`, `gio-2.0`, `gio-unix-2.0`, `gmodule-2.0` | `glib2-devel` | everything |
| `libsoup-3.0` | `libsoup3-devel` | HTTP, WebSocket |
| `json-glib-1.0` | `json-glib-devel` | wire protocol |
| `libcmark` | `cmark-devel` | markdown rendering |
| `sqlite3` | `sqlite-devel` | mailbox store |
| `libetpan` | `libetpan-devel` | libreclaw's email channel |
| `libpq` | `libpq-devel` | libreclaw's postgres backend |
| `libdex-1` | `libdex-devel` | async futures |
| `yaml-0.1` | `libyaml-devel` | yaml-glib |
| `gtk4` | `gtk4-devel` | GTK client |
| `libadwaita-1` | `libadwaita-devel` | GTK client |
| `libvirt` | `libvirt-devel` | podomation's vm_virtmanager module |
| `virsh`, `libvirtd` | `libvirt-daemon-kvm`, `libvirt-client` | VM computers (runtime) |
| `qemu-system-x86_64` | `qemu-system-x86-core` (+ `qemu-system-aarch64-core` on Asahi) | VM computers (runtime) |
| `qemu-img` | `qemu-img` | VM disk creation (runtime) |
| `virtiofsd` | `virtiofsd` | mount paths into VMs (runtime) |
| `bwrap` | `bubblewrap` | `confine: bwrap` host sandbox (runtime) |
| `podman` | `podman` | container computers (runtime) |
| — | `gobject-introspection-devel` | `.gir`/`.typelib` (auto-skipped if absent) |

Adding a dependency means adding it to `PKG_DEPS` in `config.mk` *and*
layering the package into the image. A package missing from `PKG_DEPS`
makes `pkg-config` fail for the whole list at once, which blanks
`PKG_CFLAGS` and surfaces as a misleading `glib.h: No such file or
directory`.

The GTK client is skipped with a message, not a build failure, when
libadwaita is absent -- everything else here is headless and still useful.

## Build Output

- `make` -> `build/release/` (`-O2 -DNDEBUG`)
- `make DEBUG=1` -> `build/debug/` (`-O0 -g3 -DDEBUG`)
- Both build types coexist.

| Output | What it is |
|---|---|
| `libclawt-1.0.so.0.1.0`, `libclawt-1.0.a` | the library; everything else is a thin client on top |
| `clawtillad` | the daemon |
| `clawtilla` | the CLI |
| `clawtilla-gtk` | the GTK4 client |
| `clawtilla-mcp-server` | serves the orchestration tools to an agent's CLI |
| `clawtilla-web` | the htmx web client |
| `plugins/libclawt-plugin-*.so` | bundled plugins |
| `Clawt-1.0.gir`, `Clawt-1.0.typelib` | introspection data |
| `clawt-version.h`, `clawt-default-config.h` | generated headers |

## Code Style

- C standard: `gnu89` with `gcc`
- Symbol prefix: `clawt_` (types: `Clawt`, macros: `CLAWT_`)
- Library is `libclawt-1.0`; pkg-config name is `clawtilla-1.0`; umbrella
  header is `<clawtilla.h>`; GIR namespace is `Clawt`
- GObject conventions: `G_DECLARE_FINAL_TYPE`, `G_DECLARE_DERIVABLE_TYPE`,
  `G_DEFINE_BOXED_TYPE`, `G_DECLARE_INTERFACE`
- Comments: `/* */` only, never `//`
- GObject Introspection compatible comments on all public API
- License header: `SPDX-License-Identifier: AGPL-3.0-or-later` on every file
- Header guard idiom on every public header:
  ```c
  #if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
  #error "Only <clawtilla.h> can be included directly."
  #endif
  ```
- All boxed types need `G_DEFINE_AUTOPTR_CLEANUP_FUNC` in their header
- All errors use the `CLAWT_ERROR` domain (`src/clawt-error.h`)
- Comments explain *why*, usually by naming the specific failure that
  motivated the code. Match that register -- a comment restating what the
  next line obviously does is worse than none.

## Rules That Are Not Optional

### Zero warnings

`make clean all` and `make clean test` must be warning-free on every target.
Fix warnings in the code; never quiet them by lowering GCC's verbosity.

### The config schema is the single source of truth

`src/config/clawt-config-schema.c` defines every configuration option: its
key, type, default, allowed values and documentation comment. Generated
from it by `make config-files`:

- `data/example-config.yaml`
- `data/default-config.yaml` (what `clawtilla --generate-config` prints)
- `docs/configuration-options.org`

**Adding or changing a config option means editing the schema table and
running `make config-files`, then committing the regenerated files.**
`tests/test-config-schema.c` fails if the checked-in files have drifted, so
forgetting is a test failure rather than a slow-burning documentation lie.

Do not hand-edit the generated files. They are checked in on purpose, so
that the test can compare against them and so a reader browsing the repo
sees real content.

### Docs ship with the code

`docs/*.org` is updated in the same change as the code it describes, not
afterwards. `make docs-check` fails when a public header has an exported
symbol with no doc comment, or when a doc names a config key that no longer
exists.

Docs are org-mode. Never markdown -- this file is the exception, because
the tooling that reads it expects markdown.

### Tests stay hermetic

`make test` must pass with no network, no podman, no libvirt and no
compositor running. Anything needing real infrastructure goes behind
`CLAWT_TEST_INTEGRATION=1` and runs under `make test-integration`.

Tests are `tests/test-*.c`, picked up by wildcard -- adding a file is
enough. Fixtures and golden files live in `tests/fixtures/` and are found
via the `CLAWT_TEST_FIXTURES` define, never by guessing at the cwd.

`tools/clawt-test-floor.sh` fails the suite when suspiciously few test
binaries ran, because a green run that built almost nothing still looks
green. Raise the floor as the suite grows.

## Architecture

```
ClawtDaemon (owns everything; clients hold no transports)
├── ClawtConfig            (clawtilla.yaml -> per-agent libreclaw config.yaml)
├── ClawtAgentManager
│   └── ClawtAgent
│       ├── ClawtAgentRuntime   (process: supervised libreclaw child
│       │                        embedded: lc_app_new_embedded in-process)
│       ├── ClawtComputer       (none | host | container | vm, + desktop add-on)
│       ├── ClawtMailbox        (durable SQLite queue, survives a stopped agent)
│       └── ClawtLink           (bridge-protocol connection to the agent)
├── ClawtRoomManager       (agents are managed as chats)
├── ClawtTaskManager       (delegation, each task its own libreclaw session)
├── ClawtMcpTools          (orchestration tools served to agents over their link)
├── ClawtPluginManager
├── ClawtLinkServer        (agents dial in here)
└── ClawtIpcServer         (CLI / GTK / web dial in here)
```

Message flow: client -> `ClawtIpcServer` -> `ClawtMailboxRouter` (loop
safety applied *before* enqueue) -> the target's `ClawtMailbox` -> drained
into `ClawtLink` when the agent is running -> libreclaw's clawtilla channel
-> the agent.

The daemon owns every agent process, credential and socket. Clients are a
fold over one event stream plus typed commands. Keep it that way: it is
what makes the GTK client, the web client and an in-process cmacs embed all
the same program.

## Key API Notes

### libreclaw

- `lc_app_new_embedded(config_path, main_context, shared_pod_engine)` is the
  supported way to run an agent in-process. `lc_app_run()` on an embedded
  instance returns after setup; it does not enter a main loop.
- `lc_app_new()` does **not** load the config despite its doc comment. It
  strdups the path; errors surface at start time.
- Running several `LcApp`s in one process needs: `port: 0` for dashboard /
  api / bridge / webhook then reading the real port back; distinct
  `session.persist_dir`, `database.*`, `ai.subagent.state_dir`; OTel enabled
  in at most one; and `channels.local` in none of them (it owns fd 0/1).
- `libreclaw-1.0.pc` has an incomplete `Requires:`. We fix it upstream but
  still replicate `PKG_DEPS` in `config.mk` rather than trusting it.
- `liblc-1.0.a` holds only libreclaw's own objects; a static link must also
  pull in all five bundled dep archives.

### libreclaw drives four CLI backends and nothing else

- `lc_provider_type_normalize()` knows `claude-code`, `claude-tmux`,
  `opencode` and `grok-build`. Anything else is **not** an error: it is
  rewritten to `claude-code` with a `g_warning`, so an agent configured
  for `openai` runs Claude Code and is handed `gpt-4o` as a model name.
  The HTTP providers (`claude`, `openai`, `gemini`, `grok`, `ollama`)
  belong to ai-glib and can only be used for the agent *designer*, which
  needs tool calls — the exact set the CLI backends cannot do. The two
  sets are nearly disjoint, which is why `ClawtProviderInfo` carries
  `agent` and `tools` as separate flags and `model.list` reports both.
  `tests/test-model-catalog.c` pins the `agent` column to
  `lc_provider_type_normalize()` rather than to a second copy of the list.

### An IPC handler must not wait on the network — nor may daemon start

- The handler rule below has a corollary that cost a second bug: the
  daemon used to warm the model cache in `clawt_daemon_start()`, which
  called five provider APIs on every start whether or not anyone would
  look, and made `make test` reach the network from every daemon
  fixture. Anything that leaves the machine happens when a client asks
  for it, not because the daemon woke up.

### yaml-glib

- `yaml_parser_get_document()` is transfer-none; use
  `yaml_parser_dup_document()` with `g_autoptr`
- No `YAML_IS_MAPPING()`; use `yaml_node_get_node_type(node) == YAML_NODE_MAPPING`
- No `YAML_MAPPING()` cast; use `yaml_node_get_mapping(node)`
- No `yaml_node_get_value()`; use `yaml_node_get_string(node)`
- `YamlMapping` is a ref-counted boxed type, **not** a GObject; use
  `yaml_mapping_ref`/`unref`

### ai-glib

- Build the agent designer's executor with `ai_tool_executor_new_empty()`,
  never `ai_tool_executor_new()`. The latter silently grants `bash`, `read`,
  `write` and `edit`, and `ai_tool_executor_unregister()` cannot take them
  back because it does not remove built-ins.

### yaml-glib ownership

- `yaml_node_new_mapping()` and `yaml_node_new_sequence()` take their
  argument **(transfer none)** and ref it. `yaml_node_new_mapping(yaml_mapping_new())`
  therefore leaks the caller's ref every time. Pass `NULL` and let the node
  make its own -- that is what the `(nullable)` in the annotation is for.

### sqlite

- Use `sqlite3_close_v2()`, not `sqlite3_close()`. The plain form refuses
  with `SQLITE_BUSY` when anything is still outstanding and leaves the whole
  connection -- page cache included -- allocated.
- `sqlite3_open()` leaves a usable handle **even when it fails** (that is how
  `sqlite3_errmsg()` works on it). Close it before reopening into the same
  pointer, or the connection is stranded for the life of the process.
- sqlite's own globals are released only by `sqlite3_shutdown()`. A test
  binary may call it at the end of `main()`; a library never may, because
  another user of sqlite in the same process would find it shut down
  underneath them.

### An IPC handler must not wait on the network

- Every handler runs on the daemon's main context while the client
  blocks, so a handler that calls out to the internet stalls that client
  for as long as the far end takes. `model.list` asked five provider
  APIs, and both the new-agent dialog and the agent inspector call it on
  every build -- so pressing + or clicking an agent appeared to hang.
  Anything that leaves the machine belongs in a cache warmed
  asynchronously, with the handler answering from it.

### An AI CLI can only be given tools through an MCP config

- ai-glib's CLI clients drop the tool list -- the claude-code client
  casts it to void. So anything built on tool calls (the agent designer)
  cannot use them, and an agent's own session cannot be handed
  clawtilla's tools directly. The route is a real MCP server named in a
  config file the CLI finds for itself: `clawtilla-mcp-server`, named in
  `<workspace>/.mcp.json`, which the CLI discovers in its working
  directory the same way it finds `CLAUDE.md`. Serving the tools over
  the agent's own link -- which is what clawtilla did first -- reaches
  nobody, because nothing on the agent side relays them into the
  session.

### Two things called "memory"

- `agents.memory` is libreclaw's MEMORY.md size budget. `memories.*` is
  the searchable per-agent sqlite store (`ClawtMemoryStore`). They are
  unrelated, and the second is named in the plural precisely because the
  first already exists — check before adding a key under either.

### The schema table's order is the generated file's order

- `clawt-genconfig` walks `clawt-config-schema.c` in order and opens a
  YAML section when it meets one. Inserting a new top-level section in
  the *middle* of another section's keys emits the remaining keys under
  the new section and then reopens the old one — a duplicate top-level
  key, which YAML resolves by silently discarding the first. Put a new
  section after every key belonging to the one before it.
  `tests/test-config-schema.c` catches it, as a warning count, not as
  anything that names the cause.

### An FTS5 query is syntax, not a search string

- A query typed by a person or a model goes straight into FTS5's parser,
  where a stray `"`, a bare `NOT` or an unbalanced `(` is a parse error
  rather than a search for those words — and a failed search reports no
  matches, not an error, so it looks like an empty store. Quote it as a
  phrase literal. Also: joining an FTS5 table brings a second `id` into
  scope, so an unqualified column list fails with "ambiguous column
  name" and, again, silently returns nothing.

### A limit that is never reached is not a limit

- `max_hops` counts how far a message has travelled agent-to-agent, the
  router records it on every delivery, and `ClawtMcpTools` reads it back
  — but an agent's *ordinary reply* arrives through `on_link_message()`,
  which stamped a flat depth of 1. So the one limit built for two agents
  answering each other for ever could never fire on the path where that
  actually happens; two agents traded fifty messages and nothing stopped
  them. Anything that stamps a depth must derive it from
  `clawt_agent_get_hop_depth()`, and a limit needs a test that reaches
  it, not one that shows it exists.

### An agent's mailbox is empty while it is running

- Delivery acknowledges an item the moment it reaches the socket and
  hands it over as an ordinary turn; the mailbox only holds what queued
  while the agent was stopped. So `clawtilla_mailbox_list` is almost
  always empty for a running agent and that means nothing — an agent
  that checked there for a peer's reply concluded, correctly and
  uselessly, that none had come, while the reply was in the turn it had
  just been handed. `clawtilla_room_history` takes an agent id for this
  reason. Anywhere a tool's empty result could be read as an answer, say
  why it is empty.

### An event that cannot say where it happened is not enough

- The `message` event named the sender and the body but not the room, so
  a client had nothing to match a transcript against and the GTK client
  fell back to "is this from the agent I am looking at". A reply from
  that agent to one of its *peers* matched, so two agents talking
  appeared in the user's own chat -- while the message itself had been
  routed correctly all along. It is published from
  `clawt_mailbox_router_send()`, which is the only place that knows the
  room, and its subject is the room. Anything published before routing
  is guessing.

### A client that has just connected is told things twice

- The daemon replays recent events to a subscriber, so a client that
  loads history *and* subscribes receives the messages that history
  already contains. Deduplicate on the message id; do not assume an
  event is new because it arrived. `ClawtWindow` keeps a set of shown
  ids for exactly this.

### An async reader re-arms before it dispatches

- Whatever a completion callback runs may issue a request of its own and
  wait for the answer -- a client refreshing itself when an event arrives
  does exactly that. Re-arming the read *after* dispatching means there is
  no outstanding read while that happens, so nothing can read the reply
  and the nested request blocks until it times out, taking the outer one
  with it. `ClawtClient` re-arms first, and delivers events from an idle
  so application handlers never run inside the reader at all.

### AdwActionRow is not activatable

- libadwaita clears `GtkListBoxRow:activatable` unless an
  `activatable-widget` is set, so `::row-activated` never fires for a
  plain `AdwActionRow` -- clicking an agent in the sidebar moved the
  highlight and did nothing else. Drive a list from `::row-selected`,
  which also covers arrow-key navigation. `AdwSwitchRow` and
  `AdwComboRow` derive from `AdwActionRow`; `AdwEntryRow` and
  `AdwExpanderRow` do **not**, so `adw_action_row_set_subtitle()` on one
  is a runtime assertion, not a compile error.

### Model output never reaches a markup parser

- Agent replies are rendered by `clawt_markdown_to_pango()`, which walks
  cmark's AST and emits Pango markup only for the structure cmark found,
  escaping every literal. Never call `gtk_label_set_markup()` on
  anything an agent wrote, and never add a path that passes its text
  through. Two consequences worth keeping: the emitted tags must stay
  balanced, because Pango refuses unbalanced markup and a GtkLabel that
  cannot parse renders *nothing* — a message would silently vanish, so
  the client validates with `pango_parse_markup()` and falls back to
  plain text; and links are rendered non-clickable on purpose.

### An agent's own tools run on the host, its shell does not

- A container agent's `bash`/`read`/`write` run on the host; only
  `clawtilla_computer_exec` enters the container. So anything handed to
  an agent as a path needs the *host* path if it is meant to be read,
  and the container path if it is meant to be a shell argument. The
  first attachment ever sent gave only the container path: the agent
  stat'd it with exec, confirmed the size, and had no way to open it.
  Give both, host first, and say which is which.

### GtkPicture:can-shrink defaults to TRUE

- Which makes its *minimum* width zero, so anything in the ancestry
  doing a height-for-width pass is free to squeeze it to nothing — an
  inline thumbnail rendered a few dozen pixels wide with no warning and
  nothing wrong with the texture. Turn it off for an image already
  decoded at the size it should be drawn. Leave it on only in a viewer,
  where shrinking to the window is the point.

### GTK has no maximum size

- A size request is a *minimum*, and `GtkPicture` takes its natural size
  from its paintable, so a full-resolution screenshot stays full
  resolution however `can-shrink` and `content-fit` are set. Decode
  thumbnails at thumbnail size (`gdk_pixbuf_new_from_file_at_scale()` →
  `gdk_memory_texture_new()`; `gdk_texture_new_for_pixbuf()` is
  deprecated). And use `SCALE_DOWN` rather than `CONTAIN` in a viewer,
  or a small image is blown up to fill the window and looks like a bug.

### A member in the wrong object is still valid JSON

- `json_builder_set_member_name()` puts a member wherever the builder
  currently is. Adding `busy` to `add_agent_object()` a few lines too
  early landed it inside the nested `credentials` object: the reply
  parsed, nothing warned, and the field simply never reached the client
  looking for it. Count the `begin_object`/`end_object` pairs when
  adding a field, and check the field arrives rather than that the
  request succeeds.

### Adding an agent to the config does not create it

- `clawt_config_add_agent()` plus `clawt_config_save()` leaves the
  manager with no such agent: it builds its agents from a reloaded
  config, so `clawt_daemon_reload()` and `clawt_agent_manager_load()`
  are both required. `agent.create` does this; `agent.import` did not,
  and the import wrote every file correctly and then did not appear in
  `agent list`.

### A widget's children go before its object data does

- GTK unparents a widget's children while destroying it, and object
  data set with `g_object_set_data_full()` is released after that. So a
  `GDestroyNotify` that unparents a popover it had parented to that
  widget runs on a finalized pointer — one `GTK_IS_WIDGET` critical per
  widget, which for a chat transcript means one per message every time
  it is cleared. Hold it with `g_object_add_weak_pointer()` so "already
  gone" reads as NULL rather than as garbage.

### A GtkListBox in a popover fires the moment it opens

- It selects a row when it takes focus, and a popover takes focus as it
  opens — so a right-click menu built from `::row-selected` ran its
  first entry before the person had chosen anything. Context menus are
  plain `GtkButton`s in a box: a button does nothing until it is
  clicked, which is the whole behaviour wanted.

### Two places remember an agent's AI session

- libreclaw restores a session from `session.persist_dir` *or* from its
  sqlite database, so `/reset` has to clear both — clearing one leaves
  the agent resuming from the other and looking like the reset did
  nothing. The daemon links liblc, so it does it through
  `lc_database_remove_session()` rather than by opening libreclaw's
  schema, and only with the agent stopped.

### A popover parented to a GtkEntry stops the window mapping

- Worse than the GtkListBox case below: `gtk_widget_set_parent(popover,
  entry)` left the application running with its main loop turning and
  *no window ever mapped* — nothing on screen, nothing in the log,
  nothing to attach a debugger to. The slash-command list is a
  `GtkRevealer` in the page's own box instead. Reach for a popover
  parented to a leaf widget only if there is no alternative.

### A popover parented to a GtkListBox is one of its children

- `gtk_widget_set_parent(popover, list_box)` makes the popover a child
  widget, so a "remove the first child until there are none" loop asks
  for the same child forever -- `gtk_list_box_remove()` refuses anything
  that is not a `GtkListBoxRow`, and the window hangs before it draws.
  `clear_list()` walks siblings and removes rows only. Unparent the
  popover in `dispose()`, or GTK complains at teardown.

### A refresh that iterates the main context can re-enter

- `clawt_client_request()` iterates the caller's context while it waits,
  and events are delivered from an idle, so an event handler runs in the
  middle of a rebuild and calls the same refresh again. The inner call
  emptied the list and refilled it; the outer call then carried on
  appending from where it was, and the fleet's tail appeared twice.
  Every view that rebuilds a list from a reply goes through
  `refresh_enter()` / `refresh_repeat()`, which bounces the nested call
  and re-runs the body once on the way out so the daemon still has the
  last word.

### Async callbacks own a reference

- Anything handed to `*_async()` must hold a reference to whatever the
  callback will touch. A client dropped from the server's list while a read
  is in flight is freed, and the callback then reads it. `ClawtLink` refs
  itself; the IPC server's `Client` is reference counted for the same
  reason. Both had this bug and both have a regression test.

### Main contexts

- `g_timeout_add_seconds()` and `g_idle_add()` attach to the **default**
  context, not the thread-default one. In an embedded daemon that means the timer never
  fires. Use `g_timeout_source_new_seconds()` + `g_source_attach(source, context)`.
  Three real bugs came from getting this wrong: two timers that never
  fired for an embedded daemon, and an idle that never delivered a single
  event. Use `clawt_timeout_add_seconds()`, or `g_idle_source_new()` plus
  `g_source_attach(source, context)`.

### Unix socket paths

- `sockaddr_un.sun_path` is 108 bytes. A longer path does not fail at bind
  time -- the socket is simply not created where you asked, and the first
  sign is an unrelated ENOENT somewhere else. `clawt_check_socket_path()`
  refuses it up front with the number.

### Tests that use a GSocketService

- Iterate the context after stopping one. Closing a `GSocketListener`
  finishes its outstanding accept on the *next* loop iteration, and until
  that runs the listener, its sources and its sockets are all still
  referenced. A teardown that skips it reports the whole socket stack as
  leaked and buries the leaks that are actually yours.

### GLib enum nicknames

- `g_enum_get_value_by_nick()` is case-insensitive in GLib 2.88 and was not
  always. `clawt_enum_from_nick()` therefore does its own comparison, so
  whether a config file parses does not depend on the GLib version.

### Generated version headers

- A generated version header must list `config.mk` as a prerequisite, not
  just its `.in` template. Otherwise a version bump relinks the library
  under the new soname while the header keeps reporting the old numbers, and
  `CLAWT_CHECK_VERSION` returns FALSE for a version the library genuinely
  is. libreclaw had this bug; both are fixed and both have a regression test.

### Shell scripts

- Prefix `cd` with `CDPATH=` in any script. With `CDPATH` set -- and it is
  set in any shell config offering bare `cd projectname` -- `cd` echoes the
  directory it resolved to, and a command substitution captures that as
  well as `pwd`'s output.

## Things to NEVER Do

- Never hand-edit `data/example-config.yaml` or `data/default-config.yaml`
- Never let the daemon or `libclawt` link GTK
- Never pass `environ` wholesale to a spawned agent -- use the allowlist
- Never write a secret's value into an IPC response, a log line or a transcript
- Never let a plugin load failure take down the daemon
- Never silently downgrade confinement; a missing `bwrap` is a SHADOW agent
  with a reason, not an unconfined one
- Never push to main without approval
- Never leave a generated file naming the same top-level key twice; YAML
  keeps the last and silently discards everything under the first
- Never regenerate an agent's `.mcp.json` wholesale. It is how an agent
  is given MCP servers, so people edit it. Only the `clawtilla` key is
  clawtilla's; read the rest back and write it out untouched, skip the
  write when nothing changed, and move an unparseable file aside rather
  than over it
