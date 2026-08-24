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
| `passt` | `passt` | forwarding a port to a libvirt VM's SSH (runtime) |
| `xorrisofs` | `xorriso` | the cloud-init seed that gives a VM a login (runtime) |
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

### A VM is not reachable just because it booted

- clawtilla ships no disk image and downloads none, and a cloud image
  has no account, no password and no authorised key -- so a VM built
  from one boots perfectly and admits nobody, which looks exactly like a
  VM that failed to boot. `clawt-cloud-init.c` builds a NoCloud seed:
  an ISO that must be labelled `cidata`, because that label is the only
  thing cloud-init looks for. It is acted on at **first boot only**, so
  changing `ssh_user` or the key means deleting the overlay, not
  restarting.
- The address was the other half. Commands run over SSH, `ssh_host` was
  never set by anything, and every exec in a VM's whole existence
  returned "no SSH address" -- while `hostfwd=tcp::0-:22` asked the
  kernel for a port and then discarded the answer. The port is now
  chosen on the host before the VM starts, and **remembered**: a libvirt
  domain is defined with it written into its XML, so picking a new one
  next start leaves the daemon dialling nothing.
- libvirt's `<portForward>` needs `<backend type='passt'/>`. It is not
  supported for the SLIRP backend, where the domain is *rejected* rather
  than merely unforwarded. Missing passt is a warning and an unreachable
  guest, never a silent downgrade.
- The reason both survived: nothing in that path was a pure function.
  `clawt_vm_computer_build_ssh_argv()` exists so the argv can be
  asserted on without a hypervisor, and returns NULL rather than an argv
  that dials nowhere.

### cloud-init installs a `users:` key only while creating the account

- It skips an account that already exists — and root always exists. So
  `users: [{name: root, ssh_authorized_keys: [...]}]` authorises
  **nothing** for the default login: the seed is correct, cloud-init
  reports success, and every connection is refused for a key the guest
  was never told about. The top-level `ssh_authorized_keys` is written
  by a different module and does reach root. Neither covers both cases,
  so the key is emitted twice on purpose.
- This is only findable by booting something. Every specification test —
  the user-data, the ISO label, the domain XML — passed throughout.
  `/computer/vm/boots-and-runs` (needs `CLAWT_TEST_INTEGRATION` and
  `CLAWT_TEST_VM_IMAGE`) is the one that would have caught it.

### A podomation module connects when its event source starts

- Not when it is constructed. `vm_virtmanager` opens libvirt in
  `pod_event_source_start()`, so a module the bridge only
  `g_object_new()`d held a URI and no connection, and answered every
  action — `define_xml` included — with "not connected to libvirt". The
  **default** computer backend could therefore never provision a VM at
  all. `clawt_pod_bridge_load_module_for()` now starts the source and
  treats a failure to start as a failure to load, naming the URI.
- The two modules also disagree on the property name: `container` calls
  it `connection-uri`, `vm_virtmanager` calls it `uri`. Looking for only
  the first meant every VM went to vm_virtmanager's own default of
  `qemu:///system`, whatever `computer.vm.uri` said — and an
  unprivileged user cannot reach that. Check both names.
- Starting the source is safe for `container`: its `start()` spawns the
  event-stream thread and returns TRUE unconditionally.
- `vm_virtmanager` now exposes `undefine`, which it did not for a long
  time — so a test that defines a domain can finally clean up after
  itself, and removing a VM agent removes its domain instead of leaving
  it for somebody to find in virt-manager.

### `g_subprocess_newv()` succeeding does not mean qemu is running

- It reports that the exec happened. qemu rejects its own command line
  milliseconds later, writes why to a stderr nobody reads, and exits —
  and `vm_start()` had already set the state to RUNNING. The first
  symptom was SSH refusing a connection to a port qemu never bound.
  `qemu_came_up()` waits for the QMP socket to appear and otherwise
  reports **qemu's own message**, which is always better than a guess at
  it.
- The failure that exposed this was a QMP socket path over 108 bytes —
  the `sockaddr_un` limit `clawt_check_socket_path()` already exists
  for, and which the VM path was not calling.

### A GNOME session has to be *installed* into a VM, not found

- A cloud image has no display manager, no compositor and no graphical
  target, so `computer.desktop.enabled` on a VM agent does not find a
  desktop -- it builds one, through the cloud-init seed. Four things
  would each on their own leave the agent looking at a desktop it cannot
  touch, and all four are handled: the extension is enabled through a
  **dconf system default** (`gnome-extensions enable` needs a session
  that does not exist yet); consent is pre-acknowledged (the extension
  opens a modal on first enable and nobody is there to dismiss it); a
  lock screen is disabled (nothing in the guest can answer one); and
  automation is switched on from an autostart entry, because the
  extension starts with it off and the only other way is a click on the
  top bar.
- **GDM refuses to log root in**, and `computer.vm.ssh_user` defaults to
  root -- so a VM left at its defaults gets a second account for the
  session while commands still arrive as root. The key is authorised for
  both, because the MCP server talks to GNOME Shell over the session bus
  of whoever is logged in and so has to *run as that account*.
- cloud-init acts on **first boot only**, so every one of these is
  findable exactly once per overlay. Verified by booting a real Fedora
  44 guest twice from scratch.

### A VM with no video device has no desktop, however well it installed

- The libvirt domain XML emitted no `<video>` and no `<graphics>`, so
  the guest had no `/dev/dri` at all. GDM says exactly this — *"It
  appears that your system does not have a primary GPU! Proceeding with
  any GPU"* — and then starts nothing. Every layer above it looks
  healthy: cloud-init reaches `cloud-init.target`, `gdm.service` is
  active, `systemctl get-default` is `graphical.target`, and the console
  sits on the boot log for ever, because with no video device the serial
  console is the only console there is.
- It stayed hidden because the two backends disagreed by accident. qemu
  gives a guest a VGA adapter unless told otherwise, so the qemu backend
  — the one every test and probe used — had a working desktop, while the
  libvirt backend, which is the **default** and builds its devices from
  nothing, had none. A feature verified end to end on real hardware, on
  the wrong backend.
- Both now name the GPU: `<model type='virtio'>` in the XML, `-vga none
  -device virtio-gpu-pci` in the argv. virtio because that is what a
  current GNOME on Wayland wants. A `<graphics>` on loopback comes with
  it, so a person can see whether the desktop came up — the agent does
  not need it, since it screenshots through the extension inside the
  guest.
- Given to **every** VM, not only the ones with a desktop. A guest with
  no display offers only the serial console, so a person looking at it
  cannot tell a broken guest from a working headless one. And
  virt-manager picks the graphical console only if the domain had one
  when the window was first opened — a domain defined without it keeps
  serial, per-VM, which is why the fix looked like it had not worked.

### X-GNOME-Autostart-Phase now stops an autostart entry running

- gnome-session no longer manages session services, so an entry setting
  it is **skipped**, with one line in the journal saying so. The
  automation-enable script therefore never ran, and every desktop tool
  answered "Automation disabled by user. Enable from top bar indicator."
  -- on a machine with no top bar for anyone to click. The D-Bus call
  itself was correct throughout; `gdbus call ... SetEnabled true` by hand
  returned `(true,)` on the same guest. Only the launcher was missing.

### An agent believes its own file over the tool list

- `TOOLS.org` listed the orchestration tools in a table written when the
  workspace was scaffolded, and never again. So a tool granted later did
  not appear in it -- and a chief-of-staff asked whether it could create
  agents read its own file and said no, **on the day the tool was added
  to it**. It was right about the file and wrong about the fleet.
- Two sources of truth for the same question, and the static one wins,
  because it is in the prompt and `tools/list` is a call the model has to
  decide to make. There is now a second managed region,
  `# BEGIN clawtilla tools`, written by the daemon from
  `clawt_mcp_tools_describe_for_agent()` -- which goes through
  `clawt_mcp_tools_is_permitted()` rather than walking the table, so the
  file cannot disagree with the gate.
- Written by the *daemon* and not the config renderer, because it is the
  only thing that knows both an agent's capabilities and its
  permissions. Deriving caps from config in the renderer would have been
  a second implementation of the thing being described.
- The other half was a naming failure: `chief_of_staff` and
  `tools.manage_fleet` are separate settings and the obvious-sounding one
  is not the one that grants the tool. Somebody enabled the first, asked
  their chief for an agent, and was told it had no such tool. Both are
  switches in the editor now, adjacent, with the second saying what it
  actually permits.

### An agent asked to choose will invent, unless told what exists

- `clawtilla_create_agent` is useless without `clawtilla_agent_options`,
  and the reason is on record: the designer could not name a disk image
  -- the ones that exist are the ones somebody fetched -- so `vm` was a
  choice it could never satisfy and had to be refused outright. A
  creation tool has the same problem across providers, models and images
  at once, and every wrong guess produces an agent that looks created and
  does not work.
- So the options tool reports what *exists*: providers whose `agent` flag
  is set (the HTTP ones are for the designer and would silently run as
  Claude Code), their models, images that have finished downloading, and
  every settable key straight from `clawt_config_schema_get()`. The last
  one means `settings` reaches any option without a list in the tool that
  would drift from the schema.
- Creation goes through `daemon_create_agent()`, shared with the
  `agent.create` frame. Two creation paths is exactly how validation got
  skipped before; the field-name translation stays with the caller that
  has the vocabulary and the implementation has none.
- Gated on `tools.manage_fleet`, off by default, **and** on the hook
  being set at all -- a library embedded without a daemon has no fleet to
  add to, and a tool that is listed and then fails teaches an agent to
  keep trying.

### A VM's screen size belongs to the host, not the guest

- `computer.vm.resolution` reaches the virtio GPU as its preferred mode,
  through `<resolution x= y=>` on libvirt and `xres=/yres=` on qemu.
  GNOME takes the preferred mode when there is no `monitors.xml`, so
  there is no per-distribution file to write and nothing to do at first
  boot -- which makes it one of the few VM settings that does *not* need
  the machine rebuilt. qemu's own default is 1280x800, which is why
  screenshots were that size.
- Refused at config validation rather than by the hypervisor, which
  reports a bad one as a domain that will not define: an error about XML,
  a long way from the line somebody typed.

### Writing two of a distribution's file names looks like writing all of them

- `render_autologin()` wrote `/etc/gdm/custom.conf` *and*
  `/etc/gdm3/daemon.conf`, with a comment saying an unread file on each
  costs nothing and saves "the failure where the desktop installs
  perfectly and stops at a login prompt nobody is there to answer". That
  is exactly the failure it then produced: **Ubuntu's gdm3 ships
  `/etc/gdm3/custom.conf` and no `daemon.conf`**, so both files were
  inert. The guest installed a full desktop and stopped at a login prompt
  for an account whose password is deliberately locked -- which nobody,
  and nothing, can get past.
- Three spellings across five families: `/etc/gdm/custom.conf` on Fedora,
  Enterprise Linux and Arch; `/etc/gdm3/daemon.conf` on Debian;
  `/etc/gdm3/custom.conf` on Ubuntu, which kept the upstream name under
  Debian's directory. All four checked against each distribution's own
  package file list rather than recalled.
- The shotgun is what hid it. Two paths *read* as every path, and the
  test asserted both were written -- so it encoded the shotgun as the
  intention and could never have noticed a third. **A test that says "we
  write both" cannot tell you there are three.** The path now sits in the
  flavour table beside the unit name, where a family added later cannot
  leave a hole in its row.

### A virtiofs tag is not a path, and has 36 bytes

- qemu refuses a tag over 36 bytes -- and refuses the **device**, so the
  domain does not start at all: *"tag property must be 36 bytes or
  less"*, about a property nobody set by hand. The tag was the target
  path, and `/mnt/clawtilla/exchange/ubuntu-tester` is 37. Same shape as
  `sockaddr_un.sun_path`: a hard length limit on an identifier derived
  from a path.
- It bit at an agent id of **thirteen characters**. `deb-tester` (34) and
  `arch-tester` (35) both fitted, so the feature looked finished across
  two agents and died on the third name. A near-miss boundary is worse
  than a wide one: it reads as something specific to that agent.
- I noticed the limit while writing the mounts, checked the two constant
  targets against it, and reasoned a long one was somebody else's
  problem -- then made the default mounts include the agent id in the
  target. **A limit needs a test that reaches it, not a moment's
  arithmetic that it exists.** Nothing in the suite rendered a domain
  with a long target, because every fixture used a short id.
- `clawt_mount_tag()` is a pure function and both writers -- the domain
  XML and the guest's fstab -- go through it. Two spellings would differ
  exactly once, and `nofail` would keep the guest quiet about the share
  that never appeared. It is stable for ever for the same reason: fstab
  is written at first boot, the XML on every provision.
- The hash is always appended, not only when the readable half was
  truncated. It is what makes two targets produce two tags, and a branch
  that runs only for long paths is exercised by nobody until it matters.

### Creating a thing and building it were two steps, and one had no button

- A computer is built at agent *start*, never at create -- so `agent.create`
  wrote a config file and a VM agent had no overlay, no seed and no
  domain. `defaults.autostart` does not cover it: it is **false** by
  default and answers whether an agent comes back with the daemon, which
  is a different question from whether the thing somebody just asked for
  exists yet.
- The daemon's own handler said "the client that created it will
  immediately ask to start it" -- a contract nobody implemented. The CLI
  printed `Start it with: ...` as its third line and the GTK client
  toasted "Agent created." and stopped, so one client's users were
  finished and had nothing. **Two clients disagreeing about whether the
  work is done is the daemon's to settle**, not a third place to
  remember.
- `agent.create` and `design.commit` now start what they made, take
  `start: false` to decline, and report `started` / `start_error`. A
  failure to start never undoes the creation -- rolling back because a
  hypervisor was busy discards everything the person typed.
- Found by reading the two clients side by side after a person said a VM
  had not been created. Neither client was wrong on its own.

### `id` pointed into a reply that had already been freed

- The CLI's designer path read `id` out of the `design.commit` reply
  inside a block, let the `g_autoptr(JsonNode)` go at the closing brace,
  and printed `id` three times afterwards. It printed the right thing
  every run, which is how a use-after-free survives being looked at.
  Anything from `member_or()` and its kin borrows from the node -- the
  node has to outlive every use, not just the assignment.

### A synthetic event stamped in the future makes the next call arrive in the past

- gnome-desktop-mcp's input entry points each started from
  `GLib.get_monotonic_time()` and stepped 10ms per event, while returning
  as soon as the events were *queued* rather than when they were meant to
  have happened. A burst was therefore stamped up to 100ms ahead, and the
  next call -- microseconds later in real time -- began again behind it.
  Measured against the old arithmetic: typing started **30ms before the
  clear that preceded it had finished**. So Ctrl+A and Delete landed
  after the characters meant to replace them, the field kept its
  contents, and the result was `emacsemacs`, or one leftover character in
  front of a clean `emacs`.
- Reported as `type_text` dropping and duplicating characters, which is
  what it looks like from outside and is not where the fault was. No
  character was ever lost; they arrived in an order that made the edit
  before them a no-op. Anything that fabricates timestamps needs *one*
  clock across every entry point -- pointer and keyboard alike, since a
  click and the typing after it order against each other too.
- It also built a fresh `ClutterVirtualInputDevice` per call and dropped
  it. Creating one adds it to the seat and dropping it takes it away, so
  a device per keystroke churned seat capabilities under every client --
  and GJS decides when the discarded one is finalised, which is not
  necessarily after mutter has finished with the events it queued.
- The rule now lives in `clock.js` as a pure function, so it can be
  exercised without a running GNOME Shell. That is the whole reason the
  before/after was measurable at all.

### An agent given a screen and a pointer will estimate the coordinate

- And miss by a few dozen pixels, and conclude the pointer tools are
  broken. A screen is an image; a click needs a number, and nothing in
  the tool surface bridges the two. An agent worked out
  `tesseract <file> - tsv` -- a bounding box per word, click the centre
  of the right box -- by trial and error over a session, and **reported
  it as a discovery**. Same signal as the `/dev/console` case and the
  `DISPLAY=:0` one: a technique an agent explains back to you belonged in
  the workspace files.
- The packages are per family and the language data is always separate:
  `tesseract` + `tesseract-langpack-eng` on Fedora, `tesseract-ocr` +
  `tesseract-ocr-eng` on Debian and Ubuntu, `tesseract` +
  `tesseract-data-eng` on Arch. All four verified against the real
  archives rather than recalled.
- **Not** on Enterprise Linux: it is in EPEL, which a cloud image does
  not enable, and cloud-init treats a package it cannot find as a failure
  of the whole install -- so naming it would take the desktop with it.

### A share the guest never mounts, and a path the agent cannot open

- Two halves of one gap, and together they cost a session. The domain XML
  emitted a `<filesystem>` device, which hands the guest a **tag** and
  nothing else -- nothing wrote an fstab entry, so every VM share was a
  device the guest never used. And `describe()` named only the path
  *inside*, while an agent's `read`/`write`/`bash` run on the **host**.
  So an agent looked for a shared file at the guest's path, on the host,
  found nothing, created the directory by hand to explain the absence to
  itself, and reported the whole feature missing. The host side was
  correct throughout.
- The screenshot case is the sharp end: gnome-desktop-mcp writes to
  `/tmp/gnome-mcp` in the guest and returns that path. The capture was
  perfect and unreachable, which is indistinguishable from a capture that
  failed -- so an agent spent a session reasoning from window titles
  instead of looking at the screen. `/tmp/gnome-mcp` is now a tmpfiles
  symlink into the workspace share (tmpfiles, not the installer: `/tmp`
  is a tmpfs and the link has to survive a reboot).
- Every computer now gets the agent's **workspace** as well as the
  exchange, and `clawt_computer_describe_mounts()` states both names --
  `host path = the path inside` -- for both backends, from one function,
  because two copies of that sentence would drift.
- The qemu backend emitted `-chardev socket,path=...`, which *connects*
  as a client, with no virtiofsd ever started to listen -- so qemu exited
  before the guest existed. Harmless while nothing had a mount by
  default; the moment the workspace became one it would have stopped
  every VM on that backend. It warns and carries on without the share.
- `g_ptr_array_copy()` carries the source's element-free-func across, so
  a shallow copy of an array that owns its elements owns them twice. The
  sort in the fstab renderer freed the caller's mounts; the second call
  in the same process died in malloc. Copy into a plain `g_ptr_array_new()`
  when you only want to reorder.

### The workaround an agent invents for a missing feature succeeds

- `clawtilla_computer_exec` is an SSH connection with no session
  environment, and nothing offered a way to start a GUI application in
  the guest's session -- the guest desktop's extension has no spawn
  method, and `computer.desktop.allow_spawn` guards gowl's tools, which
  a VM does not have. So an agent asked to open a browser arrived at
  `DISPLAY=:0 firefox` by itself, and **it worked**: a window appeared,
  the process ran, and the application had been put on Xwayland instead
  of in the Wayland session the desktop was built for. Different
  compositing, different route for synthetic input, and nothing an agent
  can query to say which of the two it got. The symptoms surfaced far
  away -- screenshots of that one application stale while GNOME's own
  panel updated, Return not committing in its address bar.
- `clawtilla-desktop-run` takes the environment from the session rather
  than guessing it: it becomes the session's account (root cannot use
  its Wayland socket) and starts the application with `systemd-run
  --user`, under the manager gnome-session has already told about the
  session. Verified on a real GNOME Wayland session by having the
  launched process report its own environment: `XDG_SESSION_TYPE=wayland`
  and a real `WAYLAND_DISPLAY`.
- A missing feature an agent can work around is worse than one it
  cannot, because the workaround is never reported as a problem. It was
  reported as a *discovery*, which is the same signal as the
  `/dev/console` case: when an agent explains a technique it worked out,
  that technique belonged in the workspace files.

### `focused: true` is the window manager's focus, not the keyboard's

- gnome-desktop-mcp reports `focused` from `win.has_focus()`. GNOME's
  Activities overview takes the **keyboard** without changing window
  focus, so a window reads `focused: true` while every keystroke goes to
  the overview's search entry -- and the overview is open at login,
  before anything has a window, so the first application an agent starts
  opens behind it. An agent typed a URL into the search box for a good
  part of a session; the one field that would have told it said the
  opposite, and no tool reports the shell's grab at all.
- There is no dconf key for it: `org.gnome.shell` on GNOME 50.4 has
  `welcome-dialog-last-shown-version` and nothing about the overview, so
  it cannot be turned off in the seed. The description tells the agent to
  send Escape before typing into a freshly focused window, and only when
  it has `allow_input` -- advice an observe-only agent cannot act on is
  noise in a prompt.

### Enabling an extension and installing it are two steps that fail apart

- The seed enables the GNOME Shell extension through a dconf default and
  links the checkout into `/usr/share/gnome-shell/extensions`. Only
  **Fedora's** `gnome-shell` package ships that directory, and `ln` will
  not create a parent -- so on Debian and Arch the link failed with
  ENOENT, cloud-init's `runcmd` (which runs without `set -e`) carried on
  through the venv and the pip install, and dconf went on to enable an
  extension that was not on disk. The guest looked perfect from the
  host: GDM active, a GNOME console, `Ping` answered on the bus --
  because GDBus answers `Peer.Ping` at the *connection* level for any
  object path as long as the name is owned. Every real call returned
  "DBus object has no attribute", which names nothing.
- Verified end to end on Fedora 44 throughout, which is the whole
  lesson: the one distribution whose `gnome-shell` happens to ship the
  directory is the one where the missing step could not be seen. Same
  shape as the missing `<video>` device -- two backends agreeing by
  accident, and the feature demonstrated on the lucky one.
- The install is now one script rather than a list of `runcmd` entries,
  because `runcmd` cannot stop at a failure and cannot say which step it
  was. It creates the directory, links, tests *through* the link (a
  dangling symlink enumerates as a symlink, not a directory, and GNOME
  Shell skips it in silence), and writes its result to
  `/var/lib/clawtilla/desktop-install.status`.
- Two agents each spent a long turn arriving at this from the bus --
  reading dconf, listing extensions, introspecting `org.gnome.Shell`.
  The answer was one line the guest could have written down. The agent's
  own desktop description now names that file, because a tool error that
  names nothing is an invitation to investigate the wrong layer.
- `tests/test-guest-desktop.c` asserts the `mkdir` *precedes* the `ln`,
  and that the installer is byte-identical across all five families:
  everything distribution-specific is a package name chosen before it
  runs, so a step that varied per family would be a step exercised on
  one distribution and not the others.

### A dependency range belongs to the thing being installed

- gnome-desktop-mcp asked for `mcp>=1.0.0` and imports
  `mcp.server.fastmcp`, which exists only from 1.2.0 to 2.0.0 --
  `mcp.server.mcpserver` replaced it. So the floor admitted versions
  that never had it, and the missing ceiling let a resolver take the
  2.x that removed it. pip resolves, installs cleanly, reports success,
  and the server dies on its first import: the clone works, the venv
  works, and the only symptom is an MCP server that exits the moment a
  client speaks to it.
- clawtilla pinned `mcp<2` at install time for exactly as long as it
  took to fix the range upstream, and then stopped. The guest install
  now names no versions at all, because a copy of somebody else's
  dependency ranges goes stale silently and in the wrong direction --
  `computer.vm.desktop.mcp_repo` has to be a checkout whose pyproject is
  honest, which is the ordinary contract for installing anything.
  `tests/test-guest-desktop.c` asserts that no constraint is emitted.

### An AI CLI reaches a guest's MCP server over ssh, and ssh alone is not enough

- SSH already carries stdio, so the transport is just
  `ssh ... clawtilla-desktop-mcp` with no protocol translation -- but the
  `.mcp.json` entry names the clawtilla CLI rather than ssh, for two
  reasons. The port that reaches the guest is chosen when the VM is
  provisioned, which is *after* the workspace files are written, so a
  command line captured at render time names a port nothing listens on.
  And gnome-desktop-mcp offers every tool it has to whoever connects: it
  has never heard of `allow_input`, so a bare ssh entry hands an
  observe-only agent the ability to type. `clawt-desktop-relay.c` filters
  `tools/list` and refuses `tools/call`, which is the only place that
  grant is enforced for a guest desktop.
- `clawtilla-desktop` is the second and last key clawtilla owns in that
  file, and it is *removed* when the grant is revoked -- an entry left
  behind starts an ssh to a VM that is not there.

### A schema default that no getter reads is not a default

- `clawt_agent_config_get_string_list()` was the only getter that did not
  fall back to the schema, so the first STRING_LIST default ever declared
  was generated into `data/*.yaml`, documented in
  `docs/configuration-options.org`, and never once handed to the code
  that asked for it. It also rendered as `packages:gdm,gnome-shell`,
  which is not YAML at all -- `append_value()` is not told what column it
  is writing in, so a list default is emitted in flow style. Defaults are
  comma-separated in the table; one spelling, both readers.

### A factory function nothing calls is not a feature

- `clawt_computer_factory_create_desktop()` existed, was correct, had a
  permission model and a tool list -- and no caller. `computer.desktop`
  therefore set a caps flag and did nothing else for the whole life of
  the feature. Same shape as the designer's pinning: two tested halves
  with no wire between them. When something works in isolation and not in
  the product, grep for the caller before debugging the logic.

### A missing vfunc must not answer TRUE

- `clawt_computer_teardown()` went through `CALL_OR_TRUE`, which returns
  TRUE when the vfunc is NULL — and `ClawtVmComputer` had no teardown at
  all. So `agent rm --remove-computer` on a VM took the success branch,
  recorded the computer as "removed", and removed nothing: the libvirt
  domain stayed defined and the disk stayed on disk. A missing feature
  that reports failure is a gap; one that reports success is a lie, and
  the person only found out by opening virt-manager and seeing a VM that
  should not have been there.
- The default is now a refusal naming the computer type. The two
  backends with genuinely nothing to destroy — null and host — say so in
  two lines each, so a new backend inherits the refusal rather than the
  lie. `tests/test-computer.c` asserts that every backend states its own
  answer rather than relying on a default.
- Teardown takes the guest's **disk** with the domain. Keeping it left an
  agent's whole machine behind under a name nothing referred to, and a
  later agent with the same id silently adopted it instead of starting
  clean. Only files clawtilla wrote are removed, and the state directory
  goes only if it is then empty — `g_rmdir` refuses a directory with
  anything else in it, which is what keeps a file somebody put there by
  hand.

### A stop that only sends a signal is not a stop

- `process_runtime_stop()` sent SIGTERM and cleared `running` in the next
  line, without waiting. `agent.restart` is a stop immediately followed
  by a start, so it found the runtime claiming to be stopped while the
  child was still there and spawned a **second** libreclaw against the
  same config — same ports, same `session.persist_dir`, same database,
  which is the multi-instance collision libreclaw cannot survive. The new
  one exited at once, the daemon reported the agent "stopped - exited
  with status 0", and the original ran on tracked by nothing.
- It now waits, **iterating the context** rather than sleeping — the exit
  arrives on it, so a plain sleep would burn the whole grace period every
  time and then kill a child that had gone in milliseconds — and force-
  exits anything still there after 5 seconds. `tests/fixtures/stubborn-
  libreclaw` traps SIGTERM, which is the case that exposed it.
- `is_alive()` asked `g_subprocess_get_if_exited()`, which may only be
  called after the wait has returned; asking about a live child is
  undefined, not merely wrong. It reads our own flag now, set only by the
  exit callback.

### An agent must not outlive the daemon that spawned it

- A daemon stopped cleanly stops its agents. One that is **killed** runs
  no handler, and its agents were reparented to init still holding an
  agent's ports, session directory and sqlite database with nothing
  supervising them. The next daemon knew nothing of them and started a
  second copy alongside each. Three accumulated in one afternoon of
  restarts, and the symptom was an agent that would not start, exiting 0
  immediately with no explanation anywhere.
- `prctl(PR_SET_PDEATHSIG, SIGTERM)` in the child's setup function, which
  is the only thing that survives SIGKILL of the parent. Verified by
  killing a real daemon with -9 and watching the child go with it.

### Skipping work for a running guest skipped knowing how to reach it

- `vm_provision()` returns early for a libvirt domain that is already
  running, so it cannot rebuild a live guest's overlay or seed. Correct
  — that path destroyed a guest once. But the SSH key was resolved
  *inside* `ensure_cloud_init()`, so the early return skipped that too:
  a daemon restarted against a running VM held no key path, built an ssh
  command with **no `-i` at all**, and every exec came back `Permission
  denied (publickey)` — for a key sitting in the state directory that
  worked perfectly by hand, against a VM that was plainly up.
- Building a seed is only meaningful before a guest boots. Knowing where
  its key file is matters every time a command runs. They had no business
  in one function, and `ensure_ssh_key()` is now separate and called from
  both paths.
- A libvirt domain outliving its daemon is the *ordinary* case, not an
  edge one — which is why this broke the moment anything restarted the
  daemon. `/computer/vm/provision-resolves-the-key` covers the qemu
  backend; the libvirt early return is verified against a real domain,
  since `libvirt_domain_state()` needs a hypervisor to return anything.

### Registering a tool is not telling the agent it has it

- The guest desktop's tools reach an agent through its `.mcp.json`, so
  its CLI lists them and it can see `screenshot` and `key_press` — with
  nothing to say whether they point at its own VM or at the screen a
  person is sitting in front of. Those call for completely different
  amounts of caution. `clawt_desktop_describe()` said exactly that, in
  words chosen for it, and was **called from nowhere**: the description
  an agent receives came from `clawt_computer_describe()`, and a computer
  has never heard of the desktop beside it. Both now go through
  `clawt_agent_describe_computer()`.
- Wiring it up then exposed a worse one. `describe()` reads
  `self->resolved`, which only `clawt_desktop_resolve_backend()` sets —
  and nothing on that path called it, so a desktop built from a config
  saying `auto` still held `AUTO` and fell through to the gowl branch. A
  guest agent was told it was driving the gowl compositor and that
  "anything you click is clicked on the user's real screen". Backwards,
  and backwards in the dangerous direction.
- The unit test passed throughout, because it constructed the desktop
  with `CLAWT_DESKTOP_BACKEND_GUEST` — which the factory never does. A
  test that names a value the production path derives is testing a
  different program. It uses `AUTO` now.
- Found only by reading what a live agent was actually told. Neither
  `make test` nor any amount of rereading would have shown it.

### `make test` does not relink the daemon

- It builds the library and the test binaries, so a change can be built,
  tested and passing while `build/release/clawtillad` is still the
  previous one. Two rounds of "the fix did not work" came from that —
  the library was correct and provably so from a standalone probe, and
  the running daemon was minutes old. Run plain `make` before restarting
  the daemon to check a fix by hand.

### g_enum_get_value_by_nick() asserts on a flags type

- `clawt_enum_from_nick()` calls it, so passing a flags `GType` is an
  assertion failure rather than a lookup that returns nothing. Every
  notify integration therefore failed to parse its own `events` list and
  **nothing would ever have fired** -- while the "send a test" button
  worked perfectly throughout, because it skips the list by design. A
  feature can be demonstrably working through the surface built to
  demonstrate it and dead on every real path. `clawt_flags_from_nick()`
  now exists and does its own case-insensitive comparison, for the same
  reason its enum twin does.

### A hand-written list of an option's keys drifts from the schema silently

- The daemon kept two -- one to read an integration, one to write it --
  and the CLI a third. Adding `backend` to the schema without adding it
  to all three produced a setting that was accepted, reported as saved,
  written to a file without it, and then ignored at the default. Nothing
  warned. All three walk `clawt_config_schema_get()` now and dispatch on
  `entry->type`; the schema is the source of truth for what an option
  *is*, so it may as well be the source of truth for what the options
  *are*.
- Same shape as the starter-config generator, which skipped list
  contents by naming `"agents."` and `"rooms."` outright and emitted a
  third list's two dozen keys under whichever section was open above
  them. `inside_list_of()` asks the schema instead.

### A signal nothing connects to is a feature that does not exist

- `ClawtAgentManager::agent-state-changed` had been emitted since the
  manager was written and **nothing had ever connected to it**, so an
  agent that crashed produced no event on the bus at all: clients found
  out by polling, and nobody found out at 3am. Grep for a connection
  before assuming a signal is wired; emitting one is half the work.

### g_task_new() refs its source object, so it must be a GObject

- Passing a `ClawtIntegrationBinding` -- a plain reference-counted struct
  -- ran `g_object_ref()` on a pointer that is not a GObject and took the
  daemon down on the first health check anybody asked for. It builds, it
  type-checks, and the cast in the callback looks symmetrical with every
  other async function in the file. Carry a non-GObject in the task
  *data* and pass `NULL` as the source.

### An IPC handler that must wait now has somewhere to wait

- `clawt_ipc_server_defer()` claims the right to answer later; the
  handler returns NULL and `clawt_ipc_pending_respond()` sends the frame
  when the work finishes. The dispatcher already tolerated a NULL reply,
  so it is a few dozen lines rather than a protocol change, and it is
  what lets `integration.health`, `matrix_login`, `matrix_rooms` and
  `notify_test` do real network work. Measured: a health check against a
  blackholed address took the full 10 seconds while `agent list` answered
  in 16ms.
- The token holds a reference to the client, which is already refcounted
  for exactly this reason, so a connection that closes mid-flight is
  still there to be answered into and simply drops it.

### podomation's DSL and its handler arguments both have shapes nothing documents

- The lexer cannot parse a dot in an event name, so `agent.state` had to
  become `on_agent_state` -- and every event podomation ships is named
  `on_something`, which is the convention its bindings are written
  around. Neither is written down; both took a pod that would not load.
- Worse: a handler's arguments arrive as a **positional tuple**, padded
  with empty strings, in the order the module declared its parameters.
  `notify(title: "x")` and `notify("x")` both arrive as `('x', '', '')`.
  Read as the `a{sv}` the type signature suggests, every argument was
  dropped, so every action failed with "a notification needs a title"
  while the pod, the binding and the dispatch were all correct. The one
  line in the log named the handler, not what it was handed.
- The padding must be dropped rather than stored: an empty string is not
  the same as a value, and a backend that renders an empty body would
  show a notification with nothing in it.
- `PodModule::activate` refuses by default, which is right for a module
  that has to open something and wrong for one whose daemon is already
  running. Without overriding it every pod loaded and then failed to
  activate, with one line in the log and no events ever delivered.

### A `*/` inside a C comment ends the comment

- A cron expression with a step in it -- `0 */6 * * *` -- cannot be
  written in a block comment, and the compiler's complaint points at the
  line *after* it. Worth knowing in a file that documents schedules.

### A notifier is correct precisely when nothing happens

- Which makes it the one thing in a fleet you cannot tell is working by
  looking at it, and the reason `integration.notify_test` exists and
  ignores both the event list and the quiet hours. A button that did
  nothing at half past eleven at night would be indistinguishable from a
  broken one.
- Quiet hours wrap midnight, because people sleep across it: 23:00-07:00
  is the ordinary spelling and the case that has to be right. Getting it
  backwards produces a notifier silent all day and loud all night, which
  is the same bug either way round and only noticed at 3am.

### Cron's oldest oddity is an OR, and only sometimes

- When **both** day fields are restricted the match is day-of-month OR
  day-of-week, so `0 0 13 * 5` is the thirteenth *and* every Friday, not
  Friday the thirteenth. With one at `*` the other decides. A scheduler
  that gets this backwards is wrong in a way nobody notices until the
  month a routine did not run.
- The search runs four years ahead, not one: `0 0 29 2 *` is a
  legitimate schedule that a one-year search reports as impossible.
- Presets are sugar over cron rather than a second scheduler. Two
  implementations of "when next" means the less-exercised one is wrong,
  and it would be the one behind the friendly buttons.

### A missed run is not a failed run

- A routine that did not fire because the machine was asleep is not
  broken, and showing it as broken trains somebody to ignore the one that
  is. `catch_up` runs it *once* however many were missed -- a laptop
  opened after a long weekend should not deliver a stack of good mornings
  -- and a routine that has never run has not missed anything, so adding
  one at four in the afternoon does not fire its 09:00 slot immediately.
- Run state lives beside the config, never in it. A `clawtilla.yaml` that
  rewrote itself every time a routine fired is one people stop keeping in
  git.

### A generator that names the sections it knows about grows a bug per section

- The starter config skipped list contents by testing `g_str_has_prefix`
  against `"agents."` and `"rooms."` -- the two lists that existed when
  it was written. Adding a third, `integrations`, emitted its two dozen
  keys at top level under whichever section happened to be open above
  them, which was `memories`. The file parsed. It simply declared two
  dozen options that belonged to somebody else, and the only symptom was
  a config-schema test counting 23 unknown keys. `inside_list_of()` walks
  the key's own prefixes and asks the schema, so the next list added
  needs no edit here.

### An integration is configured in two places and must behave as one

- Inline in an agent, or as a named instance with a scope. Everything
  downstream -- the rendered `channels:` block, the agent's `.mcp.json`,
  the paragraph in its `TOOLS.org`, the health check -- goes through
  `clawt_integration_resolve_for_agent()` and a `ClawtIntegrationBinding`,
  which reads through to whichever of the two holds the values. Two code
  paths would be two behaviours, and the one nobody tests is the shared
  one.
- `one_per_agent` is not a nicety. libreclaw renders one
  `channels.<type>` block per agent, so a second Matrix instance for the
  same agent has nowhere to go -- and dropping it silently leaves an
  account that looks configured in the file and receives nothing for
  ever. The inline block wins, because somebody wrote it inside that
  agent, and the instance is named in a warning.

### A shared account is a fleet-level bug that no agent can see

- A Matrix account is one login: two agents on the same `user_id` receive
  each other's messages and answer as the same person, which reads as the
  fleet misbehaving rather than as a config mistake. Each type declares
  `identity_keys` -- the keys that must differ between agents sharing an
  instance -- and `clawt_integration_validate_fleet()` is the only place
  that can notice, because it needs two agents at once.
- An unrecognised `scope` reaches **nobody**, not everybody. The two
  failure modes are not symmetric: a typo that hands a credential to the
  whole fleet is far worse than one that hands it to nothing and says so.

### g_task_new() refs its source object, so it must be a GObject

- Passing a `ClawtIntegrationBinding` -- a plain reference-counted struct
  -- ran `g_object_ref()` on a pointer that is not a GObject and took the
  daemon down on the first health check anybody asked for. It builds and
  it type-checks; the cast in the callback looks symmetrical with every
  other async function in the file. Carry a non-GObject in the task
  *data* and pass `NULL` as the source.

### An IPC handler that must wait now has somewhere to wait

- `clawt_ipc_server_defer()` claims the right to answer later; the
  handler returns NULL and `clawt_ipc_pending_respond()` sends the frame
  when the work finishes. The dispatcher already tolerated a NULL reply,
  so this is a few dozen lines rather than a protocol change, and it is
  what lets `integration.health`, `matrix_login` and `matrix_rooms` do
  real network work without stalling the daemon. Measured: a health check
  against a blackholed address took the full 10 seconds while
  `agent list` answered in 16ms.
- The token holds a reference to the client, which is already refcounted
  for exactly this reason, so a connection that closes mid-flight is
  still there to be answered into and simply drops it.

### A file a person edits gets a marked region, not a rewrite

- `TOOLS.org` is scaffolded once and then belongs to whoever edits it, so
  clawtilla owns the region between `# BEGIN clawtilla integrations` and
  `# END clawtilla integrations` and nothing else. Same contract as the
  `clawtilla` key in `.mcp.json`, and for the same reason. A file whose
  markers somebody removed gets them **appended** -- there is no position
  in a page of somebody's prose that we could claim to know is right.
- The `clawtilla-` prefix in `.mcp.json` is now reserved wholesale: every
  key beginning with it is rewritten or *removed* on each start, which is
  what lets a revoked integration's entry disappear instead of pointing
  the agent at a server the fleet has stopped offering.

### A password may cross IPC; a token may not come back

- `integration.matrix_login` takes a password, uses it once, and writes
  the resulting access token to a 0600 file under `secrets.dir`, putting
  only the *path* in the config and in the reply. The rule is about
  responses, logs and transcripts -- so the one thing that must never
  happen is handing the token back to the client that asked for the
  login, which would put a live credential into every client's memory.
  The CLI reads the password from stdin with echo off; there is
  deliberately no `--password` flag, because an argument is in the shell
  history and in the process table.

### A shared SoupSession cannot carry per-request state

- Parking a context pointer on the session with `g_object_set_data()`
  works exactly until two requests are in flight, which for a room
  listing is immediately: every room's name lookup is fired at once, each
  overwrote the last, and every callback would have decorated the same
  room. Allocate a struct per request and pass it as the callback's
  user_data.

### A rule enforced at one creation path is not enforced

- Refusing a diskless VM agent in the daemon's `agent.create` handler
  left `clawt_agent_designer_commit()` free to make one, because the
  designer writes through `clawt_config_add_agent()` and never touches
  that handler. Its own comment says "the same path as creating an agent
  by hand", which is true of the config call and not of the validation
  around it. The rule lives in
  `clawt_agent_config_validate_computer()` and both callers run it after
  applying their fields, then roll back with
  `clawt_config_remove_agent()` — an agent that exists and cannot work is
  worse than one that was never added.
- The provisioning refusal stays. It is the backstop for the paths that
  do not create agents at all, such as `agent set computer.type vm`.

### The CLI passed `--vm-image` straight through and lost it

- `agent create` builds its payload by stripping `--` and using the rest
  as a JSON member name, so `--vm-image` became a member called
  `vm-image`, which the daemon has never heard of and silently ignores.
  Every other option there happens to be a single word, so nothing had
  exposed it. Dashes are now folded to underscores, and the usage text
  names the flag — a VM agent could not be given a disk from the command
  line at all before that.

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

### Looking up a libvirt domain that is absent logs two errors

- podomation's `lookup_domain()` tries the name, then tries the same
  string as a UUID, so a domain that is not there produces `Domain not
  found` *and* `Invalid UUID` on stderr. The `get_xml` added for UUID
  adoption made that happen on every first provision — two alarming
  lines on the daemon's console for the most ordinary path there is, and
  a person reasonably read them as the reason their VM would not start.
  Ask `list_domains` first; it enumerates without complaining.

### A libvirt domain defined without a UUID cannot be redefined

- libvirt invents one per define and then refuses the name it already
  holds: `domain 'clawt-x' already exists with uuid ...`. So an agent
  provisioned once could never be started again — `define_xml` runs on
  every provision by design, so config changes reach the domain. The XML
  now carries a UUID derived from the domain name (SHA-1, version and
  variant bits set), which makes define a redefine. `virsh define` of a
  *dumped* XML works because the dump includes the UUID, which is why
  testing that way proves nothing.
- A domain defined by an *older* build has whatever UUID libvirt
  invented then, and libvirt refuses the same name under a different
  one — so simply adding a derived UUID broke every existing agent
  instead of fixing it. Provision asks `get_xml` first and adopts the
  UUID already there, which is non-destructive and needs no `undefine`
  (which `vm_virtmanager` does not expose anyway).
- `start` and `stop` are separately guarded: a libvirt domain outlives
  the daemon, so a restart finds one already running and
  `virDomainCreate` on it is an error. The qemu backend is guarded too —
  two qemus writing one qcow2 corrupt it.

### An example in a generated file is code, so run it

- The `/dev/console` example shipped with its C escaping leaking into
  the output — `echo \">>> x <<<\"` inside single quotes prints the
  backslashes, so the command as documented would have written the wrong
  text. It compiled, it read correctly at a glance, and it was wrong.
  Escaping that survives a C string literal into an org file into a
  shell has three layers; run the result.

### computer_exec takes a command, not a shell line

- Both backends `g_shell_quote()` each argument and join them, so `>`,
  `|`, `&&`, `;`, `*` and `$VAR` reach the command as literal text.
  The failure is the bad kind: `echo hi > /dev/console` **exits 0** and
  prints `hi > /dev/console` to stdout, so it reports success and does
  nothing. Measured both ways against a real guest.
- An agent worked this out by trial and error and reported it as a
  discovery, which is the signal that it belonged in the workspace files
  rather than in a person's head. `TOOLS.org` now states the rule for
  every backend, and a VM agent's own description shows the
  `/dev/console` case with its own name in it — that being the one place
  where the naive form most convincingly appears to have worked.

### The designer's pinning was right; the client never sent it

- `clawt_agent_designer_pin_identity()` works and has two tests, the
  daemon pins from the `design.agent` payload — and the GTK dialog sent
  the purpose fields and *not* the id or name it had just collected. So
  the model renamed every agent designed from the client, and no test
  could see it: the gap was between two tested halves. When a feature
  works in isolation and not in the product, suspect the wiring before
  the logic.

### A shutdown is a request, not a command

- Only a guest that is listening answers one. A hung guest — or one that
  never booted, which a VM with no disk always is — ignores it for ever,
  and the agent then could not be stopped through clawtilla at all;
  `virsh destroy` by hand was the only way out. `vm_stop()` waits 30
  seconds and then destroys, saying so.

### A VM with no disk image is three symptoms and one cause

- It defines, starts, boots nothing: a black console, SSH answering
  `kex_exchange_identification: Connection reset by peer` because the
  forward reaches a guest with no sshd, and virt-manager showing a
  running VM. The docs said this would happen; nothing enforced it.
  Provision now refuses and names `computer.vm.image`. Fedora Cloud
  writes plenty to the serial console once there *is* a disk — nearly
  300 lines to a login prompt — so a black console means no disk, not a
  console misconfiguration.

### qemu-img refuses an image a running guest holds

- `Failed to get shared "write" lock`, exit 1. The overlay-change check
  read that as "the base is something else" and **deleted the disk of a
  running VM** — which carried on against the unlinked inode while the
  file on disk was replaced by an empty one. Unknown is not different:
  `overlay_backing_file()` now reports whether it could *answer*
  separately from the answer, and an unreadable disk is left alone.
- Provisioning also skips the overlay and the seed entirely when the
  domain is running. Restarting the daemon re-provisions every agent and
  a libvirt domain outlives the daemon, so that path runs routinely
  against a VM that is up.
- A genuine base change now *moves* the old overlay to
  `overlay-superseded.qcow2` rather than unlinking it. Destroying a
  guest because a config line changed should be recoverable.

### libvirt's remove-handle callback must not be called inline

- `virEventRemoveHandleFunc` is invoked while libvirt holds the lock for
  the thing being removed; its contract is that the `ff` callback runs
  *afterwards*, from the event loop. podomation called it synchronously
  from a `GHashTable` destroy notify, inside its own mutex, so
  `virNetSocketEventFree` tried to take the lock its caller held and the
  **whole process** stopped — main loop, IPC server, every agent — and
  could not be stopped with SIGTERM, since that handler runs on the loop
  that is stuck.
- Reached two ways, both routine: the remote closing the connection, and
  *us* closing it by unreffing the module (`virConnectDispose`). Fixed
  upstream in podomation by deferring the callback to an idle. The
  before/after was measured: connect, list, drop, three times — hangs
  without the change, completes with it.

### A qcow2 overlay pins the base it was made from

- `ensure_overlay()` returned early whenever the overlay existed, so
  changing `computer.vm.image` wrote the config and changed nothing —
  the VM kept booting the old base for ever. It now compares the
  overlay's recorded backing file with the configured image and rebuilds
  when they differ, loudly, because the guest's contents go with it.
  Deleting a cached image is refused while an agent's overlay names it:
  the VM would stop booting, with an error from qemu about a missing
  backing file a long way from the button that caused it.
- Both test binaries now point `XDG_DATA_HOME` at a temporary directory
  in `main()`, **before** `g_test_init()` — `g_get_user_data_dir()`
  caches on first use, and these tests write disk overlays and cloud
  images under it. Without that the suite quietly created files in the
  developer's real `~/.local/share`.

### Double-encoded UTF-8 compiles perfectly

- A tool that reads a source file as Latin-1 and writes it back turns
  `…` into three characters. The compiler does not care — it is a string
  literal either way — and it surfaces as mojibake in the sidebar, a long
  way from the edit that caused it. This has now happened twice, so
  `make docs-check` fails on it: `\xc2[\x80-\x9f]` is a C1 control,
  which nothing legitimate contains (the range stops short of A0, a
  non-breaking space), and `\xc3[\x82\x83]\xc2` is what a two-byte
  character turns into. Verified in both directions against `… — ─ é ü`,
  and against a clean tree.
- Non-ASCII in C sources is fine and worth keeping — the section
  dividers are box-drawing characters. It is the *round trip* through a
  wrong encoding that breaks them.

### A popover unparented from object data is unparented too late

- qdata is cleared in `g_object_finalize`, and `gtk_widget_finalize()`
  checks for leftover children *before* chaining up to it — so a
  `g_object_set_data_full()` notify that unparents a hand-parented
  popover always runs after the check, and every chip in a cleared
  transcript warned "Finalizing GtkButton, but it still has children
  left: GtkPopover". Connect to `::destroy`, which is emitted from
  dispose. Confirmed both ways with a 40-line GTK program: the notify
  warns, `::destroy` is silent.
- Unrelated and *not ours*: the
  `GtkSettings:gtk-application-prefer-dark-theme` warning libadwaita
  prints comes from the user's `~/.config/gtk-4.0/settings.ini` (or
  their desktop), not from clawtilla, which never touches GtkSettings.

### A queued idle reads the adjustment before GTK has laid out

- `queue_scroll()` fired at `G_PRIORITY_DEFAULT_IDLE` and read
  `gtk_adjustment_get_upper()`, but GTK4 lays out from the frame clock,
  not the idle queue — so it scrolled to where the bottom *was* and left
  the newest message just below the fold. Follow the adjustment instead:
  `notify::upper` for content arriving, `notify::page-size` because
  typing grows the composer and shrinks the transcript above it.

### GtkListBox keeps its own record of its rows

- It wraps an appended widget in a row of its own, so unparenting
  children behind its back leaves a list that accepts appends and draws
  none of them — an empty box where the settings images had been.
  `clear_list()` exists for this and removes rows through
  `gtk_list_box_remove()`; it is also the one that survives a popover
  parented to the list.

### A mirror can refuse a request that does not say who it is

- libsoup sends no `User-Agent` by default and `cloud.centos.org`
  answers 403 to that. The 403 page is a perfectly good page in which no
  image matches, so the failure surfaced as "the catalog entry has gone
  stale" — pointing at the wrong thing entirely. The session names
  itself, and the listing fetch checks its HTTP status rather than
  inferring failure from an empty result. Same shape as the download
  path, where a 404 body would otherwise be written into a .qcow2.

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

### A test that races its own fixture proves nothing

- `/agent/stop-waits-for-the-child` sent SIGTERM microseconds after
  spawning `stubborn-libreclaw`, which is *before* bash has installed
  `trap '' TERM`. In that gap the default action still applies, so the
  child died at once, the graceful branch was taken, the test passed in
  110ms -- and the force-kill path it exists to cover never ran. Only
  under ASAN, slow enough that the trap wins the race, did the real path
  execute, and then it *failed*. A test that passes for the wrong reason
  in the build everyone runs is worse than no test.
- The fixture prints its line after installing the trap, so the test
  waits for that line rather than for a duration. And it wraps the
  expected `g_warning` in `g_test_expect_message()` -- GTest makes a
  warning fatal, so a test that provokes one on purpose has to say so.
- What the fixed test then found: `g_subprocess_force_exit()` sends
  SIGKILL but reaps nothing, and `kill(pid, 0)` succeeds on a zombie. So
  `stop()` returned reporting the child gone while the kernel still held
  an entry for it -- the same shape of lie the SIGTERM path had just been
  fixed for. It now waits for the exit to be *observed*, bounded, because
  SIGKILL cannot be caught and anything outlasting that is in
  uninterruptible sleep and must not become a hung daemon.

### "Follow the desktop" is not the same as naming the desktop's font

- The two look identical on screen and diverge for ever afterwards: one
  keeps following, the other has quietly frozen. So `ClawtAppearance`
  emits *no CSS rule at all* for an unset value rather than a rule naming
  the current default -- otherwise a person who later changed their
  desktop font would find this one app ignoring it, with no reason to
  look in a dialog they had never opened. Zeroed means "defer" for every
  field, which also makes a field added later default to deferring.
- Clearing a font must reach NULL, not `""`. An empty family emits
  `font-family: ;`, which is invalid, and GTK drops the whole block it
  appears in -- so clearing the family would silently take the size with
  it. And a font chooser has no way to express "unset", which is why the
  dialog has a clear button beside it.
- Appearance lives in the client's config, not `clawtilla.yaml`, for the
  same reason connection profiles do -- and more sharply, because the
  client switches daemons at runtime: fonts from a daemon's config would
  change when you connected to another machine.

### Pango's <tt> is not reachable from GTK CSS

- It resolves through fontconfig's generic monospace alias, so a code
  font set in the client applied to the exec console -- a real widget
  with a CSS node -- and did nothing to a chat message, which is where
  people actually read code. `clawt_markdown_to_pango_full()` names the
  family in a `<span>` instead. The family is escaped on the way in: it
  comes from a font chooser rather than a model, but the rule in that
  file is that nothing reaches a markup parser unescaped, whoever wrote
  it.
- A font family in CSS is sanitised rather than escaped -- quotes,
  braces, semicolons and angle brackets are dropped. CSS string escapes
  are their own small language and there is nothing to preserve: no font
  has a brace in its name. The test asserts *structure* (one block, one
  declaration, one pair of quotes) rather than absence of the injected
  text, because text inside the quoted family is harmless and asserting
  on it fails for the wrong reason.

### One CSS provider, reloaded

- `gtk_style_context_add_provider_for_display()` adds; it does not
  replace. A new provider per change leaves every previous sheet on the
  display at the same priority, and the oldest wins ties -- so the fonts
  change once and then appear stuck. Keep one provider and
  `gtk_css_provider_load_from_string()` into it.
- Verified by asking a realized widget for its resolved font description
  rather than by reading the sheet: 21pt arrived as `Cantarell 28px` and
  the monospace view as `DejaVu Sans Mono 22.667px`. GTK parsing a
  stylesheet without complaint is not evidence that any widget uses it.

### Help text that describes a default is not the same as printing it

- `clawtillad --help` computes what this machine would actually bind --
  the real tailnet address, the real port out of `daemon.tcp_port`, the
  real socket path -- because "the tailnet address" is not something a
  person can check against `ss -ltn`, and wanting to know exactly that is
  the reason they reached for `--bind` in the first place. GOption
  answers `--help` during the parse, so the description is built before
  it, from a small pre-scan of argv for `-c`.
- `--bind` *replaces* the configured network addresses rather than adding
  to them, and is never optional: an address clawtilla chose warns, an
  address a person named is an error, because a daemon that ignored where
  it was told to listen and started anyway is running somewhere nobody
  knows about.
- Addresses are parsed when the flag is read, not at start, so a typo is
  refused while the person is still looking at the command line rather
  than after the state directory and every agent workspace are written.

### An IPv6 address has no last colon to split on

- `<ip>:<port>` cannot be parsed with `strrchr(':')`. Splitting
  `fd7a:115c:a1e0::1` on its last colon yields a host of
  `fd7a:115c:a1e0:` and a port of `:1`, both of which are almost
  plausible and neither of which is anything. The bracket form settles
  it, and a bare IPv6 address with no port is accepted because it parses
  whole. `tests/test-bind-address.c` covers both.
- Names are refused rather than resolved: resolution is a network round
  trip on the path that starts the daemon, and a name can resolve onto a
  different network than the one intended.
- Port 0 is refused too. It asks the kernel for any free port, which for
  a daemon clients dial by number is the same as not listening.

### Two edits where the first deletes the second's target

- Rewriting a block of `clawt-ipc-server.c` removed the function a later
  edit meant to patch, so that edit matched nothing, reported nothing,
  and the definition simply vanished. It compiled -- the header still
  declared it -- and surfaced as `undefined reference` at link time, in
  the *daemon*, several steps away.
- It was then missed twice more because the build was being filtered
  through `grep ':[0-9]+:[0-9]+: (warning|error)'`, which matches
  compiler diagnostics and not linker ones. A filter that hides a whole
  class of failure is worse than reading the output.

### A convenience listener must not take daemon start down with it

- `daemon.tailscale` is on by default, so the tailnet address is bound on
  every start that finds one -- and the first thing that exposed was
  `make test`, where every daemon fixture began binding a real network
  address. That is two rules broken at once: the suite stopped being
  hermetic, and on a machine already running a daemon the bind failed
  with EADDRINUSE and took `clawt_daemon_start()` with it. A whole fleet
  refusing to start because a *second* copy wanted the same convenience
  port is a far worse failure than not being reachable from a laptop.
- `clawt_ipc_server_set_optional_tcp_address()` marks an address whose
  bind failure is a warning. The unix socket is the daemon's real
  interface and is already listening by then. An address named in the
  configuration is never optional -- somebody asked for it, so failing to
  provide it is an error rather than a footnote.
- The daemon also announced the address *before* binding it, so a daemon
  that lost the race printed "reachable on the tailnet at ..." and warned
  it could not bind that address one line later.
  `clawt_ipc_server_is_listening_on()` exists so the announcement reports
  what happened rather than what was requested.
- Test fixtures set `daemon.tailscale: false`. `make test` opens no
  network socket at all.

### Finding an address must not run a program

- The tailnet address comes from `getifaddrs()`, not from `tailscale ip
  -4`. Daemon start may not wait on anything that can hang -- the same
  rule that moved the model cache out of `clawt_daemon_start()` -- and a
  subprocess talking to a wedged tailscaled hangs exactly as well as a
  network call. An interface list is a syscall against data the kernel
  already has.
- Both the interface name and the address range are checked, because
  either alone is wrong: `utun` is any tunnel on a BSD, and a container
  can hold a 100.64/10 address for reasons of its own.
- 100.64.0.0/10 is not an octet boundary. The second octet is a *range*,
  64 through 127; `100.128.0.1` is somebody else's address on the open
  internet and `100.63.255.255` is below the range. `tests/test-tailscale.c`
  pins both ends.

### Connection profiles belong to the client, not to clawtilla.yaml

- The point of a profile is to reach a daemon that is somewhere else. A
  laptop connecting to a workstation may have no fleet, no state
  directory and no `clawtilla.yaml` at all, so reading the daemon's
  config to find out how to reach a different daemon is backwards. They
  live in `$XDG_CONFIG_HOME/clawtilla/connections.yaml`, 0600.
- That file holds bearer tokens on purpose. There is deliberately no way
  to write a secret into `clawtilla.yaml`, but here the token *is* the
  thing being remembered, and a second file to manage would mean most
  people keeping it in shell history instead.
- Rendered with single-quoted YAML, whose only escape is a doubled quote,
  so a token containing a backslash survives without an unescape step to
  get wrong. `clawt_connection_list_parse()` and `_to_data()` are pure so
  the round trip is asserted on without touching a file -- a field that
  saves and does not load, in a file only touched when somebody edits
  their connections, is otherwise found months later.
- `clawtilla daemon token` reads the token from disk rather than asking
  the daemon, because nothing may write a secret's value into an IPC
  response. It therefore only works where the daemon runs, which is the
  point.

### A window holding a client must also hold what that client is

- `clawt_window_new()` takes the `ClawtConnection` beside the
  `ClawtClient`. A client knows a host and a port, not the name a person
  gave that machine, so a window deriving its own label would be a second
  answer to "which daemon is this" -- and every one of those this
  codebase has grown turned out to be a bug.
- Switching connects the new client *before* dropping the old one. A
  remote daemon that is not running is the ordinary case here, and
  releasing the working connection first leaves the window connected to
  nothing because of a typo in a port.
- Then everything from the previous daemon is cleared, and it is a
  correctness step rather than tidying: agent ids, room ids and message
  ids are all per-daemon, so a kept `shown` set silently swallows the new
  daemon's messages on any id collision. Re-subscribe from cursor 0 for
  the same reason -- a cursor is a position in one daemon's event stream.

### AdwPreferencesGroup cannot enumerate its own rows

- It wraps every added row in a list-box row of its own, so walking its
  children hands back internal boxes rather than anything
  `adw_preferences_group_remove()` will accept. A dialog that refills a
  group keeps its own `GPtrArray` of the rows it added. Same shape as the
  `GtkListBox` lesson above: the container keeps a record you have to go
  back through.

### The same NULL meant "deny everything" and "allow everything"

- `clawt_mcp_relay_run()` reads a NULL permitted-list as *no tool is
  allowed*, which is the right fail-closed default for a grant being
  enforced: an observe-only desktop must not widen itself because a list
  went missing. A connector with no `tools:` configured means the exact
  opposite -- every tool the server offers. Reusing NULL for both would
  have handed those agents an empty tool list, which is indistinguishable
  from a server that failed to start. `clawt_mcp_relay_run_unfiltered()`
  says which is meant, in its name.

### A refusal that names the wrong feature sends somebody a long way off

- The relay began life serving guest desktops, so its refusal message
  told an agent to turn on `computer.desktop.allow_input`. Connectors
  then started using the same relay, and an agent refused a *repository*
  tool was told to enable a setting about seeing the screen --
  confidently, and about a completely different feature. The reason for
  a refusal belongs to whoever imposed the restriction, so it is a
  parameter; each caller supplies its own sentence.

### A relay that does not close the child's stdin never exits

- A stdio MCP server exits when its stdin closes and nothing else tells
  it to. The relay waited for both directions to close, so when the
  agent's CLI went away it sat holding a stdout that would never end --
  and the pair outlived the agent indefinitely. Measured: two minutes
  and still running, versus 22ms after the fix. For a connector that
  means an abandoned process holding a live credential, which is the
  worse half of the bug.

### `expires_in` is a duration and must not be stored as one

- Kept as it arrives, an hour-long token looks an hour fresh after every
  daemon restart -- so one that expired overnight reads as valid each
  morning and every call made with it fails somewhere else entirely.
  `ClawtOauthToken.expires_at` is absolute, computed when the response
  arrives, and the field in the saved file is named `expires_at` so the
  next reader is not invited to add it to the time they loaded it.
- Providers also disagree about whether these are numbers or strings.
  Reading only one silently treats the other as absent, giving a token
  that never expires or one that expires immediately.

### A device flow's normal case is an HTTP 400

- A provider answers a poll for a code nobody has typed yet with **400
  Bad Request** and `authorization_pending` in the body. A client that
  reads the status and stops has built a device flow that can never once
  succeed while looking entirely correct. The classification is in
  `clawt_oauth_read_poll()` and is a pure function for that reason.
- `slow_down` lengthens the interval *permanently*. A provider that has
  asked once to be polled less often will ask again, and reverting after
  a single slower poll spends the rest of the flow rate limited while
  the person stands at the consent screen wondering what happened.

### A format string from a config file must never reach printf

- `credential_format` is read from a YAML file somebody edits and used
  to decorate a credential (`Bearer %s`). Handed to `g_strdup_printf()`,
  a `%d` reads whatever is in that register and a `%n` writes. It is
  validated at load *and* expanded by hand, so a format string read from
  a file is never a format string to anything -- the worst a wrong one
  can do is produce a credential the service rejects. Validating and
  then trusting printf anyway leaves all of printf standing behind a
  check that has to be perfect for ever.

### /dev/urandom or nothing, for anything that must be unguessable

- GLib's `g_random_*` is a Mersenne Twister: given a little output the
  rest is computable. A predictable PKCE verifier is not weaker PKCE, it
  is *no* PKCE, since the whole mechanism is that only the client which
  began the flow can finish it. A machine with no usable randomness gets
  a refusal rather than a downgrade -- the same rule as a missing
  `bwrap`.

### A fixed request timeout cannot wait on a person

- `clawt_client_request()` gives up after two minutes, which suits a
  question the daemon answers from what it already knows. A device code
  is good for fifteen. Without `clawt_client_request_full()` the client
  would report a timeout for a flow that was about to succeed -- and
  leave the daemon holding a credential nobody had been told about.
- Which is also why authorising is two frames. `connector.begin` answers
  as soon as there is a code to show; `connector.await` is the one that
  waits. Deferring the whole thing would mean showing the person nothing
  until it was over, which is no use to anybody.

### Granted scopes must not be written back over requested scopes

- A person can untick things on a consent screen, so what was granted is
  often less than what was asked for. Storing the granted set into
  `scopes:` would mean the next authorization asks for less, and the one
  after that less again -- a permission that quietly erodes every time
  it is renewed. The granted set lives in the token file and is reported
  from there.

### Deleting our copy of a token is not revocation

- A token we forgot still works for anybody who has it, for as long as
  the provider says, which for a refresh token can be months. Where a
  provider offers a revocation endpoint clawtilla calls it and reports
  that it did; where it does not, it says so plainly rather than letting
  "revoked" mean less than the person thinks. The local copy goes either
  way -- somebody who asked to revoke wants the fleet to stop now, and an
  unreachable provider must not leave an agent holding a working
  credential until the network comes back.

### An argv is world-readable; an environment is not

- Which is why `ClawtConnectorPlan` is built before anything is started
  and tested with a *negative* assertion: the credential must be in
  `envp` or a header and nowhere in `argv`. That is not visible by
  reading the code which builds the argv -- it is visible by looking for
  the value in the result. Verified against a real relay by having the
  tool server report its own `/proc/self/cmdline` and `environ`.


### A test fixture that pins the socket can still write to the real fleet

- `test-daemon.c` set `state_dir` and `socket` into a temporary directory
  -- with comments explaining why each mattered -- and said nothing about
  `defaults.workspace_root`, whose default is `~/.clawtilla/agents`. So
  every agent any of those tests created was scaffolded into the
  developer's own agent directory, on every `make test`, and the
  leftovers are indistinguishable afterwards from agents somebody meant
  to keep. `test-agent-designer.c` was worse: it set nothing at all,
  because committing a design scaffolds a workspace and nothing in the
  test looked like it touched the filesystem.
- Found only by diffing `~/.clawtilla` across a run. It had been
  happening for a long time, and the file that exposed it was one a
  scratch daemon had written into a leftover -- the confusion was
  entirely about which of two things had created a directory that should
  never have existed.
- Three things escape a temporary directory unless a fixture says
  otherwise: the socket, the state directory and the workspace root.
  `make test` now diffs clean against a real `~/.clawtilla`.


### cloud-init picks the package manager and nothing else

- The seed still has to know the *names*, and the names turned out to be
  the smaller half. `computer.vm.desktop.packages` was documented as the
  knob for a non-Fedora guest -- "a Debian or Ubuntu guest wants gdm3 and
  gnome-core instead" -- but three other Fedora assumptions were baked
  into the renderer where no config could reach them: `systemctl enable
  --now gdm.service` (Debian's unit is `gdm3`), `python3-gobject`
  (Debian's is `python3-gi`), and the assumption that `dconf` and
  `glib-compile-schemas` exist at all (on a Debian cloud image they are
  `dconf-cli` and `libglib2.0-bin`, neither installed). So following the
  documented advice produced a guest with a desktop and no display
  manager, or a session with no automation.
- Each failure lands *inside* the guest, in a cloud-init log nobody is
  looking at, a long way from the config line that caused it. From the
  host it looks like a VM that booted fine.
- The family now comes from the image -- catalog id first, then the
  filename, since an image somebody downloaded keeps the distribution in
  its name. Ubuntu's cloud images are the exception that has to be
  handled by hand: they never say "ubuntu", only the release adjective
  (`noble-server-cloudimg-amd64.img`).
- An image it cannot place warns naming `computer.vm.desktop.flavour`
  rather than guessing quietly, and Enterprise Linux gets a shorter list
  on purpose: cloud-init treats a package it cannot find as a failure of
  the whole install, so one Fedora-only name takes the desktop with it.
- Arch needs `package_upgrade` as well as `package_update`, and it is the
  only family that does. cloud-init's update runs `pacman -Sy`, which
  refreshes the index without upgrading what is there -- installing
  against that is the partial upgrade pacman warns about, where a new
  package links against libraries the image still has old versions of.
  It breaks inside the guest, after the desktop appears to have
  installed. Arch also publishes two qcow2s and only `cloudimg` has
  cloud-init; `basic` boots perfectly and admits nobody, which is
  indistinguishable from a VM that failed to boot.
- Matching an image on the substring "arch" would place
  `~/archived-vms/debian.qcow2` and `/home/me/research/disk.qcow2` as
  Arch and install pacman's names into a Debian. The markers are
  `arch-linux` and `archlinux`. Worth remembering for any distribution
  whose name is a common English fragment.
- Ubuntu is a family of its own for a single entry. Everything about it
  is Debian's -- gdm3, python3-gi, python3-venv, dconf-cli -- except
  Firefox: Debian stable has no `firefox` package, only `firefox-esr`,
  and Ubuntu has no `firefox-esr`. Either name fails the whole install
  on the other distribution, so a shared family would have to pick one
  and be wrong half the time. Worth knowing: `firefox` on Ubuntu is a
  transitional package that installs the snap, and there is no deb to
  prefer instead.


### Remembered state about something another program owns

- `vm_start()` provisioned only when it remembered the guest as ABSENT,
  and a stop leaves STOPPED -- so an agent restarted against a domain
  somebody had deleted in virt-manager skipped provisioning on the
  strength of its own memory. On the qemu backend it then returned
  **TRUE**: a computer reported as started with no machine anywhere,
  which is the same shape of lie as a teardown that reports success and
  removes nothing.
- It looked like it worked, because restarting the whole *daemon*
  recovers: a freshly built computer starts ABSENT and provisions.
  Restarting just the *agent* does not, and nobody should have to know
  the difference. Ask the hypervisor; provisioning is idempotent, so the
  check costs one `list_domains`.
- The test has to fail without the fix to mean anything. This one does,
  and reverting the check to confirm that is two minutes well spent --
  it was asserting on a refusal message, which is exactly the kind of
  assertion that can pass for the wrong reason.

### cloud-init reads its seed once, so some settings need the machine destroyed

- The login, the desktop, the flavour and the package list are fixed for
  the life of an overlay. Changing one and restarting the agent does
  nothing visible, which reads as the setting not working rather than as
  the setting arriving too late. `computer.rebuild` is the answer, and it
  is refused while the agent runs -- destroying the machine underneath a
  working agent is not a thing to do carefully, it is a thing not to
  offer.
- cpus, memory and disk size are *not* in the seed: they reach the domain
  definition and apply at its next boot. Worth saying in the same breath,
  or people rebuild for things that never needed it.


### The designer's pinning was extended for identity and not for the computer

- The exact bug the identity pinning was written to fix, one field-set
  over: `agent.create` sent the disk image the dialog had collected and
  `design.agent` did not. So designing a VM agent produced one that
  refused to provision, naming `computer.vm.image` -- while the image sat
  filled in on screen a few rows above the button. Pressing Create
  worked; pressing Design it did not.
- Both paths now read the form through one function. A bug whose whole
  nature is two code paths disagreeing about the same widgets is fixed by
  removing the second reader, not by teaching it to agree.
- The sharper half: the designer *cannot* name a disk image, because the
  images that exist are the ones somebody fetched rather than a path to
  invent. So `vm` was a choice it could never satisfy, and `set_computer`
  now refuses it when nothing is pinned and says to use a container --
  one turn spent instead of an agent that fails to provision later,
  naming a setting the model never saw.


## Things to NEVER Do

- Never hand-edit `data/example-config.yaml` or `data/default-config.yaml`
- Never decide there is nothing to build from state clawtilla remembers.
  A libvirt domain and a qcow2 both belong to something else, and either
  can be deleted without telling us
- Never write a distribution's package names, service units or binary
  locations straight into the cloud-init seed. cloud-init chooses the
  package manager and nothing else; everything else is per family
- Never let a test fixture take `defaults.workspace_root` from the
  defaults. It points at `~/.clawtilla/agents`, so a test that creates an
  agent scaffolds it into the developer's real fleet
- Never let the daemon or `libclawt` link GTK
- Never pass `environ` wholesale to a spawned agent -- use the allowlist
- Never write a secret's value into an IPC response, a log line or a transcript
- Never let a plugin load failure take down the daemon
- Never silently downgrade confinement; a missing `bwrap` is a SHADOW agent
  with a reason, not an unconfined one
- Never push to main without approval
- Never leave a generated file naming the same top-level key twice; YAML
  keeps the last and silently discards everything under the first
- Never make the tailnet listener mandatory; a bind failure there is a
  warning, and `make test` must open no network socket at all
- Never print a bearer token from a listing command, or write one into an
  IPC response
- Never regenerate an agent's `.mcp.json` wholesale. It is how an agent
  is given MCP servers, so people edit it. Only `clawtilla` and the
  `clawtilla-*` keys are clawtilla's; read the rest back and write it out
  untouched, skip the write when nothing changed, and move an unparseable
  file aside rather than over it
- Never rewrite an agent's org files wholesale either. `TOOLS.org` has a
  marked region and clawtilla owns only that
- Never return a secret obtained on a client's behalf to that client. A
  Matrix token goes to a 0600 file and the reply names the file
- Never write a hand-maintained list of an option's keys. Walk the schema
- Never put a connector's credential anywhere the agent can read it: not
  in `.mcp.json`, not in its environment, not in an argv, not in an IPC
  response. The relay reads the 0600 file itself and hands it to the
  server it starts, and that is the only copy outside `secrets.dir`
- Never hand a format string that came from a config file to `printf`.
  Validate it *and* expand it by hand
- Never store a granted scope list over a requested one, and never store
  a blank refresh token over a good one -- both cost the person an
  authorization they cannot see the reason for
- Never give a pod an action that runs arbitrary code. Every action is a
  fleet operation the daemon already owns; `computer_exec` is declared
  and refused, with a reason, so naming it does not read as a typo
