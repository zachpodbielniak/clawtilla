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
