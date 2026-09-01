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

# Clean every vendored dep, recursively, in both build types
make clean-deps

# The above plus clawtilla's own output: the next `make all` rebuilds
# everything from nothing (about two minutes)
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
| `libxml-2.0` | `libxml2-devel` | ai-glib's HTML-parsing search tools |
| `gtk4` | `gtk4-devel` | GTK client |
| `libadwaita-1` | `libadwaita-devel` | GTK client |
| `libvirt` | `libvirt-devel` | podomation's vm_virtmanager module |
| `virsh`, `libvirtd` | `libvirt-daemon-kvm`, `libvirt-client` | VM computers (runtime) |
| `qemu-system-x86_64` | `qemu-system-x86-core` (+ `qemu-system-aarch64-core` on Asahi) | VM computers (runtime) |
| `qemu-img` | `qemu-img` | VM disk creation (runtime) |
| `virtiofsd` | `virtiofsd` | mount paths into VMs (runtime) |
| `passt` | `passt` | forwarding a port to a libvirt VM's SSH (runtime) |
| `xorrisofs` | `xorriso` | the cloud-init seed that gives a VM a login (runtime) |
| `bwrap` | `bubblewrap` | `confine: bwrap` host sandbox (runtime) |
| `podman` | `podman` | container computers (runtime) |
| `distrobox` | `distrobox` | distrobox computers (runtime) |
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

`tools/clawt-test-litter.sh` fails a **green** run that left a temporary
directory behind. Clean up with `clawt_test_remove_tree()`: `g_rmdir()`
does nothing to a non-empty directory and `g_unlink()` on the file leaves
the directory above it, so a fixture ending in either looks tidy and is
not -- three did, and 2710 directories built up in `/tmp` over six days
of green runs. It compares the same run against itself, because a
*failing* run's directories are evidence and must stay. It sees only this run's
directories: `make test` runs under a private `TMPDIR` and
`g_get_tmp_dir()` honours it, so two worktrees building at once cannot
see each other's fixtures. Sharing `/tmp` made every parallel build a
false failure. A `g_test_add()`
teardown is not the answer: a failed `g_assert` aborts, so no teardown
runs on that path either.

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

## Shapes That Keep Recurring

These are the patterns behind most of the bugs this project has had. Each has
happened at least three times. They generalise; the API notes below do not.

- **Grep for the caller, not the implementation.** A factory nothing calls, a
  signal nothing connects to, a limit nothing increments, a flag with one
  clearer on a path that guarantees the clearer never runs -- each was correct,
  tested in isolation, and reached nobody. When something works standalone and
  not in the product, find the wire before debugging the logic.
- **A rule enforced at one call site is a rule about that call site.** "An IPC
  handler must not wait on the network" was applied five times to five callers
  before it was applied to the function. Fix it where the wait is, not where
  somebody noticed it.
- **A test can encode the defect as the intention.** Three have: `argv[0] ==
  "qemu-system-x86_64"`, a hop depth asserted zero after a reply, an assertion
  that two files are always written when there are three. All were found by
  running the whole suite after an unrelated change, never by reading.
- **Two backends can agree by accident, and the feature gets demonstrated on
  the lucky one.** qemu gives a guest a VGA adapter and libvirt does not;
  Fedora ships the extensions directory and Debian does not; the container pod
  module reads both argument marshallings and distrobox read one. Verify on the
  backend that is the default, not the one that is convenient.
- **A decision recorded with a reason can outlive the reason.** The `ssh_user`
  default rested on a uid-mapping claim that was false, and only booting a
  guest showed it. Re-check the fact, not just the conclusion.
- **Never hand-maintain a list of an option's values or keys.** Walk the schema
  or the library's own enumeration. Every hand-written copy has drifted:
  three key tables, two colour-scheme lists, three integration key lists.
- **A check finds the layer it looks at.** `make parity` reported OK through
  four separate gaps, each at a layer it did not compare. Say what a check
  cannot see rather than implying it is complete.
- **A warning about a mistake a control makes is a bug report about the
  control.** If the answer to a bug report is an instruction, ask whether the
  software should have needed to give it.
- **A technique an agent explains back to you belongs in the software.**
  `DISPLAY=:0 firefox`, `tesseract -- tsv` for click coordinates, `echo >
  /dev/console`: each was discovered by an agent burning a session, and each
  was a missing feature or a missing sentence in a workspace file.
- **A scalar cannot describe N things, and a flag is a count of one.** The
  turn state -- depth, origin, whether the closing text is sent -- was one set
  of fields plus a boolean saying "a delivery set the next turn up", spent by
  the first turn after it. A peer sending five messages produced five turns,
  one described and four looking like turns nothing delivered into: `max_hops`
  unreachable, closed exchanges answered, and `clawtilla_message_user`'s guard
  silently off. Every symptom reported separately; one cause. Count the
  producers before choosing the shape of the state.
- **Saying ok about the wrong thing is worse than saying nothing**, because it
  sends the reader to investigate a different layer.
- **Verify by running it, and check what the harness holds constant.** A
  before/after that loads the same stylesheet in both arms is not a before.
  A synthetic driver removes the platform when the platform is what you are
  testing.
- **A symbol resolved against a rebuilt binary is not evidence.** gdb names a
  frame from whatever file is at that path *now*, and it does not have to be
  the file the process mapped -- one crash was symbolised from a copy at a
  different path, of a build made nine seconds before the fault, in which the
  function had moved 0x20. Rebuild the revision the process actually ran and
  check the address still lands where the trace claims, before diagnosing
  anything from it. Here it did, and the fault address turned out to be the
  answer on its own: an absolute PC equal to a PLT stub's second instruction
  is a GOT slot read without its load bias, which is a mapping that lost its
  relocations, not a logic bug.
- **A tool that reads back what a client just wrote is not a second opinion
  when both read the same field.** `podman inspect` reports a mount's `:z`
  while the container is `created` and reports nothing once it has started,
  so comparing a wedged container with running ones manufactures a
  regression in the code that renders them. Twice now. State-match the
  probe: create, inspect, start, inspect the same container.

### Verifying a fix, without fooling yourself

- `make test` does **not** relink the daemon; plain `make` does **not** relink
  `build/release/tests/*`. Build the specific binary and read the build output,
  not its exit status.
- Hiding a build behind `>/dev/null` makes a revert-proof meaningless: the
  sabotage failed to compile and the old binary passed. Grep the output for
  `error` at minimum. A diagnostics filter matching `file:line:col:` misses
  linker errors entirely.
- Do **not** `git checkout` a file to undo a test mutation -- it discards
  uncommitted work. Copy aside and copy back.
- Read the kernel's answer, not a string: `pgrep -af` matches the probe's own
  command line, `$!` after `setsid` is not the process you started, and
  `pkill -f` kills the invoking shell. Use `ss -ltnp` and `/proc/<pid>/exe`.
- A test that passes five times is not a test that cannot race. Check whether
  the mechanism it depends on actually works.
- A test that can hang is worse than one that fails. Give any nested wait a
  watchdog.

## GLib and GObject

### Main contexts -- six APIs have hit this

`g_timeout_add_seconds()`, `g_idle_add()`, `g_task_new()`,
`g_data_input_stream_read_line_async()`, `g_subprocess_wait_async()` and
`g_socket_listener_accept_async()` all take the **ambient** context. For an
embedded daemon that is a loop nobody runs, and dispatching a source does
**not** push the source's own context. Use `clawt_timeout_add_seconds()`, or
`g_idle_source_new()` + `g_source_attach(source, context)`, and push the
context explicitly around `g_task_new()`.

Capture the context in the function that **attaches the source**, not at its
callers -- naming it at the call site has failed five times. A context captured
only on a success path is NULL on the failure path that needs it.

`GTask` pushes its own context around its callback, which makes some of this
work by luck depending on how the caller was reached. That is not a plan.

**A blocking API taken onto a worker thread must bring its own context.** A
worker started by `g_task_run_in_thread()` has no thread-default, so anything
that reaches for one gets the **global default** -- the loop the window is
running. `clawt_connection_probe()` did, and then called
`g_main_context_iteration()` on it from the worker: with the main thread
holding the context the push failed with a pair of GLib criticals, and with
the main thread idle the acquire *succeeded* and the worker dispatched GTK's
own sources. The second is the dangerous one and it is silent, so the test
asserts the caller's sources were **not** dispatched rather than that no
critical fired.

### Other GLib facts

- `g_task_new()` refs its source object, so it must be a GObject. Carry a
  non-GObject in the task *data* and pass NULL as the source.
- Anything handed to `*_async()` must hold a reference to what the callback
  will touch.
- `g_enum_get_value_by_nick()` **asserts** on a flags type; use
  `clawt_flags_from_nick()`. Its case-insensitivity varies by GLib version, so
  `clawt_enum_from_nick()` does its own comparison.
- `g_ptr_array_copy()` carries the source's element-free-func, so a shallow
  copy owns the elements twice. Copy into a plain `g_ptr_array_new()` to
  reorder.
- `g_date_time_format("%e")` pads with U+2007 FIGURE SPACE, which `g_strstrip`
  cannot reach. Use `%-d`.
- `sockaddr_un.sun_path` is 108 bytes. A longer path does not fail at bind
  time; `clawt_check_socket_path()` refuses it up front.
- `/dev/urandom` or nothing for anything that must be unguessable. `g_random_*`
  is a Mersenne Twister -- a predictable PKCE verifier is no PKCE at all.
- A `*/` inside a C comment ends the comment. Cron expressions with a step
  (`0 */6 * * *`) cannot be written in a block comment.
- A generated version header must list `config.mk` as a prerequisite, not just
  its `.in` template.
- Prefix `cd` with `CDPATH=` in any script, or the resolved directory is echoed
  into a command substitution.
- A shared `SoupSession` cannot carry per-request state. Parking a pointer on it
  with `g_object_set_data()` breaks the moment two requests are in flight,
  which for a room listing is immediately. Allocate a struct per request.

### In tests

- `g_test_expect_message()` makes every **other** message fatal too. Swallow
  warnings with a handler and restore `g_log_set_always_fatal()` around the
  call.
- Iterate the context after stopping a `GSocketService`, or the whole socket
  stack reports as leaked.
- A thread parked in `accept()` outlives an autoptr listener. Cancel and
  `g_thread_join()` before the listener goes.
- `g_setenv()` does **not** reach an agent's child -- the runtime builds the
  environment from an allowlist. Use the agent's own `env:` block and check
  `/proc/<pid>/environ`.
- Point `XDG_DATA_HOME` at a temporary directory in `main()` **before**
  `g_test_init()`; `g_get_user_data_dir()` caches on first use.
- A fixture must pin `daemon.state_dir`, `daemon.socket` **and**
  `defaults.workspace_root`. The last defaults to `~/.clawtilla/agents`, so a
  test that creates an agent scaffolds it into the real fleet.
- A skip-probe is code. One that names no connection started a pod module
  against the machine's own podman, which `make test` must never need.
- The hermetic claim is checkable: `unshare -rn <test-binary>`.

## The Dependency Libraries

### libreclaw

- `lc_app_new_embedded(config_path, main_context, shared_pod_engine)` is the
  supported in-process route. `lc_app_run()` on an embedded instance returns
  after setup.
- `lc_app_new()` does **not** load the config despite its doc comment.
- Several `LcApp`s in one process need: `port: 0` for dashboard/api/bridge/
  webhook then reading the real port back; distinct `session.persist_dir`,
  `database.*`, `ai.subagent.state_dir`; OTel in at most one; `channels.local`
  in none (it owns fd 0/1).
- `libreclaw-1.0.pc` has an incomplete `Requires:`; replicate `PKG_DEPS` in
  `config.mk` rather than trusting it. `liblc-1.0.a` holds only libreclaw's own
  objects -- a static link must also pull in all five bundled dep archives.
- **It drives six CLI backends and nothing else.**
  `lc_provider_type_normalize()` knows `claude-code`, `claude-tmux`, `opencode`,
  `grok-build`, `antigravity` and `cursor`. Anything else is not an error: it is
  rewritten to `claude-code` with a `g_warning`, so an agent configured for
  `openai` runs Claude Code and is handed `gpt-4o` as a model name. The HTTP
  providers belong to ai-glib and can only drive the agent *designer*, which
  needs tool calls. Hence `agent` and `tools` as separate flags on
  `ClawtProviderInfo`.
- **A name added to clawtilla's catalog and not to libreclaw is worse than one
  left out**: the client offers it, the agent is created, and it runs on Claude
  Code holding a model name Claude Code rejects.
  `tests/test-model-catalog.c` pins the `agent` column to
  `lc_provider_type_normalize()` itself, so the two cannot drift.
- `lc_client_factory_matches_type()` answers "is this client that cli_type"
  beside the constructor. LcSession kept its own copy and it had drifted --
  `else return AI_IS_OPENCODE_CLIENT()` meant a grok-build session never
  matched its own client and spawned a fresh one per message, reported by
  nothing.
- It **ignores `database.path`**. The sqlite backend builds the filename from
  `session.persist_dir`, so the database is `<state_dir>/sessions/libreclaw.db`.
  `clawt_usage_database_path()` is the one spelling, used by the renderer,
  `/reset` and the usage reader.
- **It runs one turn per message and never merges them.** `LcSession` holds
  its own `GQueue` and `drain_next_message()` pops a single entry, so a drain
  that hands over five messages is five turns. Anything clawtilla records about
  "the turn" is really about *a message*, and there are as many of those as
  were delivered. Assuming the agent answers a whole drain in one turn is what
  made the turn state a scalar, and it was written into a test as the
  intention.
- `lc_router_resolve_session_key()` keys on channel, room and sender and
  **deliberately excludes the thread**. So a routine cannot have a session of
  its own: it lands in the operator's room, sender and queue. Point routines at
  an agent that is not also a conversational surface.
- It writes one `token_usage` row per AI turn. Read it through
  `lc_database_*`, never by opening its schema. A missing database is zero, not
  an error -- and must not be opened, since opening creates it.
- **Typing rises on `LcSession::turn-started` (the drain), never at enqueue.**
  Under agent mode one session queues several rooms, and anything armed at
  arrival describes turns that have not begun -- the daemon's per-room turn
  state arms on exactly that edge. Any new per-turn side effect in lc-app
  hangs off that signal; `CLAWTILLA_ROOM_ID` and the typing indicator both
  made this mistake at enqueue before being moved.
- Cost comes from the provider (`ai_response_set_cost_micros()`), not the rate
  card: a CLI bills cache reads and writes, which never appear in
  `input_tokens`. `-1` means unknown, never free.

### yaml-glib

- `yaml_parser_dup_document()` with `g_autoptr`; `_get_document()` is
  transfer-none.
- No `YAML_IS_MAPPING()` -- use `yaml_node_get_node_type(n) ==
  YAML_NODE_MAPPING`. No `YAML_MAPPING()` cast -- use
  `yaml_node_get_mapping()`. No `yaml_node_get_value()` -- use
  `yaml_node_get_string()`.
- `YamlMapping` is a ref-counted **boxed** type, not a GObject.
- `yaml_node_new_mapping()` / `_new_sequence()` take their argument
  **(transfer none)** and ref it, so passing a freshly made mapping leaks the
  caller's ref. Pass NULL and let the node make its own.

### ai-glib

- Build the designer's executor with `ai_tool_executor_new_empty()`, never
  `ai_tool_executor_new()` -- the latter silently grants `bash`, `read`,
  `write` and `edit`, and `unregister()` cannot take built-ins back.
- **Its CLI clients drop the tool list.** So an agent's session cannot be
  handed clawtilla's tools directly; the route is a real MCP server named in
  `<workspace>/.mcp.json`, which the CLI discovers the way it finds `CLAUDE.md`.
- `--system-prompt-file` and `--append-system-prompt-file` exist but are **not
  in `claude --help`'s flag list** -- only inside a description paragraph.
  Verified against 2.1.247 with a control.
- A relative `-Wl,-rpath` in a dep's `rules.mk` makes its tests load whatever
  is installed when run from a parent tree. `$ORIGIN/..`.

### htmx-glib

- `HtmxResponse` bodies are bytes (`htmx_response_set_bytes()`). A `gchar *`
  body written with `strlen()` truncates a PNG at its signature's first NUL --
  two bytes.
- `add_header()` **replaces** (right for `Content-Type`);
  `append_header()` for headers whose job is to repeat (`Set-Cookie`). Five
  cookies through the former arrive as one.
- SSE needs `soup_message_headers_set_encoding(SOUP_ENCODING_CHUNKED)` or
  libsoup takes the body length at handler return as Content-Length and
  finishes after one event. An SSE handler must return
  `htmx_response_new_streaming()`, since applying an ordinary response
  overwrites the content type.
- `HtmxSseConnection::closed` reaches the same end three ways; emit it from one
  place.
- `htmx_router_serve_static()` builds a filename from a request path -- the
  containment check is on the **canonical** path and on the separator after the
  root.

### podomation

- **A module connects when its event source starts**, not when it is
  constructed. `vm_virtmanager` opens libvirt in `pod_event_source_start()`, so
  a module only `g_object_new()`d answers every action with "not connected".
  `clawt_pod_bridge_load_module_for()` starts the source and treats a failure
  to start as a failure to load.
- The two modules disagree on the property name: `container` calls it
  `connection-uri`, `vm_virtmanager` calls it `uri`. Check both.
- A handler's arguments arrive as a **positional tuple** from the DSL, padded
  with empty strings, and as `a{sv}` from C. Read both marshallings, and drop
  the padding rather than storing it. The distrobox module read one, so
  `computer.type: distrobox` had never worked at all.
- `PodModule::activate` refuses by default -- override it for a module whose
  daemon is already running.
- The DSL lexer cannot parse a dot in an event name, and every shipped event is
  named `on_something`.
- libvirt's `virEventRemoveHandleFunc` must not call the `ff` callback inline;
  doing so deadlocks the whole process. Fixed upstream by deferring to an idle.
- `lookup_domain()` tries the name then the same string as a UUID, so an absent
  domain logs two errors. Ask `list_domains` first.
- Never sleep uninterruptibly on a thread something has to join -- poll the
  cancellable's fd.

### sqlite

- **`CREATE TABLE IF NOT EXISTS` does nothing to a file that already has the
  table**, so a column added later reaches new databases only. Every mailbox
  in a live fleet then fails its first read and is *quarantined as corrupt* --
  a fleet's queued work moved aside on upgrade. `apply_schema()` is the one
  place both open paths go through; ask `PRAGMA table_info` rather than
  ignoring a failed `ALTER`, which cannot tell an existing column from an
  unwritable file.
- `sqlite3_close_v2()`, not `sqlite3_close()`. The plain form refuses with
  `SQLITE_BUSY` and leaves the connection allocated.
- `sqlite3_open()` leaves a usable handle **even when it fails**. Close it
  before reopening into the same pointer.
- `sqlite3_shutdown()` may be called by a test binary's `main()`; never by a
  library.
- An **FTS5 query is syntax, not a search string**. A stray `"`, a bare `NOT`
  or an unbalanced `(` is a parse error, and a failed search reports no matches
  rather than an error -- indistinguishable from an empty store. Quote it as a
  phrase literal. Joining an FTS5 table also brings a second `id` into scope,
  so an unqualified column list silently returns nothing.

## Computers

### An agent's own tools run on the host; only exec enters the computer

A container or VM agent's `bash`, `read` and `write` run on the **host**. Only
`clawtilla_computer_exec` enters the machine. So a path handed to an agent
needs the host spelling if it is to be read and the guest spelling if it is a
shell argument -- `clawt_computer_describe_mounts()` states both, from one
function.

`computer_exec` takes a **command, not a shell line**: both backends
`g_shell_quote()` each argument, so `>`, `|`, `&&`, `;`, `*` and `$VAR` would
arrive as literal text. The failure was the bad kind -- `echo hi >
/dev/console` exited 0 and printed `hi > /dev/console` to stdout -- so
`clawt_command_shell_syntax_refusal()` now refuses such a line, naming the
construct and `bash -c`. It reads the **raw string**, before
`g_shell_parse_argv()` removes the quotes: after lexing, `grep 'a|b' f` and
`a | b` are the same argv and quoting is the only thing that said which was
meant. Globs are deliberately not refused -- an unquoted `*.log` reaches the
program unchanged, which is sometimes the point. The check runs on the tool
and on `computer.exec`'s **string** form only; a client that sends an `argv`
already means every word literally.

### Lifecycle

- A missing vfunc must **not** answer TRUE. `clawt_computer_teardown()` and
  `_stop()` refuse by default, naming the type; a backend with nothing to
  destroy says so itself. `CALL_OR_TRUE` reported a VM as removed while the
  domain and its disk stayed on disk.
- Stopping a container **destroys** it unless `computer.container.keep`
  (default true). The daemon refuses without an explicit `remove` and both
  clients warn first.
- `clawt_computer_type_has_machine()` says which types have a machine to
  start and stop. A client answering from a list of its own would offer Stop
  on a backend added later.
- Never decide there is nothing to build from state clawtilla remembers. A
  libvirt domain and a qcow2 belong to something else and can be deleted
  without telling us -- ask the hypervisor; provisioning is idempotent.
- `podman` 404 for a container that is not there is **stopped**, not a failure.
- A `relabel` is work podman does at every start: it walks every file under
  the source with `lsetxattr`, and rootless podman cannot relabel files it
  does not own -- so a mount of a system path fails the whole start as a
  libpod 500 naming a syscall and a container id.
  `clawt_mount_list_check_relabel()` refuses it up front on the container
  and distrobox backends, naming the mount and `relabel: none`. It reads
  only the source's own owner; a foreign-owned file deeper in an owned tree
  is still podman's to report. And `relabel: none` on SELinux means visible
  but unreadable inside -- for system content, mount a copy the daemon's
  user owns, or use the agent's host-side tools.
  It also runs in `clawt_daemon_mount_from_payload()`, so **both** add verbs
  refuse it rather than saving it and reporting "takes effect at the next
  start" for something that will never start.
- **A computer is derived from the config, so it goes stale like one.** It is
  built at the agent's first start and kept afterwards -- and it was kept for
  the life of the daemon, so a corrected `clawtilla.yaml`, a `config reload`
  and an `agent mount list` all agreeing meant nothing: every later start
  reused the object built before the edit and reproduced its refusal.
  `agent restart` did not help; only restarting the daemon did.
  `clawt_agent_revalidate()` marks it stale and the next start rebuilds it --
  but **not while the machine is running**, because starting a computer
  provisions it and provisioning a container replaces it, which would turn a
  restart after any `agent set` into destroying whatever the agent installed.

### VMs -- what a guest needs before it is reachable

Each of these on its own produces a VM that boots and admits nobody, which
looks exactly like a VM that failed to boot.

| Needs | Because |
|---|---|
| a disk image | no disk = black console, `Connection reset by peer` on ssh, and a "running" VM in virt-manager |
| a cloud-init seed labelled **`cidata`** | the label is the only thing cloud-init looks for |
| an SSH port chosen on the host and written into the domain XML | a port picked fresh each start leaves the daemon dialling nothing |
| `<portForward>` with `<backend type='passt'/>` | not supported for SLIRP -- the domain is *rejected*, never silently unforwarded |
| a `<video>` device (`virtio`) | with none the guest has no `/dev/dri`, GDM starts nothing, and the serial console is the only console |
| `<cpu mode='host-passthrough'/>` | libvirt's default `qemu64` is x86-64-v1; CentOS Stream 10 panics with `CPU does not support x86-64-v2` |
| an `<emulator>` path | libvirt resolves it from the **session daemon's** PATH, so another package manager ahead of `/usr/bin` gets a binary SELinux refuses to entrypoint |
| a UUID derived from the domain name | libvirt invents one per define and then refuses the name under a different one |

`clawt_vm_emulator_path()` searches `/usr/bin` then PATH and returns **NULL**
when nothing is found, leaving each backend as it was. Naming a path that does
not exist turns a working default into a domain libvirt refuses to define.

Other VM facts:

- `g_subprocess_newv()` succeeding does not mean qemu is running -- it rejects
  its own command line milliseconds later and writes why to a stderr nobody
  reads. Wait for the QMP socket and report qemu's own message.
- A shutdown is a **request**. A hung guest ignores it for ever; wait 30
  seconds then destroy.
- `qemu-img` refuses an image a running guest holds, and **unknown is not
  different** -- reading that as "the base changed" once deleted a running VM's
  disk. `vm_provision()` skips the overlay and seed entirely for a running
  domain.
- A qcow2 overlay pins its base, so changing `computer.vm.image` must rebuild
  it -- loudly, moving the old overlay aside rather than unlinking it.
- `computer.vm.resolution` reaches the virtio GPU as its preferred mode and is
  one of the few VM settings that does **not** need a rebuild.
- A **virtiofs tag is not a path and has 36 bytes.** qemu refuses the device,
  so the domain will not start. `clawt_mount_tag()` is the one spelling, used
  by both the domain XML and the guest's fstab, and always appends a hash.
- `<idmap>` on a `<filesystem>` is a trap: libvirt accepts and stores it, then
  virtiofsd's sandbox cannot set it up (`newuidmap: write to uid_map failed`)
  and **the domain fails to start**. Defining is not starting.
- An unprivileged libvirt session maps the **guest's root** to the host user;
  every other guest id lands in that user's subuid range. So `ssh_user`
  defaults to `root` -- a non-root guest account cannot write to a share or
  read its own workspace. GDM refuses to log root in, so a desktop VM gets a
  second account for the screen, and that account still cannot write to a
  share. Recorded in `docs/computers.org#vm-share-ownership`.
- A share added to an agent **after** its guest booted never arrives: the
  fstab is written by cloud-init at first boot, and hot-plugging gives the
  guest a tag and nothing else. Warned, matched on the tag, and the remedy is
  a rebuild.

### cloud-init

- **It reads its seed once, at first boot.** The login, the desktop, the
  flavour and the package list are fixed for the life of an overlay;
  `computer.rebuild` is the answer, and it is refused while the agent runs.
  cpus, memory and disk size are *not* in the seed and apply at the next boot.
- A `users:` entry **skips an account that already exists** -- and root always
  exists. The top-level `ssh_authorized_keys` is written by a different module
  and does reach root. Neither covers both cases, so the key is emitted twice
  on purpose.
- Cloud-config has **one** top-level `packages:` key. Two would be a duplicate
  YAML resolves by keeping the last, so `computer.vm.packages` is folded into
  the desktop's list when there is a desktop and given a block of its own when
  there is not -- never both.
- **A package it cannot find fails the whole install**, which on a desktop
  guest takes the desktop with it. `gnome-terminal` is absent from CentOS
  Stream 10 (replaced by Ptyxis) and cost an entire desktop. Check every name
  against the distribution's own metadata; EL is not Fedora minus things.
- It picks the package manager and **nothing else**. Names, service units and
  file paths are per family and live in the flavour table: `gdm` vs `gdm3`;
  `python3-gobject` vs `python3-gi`; `dconf`/`glib-compile-schemas` are
  `dconf-cli`/`libglib2.0-bin` on Debian; three spellings of the autologin file
  (`/etc/gdm/custom.conf`, `/etc/gdm3/daemon.conf`, `/etc/gdm3/custom.conf`);
  `firefox` vs `firefox-esr`. Arch needs `package_upgrade` as well as
  `package_update`, and only its `cloudimg` qcow2 has cloud-init at all.
- Match a family on `arch-linux`/`archlinux`, never the substring `arch`.
  Ubuntu's images name only the release adjective, never "ubuntu".
- `runcmd` runs without `set -e` and cannot say which step failed. The guest
  desktop install is one script that writes its result to
  `/var/lib/clawtilla/desktop-install.status`, and checks `gnome-shell` and the
  family's display-manager unit exist before writing `ok`.

### Guest desktops

- A cloud image has no display manager, compositor or graphical target -- the
  seed **builds** one. Four things each break it alone: the extension enabled
  through a dconf system default (`gnome-extensions enable` needs a session);
  consent pre-acknowledged; the lock screen disabled; automation switched on
  from a systemd user unit. `X-GNOME-Autostart-Phase` now *stops* an autostart
  entry running, which silently disabled automation entirely.
- Only Fedora's `gnome-shell` ships
  `/usr/share/gnome-shell/extensions`, and `ln` will not create a parent. A
  dangling symlink enumerates as a symlink and GNOME Shell skips it in silence,
  while GDBus still answers `Peer.Ping` -- so the guest looks perfect and every
  real call fails with "DBus object has no attribute".
- `focused: true` is the **window manager's** focus, not the keyboard's. The
  Activities overview takes the keyboard without changing window focus, and is
  open at login. There is no dconf key to disable it.
- An agent given a screen and a pointer will estimate the coordinate and miss.
  `tesseract <file> - tsv` gives a bounding box per word; the packages are per
  family and language data is always separate. Not on Enterprise Linux -- it is
  in EPEL, which a cloud image does not enable.
- **A share is not a road out of the guest's session.** An unprivileged
  libvirt session maps the guest's *root* to the host user, so the desktop
  account -- which GDM forces to be somebody else -- cannot enter a 0700
  workspace to write, and what it does write lands 0600 under a subuid the
  host cannot read. `docs/computers.org#vm-share-ownership` said exactly
  that, and the live screen was routed through the share anyway: every grab
  came back `Permission denied` on `frame.png.tmp-*`, which reads as the
  compositor being broken. Bytes leave a guest session over the login that
  owns them.
- **cloud-init reads its seed once, so deleting a rule from the seed reaches
  new guests only.** A tmpfiles rule is worse than most, because its whole
  purpose is to be reapplied at every boot. clawtilla wrote that file and
  named it after itself, so clawtilla takes it back -- the `.mcp.json`
  discipline, applied inside a guest. Telling somebody to rebuild a VM to
  undo a link we put there is not a remedy.
- `clawtilla-desktop-run` starts a GUI application inside the session
  (`systemd-run --user` as the session's account). Without it an agent works
  out `DISPLAY=:0 firefox`, which succeeds onto Xwayland instead of the Wayland
  session, and the symptoms surface far away.
- The MCP relay filters `tools/list` and refuses `tools/call` -- it is the only
  place `allow_input` is enforced for a guest desktop, since gnome-desktop-mcp
  offers every tool it has to whoever connects. The `.mcp.json` entry names the
  clawtilla CLI, not ssh, because the guest's port is chosen after the
  workspace files are written.

### Host confinement

- Never silently downgrade confinement. A missing `bwrap` is a SHADOW agent
  with a reason, never an unconfined one.
- `clawt_remove_tree()` takes the root it must stay inside and checks the
  **canonical** path, per child, against that same root.

## Daemon, IPC and Clients

### An IPC handler must not wait on the network -- nor may daemon start

Every handler runs on the daemon's main context while the client blocks.
`model.list` asked five provider APIs, so pressing + appeared to hang; the
daemon warmed that cache in `clawt_daemon_start()`, which made `make test`
reach the network from every fixture. Anything that leaves the machine happens
when a client asks, from a cache warmed asynchronously.

`clawt_ipc_server_defer()` claims the right to answer later;
`clawt_ipc_pending_respond()` sends the frame when the work finishes. The token
holds a reference to the client, so a connection closing mid-flight is still
there to be answered into.

Moving blocking work to an idle fixes **when** it runs, not **where it waits**
-- an idle that calls a blocking start still holds the loop. The wait has to
leave the thread. `clawt_computer_lifecycle_async()` and
`clawt_computer_exec_async()` are the library's one implementation of that;
two would differ exactly once.

The test that proves it asserts on an **unrelated timer still being
dispatched**. A test phrased in terms of the operation cannot tell the two
versions apart.

### Processes

- `prctl(PR_SET_PDEATHSIG, SIGTERM)` in the child's setup function is the only
  thing that survives SIGKILL of the daemon. Without it, agents reparent to
  init still holding ports, session directories and databases, and the next
  daemon starts a second copy alongside each.
- A stop that only sends a signal is not a stop. `agent.restart` found the
  runtime claiming stopped while the child still held the ports, and spawned a
  second libreclaw against the same config. Wait, **iterating the context** the
  exit will arrive on -- the runtime holds that context, since asking for the
  thread-default from the stop's own stack is a different loop.
- `g_subprocess_get_identifier()` returns NULL once reaped, so "pid 0 did not
  stop" means it had already exited. `g_subprocess_get_if_exited()` may only be
  called after the wait returns; read your own flag. `force_exit()` sends
  SIGKILL but reaps nothing, and `kill(pid, 0)` succeeds on a zombie.
- The daemon takes a `flock` on `<state_dir>/daemon.lock` before reading or
  writing anything in there. A **connect probe** answers "did anything reply
  just now", which a busy daemon fails -- and being wrong in the permissive
  direction is an action: it unlinked a live socket and deleted four messages
  of a real conversation. The transcript is a projection; `events/*.ndjson` is
  the record.

### The wire

- **A JsonNode can hold the object *type* and no object.**
  `json_node_new(JSON_NODE_OBJECT)` answers `JSON_NODE_HOLDS_OBJECT()` with
  TRUE and `json_node_get_object()` with NULL, so a type check is not a
  pointer check. Every payload-less reply -- eleven handlers, `agent.start`
  among them -- arrived as one, and the reader `clawt_window_request()` runs
  on *every* reply got a json-glib CRITICAL and its fallback. An empty reply
  is an **empty object**.
- **A duplicate JSON member is silent and the last one wins.** Count members as
  well as braces. And a member in the wrong object is still valid JSON --
  `json_builder_set_member_name()` puts it wherever the builder currently is.
- A client that has just connected is told things twice (history plus replay).
  Deduplicate on the message id.
- An async reader must **re-arm before it dispatches**, and deliver events from
  an idle, or a handler that issues its own request blocks until it times out.
- A subscription is an **intent**, not a request: record it before the frame is
  sent, or a client that subscribed while the socket was down reconnects
  perfectly and receives nothing for ever.
- `resumed: false` means the cursor fell off the replay ring; the client must
  re-read rather than carry a hole.
- Replayed events are not counted as unread -- they keep their timestamps, so
  one comparison against the connect time settles it.
- **An event that fires only on arrival cannot maintain a count.**
  `decision.asked` was published and settling one published nothing, so every
  client's badge could only go up: a second window, the CLI or the venture
  bridge answering left the others drawing an inbox that was already empty --
  and a count that is merely too high reads as work nobody has got to, so
  nothing looked wrong. Publish both ends, or do not let a client cache the
  number.
- An event that cannot say **where** it happened is not enough. `message` is
  published from `clawt_mailbox_router_send()`, the only place that knows the
  room; anything published before routing is guessing.
- A refresh that iterates the main context can re-enter, because events are
  delivered from an idle. Every view that rebuilds a list goes through
  `refresh_enter()` / `refresh_repeat()`.
- **A synchronous request iterates the main context too**, so a click can start
  a newer request inside an older one's wait, and the older reply resumes
  *last*. State a reply writes back must be guarded: take the answer first,
  then compare a generation before touching the view — `show_flow_room()` and
  `clawt_gtk_load_history()` both do. The chat once came back filtered on
  another agent's room from exactly this, silently, until the client
  restarted — with the unread badge counting the messages the pane refused.
- An IPv6 address has no last colon to split on. Use the bracket form; accept a
  bare IPv6 address with no port because it parses whole. Names are refused
  rather than resolved, and port 0 is refused.
- A convenience listener (the tailnet address) must not take daemon start down
  with it: a bind failure there is a warning, an address somebody named is an
  error. Announce what was bound, not what was requested. The address comes
  from `getifaddrs()`, never from running `tailscale ip` -- and 100.64.0.0/10
  is not an octet boundary.

### Orchestration

- **An AI CLI cannot end a turn without writing something**, so a peer message
  answered itself for ever: whatever the model wrote at the end was routed
  back, and "reply only if you have something to say" was advice no agent
  could take. A message carries `invites_reply`; an agent's own reply clears
  it, and a turn started by a cleared one sends nothing. One deliberate
  message earns one answer. The preamble says which of the two the agent is
  holding, because a rule the agent cannot see is a rule it will violate --
  and it names `clawtilla_message_agent` as the way to reach them anyway.
  `max_hops` stays as the backstop for chains of deliberate calls.
- **A drain hands over everything queued at once**, so a per-turn decision
  taken from the last item is taken from the wrong one. Accumulate: a
  question followed by an acknowledgement must still be answerable.
- **A turn is not one message.** The hop depth is dropped at the start of a
  turn no delivery preceded, not after a reply -- a chief that answers its
  operator *and* delegates in one turn sent the second message at depth 1, and
  two agents could sign off at each other for ever. Every field describing
  that turn (depth, origin, whether it replies) arms the same "a delivery set
  this up" flag: one that does not is silently discarded by the next
  `clawt_agent_begin_turn()`.
- An ordinary reply goes back to the room the delegation arrived in.
  `clawtilla_message_user` is **refused** during a peer-started turn: a
  redirect delivers a message written for the operator to somebody else. A
  tool's description is part of its behaviour -- one sentence in
  `clawtilla_message_user`'s sent every agent in a chain into the operator's
  chat.
- A limit needs a test that **reaches** it. `max_hops` could not fire on the
  path where it matters, and `orchestration.task_budget_usd` was checked on
  every message while `record_spend()` was called by nothing outside a test.
- An agent's mailbox is empty while it is running -- delivery acknowledges at
  the socket. Anywhere an empty result could read as an answer, say why it is
  empty.
- **A task stamps itself in seconds**; every other timestamp in the tree --
  events, alerts, messages -- is microseconds, which is what
  `clawt_time_ago_label()` takes. Handing it `clawt_task_get_created_at()` raw
  renders `20694d ago` (the epoch) on every row, and a test asserting only on
  the *shape* of a listing passes throughout. Assert on the value.
- **An agent runs a turn per room, so its turn state is per room.** `LcSession`'s
  `processing` is per session and a session is keyed on room, so one agent
  talking to three peers has three turns that can each be running. As scalars
  on the agent they shared one description and each was judged by whichever
  room wrote last -- and the reply flag is wrong both ways: a real answer
  swallowed because *another* room's turn was closed (silent), or a sign-off
  delivered because another's was not. Deliveries are tagged with their room
  and a turn takes the oldest entry for its own; arrival order and turn-start
  order are independent, so a queue drained in arrival order hands one room's
  turn another's message. The room round-trips -- router → `clawt_link_deliver()`
  → libreclaw echoes it on both the typing frame and the reply -- which is what
  makes this possible at all. A tool call still carries no room (both entry
  points are agent-scoped and `.mcp.json` is per workspace), so the agent-wide
  getters fold across the running turns and each picks the *safe* direction:
  deepest depth, a task id only when they agree, any peer origin. The fold is
  the *fallback*, not the answer: libreclaw sets `CLAWTILLA_ROOM_ID` on the
  session's CLI client, ai-glib applies a client's environment at spawn and
  the CLI spawns per turn, so `clawtilla-mcp-server` inherits it and puts it
  in the `tool.rpc` frame. A peer delivery's preamble also names the room and
  asks for it back as `turn_room` -- believed only while the agent has a turn
  running there, because a turn's description outlives its turn and a
  *finished* room would otherwise answer, buying a lower hop depth. And a turn's
  description outlives its turn, because the indicator drops before the answer
  is posted.
- **A typing indicator is a level, not an edge, and it is per room while the
  turn state is per agent.** libreclaw holds it up for a whole turn and
  re-sends it every 25 seconds (`TYPING_REFRESH_MS`), and an agent in three
  rooms raises three streams of it. Acting on the frame restarted the turn
  every 25 seconds: depth back to 0 so `max_hops` could not climb, the
  closed-exchange flag back to TRUE so sign-offs were delivered and answered,
  the origin cleared so `clawtilla_message_user`'s guard was *off*, the task
  dropped so a late delegation was parentless -- and
  `clawt_turn_watch_begin()` installs a fresh deadline, so the watchdog and
  the room timeout could not be reached by any turn worth watching. 11,869
  frames against 549 turns on a live fleet. `clawt_agent_note_typing()` keeps
  the set of rooms and reports the edge; a set, not a counter, because the
  frames neither are unique nor pair reliably. Whatever else settles a turn
  must clear the set, or the next frame is not a rising edge and the new turn
  runs with the abandoned one's state.
- **A delegated task's parent comes from the delegating agent's turn.**
  `clawtilla_delegate` passed NULL for its whole life, so every agent-created
  task was a root: `clawt_task_manager_create()`'s depth limit compared 0
  against the ceiling however long the chain was, and
  `clawtilla_task_cancel` -- whose description promises "and everything it
  spawned" -- matched on `parent_id` and found none. Two features, both
  correct, both reaching nobody, and the docs recorded the second as intended
  behaviour. `clawt_agent_get_turn_task_id()` is the wire, per delivery beside
  the hop depth. Validate the id against the manager before using it: the turn
  state is only as fresh as the last `clawt_agent_begin_turn()`, which needs a
  typing indicator, which needs a room.
- **A turn ending is not the work finishing.** An AI CLI cannot end a turn
  without writing something, so the last thing an assignee wrote is taken as
  its task's result. That is right for work-answer-done and wrong for every
  assignee that batches -- finish your share, hand the rest on, report once --
  which is what the rest of the guidance asks for. The lifecycle rewarded
  chatter. Three vetoes, all in `clawt_task_manager_complete_on_turn_end()`
  rather than at the call site: still busy, `clawtilla_task_progress` called
  this turn, or children of its own still running. A task that does end this
  way is marked inferred and says so when read, because "they said it was
  done" and "they stopped talking" need different follow-ups.
- **`running` means the assignee started a turn on it**, set at
  `clawt_agent_begin_turn()`. Creating a task says work was handed out and
  delivering it says a mailbox took it; neither says anybody looked, because a
  stopped agent has a full mailbox and does nothing.
- A thread carries more than the answer: progress notes, guardian refusals,
  restart notices. Act only on a message arriving after the typing indicator
  drops. A task that ends late is a delay; one that ends early is a lie.
- **A filter's description is a claim about scope, and a wrong one is answered
  confidently.** `clawtilla_task_list`'s `agent_id` filtered the caller's own
  tasks by counterparty while documenting itself as "tasks involving this
  agent", so asking about a worker two levels down returned "No tasks
  involving kudu" with kudu's task running -- and then blamed a daemon restart,
  which is a plausible wrong cause and sends the reader to a different layer.
  Say what a listing looked at *and* what it did not.
- Team permissions are two gates: the *tool* is offered only to an agent that
  can assign to somebody, and the *target* is checked at call time. A refusal
  about assigning must say that talking is still allowed, or an agent reads it
  as being cut off from its peers.
- Fleet-level mistakes (two leads on one team, an undeclared team, a mount id
  in the wrong list) are **warnings**, not errors. A fleet is edited by hand
  and half-built states are ordinary. But a selector that matches nothing must
  still say so when the entry then reaches nobody.
- Every caller of `render_all_agents()` passes a refusal array; there is no
  wrapper that discards one. `agent.set` wrote to clawtilla.yaml and left the
  agent on its old config.yaml, reporting success.
- An identity change needs the session **cleared**, not a restart: an AI CLI is
  not handed a system prompt when it resumes. A tool list is read at session
  start, so `tools.*` needs only a restart. `agent.set` reports
  `restart_required` and the clients give different advice for each.
- **Every region clawtilla owns in a persona is refreshed on agent start**, by
  `refresh_agent_regions()` -- one function, because the list of them at each
  call site had already drifted: a start refreshed the integrations and
  computer regions and left the tools, the skills and the operator profile to
  `clawt_daemon_render_all_agents_into()`, which a *daemon* start runs and an
  agent one does not. The tools marker said "rewritten on every start"
  throughout, so `agent restart` looked like the way to hand an agent a
  corrected file and was not. Each region compares before it writes, so
  repeating it on both paths costs a read.
- An agent believes its own `TOOLS.org` over `tools/list`, so the daemon writes
  a managed region from `clawt_mcp_tools_describe_for_agent()`, through the
  same permission gate. **That region must be the only list of tools in the
  file.** The scaffold also tabulated them, and the region was added above
  those tables rather than instead of them -- with no marker in the template
  it appended at the *end*, so the stale copy was read first. Both drifted
  again within the year. Anything the scaffold says about tools is now advice
  on when to reach for one, never an enumeration, and
  `tests/test-workspace.c` fails on a `| ~clawtilla_*~` table row appearing in
  a scaffolded file. A workspace made before that keeps its tables, so the
  generated section opens by saying it is the authoritative one -- the only
  fix that reaches a fleet already on disk.
- An agent's persona grows until it cannot start: `MAX_ARG_STRLEN` is 131072
  bytes for a **single** argv word, whatever `ARG_MAX` says.
  `clawt_workspace_measure_identity()` warns before anything fails, and the
  arithmetic is checked against `lc_agent_context_load_identity()` rather than
  reimplemented.

### Config

- The schema table's **order is the generated file's order**. A new top-level
  section inserted mid-way emits the remaining keys under it and then reopens
  the old one -- a duplicate top-level key YAML resolves by discarding the
  first.
- An agent-relative key is not the schema key: `orchestration.mailbox.*` is
  `mailbox.*` inside an agent, `memories.enabled` keeps its whole name.
  `clawt_config_schema_agent_keys()` / `_agent_name()` state that once; three
  private copies made nine settings unreachable from every client.
- Every getter must fall back through the same resolver. Three did not, and
  gave three different wrong answers: a string that worked, an enum that
  returned 0 (`reject` instead of `block-sender`), and a list that never
  consulted the fleet value.
- A setter must **dispatch on `entry->type`**. Writing a list as a scalar is
  accepted, echoed, saved and read back as the default --
  `computer.host.deny_paths` denied nothing.
- A schema default is not a default unless every getter reads it. List defaults
  are comma-separated in the table; one spelling, both readers.
- Two things called "memory": `agents.memory` is libreclaw's MEMORY.md budget,
  `memories.*` is the searchable per-agent store.
- `make docs-check` scans `=key=` markup only. Agent-relative keys are written
  `~computer.vm.image~` throughout the docs, so **every `computer.*` name is
  unchecked**. Widening the scan to tildes produced 18 false positives (frame
  kinds, filenames, tool names), so the gap is recorded rather than
  half-closed.
- It **does** check that every `clawtilla_*` named in `docs/` or `README.org`
  is registered in `clawt-mcp-tools.c`. Four documented tools had never existed.
- It also fails on double-encoded UTF-8, which compiles perfectly and surfaces
  as mojibake a long way from the edit: `\xc2[\x80-\x9f]` is a C1 control and
  `\xc3[\x82\x83]\xc2` is what a two-byte character becomes. Non-ASCII in C
  sources is fine; it is the round trip through a wrong encoding that breaks.
- Adding an agent to the config does not create it. `clawt_config_add_agent()`
  plus `_save()` leaves the manager with no such agent -- `clawt_daemon_reload()`
  and `clawt_agent_manager_load()` are both required.
- A file a person edits gets a **marked region**, not a rewrite. `TOOLS.org`
  has one; `.mcp.json` reserves the `clawtilla-` prefix wholesale, so a revoked
  integration's entry disappears instead of pointing at a server nobody serves.
- Connection profiles and appearance live in the *client's* config
  (`$XDG_CONFIG_HOME/clawtilla/connections.yaml`, 0600), not `clawtilla.yaml` --
  a laptop reaching a workstation may have no fleet at all, and the client
  switches daemons at runtime.
- Resolve a binary the way `clawt-pod-bridge.c` does: beside the running
  binary, then the install location, then PATH -- and name all three when it is
  missing. A key somebody set once and forgot is a config that answers for a
  machine rather than for the software.
- A command-line variable reaches every sub-make. The `deps` target pins
  `DEBUG=0 ASAN=0 UBSAN=0`; pinning one of three knobs left libreclaw's release
  tree built with ASan and unable to build anything afterwards.

### Routines and integrations

- **Cron's oldest oddity is an OR, and only sometimes.** When *both* day fields
  are restricted the match is day-of-month OR day-of-week, so `0 0 13 * 5` is
  the thirteenth *and* every Friday. With one at `*` the other decides. Search
  four years ahead, not one -- `0 0 29 2 *` is legitimate.
- A missed run is not a failed run. `catch_up` runs once however many were
  missed, and a routine that has never run has not missed anything. Run state
  lives beside the config, never in it.
- An integration is configured inline in an agent or as a named instance with a
  scope; everything downstream goes through
  `clawt_integration_resolve_for_agent()` and a `ClawtIntegrationBinding`. Two
  code paths would be two behaviours.
- `one_per_agent` is not a nicety: libreclaw renders one `channels.<type>`
  block per agent, so a second instance has nowhere to go and would look
  configured while receiving nothing. Each type declares `identity_keys` --
  the keys that must differ between agents sharing an instance -- because a
  shared Matrix account means two agents answering as the same person, which
  reads as the fleet misbehaving. An unrecognised `scope` reaches **nobody**,
  not everybody.
- A notifier is correct precisely when nothing happens, which is why
  `integration.notify_test` ignores both the event list and the quiet hours.
  Quiet hours wrap midnight, because people sleep across it.

### Secrets

- A password may cross IPC; a token may not come back. `integration.matrix_login`
  writes the token to a 0600 file and puts the **path** in the reply. There is
  deliberately no `--password` flag -- an argument is in the shell history and
  the process table.
- An argv is world-readable; an environment is not. `ClawtConnectorPlan` is
  tested with a *negative* assertion: the credential must be in `envp` or a
  header and nowhere in `argv`.
- A format string from a config file must never reach `printf`. Validate it
  **and** expand it by hand.
- `expires_in` is a duration and must be stored as an absolute `expires_at`,
  computed when the response arrives. Providers disagree about whether these
  are numbers or strings.
- A device flow's normal case is an **HTTP 400** with `authorization_pending`.
  `slow_down` lengthens the interval permanently.
- Granted scopes must not be written back over requested scopes, or the
  permission erodes on every renewal. Deleting our copy of a token is not
  revocation -- call the endpoint where one exists, say so plainly where none
  does, and drop the local copy either way.
- The same NULL meant "deny everything" (a grant being enforced) and "allow
  everything" (a connector with no `tools:`). `clawt_mcp_relay_run_unfiltered()`
  says which is meant in its name. A relay that does not close the child's
  stdin never exits.
- A refusal names the feature that imposed it, supplied by the caller. The
  relay's own message told a connector-refused agent to enable a setting about
  seeing the screen.

## GTK4 and libadwaita

### Widgets that do not do what they look like they do

- **`AdwActionRow` is not activatable.** libadwaita clears
  `GtkListBoxRow:activatable` unless an `activatable-widget` is set, so
  `::row-activated` never fires. Drive a list from `::row-selected`, which also
  covers arrow keys. And the activatable widget must be a **different** widget:
  `set_activatable_widget(row, row)` recurses to a segfault while looking
  fixed.
- `AdwSwitchRow` and `AdwComboRow` derive from `AdwActionRow`; `AdwEntryRow`
  and `AdwExpanderRow` do **not**, so `adw_action_row_set_subtitle()` on one is
  a runtime assertion.
- `AdwPreferencesGroup` cannot enumerate its own rows -- it wraps each one.
  Keep your own `GPtrArray`.
- `GtkListBox` keeps its own record of its rows; remove through
  `gtk_list_box_remove()`.
- A **`GtkListBox` in a popover fires `::row-selected` the moment it opens**,
  because a popover takes focus. Context menus are plain `GtkButton`s.
- A popover parented to a leaf widget is one of that widget's children: to a
  `GtkListBox` it makes a remove-loop spin for ever; to a `GtkEntry` the window
  **never maps at all** -- no window, no log line, nothing to attach to.
  Unparent it from `::destroy` (dispose), never a `g_object_set_data_full()`
  notify, which runs after `gtk_widget_finalize()` has already complained.
  Hold such a pointer with `g_object_add_weak_pointer()`.
- **A `GtkScrolledWindow` scrolls whatever takes the keyboard focus into view,
  and a list is a focusable widget whose origin is its first row.** So anything
  that hands the focus to the *list* -- rather than to a row -- scrolls to the
  top, and both ways of doing that were in the sidebar. A popover parented to
  the list gives the focus back to its parent when it closes, so every
  right-click sent the fleet to the top as the menu shut; and a rebuild that
  destroys the focused row hands the focus to the list on the way to
  re-selecting, so an event arriving while somebody read the bottom of a long
  sidebar pulled them back. Neither logs anything and both look like a list
  that lost its place. Parent such a popover to the **scroller**, which is
  outside the area it scrolls, and translate the gesture's coordinates with
  `gtk_widget_compute_point()`; park the focus there across a rebuild and give
  it back only to a row that was on screen, because a grab scrolls its row into
  view and a refresh must not move the view.
- `GtkPopoverMenu` follows a submenu model filled **after** it was built, which
  is what lets a menu be refilled per right-click. A stateful `GSimpleAction`
  with a string parameter renders its items as radios and hands the action the
  *target*, so menus are built from ids while the labels are names.
- A `GtkDropTarget` on a row still fires when a button fills the row -- drop
  targets are BUBBLE phase and a `GtkButton` carries no drag controller. Use
  `gtk_list_box_drag_highlight_row()` for the feedback, and unhighlight on the
  **drop** as well as on leave.

### Layout and drawing

- **A wrapping `GtkLabel` still reports the unwrapped string as its natural
  width**, and a `GtkScrolledWindow` left at the default
  `GTK_POLICY_AUTOMATIC` gives its child exactly that. So a long sentence
  makes its row as wide as itself and the whole page scrolls sideways --
  nothing is ellipsised and nothing is logged, so it reads as text that was
  cut off rather than as a page wider than the screen. `GTK_POLICY_NEVER`
  horizontally is what makes the wrap actually happen. The decisions page
  had this; every other `boxed-list` page still does, and is only saved by
  its rows being short.
- **A `GtkListBox` in a `GtkScrolledWindow` fills the viewport** unless its
  alignment says otherwise, so one short row draws a card with eight hundred
  pixels of empty frame under it. `GTK_ALIGN_START`.
- **A size request is a floor, not a cap**, so a child in a box sets the box's
  width. Use an *overlay* child for something that must cost no width (the
  chat gutter's timestamps).
- `GtkPicture:can-shrink` defaults to TRUE, making its minimum width zero. And
  **GTK has no maximum size** -- a size request is a minimum and `GtkPicture`
  takes its natural size from its paintable, so decode thumbnails at thumbnail
  size (`gdk_pixbuf_new_from_file_at_scale()` then `gdk_memory_texture_new()`).
  Use `SCALE_DOWN`, not `CONTAIN`, in a viewer.
- **GTK4 lays out from the frame clock**, not the idle queue. A queued idle
  reads the adjustment before layout, so follow `notify::upper` and
  `notify::page-size` instead. A window nobody is presenting never lays out at
  all -- `mapped` stays TRUE while the frame counter is frozen, so every widget
  looks healthy. A `GtkWidgetPaintable` must exist **before** the frame that
  fills it, and the clock has to actually tick
  (`gdk_frame_clock_get_frame_counter()` in a loop, not
  `g_main_context_iteration()`).

### Styling

- One CSS provider, **reloaded**. `add_provider_for_display()` adds rather than
  replaces, and the oldest wins ties, so fonts appear stuck after one change.
  Verify by asking a realized widget for its resolved font description.
- Pango's `<tt>` is not reachable from GTK CSS -- it resolves through
  fontconfig's monospace alias. `clawt_markdown_to_pango_full()` names the
  family in a `<span>`.
- **Model output never reaches a markup parser.** `clawt_markdown_to_pango()`
  emits markup only for structure cmark found and escapes every literal. Tags
  must stay balanced -- Pango refuses unbalanced markup and a `GtkLabel` that
  cannot parse renders *nothing*, so the client validates with
  `pango_parse_markup()` and falls back to plain text.
- A palette is a list, so check what is **not** on it. libadwaita 1.9 has 51
  colour tokens; recalling them produced 37, and every omitted name keeps stock
  GNOME's value -- one widget in the wrong grey with nothing to say which.
  `required_colors[]` and a test cover the built-in palettes; a partial palette
  on disk is supported and deliberately not warned about. Find new tokens with
  `gresource extract /usr/lib64/libadwaita-1.so.0
  /org/gnome/Adwaita/styles/gtk.css` and grep its `var(--*)`.
- `--active-toggle-bg-color` has no `@define-color` behind it at all, so
  redefining named colours cannot reach it.
- libadwaita styles panes by **position in the widget tree**: a `.sidebar-pane`
  inside a `.content-pane` is painted as a nested sidebar. `.isolated` says
  that a layout decision is not hierarchy.
- The `GtkSettings:gtk-application-prefer-dark-theme` warning comes from the
  user's `settings.ini`, not from clawtilla.
- **A coloured caption at badge size loses its horizontal strokes.** At
  `.caption`, the crossbar of a `T` and the bars of an `F` cover under a whole
  pixel; grayscale antialiasing draws them at partial alpha, and in a pale
  accent colour on a dark row there is no luminance left to spend, so the
  stroke reads as *missing* rather than thin. `HOST` and `CHIEF` did. Whether
  it is visible is a question about the monitor, so it appears on one machine
  and not the next with byte-identical software -- and asking the software what
  it drew will not show it. Render at 1x and magnify with a *point* filter.
  `.clawt-badge` is bold for this reason; `.clawt-unread-badge` and the web
  client's `.badge` already were.
- Emit **no CSS rule at all** for an appearance field somebody left empty.
  "Follow the desktop" and naming the desktop's current font look identical and
  diverge for ever. Clearing a font must reach NULL, not `""` -- an empty
  family emits `font-family: ;`, which is invalid, and GTK drops the whole
  block.

## The Web Client

- **A handler in the page head cannot reach `document.body`** -- it is null
  there, and the throw takes every later handler in the same script with it.
  Listen on `document`.
- htmx swaps with `outerHTML`, so the element a listener was bound to is gone
  after the first arrival, and `scroll` does not bubble -- the delegated
  version must be `capture`. The swap also leaves nothing to restore the scroll
  offset from.
- **A catch-all route swallows everything registered after it.** `/a/:id/:view`
  matches every path under an agent and renders the chat page with a 200, so
  the export came back as `text/html`. Register it last, from its own function.
- `clawt_web_app_last_error()` returns a **borrowed** string that the next call
  frees, and rendering makes several. Copy at the point of failure.
- `clawt_web_add()` **consumes** its child's reference; `htmx_node_add_child()`
  does not. Pairing the first with `g_autoptr` frees the object while it is
  still in the tree, and it renders as nothing -- which looks like a CSS bug.
- Never build an element by appending to a string: the typed classes escape and
  `g_string_append` does not.
- Never fetch anything at page load. The page can drive the whole fleet, and it
  must work on a tailnet with no route out. The htmx script is vendored with
  its checksum in `data/web/README.org`.
- Sanitise a CSS font family with an **allowlist** (letters, digits, space,
  hyphen, underscore, stop, comma). A denylist missed the comment-opener, and a
  CSS comment swallows the rest of the sheet.
- A palette block must come **after** the dark ones: it carries `data-theme`
  too, specificity is identical, and source order decides.
- Assert on `class="composer-inner"`, not the bare class name -- the stylesheet
  is in every page, so the bare form passes against a page that drew nothing.
- A row count cannot describe a layout whose item count changes: a
  `display: none` child is not a grid item at all. Place both children
  explicitly, and use `minmax(0, 1fr)` for a row holding a scrolling box.

## Rules Both Clients Share

- **Never add a capability to one graphical client and not the other.**
  `make parity` compares five layers: IPC frame kinds, slash commands,
  library enumerations, vocabularies, and declared affordances.
- It cannot see a frame kind built with `g_strconcat()` -- name them as
  literals. Its affordance layer reads a corpus with `web-style.c` **excluded**,
  because a class name in the stylesheet is not a capability. Layers 5 needs
  declaring by hand, and cannot catch a feature nobody declared.
- Comments are text too, so a grep-based check strips `/* */` before matching
  -- in **every** layer, not the one where somebody noticed. Layers 4 and 5 did
  and layer 3 did not, so a comment reading "walked out of
  `clawt_section_count()`" reported the enumeration as walked with the loop
  under it deleted, and `clawt_page_count()`'s comment explaining that it
  deliberately has *no* `_nth()` twin made the pair look present -- a real
  failure announced about an API that does not exist. And `cmd | grep -q` under
  `pipefail` fails on success (SIGPIPE, 141). Write to a file and grep the
  file. A parity check that has never been shown to fail is a parity check that
  reports OK.
- A rule both clients apply belongs in the **library**, where it is testable
  without a window or a browser: `clawt_unread_should_count()`,
  `clawt_alert_tier_for_event()`, `clawt_chat_run_is_start()`,
  `clawt_chat_day_label()`, `clawt_chat_time_label()`,
  `clawt_chat_conversation_peer()`, `clawt_transcript_is_at_bottom()`,
  `clawt_mount_sort_scope()`, `clawt_ipc_reply_refusal_text()`,
  `clawt_task_state_tone()`.
- **A colour decided by comparing against a spelled-out nickname is a
  colour nobody checks.** `task_tone()` compared a `ClawtTaskState` against
  `"done"` and `"complete"` -- neither is a nickname the enum produces --
  so a completed task fell through to neutral and had never been drawn
  green, while the GTK client drew every task badge the same grey. A wrong
  colour looks like a design choice, so it reports itself to nobody.
  Resolve the nick to the enum and `switch` with **no `default:`**, and
  walk the enum in the test: `-Wswitch` names an unclassified state, and
  the test fails on a tone the stylesheet does not paint.
- Two row builders for one kind of content drift; fix it by **deleting** one.
- **A switcher does not tell you it ran out of room.** `AdwViewSwitcher` at
  `POLICY_WIDE` neither ellipsises nor overflows: it is clipped, silently, so
  eleven tabs meant the pages somebody could reach depended on the monitor --
  and on the machine it was written on they all fit. Pages are grouped into
  `ClawtSection` (six), each section drawing its own row when it holds more
  than one, walked from the library by both clients. A section's tab carries
  the **sum** of its pages' badges, or a count one level down is invisible
  until somebody opens the section -- which for Decisions defeats the point of
  having one.
- A timestamp rendered on the server is wrong before it arrives -- nothing
  re-renders an unchanged message, so a page left open says "2m ago" for an
  hour. The chat stamps the clock (24-hour: a 12-hour locale does not fit the
  avatar's slot); the lists that are *about* recency keep relative times. A
  message with no timestamp gets no stamp.
- Toasts answer a question somebody is holding right now; a condition the
  window is *in* until something changes is a **banner**, and one that arrived
  on its own goes in the alerts panel. Two of 89 toast call sites were
  notifications.
- **A polled request that fails is a toast per refresh.**
  `clawt_window_request()` toasts every failure, so a handler that reports an
  ordinary condition as an error stacks one bar per tick over the controls
  underneath -- `computer.frame` answered `NOT_FOUND` while a VM booted, once
  a second, above a panel already saying "No frame yet." Fix it where the
  condition is called a failure; `clawt_toast_should_show()` is the backstop
  for the next one, at the single choke point rather than at the 87 call sites.
- A client that loses its daemon must still be able to reach another one. The
  window opens whether or not the first connect succeeds -- a failed local
  daemon must not stand between somebody and the rest.
- **Arrange first, announce second.** `::disconnected` was emitted before the
  retry was scheduled, so `is_reconnecting()` answered FALSE inside the handler
  written to draw it. No test could see it; a polling loop always samples after
  the handler returns.
- A control that cannot represent the current value will silently replace it.
  A combo box gets an unknown value inserted into its list; a menu gets an
  entry for a team nobody declared.
- Grouping and ordering belong to whoever already decides them -- the daemon
  returns the fleet grouped, and a client gathering it itself would be a second
  answer. `agents.order` is in `clawtilla.yaml` because it is about the agents;
  which teams are collapsed is client-side because it is about the person.
- A group with nothing in it still needs its heading when the heading is the
  drop target. And when a sentinel and a real value can both be absent
  (`team: ""` vs no key at all), they need different spellings.
- A drag carries the **id**, never the widget -- a refresh can arrive mid-drag.
  The drop sends the whole list, so one frame describes the arrangement
  completely.

## Things to NEVER Do

- Never hand-edit `data/example-config.yaml`, `data/default-config.yaml` or
  `docs/configuration-options.org` -- run `make config-files`
- Never write a hand-maintained list of an option's keys or values. Walk the
  schema, or the library's own `_count()`/`_nth()` enumeration
- Never state a relationship between two config keys anywhere but the schema
- Never write a config value without dispatching on what the schema says it is
- Never leave a generated file naming the same top-level key twice; YAML keeps
  the last and silently discards everything under the first
- Never add a JSON member whose name is already in that object
- Never decide there is nothing to build from state clawtilla remembers
- Never let a missing vfunc answer TRUE. A missing feature that reports
  failure is a gap; one that reports success is a lie
- Never offer a lifecycle verb for a computer type that cannot honour it --
  ask `clawt_computer_type_has_machine()`
- Never write a distribution's package names, service units or binary locations
  straight into the cloud-init seed; everything but the package manager is per
  family
- Never route what a guest's graphical session writes through the workspace
  share -- the account that writes it and the account that owns the share
  are never the same one
- Never let a libvirt domain name no emulator, no CPU and no video device --
  libvirt fills each in with something wrong
- Never write a config key whose documentation names an event the code does not
  have
- Never silently downgrade confinement; a missing `bwrap` is a SHADOW agent
  with a reason
- Never let the daemon or `libclawt` link GTK
- Never pass `environ` wholesale to a spawned agent -- use the allowlist
- Never set an agent child's environment with `g_setenv()` in a test
- Never write a secret's value into an IPC response, a log line or a
  transcript; never print a bearer token from a listing command; never return a
  secret obtained on a client's behalf to that client
- Never put a connector's credential in `.mcp.json`, an environment, an argv or
  an IPC response
- Never hand a format string that came from a config file to `printf`
- Never store a granted scope list over a requested one, or a blank refresh
  token over a good one
- Never let a plugin load failure take down the daemon
- Never make the tailnet listener mandatory, and `make test` must open no
  network socket at all
- Never decide whether a test can run by touching real infrastructure
- Never let a test fixture take `defaults.workspace_root` from the defaults
- Never regenerate an agent's `.mcp.json` wholesale, or rewrite its org files
  wholesale -- clawtilla owns the `clawtilla-` keys and the marked regions only
- Never scaffold a file the agent will not read
- Never give a pod an action that runs arbitrary code
- Never add a capability to one graphical client and not the other. The
  `make parity` exception map is for decisions, not for silencing the check
- Never build an IPC frame kind with `g_strconcat()` in a client
- Never let an affordance marker in `make parity` be one the stylesheet also
  contains
- Never build an element in the web client by appending to a string
- Never let the web client fetch anything at page load
- Never widen where `clawtilla-web` listens because an address was missing
- Never register a route in the web client after `/a/:id/:view`
- Never pass `clawt_web_app_last_error()` to anything that renders
- Never pair `g_autoptr` with a helper that takes the reference
- Never emit a CSS rule for an appearance field somebody left empty
- Never assert a layout with a track count when the number of items can change
- Never re-arm an async read, or schedule a timer, without naming the context;
  capture it where the source is attached, not where the work succeeded
- Never let a client record that it is subscribed only when the subscribe
  succeeded
- Never describe N deliveries with one set of fields. libreclaw runs a turn per
  message, so a per-turn decision needs a per-message record
- Never judge a message by the agent's turn state when the caller knows which
  room it is for -- an agent can be mid-turn in several at once
- Never create a task without a parent from a path that has one. A flat tree
  makes the depth limit measure nothing and the cancel cascade reach nothing,
  and neither reports that it did
- Never take the end of a turn as the end of the work without asking the task
  whether anything it handed on is still running
- Never treat a repeated signal as an edge. A typing frame, a keepalive and a
  refresh all arrive again while nothing has changed; derive the edge from
  state you keep, and clear that state wherever the thing it describes can
  end another way
- Never lex a command line into an argv and spawn it without saying that the
  shell operators in it did nothing
- Never add a mount to a computer without going through
  `clawt_computer_add_mount()` -- it is where the backend fills in the type,
  and a VM mount left untyped gets a `<filesystem>` device with no fstab entry
- Never let a limit be a float -- a threshold has to have exactly one value
- Never write a test that can hang where it could fail
- Never toast a condition the banner is already holding open
- Never guard a shared resource with a probe when the question is ownership --
  `<state_dir>` is one daemon's at a time, and a `flock` is what says so
- Never write a `switch` over a state enum with a `default:` when the default
  is the permissive answer -- name every value so `-Wswitch` catches the next
  one
- Never write a module handler that reads only one argument marshalling
- Never sleep uninterruptibly on a thread something has to join
- Never let a client decide whether a name is a team
- Never accept a value the same daemon can already prove it will refuse
  later. Refuse it where it arrives, and make sure some client can spell the
  remedy the refusal names
- Never overwrite a loadable module in place. `cp` opens with `O_TRUNC`, and
  truncating a mapped file unmaps its pages from every mapping -- COW pages
  included -- so a running process faults new text back in over an
  unrelocated GOT and dies. Copy to a temporary name and `mv` it into place
- Never let a selector that matches nothing stay silent when the entry it is on
  then reaches nobody
- Never push to master without approval
