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

### An agent's system prompt has a hard ceiling, and it is not ARG_MAX

- The kernel caps a **single** argv word at `MAX_ARG_STRLEN` -- 32 pages,
  131,072 bytes. `ARG_MAX` is the total and is 2MB here, so headroom
  there buys nothing: `execve` refuses the whole call over one long
  word. A clawtilla agent's system prompt is `persona.identity_files`
  concatenated plus the managed `TOOLS.org` region, so it grows with
  what the product itself writes -- and past that figure the agent could
  not start at all, saying only *"Failed to execute child process
  (Argument list too long)"*.
- Fixed in ai-glib (`--system-prompt-file`, a 0600 temporary), so there
  is no practical ceiling now. Worth knowing anyway: the same limit
  applies to anything else clawtilla puts in an argv, and the failure
  names neither the argument nor the limit.
- `--system-prompt-file` and `--append-system-prompt-file` are **not in
  `claude --help`'s flag list** -- only inside a description paragraph.
  They do exist; verified against 2.1.247 with a control, because an
  invented flag answers `error: unknown option` and these do not.

### ai-glib's test binaries used to test whatever was installed

- Its `rules.mk` linked tests with `-Wl,-rpath,$(OUTDIR)`, and `$(OUTDIR)`
  is *relative* (`build/debug`). Run a test from ai-glib's own directory
  and it loads the library just built; run it from a parent tree and the
  loader falls through to `/lib64/libai-glib-1.0.so.0`. Same soname, no
  warning -- the tests simply report on the installed copy.
- It cost most of a merge: the merged tests failed from clawtilla's root
  and passed from ai-glib's, which reads exactly like a regression in the
  change. `LD_TRACE_LOADED_OBJECTS=1` is what settles it. `$ORIGIN/..`
  now, so all 58 binaries pass from anywhere.

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

### Two accounts for one machine is a workaround waiting to be invented

- `computer.vm.ssh_user` defaulted to **root**, and GDM will not log root
  in -- so a desktop VM got a second account for the screen while every
  command arrived as the first. That was recorded at the time as a fact
  to work around rather than as the bug it was: anything touching the
  session (the display, the session bus, a file in that home directory)
  needed the agent to bridge two logins by hand, and one duly did --
  re-running things as `clawt` with `XAUTHORITY` pointed at the Xwayland
  cookie it went and found. **A technique an agent explains back to you
  is a thing the software should have done.**
- The default is `clawt` now, with `NOPASSWD:ALL`, and the seed renders
  *one* account: `clawt_guest_desktop_resolve_user()` already returned
  ssh_user unchanged when it was not root, so the two collapse on their
  own. An agent needing root writes `sudo`.
- It also lines the uid up. The account is the guest's first, so uid
  1000 -- the same as the host user owning the files on a virtiofs share,
  which root was not.
- `ssh_user: root` still works and still gets the second account, because
  GDM's refusal is not ours to overrule. A guest built before this has no
  such account unless it had a desktop, so that one needs root named
  explicitly or a rebuild -- cloud-init reads its seed once.

### One package name this distribution lacks costs the whole desktop

- RHEL 10 replaced GNOME Terminal with Ptyxis, so `gnome-terminal` is not
  in CentOS Stream 10's repositories at all -- and cloud-init treats a
  package it cannot find as a failure of the **whole** package install.
  A guest therefore booted to a text login prompt with no GNOME on it, on
  account of a terminal emulator nobody was going to open.
- The rule was already written down, in the comment above the very list
  that broke: "cloud-init treats a package it cannot find as a failure of
  the whole install, so asking for one takes the desktop with it." The
  Enterprise list was built by *removing* Fedora names that EL lacks, and
  `gnome-terminal` was never checked because Fedora has it too -- but EL
  is not Fedora minus things, it is its own distribution. Every name in
  that column is now checked against the real BaseOS and AppStream
  metadata.
- The worse half: the installer said `ok`. Its own work -- the clone, the
  venv, the extension -- had all succeeded, so it reported success about
  a guest with no desktop. **Saying ok about the wrong thing is worse
  than saying nothing**, because it sends whoever reads it to investigate
  a different layer. It now checks `gnome-shell` exists and that the
  family's own display-manager unit does, before writing `ok`.
- `test-guest-desktop.c` asserted the installer was byte-identical across
  families. That was true and is not any more, because the unit name is
  now in it -- so the test folds `gdm3` to `gdm` and compares the rest.
  The intent was never "identical", it was "nothing varies per family by
  accident".

### Naming no CPU is not neutral -- libvirt names one for you

- The domain XML emitted no `<cpu>`, so libvirt filled in its default:
  `<model fallback='forbid'>qemu64</model>`, x86-64-v1, no SSE4.2, no
  AVX. Fedora, Debian, Ubuntu and Arch all boot on it. **CentOS Stream 10
  does not** -- RHEL 10 raised its baseline, and the guest's own serial
  console says `Fatal glibc error: CPU does not support x86-64-v2`
  followed by a kernel panic killing init.
- From outside: a VM that is definitely running, a console reading
  "Display output is not active", and ssh answering
  `kex_exchange_identification: Connection reset by peer`. Those are the
  *same three symptoms* as a VM with no disk image, which is already a
  documented cause -- so the obvious diagnosis was the wrong one, and
  nothing in the whole path named a CPU.
- Proved by booting the image both ways with nothing else changed:
  `-cpu qemu64` panics, `-cpu max` reaches a login prompt. Two minutes,
  and it turns a plausible theory into the answer.
- `host-passthrough` rather than a named model. The domain is already
  `type='kvm'`, so the host CPU is available by definition, and choosing
  a model would be choosing a baseline to be wrong about again the next
  time somebody raises theirs. The qemu backend gets `-cpu max`, not
  `host`, because its machine line falls back to TCG where `host` is not
  valid.
- `host-passthrough` contains the substring `passt`, which broke a test
  asserting the domain had no passt *network backend*. Assert on the
  element, not the word.

### Naming no emulator is not neutral either, and libvirt resolves it by PATH

- The same shape as the CPU one directly above, and worse, because the
  default depends on a *different process's environment*. A domain that
  names no `<emulator>` gets one from libvirt, which finds it by
  searching the **session daemon's** `PATH` -- so a host with another
  package manager ahead of `/usr/bin` gets
  `<emulator>/var/home/linuxbrew/.linuxbrew/bin/qemu-system-x86_64</emulator>`
  and SELinux refuses it: `svirt_t` cannot entrypoint a binary labelled
  `user_home_t`. The domain defines, the start fails, and the only record
  is an AVC denial naming a path nobody chose.
- Proved on this machine rather than reasoned about, and the proof is two
  commands: `virsh -c qemu:///session domcapabilities` reports
  `/usr/sbin/qemu-system-x86_64`, which is exactly what `which -a` lists
  first, and an existing `clawt-*` domain has that same string baked into
  it. **libvirt resolving by PATH is checkable in one line**, and the
  report that arrived attributed the path to clawtilla writing it -- the
  fix is the same either way, but the layer matters for knowing what else
  is affected.
- Which it was: `build_qemu_argv()` had `argv[0] = "qemu-system-x86_64"`,
  resolved through the *daemon's* PATH. Two backends, one cause, and the
  qemu one is unambiguously ours.
- `/usr/bin`, then PATH -- and **NULL when nothing is found**, which
  leaves each backend exactly as it was. Naming a path
  that does not exist would turn a working default into a domain libvirt
  refuses to define; that is the failure mode a "safe" fallback would
  have introduced.
- One system root, not a list. `/usr/libexec` was in the first draft on
  the strength of "some builds land there", which is a claim rather than
  a fallback -- and Enterprise Linux's `/usr/libexec/qemu-kvm` is a
  different *name*, so that directory would not have reached it anyway.
  A root nothing has ever been found in is a stat that reads as coverage.
- `clawt_vm_emulator_path()` takes its search roots as a parameter so the
  preference is tested against a directory the test made rather than
  against whatever this machine has installed -- and the test forks,
  because proving a preference over PATH means setting PATH, and by then
  the suite has spawned subprocesses.
- One existing test asserted `argv[0] == "qemu-system-x86_64"`, i.e. it
  pinned the bug. Found by the full run after the sabotage pass, not by
  reading. **A test can encode the defect as the intention**, and the
  only thing that catches it is running everything.

### The control that caused the mistake was the one telling you to fix it

- The warning above was right and the dialog was wrong. `defaults.mounts`
  offers **one** field in both graphical clients -- "teams or agents" --
  and the GTK client sorted it itself by asking `team.list`. That reports
  the teams somebody **declared**; an agent can perfectly well be on a
  team nobody declared, which the sidebar already draws and the context
  menu already offers an entry for. So every such name was filed under
  `agents:`, matched nothing, and the folder reached nobody.
- Which means the *first* fix shipped a warning that told somebody to
  hand-edit YAML to work around a control that had put the wrong thing
  there. **A warning about a mistake a control makes is a bug report
  about the control.** It was found by the person reading the warning
  next to the dialog that produced it, not by any check here.
- `clawt_mount_sort_scope()` is in the library beside
  `clawt_mount_covers()`, and the daemon does the sorting from a `who`
  list both clients now send. The two answer the same question -- a team
  is one that is declared *or* one an agent names -- so the sorter cannot
  write something the validator then complains about, and there is a test
  asserting exactly that.
- The web client had the other half: **two** boxes, so somebody typing a
  team id into "Agents" got the same silent nothing. `docs/computers.org`
  had said "the graphical clients take one field and sort it out for
  you" the whole time -- true of one client, and true of neither's
  implementation.
- An unknown name goes to `agents`, not `teams`. Guessing "team" would
  silently widen a folder to a group somebody may be about to create;
  as an agent id it matches nothing and the validator names it.

### Two lists, and an id in the wrong one matches nothing

- `defaults.mounts` takes `agents:` for agent ids and `teams:` for team
  ids. A team id written under `agents:` matches nothing -- deliberately,
  and the schema said so: "an id that names no agent is ignored rather
  than refused", because an agent removed for the afternoon must not stop
  the fleet starting.
- That silence is right for the case it was written for and wrong for the
  case that happens. The folder reaches nobody, **every agent that was
  meant to get it starts perfectly**, and the whole symptom is a code
  reviewer with no source tree -- discovered by the agent, which has no
  way to know a folder was ever intended. Nothing anywhere connects the
  two.
- Reported, not refused, like the team rules. Each warning names the
  entry by its target, says which list the id belongs in, and stops
  there: the fix is one word and the diagnosis was the entire cost.
- The summary sentence is **suppressed when a per-id warning already
  fired**. An entry naming one bad id has been explained; adding "and it
  reaches nobody" underneath would put the useful line second.
- `fleet_has_team()` counts a team as existing if it is declared *or* if
  an agent claims to be on it, because `clawt_mount_covers()` matches on
  the string the agent carries. A warning that contradicted the rule
  would send somebody to fix the file that was already right -- the team
  validator is what reports a missing declaration, and saying it twice in
  two vocabularies is worse than saying it once.

### An agent and the machine it runs commands in are different things

- The sidebar's right-click menu had Start, Stop and Restart for the
  *agent*; the machine underneath had no control anywhere. A libvirt
  domain outlives the daemon and a container with `keep: true` outlives
  everything, so "the machine is up and the agent is not" is the ordinary
  state, not an edge case -- and the only way out of it was `virsh` by
  hand.
- The verbs go in a **Computer (container)** submenu rather than beside
  the agent's own. Flat, "Stop" and "Stop machine" is a guess every time;
  under a named submenu the bare verbs are unambiguous. Absent rather
  than greyed out for an agent with no machine -- three permanently dead
  entries are three things to read past on every right-click, and an
  empty `GMenu` section draws as nothing.
- Which types have a machine is `clawt_computer_type_has_machine()` and
  the daemon reports it per agent as `computer_machine`. A client
  answering it from a list of its own would offer Stop on a backend added
  later, or fail to, with nothing to say which -- the same rule as the
  colour schemes and the computer types already in the parity check.
- **`clawt_computer_stop()` now refuses by default**, like teardown. It
  went through `CALL_OR_TRUE`, so a backend without one would report the
  machine as stopped and leave it running -- which is the same lie one
  function down, in front of somebody watching for it to go. `null` and
  `host` say so in two lines each.
- `container_stop()` set the state to STOPPED *before* reading podman's
  answer, and `distrobox_stop()` did too. And podman's 404 for a
  container that is not there was reported as a failure to stop, so
  pressing Stop on a machine that had already gone produced an HTTP
  status and a URL. Not there is stopped.
- The wait is `clawt_computer_lifecycle_async()`, in the library. "An
  IPC handler must not wait on the network" has now been applied at
  **five** call sites, and every time it was applied at the site rather
  than to the function the next caller inherited the bug.
- Stopping a container **destroys** it -- `computer.container.keep` is
  false by default -- so the daemon refuses without an explicit `remove`
  and both clients warn first. Both, deliberately: the fence protects a
  client that does not know to warn, the dialog stops the fence being an
  error message somebody has to decode.
- The reply says `removes` (this machine does not survive a stop), not
  `removed` (one was destroyed just now). A computer built from the
  config knows nothing about a machine it did not start, and the first
  draft announced that a container nobody had ever created was gone.
- Both clients name the three frames as **literals** rather than
  `g_strconcat("computer.", verb)`. `make parity` reads the kinds each
  client mentions, and an assembled one is invisible to it -- so the
  check reported OK with the feature in one client only. Verified by
  breaking one and watching it fail.

### A duplicate JSON member is silent, and the last one wins

- `add_agent_object()` already emitted a `computer` string. Adding the
  new facts as an object under that same name gave the reply two members
  called `computer`, and json-glib keeps the **last** -- so the object
  was silently discarded and the only symptom was one client reading a
  string where an object was expected. Flat `computer_machine` and
  `computer_stop_removes` beside it instead, which is also the file's
  existing convention (`vm_cpus`, `desktop_enabled`).
- Same family as "a member in the wrong object is still valid JSON",
  already in this file. Count the members as well as the braces: the
  builder will happily write a name twice.

### `make docs-check` cannot see an agent-relative config key

- It scans `=key=` org markup and looks the name up in the schema.
  Agent-relative keys are written `~computer.vm.image~` throughout the
  docs -- tilde, not equals -- so **every `computer.*` name in the tree
  is unchecked**, which is most of them. A `=computer.vm.emulator=`
  written by habit is what exposed it: it failed, correctly, for a key
  that does exist, because the schema spells it `agents.computer.vm.emulator`.
- Widening the scan to `~...~` was tried and reverted. The tilde is also
  how the docs write IPC frame kinds (`task.list`), filenames
  (`seed.iso`) and tool names, so it produced **18 false positives in one
  run** -- and a check that cries wolf is one people learn to ignore.
  Telling a config key from a frame kind needs more than a regex, so the
  gap is written down here rather than half-closed.

### A combo box cannot say "something else"

- The screen-size row offers the common resolutions, and a value that is
  not among them is **inserted into the list** rather than dropped.
  Without that, `index_of()` returns 0 for an unknown value, the row
  opens showing the first entry, and saving the page -- without touching
  that row -- writes it back over whatever somebody had chosen. Same
  shape as the appearance lesson: a control that cannot represent the
  current value will silently replace it.
- And the daemon has to *report* the value for the row to show it.
  Adding a config option, wiring it through both VM backends and never
  putting it in the editor is the same "no caller" gap as the desktop
  factory -- the option worked and was unreachable from the place
  somebody would look for it.

### A setting somebody set once makes a first-run failure invisible

- `defaults.libreclaw_binary` was unset by default and fell back to
  `PATH` alone, so a fresh clone failed at the first agent start with
  "the libreclaw binary is not on PATH" -- while the binary sat in
  `deps/libreclaw/build/release`, built minutes earlier by the same
  `make` that produced the daemon doing the complaining.
- Invisible here for months because this machine's config sets the key to
  an absolute build-tree path. Anybody who ever set it, or ever installed
  libreclaw, cannot reproduce it; anybody cloning fresh hits it
  immediately. **A key somebody set once and forgot is a config that
  answers for a machine rather than for the software.**
- The fix already existed one directory over: `clawt-pod-bridge.c`
  resolves pod modules beside the running binary, then in the install
  location, with a comment recording the identical bug -- "missing for
  anyone running straight out of a checkout, which is everyone, until the
  first `make install`". The binary needed the same treatment and had
  never had it.
- The refusal now names all three places it looked. "Not on PATH" sent
  somebody to install something they had already built.
- Verified by reading `/proc/<child>/exe` rather than grepping `pgrep`
  output -- twice in a row a `pgrep -af libreclaw` matched the *probe's
  own command line* and then a different daemon's child, and the second
  one looked exactly like a pass. That trap is already in this file; I
  still fell into it. Read the kernel's answer, not a string.

### A command-line variable reaches every sub-make, including the deps

- The `deps` target pins `DEBUG=0` with a comment saying deps use release
  output whatever we are building -- and said nothing about the
  sanitizers. make passes command-line variables down, so
  `make DEBUG=1 ASAN=1` built **libreclaw's release tree** with ASan
  linked in, which is the one tree `LIBRECLAW_OUTDIR` points at
  unconditionally.
- The next ordinary `make` then failed inside libreclaw's own GIR
  scanner, which runs a binary it has just linked: *"ASan runtime does
  not come first in initial library list"*. Nothing in that message
  mentions sanitizers being on, or a previous build, or the dep -- and
  the failing target is `Lc-1.0.gir`, several layers from anything the
  person changed. One documented command (`make DEBUG=1 ASAN=1 clean
  all`) left the tree unable to build until somebody guessed to clean
  the dep.
- `ASAN=0 UBSAN=0` beside the `DEBUG=0`. Pinning one of three knobs is
  the same gap as writing two of a distribution's three file names:
  it reads as "we pin the build type" and is not.
- Verified by running the sequence that broke it -- ASAN build, then
  plain build -- and watching both exit 0.

### A limit nothing increments is a limit that cannot fire

- `orchestration.task_budget_usd` has been **enabled by default at $5.00**
  since the schema was written, generated into `data/*.yaml`, documented
  in `docs/`, and checked by `clawt_loop_guard_check()` on every routed
  message -- while `clawt_loop_guard_record_spend()` was called by
  **nothing outside a test**. So the counter the check reads was
  permanently zero and the one limit built to stop an expensive loop
  could never once refuse anything.
- Exactly the shape of the `max_hops` bug already in this file, and found
  the same way: grep for the writer, not the reader. A limit needs a test
  that *reaches* it. `/usage/budget-refuses-when-spent` does, and it
  caught its own first draft -- three identical message bodies were
  refused by the **cycle detector** instead, which would have passed
  while proving nothing about the budget.
- Charged from `on_link_message()`, which is where an agent's turn
  actually ends. Drained on **every** reply, not only the ones carrying a
  task id: the drain is what advances the watermark, so skipping the
  untasked turns would bank them and hand the whole accumulated bill to
  whichever task happened to be answered next.

### Every agent had been recording its own cost, and nothing had ever read it

- libreclaw writes one `token_usage` row per AI turn -- tokens and cost
  in micro-dollars -- into each agent's own database, and has since
  0.24.0. Nine agents on this machine had 105 turns and $1.10 between
  them, on disk, invisible to every clawtilla surface. The first question
  an operator running a paid fleet asks had no answer anywhere in the
  product.
- Read through libreclaw's own API rather than by opening its schema, the
  same rule `/reset` already followed -- so there is no copy of that
  column list here to fall out of step with a table we do not own. The
  test fixture writes its rows through `lc_database_add_token_usage()`
  for the same reason: one that spelled the table itself would keep
  passing after libreclaw changed it.
- A missing database is **zero, not an error** -- and must not be opened,
  because opening a sqlite database creates it, and asking what a stopped
  agent cost should not leave one behind.

### libreclaw ignores `database.path`, so clawtilla was naming a file that never existed

- Its sqlite backend builds the filename from `session.persist_dir`, so
  the database is at `<state_dir>/sessions/libreclaw.db`. clawtilla
  rendered `database.path` as `<state_dir>/libreclaw.db` and `/reset`
  then tested for that -- a path that has never existed on any machine.
  The session-clearing branch was therefore skipped **every single time**
  and `sessions_cleared` was always 0.
- It looked like it worked because moving the sessions directory aside
  takes the database with it. So the "two places to clear" this file
  already documents was only ever clearing one, by luck. A branch that
  never runs and a branch that always succeeds are indistinguishable from
  the outside.
- One `clawt_usage_database_path()` now, used by the renderer, by
  `/reset` and by the usage reader. It was two spellings that made this
  possible, and the second one was never checked against a real machine.

### The provider states what a turn cost; libreclaw was recomputing it

- `lc_app_record_usage()` priced the two reported token counts through
  its rate card and discarded the CLI's own `total_cost_usd` -- which
  ai-glib had already parsed and was carrying on the usage *event* that
  nothing in libreclaw's session path listens to.
- The recomputed figure is not merely different, it is **structurally
  low**: a CLI backend bills cache reads and cache writes, and neither
  appears in `input_tokens`. An agent with a persistent session has a
  warm context on every turn after the first, so the gap grows with the
  context -- worst exactly where somebody is watching the bill. A real
  row on this machine: 4 input tokens, 278 output, priced at $0.004182,
  which is the rate card to the micro-dollar and nowhere near the truth.
- Fixed at the source: `ai_response_set_cost_micros()` in ai-glib,
  carried on `LcSession`, preferred over the price table in libreclaw.
  The table stays for providers that report nothing, and `-1` means
  unknown rather than free -- zero would be indistinguishable from a turn
  that genuinely cost nothing, and a fleet summing those would report
  spending nothing at all.
- Verified by reverting the setter and watching the test fail
  (`-1 == 20400`), not merely by watching it pass.

### A response body that is a C string cannot carry a PNG

- htmx-glib's `HtmxResponse` held its body as a `gchar *` and wrote it
  with `strlen()`. That is right for HTML and silently truncates
  anything binary at its first zero byte -- for the eight-byte PNG
  signature, at **two bytes**. So an image served through it arrives
  broken at a length that depends on the picture, which is the worst kind
  of wrong: it works for some files.
- `htmx_response_set_bytes()` upstream, with bytes winning over content
  when both are set, and a test whose sample has a NUL at index 2 on
  purpose. Verified end to end by fetching a real PNG over HTTP and
  running `cmp` against the original.
- The attachment itself follows the rule the issue named: **the daemon
  copies the bytes at send time**, because a path only resolves when the
  client and the file are on the same machine and the failure when they
  are not looks like a broken image rather than an unsupported setup.
  The id is checked, not trusted -- it arrives in an IPC payload and
  becomes a filename.
- No `attachments` on the agent-to-agent verbs, deliberately. The
  receiving side is an agent whose `read` and `bash` run on the host, so
  it would get an id it can do nothing with -- the exact shape of missing
  feature an agent invents a workaround for and reports as a discovery.

### Two of 89 toasts were notifications, and only those two moved

- The complaint was that toasts cover the composer, and the first answer
  was to rescope the overlay. Classifying all 89 `clawt_window_toast()`
  call sites first showed why that would have been wrong: **two** are
  notifications -- a failed download and a refused message, both in
  `on_daemon_event()` -- and the other 87 are form validation and request
  results. *"Saved."*, *"An agent needs an id."*, *"That is not a port."*
  Those answer a question somebody is holding right now, in a dialog,
  and filing them into a drawer would bury the one line that matters.
- So the split is by **origin**, not by severity: arrived on its own →
  the panel; answers what you just did → stays a toast.
  `clawt_window_toast()` is unchanged and so are its other 87 callers.
  Grouping the sites by *enclosing function* is what made the
  classification cheap -- and re-checkable after a rebase, which reading
  89 strings is not.
- The panel is a second `AdwOverlaySplitView` **inside** the first one's
  content, not wrapping it: opening alerts must not hide the agent list,
  because that is navigation. 320px is derived rather than conventional
  -- 281 + 600 + 320 = 1201, so on an ordinary window all three fit and
  the transcript does not move when the panel opens. The breakpoint is
  1200px for that reason and no other.
- **`ClawtEventLog` had been recording every event since the daemon was
  written and was read back by nobody.** `event.list` is the whole gap
  between "a file nobody can see" and a history the panel can page into
  -- one handler, and it is what turns "diagnose a message loop with
  sqlite3 on the host" into a control.
- Verified by driving a real daemon: the GTK panel drew 2 rows filtered
  and 20 unfiltered from the same 20 events, and the web page 2 and 24,
  with dismiss and clear-all checked over HTTP. The GTK check needed an
  env-gated probe that opened the panel and counted the rows it drew,
  because GNOME refuses `org.gnome.Shell.Screenshot` here.
- One self-inflicted bug worth the note: `self->alerts` was created in
  `forget_daemon_state()` rather than in `_init()`, so it was NULL until
  the first daemon switch and **nothing was ever recorded**. The probe
  printed nothing at all, which is what gave it away -- a feature that
  produces no output is indistinguishable from a feature nobody
  triggered.

### A window nobody is presenting does not lay out, so it does not scroll

- The reported symptom was "sending a message does not reliably bring the
  view down", and two hypotheses had already been tested and disproved
  under instrumentation. Re-running it against a real compositor found
  the shape immediately: a message appended, `following` TRUE, the widget
  `mapped`, and `upper` never changing -- so `notify::upper` never fires
  and `on_transcript_grew()`, which is the only thing that actually
  works, never runs.
- `gdk_frame_clock_get_frame_counter()` in the trace is what named it:
  **2, 3, 3 and stopped**. GTK4 lays out from the frame clock, so with no
  frames there is no allocation, and `mapped` stays TRUE throughout --
  every widget looks healthy while no layout pass ever runs. Reproduced
  on the X11 backend too, where the counter never left 0.
- The earlier test could not have shown it: it drove
  `gtk_adjustment_set_value()` directly, so the clock was never a
  variable. **A synthetic driver removes the thing you are testing when
  the thing you are testing is the platform.**
- **I claimed it self-heals on presentation before testing that**, and
  the test did not support the claim: `gtk_window_present()` produced no
  layout, and a heartbeat showed the counter and `upper` frozen for
  twelve seconds afterwards. What that actually proves is only that the
  window was never presented -- which is also why the ticking-clock case,
  the one the operator is in, could not be reached from here at all.
  Saying "it self-heals" without a trace was the mistake; the honest
  statement is that no defect was found in the machinery and the case
  that matters is not reproducible in this environment.
- The idle scroll was left alone for the same reason. Both the reporter's
  five traces and mine show it never does work `on_transcript_grew()` had
  not already done -- but removing it is a behaviour change in a path
  neither of us can exercise, and changing what you cannot test is how
  the untested case becomes the broken one.
- What *is* now tested is the predicate it all turns on:
  `clawt_transcript_is_at_bottom()`, on both sides of the 32px tolerance
  and at its boundary. It was arithmetic inside a signal handler and
  could not be tested at all.

### A rule both clients apply belongs in the library, and then it is testable

- The unread rule (four conditions) and the alert tier (six kinds) were
  each written twice, once per client. Neither sends an IPC frame nor
  answers a slash command, so `make parity` could not see them --
  precisely the blind spot that check already has recorded against it,
  and precisely the shape that drifts.
- Moved to `clawt_unread_should_count()` and
  `clawt_alert_tier_for_event()`, and the second one takes the **event**
  rather than a kind and a loose string: which detail decides the tier
  varies per kind, and a caller passing the wrong one would classify
  silently and wrongly. That change also found a real bug on the way --
  classifying `image.finished` on its kind alone made every *successful*
  download an error.
- The payoff is that both are now exercised by
  `tests/test-client-rules.c` without a window, a browser or a daemon:
  twelve cases including the ones nobody would drive by hand, like an
  event with no timestamp and a kind the daemon grows later.
- Declaring them in the parity table made the check earn its keep
  immediately: the old row named `CLAWT_WEB_ALERT_ROUTINE`, which stopped
  existing the moment the web client started using the library's enum,
  and `make parity` failed on exactly that.

### Two row builders for one kind of content drift, and only deletion stops it

- The chat transcript and the Flow tab each built their own message row.
  The chat gained runs, avatars, day dividers and a measure; Flow stayed
  a flat list of captions. Two visibly different renderings of the same
  messages, and a reader moving between them saw two conventions.
- Fixed by deleting one, not by teaching the second the same tricks --
  which is the only version that cannot drift again. What genuinely
  differed turned out to be a five-field struct: the box, the run state,
  and how the other party is drawn.
- It also went the other way: the chat had never shown the task chip or
  the hop count, both of which Flow had since it was written -- and both
  of which the *web* client already drew in its chat. Unifying the
  builders closed an asymmetry between the two clients that `make parity`
  cannot see, because neither chip sends a frame or answers a command.

### GLib pads `%e` with a figure space, and `g_strstrip` cannot reach it

- `g_date_time_format(when, "%A %e %B")` renders "Wednesday" then an
  ordinary space then **U+2007 FIGURE SPACE** then "5" -- so a
  single-digit day comes out looking double-spaced, on the nine days a
  month it happens, and stripping the result does nothing because the
  padding is in the middle of the string. `%-d` is the fix.
- Found by the test asserting the string rather than the shape, on its
  first run. A test that had checked "contains the month name" would have
  passed for ever.

### Where a run begins is one rule, so it lives in the library

- Both clients group consecutive messages from one sender into a run, and
  two implementations of "is this a new run" would differ exactly once,
  on the case nobody looked at. `clawt_chat_run_is_start()` and
  `clawt_chat_day_label()` are pure, in libclawt, and tested without a
  window -- which is also the only way the year-boundary case gets
  covered, since a comparison on day-of-year alone reads 1 January as a
  jump backwards rather than as a change.
- The measure was **measured**, in the browser, from its own text
  metrics: 52rem rendered 117 characters a line against a comfortable 45
  to 90, and 40rem renders 89. The GTK client's `AdwClamp` defaults were
  already right and are left alone -- a default is a number the platform
  can revise, a hardcoded one is a number somebody has to maintain.
- The redesign asked for 18px between runs. The tree already had 30,
  measured against the 27px a markdown paragraph break costs, and that
  measurement has not changed -- so the measured number stands and the
  drawn one does not. A spec that cannot see the rendered output is a
  proposal, not a finding.
- Contrast was checked in all three palettes through the page's own
  computed styles rather than by eye: the operator's bubble is 5.67:1 in
  light, 8.29:1 in dark and 8.34:1 on Catppuccin, because every colour is
  a token rather than a hex value.

### The badge that said "something happened elsewhere" counted the wrong queue

- The sidebar drew an accent number from `mailbox_depth`, tooltipped
  "messages waiting", and the event handler's own comment called it "what
  tells you something happened elsewhere". The intent was right and the
  wire was wrong: the depth is the *agent's* inbound queue -- work
  waiting for **it** to read -- which is close to the opposite. An agent
  that has just answered you has depth 0 and showed nothing, while one
  buried in peer traffic showed a large number and had said nothing to
  you at all. It was also already written out in the row's subtitle, so
  the number was a duplicate of the wrong thing.
- The count is keyed by **agent**, not by room, because a client only
  learns a room id for an agent it has already opened -- and the agent
  this exists for is precisely the one nobody has opened. The daemon
  reports `dm_room` beside every agent so no client takes `dm:a:b`
  apart; `clawt_room_manager_direct_id()` is the one spelling, used by
  the listing and by the creation.
- **The reporter's design said `to` was the user's id and told me to read
  it rather than trust the paragraph.** It is the *room* id --
  `clawt_message_get_room_id()` -- so the rule built on it would have
  counted nothing. The instruction to check was worth more than the fact.
- **Replayed events are not counted.** A client subscribes from cursor 0
  and the daemon replays; counting those opens a fresh window already
  showing a number for a conversation nobody has touched, and makes the
  count depend on whether the replay beat the first fleet listing.
  Replayed events keep their timestamps, so one comparison against the
  connect time settles it.
- Verified by driving a real daemon and a real client rather than by
  reading: a temporary `g_message` in the row builder printed 0 across
  the replay and 2 after two live messages, and the web client was
  checked the same way with `curl` against the sidebar fragment. The
  first attempt proved nothing because it sent into `dm:beta:user`, a
  direct room that **did not exist yet** -- they are made on demand, and
  the daemon refused with "there is no agent or room called ...". An
  end-to-end test that fails for a reason unrelated to the feature reads
  exactly like the feature not working.
- The libadwaita half was measured, not assumed: a 120-line probe against
  real GTK4 showed the unread row's title at weight 700 against 400 for a
  plain one, and `adw_view_stack_page_set_badge_number()` +
  `set_needs_attention()` both taking. Neither could be screenshotted --
  GNOME refuses `org.gnome.Shell.Screenshot` and wlr-screencopy is not
  there -- so the check is the widget's own answer rather than a picture.

### One handler taught to report a failure teaches nobody else

- `render_all_agents()` warned and carried on, which is right -- one
  agent's bad block must not hold the fleet hostage -- and returned
  nothing, so its seven callers each told their caller the operation had
  succeeded. `control.reload` was given a `refused` array and the other
  six were not, so `agent.set` wrote a key to clawtilla.yaml, answered
  `{"agent": ...}`, and left the agent running on the config.yaml it
  already had. Rendering is the *whole point* of the call there: a
  refusal means it accomplished nothing and said it had.
- The convenience is what made it possible, so it is gone. Every caller
  passes a refusal array now and there is no wrapper that discards one;
  a comment where it used to be says why. A helper whose only job is to
  drop an error is a trap with a name.
- Reported once per client, in one place each: the CLI has a
  `report_refusals()` beside `member_or()`, the GTK client checks in
  `clawt_window_request()`, and the web client records it on the app and
  turns the banner in `clawt_web_after_action()`. A handler that starts
  carrying refusals later is covered without anybody remembering to
  look -- which is exactly what did not happen the first time.
- The sentence itself is `clawt_ipc_reply_refusal_text()` in libclawt.
  Three clients writing their own would be three answers to "what does
  this mean", and the one nobody reads would be the one that is wrong.
- `clawtillad` is the caller with nobody to answer. It warns once per
  agent and once in summary rather than refusing to start.

### `make tests` did nothing at all, and exited 0

- CLAUDE.md has told people to run `make clean tests` for as long as the
  zero-warning rule has existed. There was no such target: make answered
  *"Nothing to be done for 'tests'"* and exited 0, so a documented check
  for warnings compiled nothing and passed. Found while chasing a test
  that would not pick up an edit -- the same shape as "`make test` does
  not relink the daemon", except here the *documented* command was the
  broken one.
- A phony alias now, beside `test`. Worth the habit: run a build command
  from the docs and read what it built, not what it exited with.

### A doc naming a tool nobody built is worse than a doc naming none

- `docs/computers.org` promised `clawtilla_computer_put_file`, `get_file`
  and `exchange_list` for a long time, and none of the three were ever
  built. `docs/orchestration.org` and `docs/configuration.org` both told
  people to allow or deny `clawtilla_spawn_agent`, which does not exist
  either -- the real key is `tools.manage_fleet`. A reader trusts the
  name, tells their agent to use it, and the agent reports, accurately,
  that there is no such tool.
- `make docs-check` now fails on any `clawtilla_*` named in `docs/` or
  `README.org` that is not registered in `clawt-mcp-tools.c` -- the same
  shape as the stale-config-key check that already existed, and it would
  have caught all four. A trailing underscore is skipped, because
  `clawtilla_memory_*` in prose is a family rather than a name.
- The general rule: whenever a doc can name something the code owns,
  that naming is checkable, and a check beats a habit. Config keys were
  already covered; tools were the obvious second set and nobody had
  looked.

### A permission about *assigning* must say that talking is still allowed

- Teams give an agent a lead who may hand it work; a member may hand work
  to nobody. Stated as a bare refusal, an agent reads "you cannot
  delegate" as "you are cut off from your peers" and stops messaging them
  at all -- which is the opposite of what a fleet is for. Every refusal
  from `clawt_team_may_assign()` therefore says what is *not* restricted,
  and names where to send the work instead. A refusal that only says no
  gets retried in a different shape.
- The rule is a pure function over two `ClawtAgentConfig`s, so it is
  exercised without a daemon, a fleet or a running agent. A permission
  check that needs all three to test is one that gets tested once.
- Two gates, not one: the *tool* is offered only to an agent that can
  assign to somebody (chief or lead), and the *target* is checked at call
  time, because which targets are allowed depends on the target rather
  than on the caller. Neither alone is enough — the tool list cannot know
  who is about to be named, and a member offered a tool it can never use
  will keep trying it.
- Fleet-level mistakes -- two leads on one team, an agent naming a team
  nobody declared -- are **warnings**, not errors. A fleet is edited by
  hand and half-built states are ordinary; refusing to start over one
  would be far worse than saying so. They are printed by `team list`, and
  shown in the client, because the symptom otherwise is work quietly
  going nowhere.

### Grouping belongs to whoever already decides the order

- The sidebar groups agents by team by emitting a header whenever the
  team changes, which only works because `agent.list` returns them
  *grouped*. Letting the client gather them would be a second answer to
  what order the fleet is in, and the two would differ the first time a
  team was reordered.
- Teamless agents sort **first**, because that is where the chief of
  staff lives. Last would bury the agent somebody talks to most under
  every team in the fleet.
- An agent on an undeclared team still appears, under that team's id.
  Hiding it is how a typo in `agents.team` survives being looked at.
- Which teams are collapsed is client-side, like the fonts: it belongs to
  the person at that screen, not to the fleet. The *tally* on the header
  is what makes collapsing safe -- a folded team still says what is
  behind it -- and it is counted from the same reply the rows are built
  from, so the two cannot disagree by one.

### An order belongs where the thing being ordered lives

- `agents.order` is in `clawtilla.yaml`, not in the client, because it
  is about the agents rather than about *reaching* them -- which is the
  line that puts connection profiles and appearance on the other side.
  So a chief dragged to the top is at the top in every client and on
  every machine.
- Numbered in tens so one agent can be placed between two others by
  hand, and sorted **stably** so a fleet nobody has dragged -- every
  agent at the default 0 -- comes back in the order the file has it.
  `g_ptr_array_sort()` is documented stable since GLib 2.32.
- The drag carries the **id**, never the widget. A row is rebuilt from
  the daemon's reply on every refresh, and a refresh can arrive
  mid-drag, because events are delivered from an idle -- so a pointer to
  the row being dragged may not exist by the time it is dropped.
- The drop sends the *whole list*, not "move this one here". The daemon
  numbers from what it is given, so one frame describes the arrangement
  completely and a stale client cannot produce a half-applied reorder.
  An id the daemon no longer has is skipped, not refused: the alternative
  loses the arrangement over an agent that was removed a second ago.
- Dropping on the upper or lower half decides above or below. Without
  that a row can never be placed last, because every drop lands before
  something.

### A tidy-up with no undo needs a fence, not care

- `agent rm --purge` removes a workspace, a state directory and a set of
  transcripts, and every one of those paths comes from configuration
  somebody edits. `clawt_remove_tree()` takes the root it must stay
  inside and refuses anything else -- checked on the *canonical* path, so
  a symlink or a `..` cannot carry it out, and checked per child against
  the same root rather than against its parent, since a symlink pointing
  out of the tree is only visible from there.
- The files go before the config entry does. Every path is derived from
  that entry, and afterwards there is nothing left to derive them from --
  the same ordering the computer teardown already needed.
- Opt-in, and both clients say which half happened: removing an agent
  from the fleet is reversible and deleting what it wrote is not, so
  "removed" and "kept, because ..." are different sentences.

### Saving a setting did not rewrite what the setting produces

- `agent.set` wrote clawtilla.yaml and stopped, so nothing the agent
  reads was touched. Every setting that only matters at the *next* start
  hid this; `tools.manage_fleet` exposed it, because the gate answers
  from the live config and was right immediately while `TOOLS.org` went
  on listing the tools as they stood at the last daemon start. Two
  answers to "what do I have", and the file is the one in the prompt --
  so a chief-of-staff that had just been granted the tool went on saying
  it had none.
- It calls `render_all_agents()` now. And the reply carries
  `restart_required`, because an AI CLI lists its tools **once**, when
  its session starts: a permission granted under a running agent reaches
  its files and not its session, and the agent then reports, accurately,
  not having the tool. Both clients say so.
- Three rounds of "still broken" came from this, each one me telling
  somebody to flip a switch that was already flipped by the time they
  read it. When the answer to a bug report is an instruction rather than
  a change, check whether the instruction is one the software should not
  have needed to give.

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
  all. So `agent rm --with-computer` on a VM took the success branch,
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

### Moving blocking work to an idle fixes when it runs, not where it waits

- `clawt_daemon_start()` started every autostart agent inline, before any
  main loop existed. For a container agent that is a blocking podman
  request each, so on ~30 agents the daemon sat in there for minutes
  answering no IPC frame and dispatching no signal source: `agent list`
  hung with no reply, `kill -TERM` did nothing, and systemd's stop timed
  out, escalated to **SIGABRT** and dumped core -- so the agents were
  SIGKILLed rather than stopped.
- The rule was already written down as "an IPC handler must not wait on
  the network -- **nor may daemon start**", and the comment recording it
  is four lines below the loop that broke it. It had been applied to the
  caller somebody noticed (the model cache) rather than to the function.
  **A rule about a function that is enforced at one call site is a rule
  about that call site.**
- Moving autostart to an idle source is the obvious fix and is **half of
  one**. An idle that calls the blocking start directly still holds the
  loop for the length of the call: measured against a podman socket that
  accepted and went quiet, 60 seconds an agent, with all three symptoms
  unchanged -- from a version that looks fixed and whose unit tests pass.
  The wait itself has to leave the thread.
- So `clawt_daemon_start_agent()` is split at the one step that waits on
  somebody else's socket. `start_agent_prepare()` and
  `start_agent_launch()` stay on the main thread and own all the state;
  only `clawt_computer_start()` goes to a `GTask` worker. The sync entry
  point keeps all three in a row on purpose -- every caller of *that* one
  has somebody waiting on the answer.
- The test that matters asserts on **an unrelated timeout source still
  being dispatched**, not on anything about the agent. A test phrased in
  terms of the fleet cannot tell the two versions apart;
  `/daemon/loop-runs-while-a-computer-provisions` fails against the
  idle-only one, which is the whole reason it exists.
- Verified against a real daemon and a real mute socket, because that is
  the only thing that could have caught the half-fix: `agent list` 14ms
  (was a 30s timeout with no reply) and SIGTERM honoured in 1s (was still
  running after 40).

### g_task_new() captures the thread-default, and dispatching a source pushes nothing

- A `GTask` created inside a source callback takes whatever context was
  thread-default on that *thread*, which is the process default unless
  somebody pushed one -- GLib does not push a source's own context on the
  way into its dispatch. So the autostart task completed on a loop the
  daemon never runs, and the whole fleet sat queued having plainly been
  scheduled. Push the context explicitly around the `g_task_new()`.
- Same family as the timers and the idle already in this file, one API
  along, and it will keep happening: **anything that captures "the
  current context" is a bug in an embedded daemon unless it was told
  which one.**
- `g_task_return_boolean()` from the calling thread still completes in an
  idle, which is what lets the no-work path stay asynchronous without a
  thread. Relying on that is deliberate: a synchronous answer there would
  run the whole fleet inside one callback.

### A runtime must wait on the context its child's exit will arrive on

- `process_runtime_stop()` asked for `g_main_context_get_thread_default()`
  from its own call stack, while `g_subprocess_wait_async()` had captured
  a different one at start. On `clawtillad` both are the process default
  and it worked by luck; the moment a start ran from a GTask callback --
  which pushes the task's context -- the exit arrived somewhere stop()
  was not looking, so **every clean shutdown waited out the full grace
  period and SIGKILLed a child that had already gone.** The runtime holds
  the context now.
- It surfaced as `pid 0 did not stop within 5 seconds`, and the 0 is the
  tell: `g_subprocess_get_identifier()` returns NULL once the child has
  been reaped, so the message was reporting a process that had already
  exited.

### Four ways a test of this passed for the wrong reason

- **The pod module was not on the test binary's search path**, so the
  container backend refused before opening a socket and start was fast
  for a reason unrelated to the fix. It passed against a deliberately
  broken daemon. Tests that need a real module take
  `CLAWT_TEST_POD_MODULE_DIR` and **skip** when it is absent.
- **A "mute" server that unrefs the connection is not mute** -- it hangs
  up, which is answered instantly. Hold the connections.
- **`g_setenv()` does not reach an agent's child.** The process runtime
  builds the environment from an allowlist, so the fake agent never got
  its sleep, exited at once, and every agent read as STOPPED -- which
  looks exactly like autostart never running. Per-agent `env:` is the
  route.
- **A fixed count of non-blocking iterations proves nothing** once work
  is on a thread: they burn through while the worker is still running.
  Pump against wall time.

### A thread parked in accept() outlives an autoptr listener

- `g_socket_listener_accept()` blocks, so dropping the last reference to
  the listener at the end of the test hands the thread a finalised
  object. Invisible in an ordinary build; under ASAN it is
  `g_socket_accept: assertion 'G_IS_SOCKET (socket)' failed`, printed
  *after* the last test reported ok, which reads as a suite-level fault
  rather than as one test's teardown. Cancel and `g_thread_join()`
  before the listener goes.

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
- The mirror image bites the *revert-proof*: plain `make` builds the
  library but does **not** relink `build/release/tests/*`. So sabotaging
  a fix, rebuilding, and watching the test still pass proves nothing —
  the binary is the old one. Three people hit this in one session, and a
  pass is exactly what a working sabotage looks like from outside. Build
  the specific binary (`make build/release/tests/test-foo`), and read the
  build output rather than its exit status: a `>/dev/null` that hides a
  compile error produces the same false pass.

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

### An AdwActionRow made its own activatable widget recurses until it segfaults

- The fix for a dead row is an *activatable widget*, and it has to be a
  **different** widget. `adw_action_row_set_activatable_widget(row, row)`
  sets `activatable` to TRUE, so the row looks fixed -- and activating it
  calls `gtk_widget_activate()` on itself, forever. Measured on real
  libadwaita: 2502 `gtk_root_get_focus` criticals and then SIGSEGV. The
  chevron works and returns cleanly.
- Which made the connectors list crash the client on the first click,
  while the integrations and routines lists next to it were plain
  AdwActionRows that did nothing at all. Three lists, three answers, two
  of them wrong in different ways -- so there is one
  `row_opens_something()` now and it adds the chevron *and* sets it,
  because a row that opens something should also look like it does.
- The property to check is `gtk_list_box_row_get_activatable()`. It is
  FALSE on a plain AdwActionRow, which is the whole bug, and a
  four-line program against real libadwaita answers it in a second --
  far quicker than reasoning about what the widget "should" do.

### A check finds the layer it looks at, and only that layer

- `make parity` compared frame kinds, then grew a slash-command layer
  when thirteen commands went missing under a green check. It then
  reported OK through **two more** gaps -- a palette in libclawt
  selectable in one client, an unread marker built for one transcript --
  because neither sends a frame or answers a command. Three layers, three
  times the same lesson.
- It compares five now, and the two that need no bookkeeping are the ones
  worth having. A `_count()`/`_nth()` pair in the public API is the
  library saying "here is a set, walk it", so if either client walks one
  both must -- that is the palette shape exactly. And a client that
  *spells out* a value the library enumerates is holding a copy of the
  list, which is the cause rather than the symptom: the check fires on
  the copy whether or not the new value has gone missing from it yet.
  Values are read from the library's own table, so one added there is
  checked from the moment it exists.
- The fifth layer needs declaring and says so. Some capabilities are pure
  interface -- a rule and a pill -- and touch no shared symbol at all.
  A row naming a marker per client catches one half being removed or
  never written, and cannot catch a feature nobody declared. Better to
  have it and state the limit than to pretend the check is complete.
- Prefix exceptions were considered and rejected. `clawt_appearance_*` as
  one entry would collapse thirteen legitimate asymmetries into a line --
  and would have silenced the exact bug being fixed.

### Two ways a grep-based check reports the opposite of the truth

- **Comments are text too.** This codebase explains itself at length, so
  the comment saying why the stylesheet must spell `data-theme="light"`
  contains "light", and the one about the fallback contains "system".
  The first run of the hardcoded-vocabulary check reported two
  hardcodings that were not there. A check that cries wolf is one people
  learn to ignore, so it strips `/* */` with an awk state machine before
  matching -- exact, because that is the only comment form the codebase
  allows.
- **`cmd | grep -q` under `set -o pipefail` fails on success.** grep
  exits the moment it matches, the upstream process dies of SIGPIPE with
  141, and the pipeline takes the worst status -- so every declared
  marker that was *present* reported missing. Write to a file and grep
  the file. It was three findings that made no sense that gave it away,
  not reading the code.
- Both were found by sabotaging the thing being checked and watching:
  the palette gap reproduced, the marker removed. A parity check that has
  never been shown to fail is a parity check that reports OK.

### A capability in the shared library still has to be reachable from both clients

- Catppuccin Mocha went into `clawt-appearance.c`, which *both* clients
  link, and was selectable in one of them. Each named the schemes itself
  -- the GTK combo listed four, the web select listed three -- so the
  palette existed in the library, shipped in both binaries, and a web
  reader could not choose it. `make parity` reported OK throughout: a
  colour scheme sends no IPC frame and is no slash command, which is the
  blind spot that check already has recorded against it.
- Fixed by deleting both lists. `clawt_appearance_theme_count()` /
  `_nth()` / `_nick()` / `_label()` is the one list, and each client
  builds its control by walking it, so the next palette reaches both
  without either being edited. The same rule as the config schema, one
  layer over: **never write a hand-maintained list of an option's
  values.**
- The colours are shareable and the mechanism is not -- GTK redefines
  libadwaita's named colours, the web sheet has its own tokens -- so the
  palette is written twice on purpose, in the vocabulary each renders in.
  The web block must come *after* the dark ones: a palette carries
  `data-theme` too, so `:root:not([data-theme="light"])` matches it,
  specificity is identical, and source order is the only thing deciding.

### A handler in the page head cannot reach document.body

- It is `null` there, so `document.body.addEventListener(...)` throws and
  takes every later handler in the same script with it. The unread-marker
  script registered four and got none; the page looked completely normal,
  because everything it does is invisible until something arrives. One
  line in the console said so on the first real load, and nothing else
  would have. Listen on `document` -- htmx's events bubble to it, and it
  exists before the body does.
- Two more in the same script, both found the same way and neither
  visible by reading it. htmx swaps the transcript with `outerHTML`, so
  **the element the listener was bound to is gone after the first
  arrival** -- a `scroll` handler bound directly to it survives exactly
  until the moment it is needed, and `scroll` does not bubble, so the
  delegated version has to be `capture`. And the swap leaves the browser
  nothing to restore the scroll offset from, so a reader who had scrolled
  up was thrown to the top of the conversation on every fleet event --
  the exact opposite of what declining to auto-scroll is for.
- All three read as correct. What found them was driving a real browser:
  scroll up, send a message, assert on the pill, the rule, and the
  offset. A feature whose whole subject is where the viewport is cannot
  be verified from the markup.

### A thread carries more than the answer

- `on_link_message()` completed a task on **any** message carrying its
  thread id, and libreclaw puts more than the answer in a thread: a
  progress note every five minutes (on by default for every channel that
  is not email), a guardian refusal, a restart notice. So a routine
  reported `completed` within seconds of starting, its result was the
  string "⏳ Still working... (5m elapsed)", and the work itself ran for
  minutes afterwards against a task nothing was waiting on. **A state
  that says finished while the work runs is worse than no state**:
  anything polling it gets a false positive and stops looking.
- Fixed at both ends, and they are different defences rather than a
  shotgun. The note is no longer generated -- `progress_enabled: false`
  in the rendered `channels.clawtilla` block, which costs nothing because
  the same turn already raises the typing indicator that both clients
  draw as a live activity line. And the daemon no longer acts on a
  mid-turn message: libreclaw brackets a turn with that indicator and
  drops it in `on_process_message_finish()` **before** the answer is
  posted, so anything arriving while the agent is still busy is by
  construction not the answer.
- An agent that never raises the indicator (it needs a room and is
  skipped without one) completes exactly as before. That is the safe way
  round on purpose: **a task that ends late is a delay, one that ends
  early is a lie.**
- The first reproduction reported RUNNING and looked like the report was
  wrong. The test binary had not been relinked against the library --
  the trap this file already records for the daemon, one directory over.
  `strings <binary> | grep` on the probe string is what settled it, not
  reading the code again.

### A routine has never had a session of its own

- `docs/routines.org` promised one -- "one morning's run never
  contaminates the next" -- `run_routine()` asserted it in a comment, and
  `clawt_task_new()` built `clawtilla-task-<id>` with a comment saying
  that was how it happened. **Nothing outside a test has ever read that
  string.** Three statements of a fact, no implementation, and the shape
  is already in this file twice: the factory nothing called, the limit
  nothing incremented.
- The reason it cannot work as built is in libreclaw and is deliberate:
  `lc_router_resolve_session_key()` keys on channel, room and sender and
  carries a note saying the thread is *intentionally* excluded, because
  there a thread anchors a Matrix reply rather than dividing a
  conversation. (Its own doc block above that note still describes
  appending the thread -- worth fixing upstream; the code is what runs.)
  A routine is sent from `user` to the agent, so it lands in the
  operator's room, from the operator's sender, in the operator's session
  and its queue.
- So a routine inherits the last conversation's context and waits for
  whatever the operator is doing. Documented as the constraint it is:
  point routines at an agent that is not also a conversational surface.
  Delivering the isolation means routing a task into a room of its own,
  which takes its output out of the operator's transcript -- a product
  decision, not a repair, so it is not made here.

### A group with nothing in it has no heading, and a heading is the drop target

- The sidebar drew a heading when the *team changed between two agents*,
  so an empty team had none -- and once a heading became the thing an
  agent is dragged onto, that made the feature unreachable exactly where
  it is wanted: a team created a minute ago in Settings could not be
  filled by dragging, and a fleet where everyone had a team had no "No
  team" heading to drag back out to. A gesture that works in one
  direction only reads as broken rather than as narrow.
- Both clients emit the missing headings now, in the daemon's own group
  order -- teamless first, then `team.list`'s order, which is the array
  `group_position()` indexes into. Not a second opinion about the order:
  the two would differ the first time a team was reordered, which is the
  same reason the client does not gather the fleet into groups itself.
- The spelling of "no team" cost a wrong sidebar in **both** clients,
  from the same cause. `clawt_json_string(agent, "team", NULL)` answers
  NULL for an agent whose config never had the key, and NULL was also
  the sentinel for "flush every remaining heading" -- so all four
  headings were drawn in a row above the whole fleet. Only for an agent
  that had *never* been on a team: one taken off a team has `team: ""`
  and looked perfectly correct, which is why the first probe run showed
  it on one line and not on the four below it. The web copy had it too,
  one layer along: `""` matches no declared team id, so the loop ran to
  the end instead of stopping. **When a sentinel and a real value can
  both be absent, they need different spellings.**

### A drop target on a row still fires when a button fills the row

- The team heading is a `GtkListBoxRow` whose whole area is a
  `GtkButton`, and the drop target is on the row -- because a target on
  the button would miss the strip of row around it, which is where
  somebody aiming at a heading lets go. Whether the button swallows the
  drag is the load-bearing question, and it was measured rather than
  reasoned about: `GtkDropTarget` is BUBBLE phase, and picking the
  widget under a point in the middle of the button and walking up gives
  `GtkLabel -> GtkButton -> GtkListBoxRow`, with the button carrying
  only `GtkEventControllerKey`, `GtkGestureClick` and
  `GtkShortcutController` -- none of which handle a drag event.
- `gtk_list_box_drag_highlight_row()` for the hover feedback, not a
  class of our own. It is what every list on the desktop uses and it
  carries the theme; a hand-rolled colour would be right in one theme
  and wrong in the other, in an application whose whole appearance
  system exists because people change theirs. Unhighlight on the *drop*
  as well as on leave -- a drop does not promise a leave after it, and a
  heading left lit under a row that has just moved away reads as the
  drag still being in flight.
- Dropping among another team's agents has to change the **team**, not
  only the order. The daemon returns the fleet grouped, so an agent
  merely reordered under somebody else's heading is sorted straight back
  under its own on the next redraw: a drop that visibly worked and then
  undid itself, which reads as the sidebar being broken.

### GtkPopoverMenu follows a submenu model filled after it was built

- Which is what lets the sidebar's Team submenu be an empty `GMenu` at
  construction and be refilled on every right-click, from the fleet the
  sidebar last saw, instead of rebuilding the whole popover per click or
  asking the daemon between the press and the menu appearing. Not
  obvious, and the defensive alternative is the expensive one -- so it
  was measured rather than assumed: a 120-line program against real GTK4
  fills the submenu after `gtk_popover_menu_new_from_model()` and finds
  all three entries in the popover's widget tree.
- The same program answered the other two questions in the same second.
  A stateful action with a string parameter renders its items as radios
  (`GtkModelButton:role` is 2), exactly one is `active` -- the one whose
  target matches the action state -- and activating an entry hands the
  action the *target*, so the menu is built from team **ids** while the
  labels are names. A menu offering a choice has to say which one is
  currently taken; without the state a person would open the inspector
  to find out where they were before deciding where to go.
- A team an agent names but the fleet does not declare gets its own
  entry. Without it the tick lands on "No team", moving the agent away
  looks like it did nothing, and the typo in `agents.team` -- the reason
  somebody opened the menu -- is the one thing the menu cannot show.
  Same rule as the screen-size row: a control that cannot represent the
  current value will silently replace it.
- Verified end to end afterwards, not only in the probe: a temporary
  `g_message` in `fill_team_menu()` against a real daemon printed the
  entries and the tick for a teamless agent, one on a declared team, and
  one on an undeclared one. The probe proves the widget; only the real
  fleet proves the wiring, and this file records more than one feature
  that worked perfectly in isolation and reached nobody.

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

### A size request is a floor, so a child in a box sets the box's width

- The continuation times in the chat gutter needed right-aligning into a
  rail. Packed into the gutter box with `set_size_request(CHAT_AVATAR)`
  and `xalign 1.0` the rail comes out perfectly straight -- and **every
  body in the run moves three pixels**, because a `GtkBox` measures from
  its children and the request is a floor rather than a cap: an `HH:MM`
  caption is 35px against the 32 the arithmetic assumed. The run header
  stays at 44 and its own messages start at 47, which is precisely the
  invariant the gutter exists for.
- An **overlay** child is the fix: it does not contribute to the
  measurement, so the time costs no width, and one too wide for the slot
  overhangs into the row margin instead of moving anything. The web
  client had already arrived at the same place from CSS with
  `position: absolute` and the comment "so it costs no width that was
  not already reserved" -- the answer was written down in the other
  client.
- Measured, three variants, ink right edges of `00:00 23:14 11:11 18:48`
  and where the body column lands: packed left `34 31 22 31` / body 44;
  packed right `34 34 34 34` / body **47**; overlaid `31 31 31 31` /
  body 44. **A label's allocation is not the rail** -- with `xalign` the
  ink sits somewhere inside it, so the measurement has to be
  `gtk_label_get_layout_offsets()` plus the layout's ink extents, in the
  root's coordinates. My first probe reported label bounds and made all
  four look identical *before* the fix.
- And the first probe loaded the `tnum` stylesheet in both builds, so
  its "before" was not one. **A before/after harness has to be checked
  for what it holds constant**, not only for what it varies.

### A timestamp rendered on the server is wrong before it arrives

- The web transcript stamped messages "2m ago" while the GTK transcript
  stamped the clock, so one conversation carried two conventions
  depending on which client it was opened in. The clock won on more than
  consistency: nothing re-renders a message that has not changed, so a
  page left open goes on saying "2m ago" for an hour -- **a relative
  time is only true at the instant it is generated, and a server
  generates it once.** Recency belongs to the views that are *about*
  recent activity, and the mailbox, task and alert lists keep theirs.
- `clawt_chat_time_label()` in libclawt, called by both clients in both
  places each draws a stamp -- four spellings before. Declared in
  `make parity` as an affordance, and the row was proved to fire by
  reverting the web caller to its own `g_date_time_format`.
- 24-hour is load-bearing rather than a preference: the stamp is drawn
  in the slot the avatar reserves, and a 12-hour locale renders
  "4:23 PM", which does not fit it. The test asserts the absence of
  `AM`/`PM` as well as equal lengths, because a length check alone
  passes on `"1:01 AM"` vs `"12:00 AM"`... and would fail for the right
  reason only by luck.
- **A message with no timestamp gets no stamp**, in both clients. Both
  fall back to the current time for run *grouping*, which needs a day
  whatever happens -- inheriting that fallback into the stamp would put
  a plausible wrong time on a record.

### A relative timestamp does not fit a slot sized for a clock

- The web transcript's gutter time was "46s ago", which **wraps to two
  lines in the 28px the avatar reserves** -- so a run of continuations
  drew a column of broken two-line stamps, and the longer the
  conversation the longer the strings got. `HH:MM` is four digits and a
  colon whatever the age of the message, which is the property that slot
  needs and the reason the GTK transcript had always used it there.
- Found only by measuring the rendered element in a real browser. The
  markup, the CSS and the width are all individually correct; the
  mismatch is between a string that grows and a box that does not.

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

### A palette is a list, so the thing to check is what is not on it

- The Catppuccin table named 37 of libadwaita 1.9's colours and the
  comment above it said "the full set rather than the handful that are
  obviously wrong without it". There are **51**. Every name a palette
  omits keeps stock GNOME's value, so the symptom is one widget in the
  wrong grey in a window that is otherwise perfect -- and nothing,
  anywhere, says which colour or which widget.
- Found through the alerts panel, which is a second `AdwOverlaySplitView`
  inside the first one's content. libadwaita styles panes by *position in
  the widget tree*: a `.sidebar-pane` inside a `.content-pane` is a
  nested sidebar and is painted from `--secondary-sidebar-bg-color`,
  which no palette here had ever heard of. Measured: `#28282c` in a
  window whose agent sidebar was `#181825`.
- **The nesting was a layout decision and libadwaita read it as
  hierarchy.** The panel is inside the content so that opening alerts
  does not hide the agent list; it is a peer of the agent sidebar, not a
  sidebar of it. `.isolated` says exactly that, and it is the half of the
  fix that works under *every* scheme -- on stock GNOME dark the same
  panel was `#28282c` against `#2e2e32`, so the mismatch was never
  specific to a palette.
- `--active-toggle-bg-color` is the sharper half. libadwaita has no
  `@define-color` behind it at all -- it exists only as a custom
  property, literal `rgb(255 255 255 / 20%)` -- so no amount of
  redefining named colours reaches it, and the checked toggle stayed
  white in a Catppuccin window. It is written in the palette's own
  dialect anyway, because the `@define-color` → `--token` bridge is what
  keeps a palette one list; the inert `@define-color` it also produces is
  cheaper than a second list to maintain.
- The list is `required_colors[]` now and `make test` checks the built-in
  palettes against it. Deliberately **not** checked for a palette on
  disk: the two-line example in `docs/clients.org` is a supported,
  documented partial palette, and warning about it would be crying wolf
  at the case the feature exists for.
- Nothing hermetic can notice libadwaita growing a token, so that is
  written down rather than pretended about -- `gresource extract
  /usr/lib64/libadwaita-1.so.0 /org/gnome/Adwaita/styles/gtk.css` and
  grep its `var(--*)`. That is where all 51 came from; recalling them
  produced 37.
- Verified by rendering, not by reading: `gtk_widget_paintable_new()` on
  the window's content, then `gsk_renderer_render_texture()` to a PNG, on
  the real client with the panel open. **The paintable has to exist
  before the frame that fills it** -- created afterwards it snapshots to
  a NULL render node, which is indistinguishable from a broken renderer.
  And the frame clock has to actually tick: `wait_for_frames()` polling
  `gdk_frame_clock_get_frame_counter()` beats any number of
  `g_main_context_iteration()` calls, which left it at 0 for ever. Same
  trap as the transcript-scroll case already in this file.

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


### `make clean-all` cleaned six of fifteen deps and reported success

- The vendored tree is **fifteen submodules three levels deep** --
  libreclaw brings five, podomation brings six of its own, ai-glib
  brings one -- and each level's clean named only its *direct* children.
  So `clean-all` reached six, and the nine below them were reused by the
  next `make all`.
- It also pinned `DEBUG=0` for podomation and htmx-glib, so a debug
  build survived every clean there had ever been: **166 object files,
  counted**, sitting in `podomation/build/debug` after `make clean-all`
  said it was done.
- And every recursion was `$(MAKE) -C ... clean 2>/dev/null || true`, so
  a dep whose clean failed, or whose submodule was never checked out,
  was indistinguishable from one that cleaned perfectly. Same family as
  `make tests` printing "Nothing to be done" and exiting 0.
- The list is **found** now, not written: a project root is a directory
  with a `Makefile` whose parent is named `deps`. That predicate returns
  exactly the fifteen `git submodule status --recursive` does, and
  excludes podomation's per-module Makefiles under `modules/`, which are
  not standalone. Found by walking rather than by asking git, so it
  works in an exported tree.
- A missing Makefile is an uninitialised submodule and is skipped in
  silence; a clean that *runs* and fails names the dep and the build
  type and fails the target. Verified both ways -- 976 artifacts to 0,
  and a deliberately broken dep clean reported by name.

### A guard that tests for presence is not testing for workability

- `test-export` skipped unless `g_find_program_in_path("pandoc")` found
  something, which answers *"there is a file called pandoc on PATH"* --
  a different question from *"running it will convert something"*. On
  this machine `pandoc` is a **distrobox shim**: a two-line shell script
  that starts a container. So `make test` needed podman, reached a
  registry, and on a host without that image already built **prompted
  `[Y/n]` and waited for a person for ever**.
- A test that can hang is worse than one that fails. A failure names
  itself; a hang is indistinguishable from a slow machine or an infinite
  loop somewhere else, and it stops every test after it, so a
  green-looking partial run cannot be told from a complete one.
- The three tests that actually spawn pandoc are behind
  `CLAWT_TEST_INTEGRATION` now. The fourth stayed hermetic on purpose: it
  runs only when pandoc is *absent*, where `clawt_export_convert()`
  refuses before spawning anything.
- **The hermetic claim is checkable, and was not being checked.**
  `unshare -rn <test-binary>` runs it with no network at all, which is
  the actual rule this file states. The whole suite passes that way --
  874 ok, nothing hung -- and `test-export` was the one thing that did
  not, having been green on every ordinary run for as long as the
  developer's toolbox container happened to exist.

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


### A stream that delivers its first event and then stops

- htmx-glib's SSE never worked, and looked like it did. The handler's own
  inline event arrived; nothing after it ever did. libsoup takes the
  response body's length at the moment the handler returns as the
  Content-Length, writes exactly that many bytes and finishes -- so the
  fix is one line, `soup_message_headers_set_encoding(headers,
  SOUP_ENCODING_CHUNKED)`, and without it a browser sees a server that
  pushes one event and dies.
- It was also unreachable from a route at all: `handle_request()` applies
  whatever the handler returned, and applying a response *replaces*
  Content-Type and appends a body -- so an SSE handler's
  `text/event-stream` was overwritten before the first event. Returning
  NULL is no better; that is the 404 path.
  `htmx_response_new_streaming()` means "the handler owns this message"
  and `htmx_response_apply()` leaves it alone.
- Measured both ways against a real client: five later events arrive with
  the encoding line and none without it. The test asserts on events
  produced *after* the handler returned, deliberately, because an event
  written inline arrives even when streaming is broken -- which is
  exactly how this stayed hidden.

### A signal with three ways to fire must fire once

- `HtmxSseConnection::closed` reaches the same end from the peer hanging
  up, from soup finishing the message, and from our own close. Emitting
  it from the close path *and* letting soup's `::finished` emit it again
  meant a subscriber list told twice that one connection had gone, which
  removes somebody else's entry with the second. All three go through one
  door now. The test asserted "exactly once" and caught it on the first
  run.

### A library that does not check is a library trusting somebody else's version

- `htmx_router_serve_static()` built a filename from the request path and
  read it, with no containment check at all. Nothing had ever got through
  -- libsoup normalises a literal `../` out of the request and answers an
  encoded one with 400 -- but that is a guarantee made by a version of
  libsoup nothing pins, about the one function whose whole job is turning
  a request into a filename. The check is on the *canonical* path and on
  the separator after the root, so `/srv/static-secrets` does not count
  as inside `/srv/static`.
- Testing it needed `htmx_request_new_for_path()`, which did not exist:
  routing could only be exercised by standing up a server and talking to
  it over a socket, so the cases worth testing hardest -- the ones a real
  client cannot send -- could not be tested at all.

### `setsid` means `$!` is not the process you started

- Three rounds of "the fix did not work" came from testing a *stale*
  server: `$!` after `setsid` is the setsid process, which has already
  exited, so the kill hit nothing and the old binary went on serving.
  `readlink /proc/<pid>/exe` said `(deleted)` -- the binary had been
  replaced under it. Kill whoever the kernel says holds the port
  (`ss -ltnp`), and read `/proc/<pid>/exe` to confirm what is running.
  `pkill -f` is worse: its pattern matches the invoking shell's own
  command line, and it killed that instead. Both traps are already in
  this file for `pgrep`; they apply to every one of its relatives.

### Emitting a heading when the key changes needs the keys grouped

- The sidebar gets away with it because the daemon returns the fleet
  already grouped. The config schema does not: it is in the order the
  generated YAML wants, which interleaves `agents.persona.*` with the
  bare `agents.prompt_suffix` -- so the web inspector drew two cards both
  called Identity with the fields split between them. Collect into
  buckets and render, or sort first; do not assume an order nobody
  promised.

### Two clients drifting apart is invisible

- Nothing breaks, nothing warns, and somebody finds out by reaching for
  the half that was not built. `make parity` compares the set of IPC
  frame kinds `clients/gtk/*.c` and `clients/web/*.c` each send and fails
  when one has a capability the other lacks. It found ten features the
  web client was missing and four the GTK client was -- dead letters,
  purge, requeue and cancelling a task, all of which the GTK client had
  simply never grown.
- A frame kind is a proxy for a feature rather than a definition of one.
  But every real feature has to talk to the daemon to do anything, so a
  kind one client sends and the other never mentions is a capability in
  one place only. Deliberate exceptions live in an exception map with
  their reasons; an entry there is a decision, not a to-do.

### The agent-relative spelling of a config key is not in the schema

- `agent.show` reports every settable key with its value so a client can
  build an editor from the schema rather than from a list of its own.
  Nine keys are left out, and cannot be included: `orchestration.mailbox.*`
  becomes `mailbox.*` inside an agent block while `memories.enabled`
  keeps its whole name, and both rules live in hand-written tables in
  `clawt-agent-manager.c` and `clawt-config.c`. Guessing produced a
  collision -- `memories.enabled` and `agents.enabled` both landing on
  `enabled`, with json-glib keeping the last.
- Reporting a guessed name would give an editor whose fields are
  accepted, reported as saved, and read from nowhere: the exact failure
  the "walk the schema" rule exists to prevent, arrived at from the other
  direction. Leaving them out and saying why is the honest answer until
  those two tables are folded into the schema.

### A CDN script on a fleet console is an open door

- The page that serves `clawtilla-web` can start, stop, reconfigure and
  delete every agent on the machine and run commands inside their
  computers. A script fetched from a third party on every load can change
  all of that. htmx-glib's own examples all point at unpkg; clawtilla
  vendors the file instead, with its checksum recorded in
  `data/web/README.org`.
- It is also the difference between working and not working on a tailnet
  with no route to the open internet -- which is the case the client was
  built for.


### A catch-all route swallows everything registered after it

- `/a/:id/:view` matches every path under an agent, and the router
  answers with the first pattern that matches -- so `/a/x/export`,
  `/a/x/copy` and `/a/x/file` were all being served by the *view*
  handler. Nothing failed: an unrecognised view falls back to chat, so
  each one rendered the chat page and returned 200, and the export even
  came back with a `Content-Type` of `text/html` that read as a
  formatting problem rather than a routing one.
- It is registered last now, from its own `clawt_web_register_views()`,
  with the reason in the header beside it. And routing is checked against
  a running client by `make web-smoke`, which asserts on a *marker string
  per route* rather than on the status -- a status can only tell you
  something answered, not which thing.

### An error message borrowed from the client is gone by the time it is drawn

- `clawt_web_app_last_error()` returns a string the app owns, and every
  `clawt_web_app_call()` frees it and writes a new one. Rendering a page
  makes half a dozen calls -- so a handler that failed, then built the
  page, then reported the error, was reporting whatever now lived at that
  address. A refusal about a path traversal came out as *"Which CLI
  backend answers, and with what"*, which is a static string from the
  inspector's group table.
- The same shape as the CLI's `id` pointing into a freed reply, and just
  as invisible: it printed *something* every time. Copy at the point of
  failure, before anything else asks the daemon anything. The header says
  so now, and a scan for "a render between the call and the report" found
  five more sites after the first.

### One header name, one value -- except the one whose job is to repeat

- `htmx_response_add_header()` is backed by a hash table, so five
  `Set-Cookie` headers arrived as one and the other four were silently
  not set. The appearance page stored the last field and lost the theme,
  the fonts and one size, with nothing to say so.
- `htmx_response_append_header()` added upstream for headers that may
  repeat. `add_header` keeps replace semantics, which is right for
  `Content-Type` and wrong for `Set-Cookie`.

### A test that #includes a source needs it as a prerequisite

- `tests/test-web-render.c` includes the web client's renderers, because
  they live in a binary rather than in libclawt. The generic test rule
  lists only the test's own `.c`, so editing `web-ui.c` left the test
  binary untouched -- and *passing*.
- Which meant the check that matters most was itself broken: reverting
  the fix to prove the test catches it produced a pass, twice, because
  the binary was never rebuilt. The Makefile names the included sources
  now. Same family as "`make test` does not relink the daemon", one
  directory over, and the reason it was found at all is that the
  revert-and-confirm step is a habit here.

### A denylist for a CSS value misses the one that matters

- The font family comes out of a cookie and is spliced into a
  declaration. The first sanitiser dropped `{}<>;"` -- which stops a
  quote closing the string, and lets a comment-opener through, and a CSS
  comment swallows the rest of the sheet. An allowlist of letters,
  digits, space, hyphen, underscore, stop and comma loses nothing: no
  font has a brace in its name.
- The general rule: when the safe set is small and known, name the safe
  set. A denylist is a claim to have thought of everything.

### Parity checked at one layer finds nothing at the others

- `make parity` compared IPC frame kinds and reported OK -- while the web
  client had five of the GTK composer's eighteen slash commands and none
  of its font settings. `/files`, `/agents` and `/export` are built out
  of frames both clients already sent, so a frame-level check cannot see
  them missing.
- It compares commands as well now, and that check fails if one is
  removed -- verified by removing `/retry` and watching it exit 1. The
  appearance gap is still only caught by reading, which is worth knowing:
  a check finds the layer it looks at.

### mailbox.purge is per-agent, and both clients thought otherwise

- The daemon resolves a mailbox from the payload's agent and refuses
  without one -- "which agent's mailbox?". Both clients sent no agent,
  and the GTK button's own tooltip said it swept "every mailbox in the
  fleet". So a button existed in two clients for an operation the daemon
  has never offered.
- Found by a smoke test asserting on the *reply*, not the status: the
  refusal came back as a rendered page with 200 on it.


### A relationship the schema could not state cost nine settings

- `orchestration.mailbox.max_depth` is `mailbox.max_depth` inside an
  agent; `memories.enabled` keeps its whole name; `computer.type` takes
  its default from `defaults.computer`. None of that is derivable, and it
  was written down in two private tables -- `configure_mailbox()` in
  clawt-agent-manager.c and `inherited[]` in clawt-config.c. Between
  them they were the only thing that knew an agent's spelling for a fleet
  option, so `add_agent_settings()` had to skip all nine, and neither
  client could offer them. They worked when typed into the file and
  existed nowhere else.
- `clawt_config_schema_agent_keys()` states it once, beside the schema,
  because it *is* schema: it says where an option may be written. Both
  tables now read it, and `clawt_config_schema_agent_name()` gives the
  daemon and both clients one answer to "what is this called on an
  agent" -- they had three, which is how the daemon reported an option
  the web client did not draw.
- A field on `ClawtSchemaEntry` would have been tidier and was not worth
  it: `-Wextra` warns on a short positional initialiser, so one more
  field means touching all 250 rows. The test is the guard instead --
  a PER_AGENT key with no entry fails `make test`.

### Three getters, three different wrong answers to the same lookup

- Fixing the above exposed what "no `agents.mailbox.overflow` row" had
  been doing. `get_string` fell back to the fleet key and was right;
  `get_enum` looked only under `agents.` , found nothing, and returned 0
  -- which for the overflow policy is `reject`, so an agent asking for
  `block-sender` got the opposite, silently; `get_string_list` never
  consulted the fleet value at all, so `memories.readers` set fleet-wide
  reached no agent.
- One resolver now, `agent_schema_entry()`, used by all of them. Three
  copies of "find the schema entry for an agent-relative key" is the
  same shape as the two key tables, one level down.
- And `configure_mailbox()` read `overflow` from the fleet alone while
  the schema had flagged it PER_AGENT since it was written. A flag that
  says an agent may override an option, and code that ignores the
  override, is a promise nothing keeps -- and the test that now writes
  every relation key into an agent and reads it back is what makes that
  checkable rather than noticed.

### Do not `git checkout` a file to undo a test mutation

- Reverting a deliberate break to confirm a test catches it is a habit
  worth keeping; `git checkout <file>` is the wrong tool for it when the
  file has uncommitted work, which during a change is always. It threw
  away the relation table minutes after it was written. Copy the file
  aside and copy it back.


### A setter that writes the wrong node type is accepted all the way down

- `agent.set` called `clawt_agent_config_set_string()` for every key,
  which writes a scalar -- and `node_to_strv()` returns NULL for anything
  that is not a sequence. So a list was accepted by the daemon, echoed by
  the CLI, written into clawtilla.yaml, and read back as the schema
  default. Every surface agreed it had been saved.
- Nine keys under `agents.` are lists, and the sharp one is
  `computer.host.deny_paths`: denying `~/.ssh` through `agent set` was
  accepted, saved, and denied nothing. A confinement setting that reports
  success and confines nothing is worse than one that is missing.
- The handler dispatches on `entry->type` now, and
  `clawt_agent_config_set_string_list()` builds a real sequence. The
  comment on `clawt_agent_config_add_mount()` had said mounts were the
  only list an agent holds -- true when written, false from the day
  `persona.identity_files` arrived, and nobody re-read it. **A comment
  asserting "this is the only X" is a claim that expires.**

### Two spellings of the same file never collide, so nothing warns

- clawtilla names identity files in org; a workspace from anywhere else
  names them in markdown. `clawt_workspace_scaffold()` correctly never
  overwrites -- and it tested for `SOUL.org` against a directory holding
  `SOUL.md`, found nothing, and wrote a blank set beside the real one.
  The blanks are what `identity_files` defaults to, so the agent loaded
  seven templates saying "(fill in)" with its actual persona in the same
  directory.
- An import reported every file copied and produced something wearing
  the right name with no character. The fix is two halves: the scaffolder
  writes no identity file that nothing will load, and the importer adopts
  a markdown persona it finds -- and says which files, because an agent
  that silently changed what it reads is its own problem.
- Both halves ask *one* function what an agent loads.
  `clawt_workspace_effective_identity_files()` is used by the scaffolder
  and the renderer, because a file one writes and the other never reads
  is exactly how a workspace fills up with things nobody looks at.

### Two directories that are the same by default hide which one is meant

- `skills.dir` was built from the state directory, grouped with
  `session.persist_dir` and `database.path` under a comment explaining
  why *those* must be per-agent. Skills are authored content and belong
  in the workspace; the two are the same directory for every agent that
  does not set `agents.workspace`, so the mistake was invisible until one
  did -- and then `skills.dir` named a directory clawtilla never creates
  and nothing writes to, and the agent silently had no skills.
- The string appeared exactly once in the tree, at the point of use. That
  is the tell: a path nothing else ever constructs is a path nothing else
  ever creates.

### An identity change needs a session cleared, not a restart

- An AI CLI is not handed a system prompt when it *resumes* a session, so
  editing an identity file or repointing `persona.identity_files` reaches
  a running agent's files and not its prompt. Restarting is not enough.
  `agent.reset` is what applies it, and it existed only in the GTK client
  -- from a terminal there was no supported way to apply an identity
  change at all. `clawtilla agent reset` now exists.
- `agent.set` reports `restart_required` for `persona.*` as well as
  `tools.*`, and the CLI gives different advice for each, because the
  remedy differs: a tool list is read at session start, so a restart is
  enough; a resumed session is never handed a prompt, so only clearing it
  works.

### Hiding a build behind >/dev/null makes a revert-proof meaningless

- Confirming a test fails without its fix is only worth doing if the
  sabotage compiled. Three times this session a `make ... >/dev/null 2>&1`
  hid an error, the old binary ran, and the "proof" was a pass. The first
  attempt at proving the list fix used `yaml_node_new_scalar()`, which
  does not exist. Read the build output, or at minimum grep it for
  `error` -- the same rule this file already records for linker errors
  hidden by a diagnostics filter.


### A probe answers a question about now; a lock answers one about ownership

- Three daemons ran against `~/.clawtilla` in one evening. Each keeps
  its own `ClawtRoomManager`, and `save_room()` rewrites the **whole**
  transcript from memory on every message -- so the last to flush wins
  and the others' messages are gone. Four delivered messages of a real
  conversation with the chief of staff were deleted that way, after
  being routed correctly and answered.
- Recoverable **only** because `events/*.ndjson` is append-only and had
  them. The transcript is a projection and the event log is the record,
  and the two are worth telling apart *before* the day one of them loses
  something.
- The socket was already guarded -- by a **connect probe**, which
  answers "did anything reply just now". A busy daemon fails that
  question: a connect is refused when the accept queue is full, and one
  blocked on its main thread is not accepting at all. So the probe
  unlinked a **live** socket and left the daemon running and
  unreachable, which is exactly what `clear_stale_socket()`'s own
  comment says must not happen.
- **A guard that can be wrong in the permissive direction is worse than
  no guard, because being wrong is an action.** With no guard somebody
  sees "address in use" and looks into it. With that one, the check
  removed the service and the evidence in a single step, and the
  symptom -- a daemon nothing could reach -- pointed at the socket
  rather than at the second daemon.
- `flock` on `<state_dir>/daemon.lock`, taken before anything in there
  is read or written and released in `clawt_daemon_stop()`. The kernel
  holds it and drops it when the last descriptor closes, SIGKILL
  included, so there is no stale lock to reason about and nothing to
  clear by hand. Released explicitly rather than at process exit,
  because an embedded host stops and starts the daemon in one process.
- The **state directory** carries it, not the socket. The socket is
  only one of the things two daemons fight over, and the easiest to be
  configured not to share. The test that proves the lock changes the
  socket path and nothing else -- the existing one shared the whole
  config, so the socket probe refused it and it could not have told you
  whether the lock worked at all.


### A helper that consumes its argument, beside one that does not

- `clawt_web_add()` unrefs the child; `htmx_node_add_child()` does not.
  Two adjacent blocks in one change used one each, and the one paired
  with a `g_autoptr` was unreffed twice and **freed while still in the
  tree** -- so the drawer's checkbox rendered as nothing while the
  hamburger three lines away in another file went on being drawn and
  toggling an element that did not exist. From outside that is
  indistinguishable from a working drawer until somebody presses it.
- Every assertion in the change was against the **stylesheet**, and the
  CSS was correct throughout. **A stylesheet test cannot see a missing
  element.** The test that catches it renders the page and looks at the
  emitted HTML, and it fails with the underlying cause in the message:
  `g_object_unref: assertion 'G_IS_OBJECT (object)' failed`.

### A row count cannot describe a layout whose item count changes

- The narrow grid was given `auto auto 1fr` for what is at most two
  children -- and a `display:none` child is **not a grid item at all**,
  so with the drawer shut there was one item, it took the first `auto`
  track, and 320px of an 812px phone below the composer was blank.
  Placing both children explicitly is the only version that is right for
  both counts.
- `minmax(0,1fr)` rather than `1fr` for a row holding a scrolling box:
  `1fr`'s own minimum is min-content, so a long transcript pushes the
  track past the viewport instead of scrolling inside it.
- Found by rendering. The change had been measured in a headless browser
  against the stylesheet extracted from `web-style.c` with hand-written
  markup, which is a different program from the one the client serves.

### A test that names the fix refuses the next correct answer

- A sweep asserted every `minmax()` floor begins `min(`, which was the
  spelling that change had applied. `minmax(0,1fr)` is a *stronger*
  floor and failed it immediately. The rule is that the floor can shrink;
  the test says that now, and says which spelling is for what.
- Same family as the `render_autologin()` lesson: a test that encodes
  "we do it this way" cannot tell you there is another way.

### `g_test_expect_message()` makes every *other* message fatal too

- Provoking one warning on purpose does not mean expecting one message.
  With an expectation pending, the daemon's ordinary MESSAGE and INFO
  lines during start each became fatal in turn -- three rounds of a test
  failing on a different innocuous log line. `test-daemon.c` already had
  the right idiom: swallow warnings with a handler and restore
  `g_log_set_always_fatal()` around the call.

### A skip-probe is code, and it can break the rule the suite is built on

- Two tests decided whether to skip with
  `clawt_pod_bridge_load_module(bridge, "container", NULL)`. That names
  no connection, so it **starts the module's event source against
  whatever podman the machine has** -- measured with strace,
  `/run/podman/podman.sock`, twice per run. `make test` needs no podman,
  and the line that broke the rule was the one whose only job was to
  decide whether the test could run.
- Load for the connection the test itself stands up. Which then made the
  test ten seconds faster and exposed a dep bug underneath it.

### An uninterruptible sleep in a thread that must be joinable

- podomation's container module waited `reconnect_delay_ms` between event
  stream reconnects with `g_usleep()`. `stop()` cancels and then joins,
  so a cancel arriving during the backoff was not noticed until it ran
  out: **five seconds to dispose the module, every time, per connection,
  paid by whoever is shutting down.** `g_poll()` on the cancellable's own
  fd ends on either.
- `stop()`'s own comment recorded this hazard one function over -- "the
  thread is parked in a read nothing else can interrupt" -- and the sleep
  two functions away was the same hazard. The existing test could not see
  it: its fake server **holds the connection open**, and says so in a
  comment, because closing would wake the reader and hide what that test
  was for. True, and it meant the path a close leads to had no test at
  all. A helper documented as avoiding a case is a case nobody covers.

### A module that reads its arguments one way receives nothing from the other caller

- podomation hands a handler's arguments as a positional tuple from the
  DSL and as an `a{sv}` from C. The distrobox module read only the tuple,
  so **every argument from clawtilla was dropped** and each action
  refused for a missing required parameter -- which means
  `computer.type: distrobox` had never worked at all, in any version.
- The refusal names the *argument*, not the marshalling, so it reads as a
  caller that forgot one and sends whoever is looking to the wrong layer.
  The container module had accepted both since it was written, and it is
  the one everything was tested against -- two backends agreeing by
  accident, and the feature demonstrated on the lucky one, for the third
  time in this file.
- Found only because a timeout test was the first thing in this tree ever
  to drive that module end to end.

### GTask pushes its own context around its callback, and that is not a plan

- `clawt_mcp_tools_call_async()`'s comment said its tasks were created on
  the daemon's context "because dispatching a source does not make its
  context thread-default" -- correct about the trap and wrong about the
  code, which pushed nothing. It worked only because
  `g_task_return_now()` pushes the *task's* context around the callback
  it invokes, so the IPC server's async read leaves the daemon's context
  pushed by the time a request arrives. Every caller inherited that by
  luck of how it was reached.
- Measured rather than assumed: an idle callback on a fresh context sees
  a NULL thread-default, and so does one under `g_main_loop_run()`.
- The fourth API this has appeared behind. Name the context; the object
  that owns it is the one that must say so.

### A rule enforced at the caller somebody noticed is a rule about that caller

- "An IPC handler must not wait on the network" has now been applied
  three times to three call sites: the model cache, the daemon's
  autostart loop, and the agent's `clawtilla_computer_exec`. The
  *operator's* `computer.exec` verb sat on the daemon's main context
  throughout, blocking every agent's messages, task delivery and timers
  for up to the advertised 120-second default -- and it is the worse of
  the two exec paths, because a person at a terminal is the caller most
  likely to run something long on purpose and a fleet that hangs while a
  command they can *see* running is still going reads as the fleet being
  broken.
- Fixed by moving the wait into the library rather than into the second
  caller. `clawt_computer_exec_async()` is one implementation of "get off
  the main thread"; two would differ exactly once, on the case nobody
  looked at, which is precisely what had already happened.
- The audit line has to move with the wait. It is written from the
  completion callback so the recorded exit is one something produced --
  and a command that could not be run at all is recorded with `-1`,
  because a trail holding only the successes reads as "it did not
  happen" rather than as "we do not know what it did".
- The test that means anything asserts on an **unrelated timer**. A test
  phrased in terms of the exec cannot tell the two versions apart: the
  answer arrives either way. 10+ ticks of a 50ms source with the fix, 0
  without.

### An agent's persona grows until it cannot start, and nothing says so

- The identity files are concatenated into one system prompt. Past
  `MAX_ARG_STRLEN` -- 32 pages, 131072 bytes *including the NUL*, so
  131071 is the largest word that works -- a backend passing it as an
  argument cannot spawn at all, and the kernel's `Argument list too long`
  names neither the files, the size, nor the limit. `getconf ARG_MAX`
  says 2097152, so the error reads as impossible until you know the
  per-argument cap exists.
- Three things hide it. A **resumed** session is never handed a system
  prompt, so the agent works until something starts a fresh one and the
  symptom appears long after the paragraph that caused it. It is silent
  right up to the cliff. And the growth is **self-inflicted by design**:
  the scaffolded `AGENTS.org` tells the agent to keep `PROJECTS.org`
  current, and `PROJECTS.org` is an identity file.
- So it is measured and shown before anything fails --
  `clawt_workspace_measure_identity()`, on `agent.show`, in both clients,
  and as an `agent.identity` event classified NOTICE so it reaches the
  alert panels. Warned rather than refused: ai-glib spills the prompt to
  a file for claude-code, so refusing would stop an agent that works, and
  the diagnosis for backends that still build an argument belongs where
  the argument is built. What clawtilla can say and nothing below it can
  is *which file* accounts for the size.
- The arithmetic is checked **against libreclaw**, by building the same
  prompt through `lc_agent_context_load_identity()` and comparing byte
  counts. A count of our own that merely looked like the assembly would
  be wrong by a header per file for ever, because the only other thing
  that ever compares them is the kernel -- once, at the cliff.
  `strlen()` rather than the file's size, too: the assembly is a
  `printf`, so an embedded NUL costs less than the bytes on disk.

### A threshold expressed as a float has two values

- `(gsize)(limit * 0.8)` truncates and `total < limit * 0.8` does not, so
  they disagree by one byte -- and a boundary test written against either
  spelling fails against the other for a reason that has nothing to do
  with the feature. That is not hypothetical; it is how the first draft
  of the identity-threshold test failed. A percentage applied with
  integer arithmetic, in one function, is the only version with a single
  answer.

### `make parity`'s affordance layer was satisfied by the stylesheet

- The check globbed `clients/web/*.c`, which includes `web-style.c`. The
  marker for a drawn thing is usually its class name, and the class name
  is in both the renderer and the sheet -- so **eight** of the declared
  rows would have reported OK with the renderer half deleted. A CSS rule
  is not a capability; a class the sheet styles and nothing renders is
  dead CSS.
- Layer 5 reads a corpus with the stylesheet excluded now. Layers 1 to 4
  keep the whole thing, because a stylesheet genuinely does hold library
  vocabulary values, on purpose, in each client's own dialect.
- The same trap one layer along: a *render* test asserting on the bare
  class name passes against a page that drew nothing, because the sheet
  is in the page too. Assert on `class="..."`, which only the element
  emits.

### A client that loses its daemon has four ways to stay lost

- Found in one sitting, all in `ClawtClient`, and each invisible in a
  client whose context happens to be the process default -- which is both
  graphical clients and neither embedded one.
- **The retry timer** went in through `g_timeout_add_seconds()`, which
  attaches to the global default. Dispatching a source pushes nothing, so
  a `handle_disconnect()` reached from the reader had no thread-default
  to inherit either.
- **The reader** was re-armed with `g_data_input_stream_read_line_async()`
  from inside a timeout callback, which captures the *current*
  thread-default -- so a reconnect armed the socket on a loop nobody
  runs. The client reconnected, said hello, and never received another
  line. Same trap as the timer, one function along.
- **`resumed: false` reached nobody.** The daemon replays from a bounded
  ring, so an outage longer than it leaves a hole -- and the only
  consequence was a warning in the journal while the window showed
  pre-outage state indefinitely. `ClawtClient::resync` now says so and
  both clients re-read.
- **`set_auto_reconnect(FALSE)` could not stop a retry already running.**
  The failure path rescheduled unconditionally, and a connect blocks for
  the whole request timeout -- so a caller that said stop was ignored and
  the loop ran for ever, each turn holding its context for another two
  minutes.
- Nothing in either graphical client had ever connected to
  `::disconnected`. The state is drawn in a **banner**, not a toast:
  a toast answers a question somebody is holding right now and then goes,
  and this is a condition the window is *in* until something changes.
- And a fifth, which only driving the real client found. `::disconnected`
  was emitted **before** the retry was scheduled, so
  `clawt_client_is_reconnecting()` answered FALSE inside the very handler
  written to draw it: both clients were told the connection had gone and
  then had nothing to say about it. No test could see it -- one that
  samples the state in a polling loop always samples it after the
  handler has returned. Found by killing a daemon under the real GTK
  client with a `g_message` in the banner and reading
  `reconnecting=0`. The test now records the answer *from inside* the
  handler.
- **A subscriber exists to act on the state, so the state has to be true
  when the signal fires.** Arrange first, announce second.

### The only way out of a broken connection was through a working one

- `clawtilla-gtk` put up an `AdwStatusPage` saying "The daemon is not
  running" when its first connect failed. The page had a header bar with
  **nothing packed into it** -- and the connection menu, the one control
  that reaches every other machine, lives in the *main window's* header,
  which is built only on a successful connect. So a laptop with two
  workstations in `connections.yaml` could not open either of them
  unless it named one on the command line, on the exact screen where
  somebody most needs them.
- The local daemon is the connection **least** likely to matter to a
  client whose whole point is reaching daemons elsewhere, and it was the
  only one that could stand between you and the rest. The window opens
  either way now, and the failure is a banner with a button rather than
  a page instead of the application.
- Three separate things had to be true for that to work, and each was
  its own bug. The retry loop is armed by a connection *going away*, so
  a first connect that failed left the client inert for ever --
  `clawt_client_start_reconnecting()` is the caller saying it wants one,
  kept out of `connect()` because the connection menu uses the same
  function and there a failure must be reported once, not retried behind
  somebody. `clawt_window_request()` toasted every failure, and a window
  makes a dozen requests before it has drawn anything, so the way out
  was buried under a dozen copies of one sentence -- `NOT_CONNECTED` is
  now the banner's alone, the same origin split as the alerts panel.
  And nothing re-read the window when a connection finally arrived:
  events describe what *changes*, so a window that came up empty stays
  empty for ever unless the arrival of the daemon is itself the trigger.
- Verified by driving the real client against a real daemon, because
  none of it is visible from a test: window up with no daemon, banner
  and button correct; daemon started, agent list drawn a second later;
  and the actual reported scenario -- no local daemon, switch to a live
  TCP daemon from the menu, its fleet drawn.

### A context captured only on success is no context at all on failure

- `ClawtClient` took its `GMainContext` at the *bottom* of
  `clawt_client_connect()`, after the socket was up. Harmless for as
  long as a failed first connect was the end of the story. The moment
  such a client started arming a retry, it armed it with a NULL context
  -- so `schedule_reconnect()` fell through to `g_main_context_default()`,
  which for an embedded host is a loop nobody turns. Reported as
  reconnecting for ever and reconnecting never.
- The sixth API in this file behind that trap, and the fix is the one
  that generalises: `ensure_context()` is called from
  `schedule_reconnect()` itself rather than from either caller, because
  **the function that attaches the source is the only one that can be
  wrong**. Naming it at the call sites is what has failed five times.
- Not moved to `_init()`, though that was the first attempt. A client
  may legitimately be built on one thread and connected on another, and
  `/connection/client-reconnects-on-its-own-context` exists precisely to
  pin that -- it constructs outside the pushed context and connects
  inside it, and capturing at construction broke it. A test written to
  catch this class of bug caught the fix for it.

### A subscription is an intent, not a request

- `clawt_client_subscribe()` set `subscribed` only after the reply
  landed, and `try_reconnect()` re-subscribed only `if (self->subscribed)`.
  So a client asked for the event stream while its socket was down
  recorded nothing, reconnected perfectly, and **received nothing for
  ever**: a live connection, an empty fleet, and no event arriving to
  correct it. Worse than a connection that fails, which at least says so.
- Renamed `subscribe_wanted` and recorded *before* the request. Asking
  while the socket happens to be down is a request that fails; it is not
  somebody changing their mind.

### The duplicate did not show as a second connection

- `/connection/start-reconnecting-does-not-stack` asserted the daemon
  accepted exactly one connection, and passed with the guard against
  arming a second timer deliberately removed. Arming twice overwrites
  `reconnect_source` without releasing it, so the first source is leaked
  *and still attached* -- but every extra firing finds the socket already
  up, and `clawt_client_connect()` returns TRUE immediately. The
  duplicate work is a second **subscribe**, not a second connect.
- Found by sabotaging the guard rather than by reading, which is the
  whole reason to do it: asserting on `subscribes` shows 3 against 1.
  Two of four sabotages that session passed, and both times the test was
  measuring something the bug does not touch.

### A test that hangs is worse than one that fails, and a bare "ok" is not a pass

- A reconnect happens from a timeout callback, and the connect inside it
  waits by iterating the same context -- so a reply that never arrives
  runs out the full two-minute request timeout and the test's own
  deadline never gets another turn. Two sabotage runs hung rather than
  failing. A watchdog source that disconnects the client breaks the
  nested wait, and the assertions then fail with the reason.
- The first version of that test needed the daemon's context iterated
  while the client's synchronous connect iterated its own, which cannot
  happen on one thread. It was rewritten against a fake daemon on a
  thread of its own -- the same shape the resync test needed.
- I recorded it as passing on the strength of a bare `ok` in a filtered
  build log. TAP prints `ok <n> <path>`; a lone `ok` is some other
  command's output. **Read the test name, not the word.**

### A flag with one clearer, on a path that guarantees the clearer never runs

- `busy` was set by delivery and cleared by the agent's link reporting
  typing = FALSE. Every route out of RUNNING **closes that link** -- which
  is precisely what guarantees the message that would clear the flag can
  never arrive. So an agent stopped or killed mid-turn stayed "working"
  for the life of the daemon: a live spinner beside a state dot reading
  stopped, one row asserting two contradictory things, and the subtitle
  is the one people read. Two agents on a real fleet sat like that for
  nine hours.
- The clear belongs to the **transition**, not to either stop path. A
  killed agent reaches neither `clawt_daemon_stop_agent()` nor
  `clawt_agent_stop()` -- its runtime reports an unclean exit outside
  STOPPING, so `on_runtime_exited()` takes it to ERROR. `set_state()` is
  the one line every route passes through, and it is the transition that
  makes the turn impossible rather than any particular way of reaching
  it. Both suggested placements were tried and both fail the killed case.
- The predicate names **every** state and has no `default:`, so
  `-Wswitch` fails the build when one is added rather than sweeping it
  into "still working" -- the answer that would reintroduce this bug
  silently for whichever state comes next. Verified by deleting a case
  and watching the warning.

### `g_setenv()` in a test still does not reach an agent's child

- Already recorded here, and a merge request walked into it anyway:
  `FAKE_LIBRECLAW_SLEEP` set with `g_setenv()` reached nothing, so the
  fake exited at once and the test's `kill()` was **racing the fake's own
  exit** for which state the agent would land in -- a clean exit goes to
  STOPPED and the assertion was on ERROR. It won every time on this
  machine and would not have on a loaded one.
- Proved by reading `/proc/<child>/environ` rather than by reasoning: it
  held PATH, HOME, USER, LOGNAME, SHELL, LANG and the XDG entries and
  nothing else. Per-agent `env:` is the route, and with it the variable
  is there.
- **A test that passes five times in a row is not a test that cannot
  race.** The thing to check is whether the mechanism it depends on
  actually works, not how often the result comes out right.

### A comment claiming a user-visible benefit that no client delivers

- "The peer is kept so a stopped agent can still say who its last turn
  was for" was true of the data and false of every surface: both sidebars
  draw the activity only while `busy` is true and fall back to the
  description otherwise. Keeping the peer is still right -- it costs
  nothing and leaves the choice open -- but the comment now says that
  rather than describing a feature nobody would find.

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
- Never let a libvirt domain name no emulator. libvirt fills one in by
  searching the session daemon's PATH, so the binary a guest runs is
  decided by somebody else's environment -- and SELinux refuses one
  under a home directory. Name a system path, or name none at all; never
  name a path that is not there
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
- Never let a client decide whether a name is a team. It can only ask
  `team.list`, which reports the teams somebody *declared*, and an agent
  can be on a team nobody declared -- so the name lands where it matches
  nothing and the folder reaches nobody. The daemon sorts a `who` list
  with `clawt_mount_sort_scope()`, beside the rule it has to agree with
- Never offer a lifecycle verb for a computer type that cannot honour it.
  Ask `clawt_computer_type_has_machine()`; a `host` agent's machine is
  the one clawtilla is running on
- Never build an IPC frame kind with `g_strconcat()` in a client.
  `make parity` reads the kinds each client mentions and cannot see an
  assembled one, so a feature can exist in one client only under a green
  check
- Never add a JSON member whose name is already in that object. json-glib
  keeps the last and drops the first without a word
- Never let a selector that matches nothing stay silent when the entry it
  is on then reaches nobody. `defaults.mounts` ignores an unknown id on
  purpose, and that is right; what is not right is a folder shared with
  nobody while every agent it was meant for starts perfectly
- Never write a config value without dispatching on what the schema says
  it is. A list written as a scalar is accepted, saved, and read back as
  the default, and `computer.host.deny_paths` is one of them
- Never scaffold a file the agent will not read. Two personas in one
  workspace disagree, and the templates are the ones that load
- Never state a relationship between two config keys anywhere but the
  schema. Two private copies of "what an agent calls this fleet option"
  is how nine settings came to be unreachable from every client
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
- Never add a capability to one graphical client and not the other.
  `make parity` fails on it; the exception map is for decisions, not for
  silencing the check
- Never build an element in the web client by appending to a string. The
  typed htmx-glib classes escape; a `g_string_append` does not, and
  everything on that page was written by a person or a model
- Never let the web client fetch anything at page load. It must work on a
  tailnet with no route to the internet, and a script it fetches can
  drive the whole fleet
- Never widen where `clawtilla-web` listens because an address was
  missing. No tailnet means the loopback alone, never every interface
- Never register a route in the web client after `/a/:id/:view`. It
  matches everything under an agent, and a swallowed route renders the
  chat page and answers 200
- Never pass `clawt_web_app_last_error()` to anything that renders. Copy
  it at the point of failure; rendering makes calls that free it
- Never emit a CSS rule for an appearance field somebody left empty.
  Empty means follow the browser, and naming the current value freezes it
  while looking identical
- Never pair `g_autoptr` with a helper that takes the reference. In the
  web client `clawt_web_add()` consumes and `htmx_node_add_child()` does
  not, and the object is freed while still in the tree -- it renders as
  nothing, which looks like CSS
- Never assert a layout with a track count when the number of items in
  it can change. A `display:none` child is not a grid item; place both
  children explicitly
- Never decide whether a test can run by touching real infrastructure.
  A skip-probe that names no connection starts a module against the
  machine's own podman, and `make test` needs none
- Never write a module handler that reads only one argument marshalling.
  podomation sends a positional tuple from the DSL and an `a{sv}` from
  C; reading one means the other caller silently gets nothing
- Never sleep uninterruptibly on a thread something has to join. Poll the
  cancellable's fd, or a stop waits out the whole backoff
- Never write a `switch` over a state enum with a `default:` when the
  default is the permissive answer. Name every value so `-Wswitch` makes
  adding one a build failure rather than a silent inheritance
- Never set an agent child's environment with `g_setenv()` in a test. The
  runtime builds it from an allowlist; use the agent's own `env:` block
  and check `/proc/<child>/environ` if in doubt
- Never let an affordance marker in `make parity` be one the stylesheet
  also contains. A CSS rule is not a capability, and eight declared rows
  were passing on the sheet alone
- Never re-arm an async read, or schedule a timer, without naming the
  context. `g_data_input_stream_read_line_async()` and
  `g_timeout_add_seconds()` both take the ambient thread-default, and a
  reconnect runs from a source dispatch where that is not the client's
- Never let a limit be a float. A threshold has to have exactly one
  value, and `(gsize)(x * 0.8)` and `x * 0.8` are two
- Never write a test that can hang where it could fail. A reconnect that
  waits by iterating its own context takes the whole request timeout with
  it; give the wait a watchdog that breaks it
- Never make a client's route to other daemons depend on one of them
  answering. A window that only exists after a successful connect cannot
  be how somebody fixes a failed one, and the local daemon is the one
  least likely to matter
- Never toast a condition the banner is already holding open. A window
  makes a dozen requests before it draws anything, and a dozen copies of
  "not connected" bury the control that leads out of it
- Never let a client record that it is subscribed only when the
  subscribe succeeded. A reconnect then delivers a live socket that
  receives nothing, which looks like a working connection to an empty
  fleet
- Never capture a main context at the point work succeeds. Capture it
  where a source is attached -- `schedule_reconnect()`, not its callers
  -- because the function doing the attaching is the only one that can
  be wrong about it
- Never guard a shared resource with a probe when the question being
  asked is ownership. A probe answers "did anything reply just now",
  which a busy daemon fails -- and a guard wrong in the permissive
  direction *acts*: this one unlinked a live socket and left a running
  daemon unreachable. `<state_dir>` is one daemon's at a time, and a
  `flock` is what says so
