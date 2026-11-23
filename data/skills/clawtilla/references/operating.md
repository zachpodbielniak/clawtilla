# Operating clawtilla

Building, running, reaching, holding, backing up and repairing a fleet. Source docs: `docs/getting-started.org`, `docs/maintenance.org`, `docs/troubleshooting.org`, `docs/configuration.org`. Every key below is in [config-keys.md](config-keys.md).

Global CLI options go **before** the verb (`clawtilla -s PATH agent list`, not `agent list -s PATH`): `-c/--config`, `-s/--socket`, `-p/--profile`, `-H/--host`, `--port`, `--token`. Full verb list: [cli.md](cli.md).

## Build and install

```bash
git clone --recurse-submodules <url> clawtilla && cd clawtilla
git submodule update --init --recursive     # after any pull that moves deps/libreclaw
make                                         # build/release/; first build compiles libreclaw + 5 deps
make test                                    # hermetic suite
make install-deps INSTALL_DEPS_FLAGS=--print # package list (ostree hosts: layer these into the image)
```

- `make install-deps` refuses an ostree-booted host (prints the list, exits 2) and Enterprise Linux. Flags via `INSTALL_DEPS_FLAGS`: `--build-only`, `--runtime-only`, `--print`, `--dry-run`, `--yes`, `--distro fedora|debian|arch`.
- `glib.h: No such file or directory` means one package in `PKG_DEPS` is missing (pkg-config fails for the whole list).
- Runtime tools only for features that use them: `podman` (container), `distrobox`, `bubblewrap` (bwrap confinement), libvirt + qemu + `qemu-img` + `virtiofsd` + `passt` + `xorriso` (VMs), `Xvfb` (`GSURF=1` browsers). See [computers.md](computers.md).
- The daemon finds `libreclaw` beside `clawtillad`, then `deps/libreclaw/build/release`, then `PATH`. Leave `defaults.libreclaw_binary` unset unless the binary is elsewhere.
- `make install` installs to `PREFIX` (`/usr/local`).

## First run

```bash
mkdir -p ~/.clawtilla
clawtilla --generate-config > ~/.clawtilla/config.yaml   # starter: live choices, rest commented at defaults
clawtillad --foreground                                   # or: clawtilla daemon (same code, in-process)
clawtilla agent create --id researcher --model sonnet --computer none   # creating starts it (--no-start declines)
clawtilla agent list          # STATE running, LINK up (link takes a few seconds; '-' until then)
clawtilla chat researcher     # or: clawtilla send researcher "hello"
```

Config path: `-c PATH`, else `~/.clawtilla/config.yaml`. A missing file loads defaults. The daemon renders `<state_dir>/agents/<id>/config.yaml` from it on every start and reload; that file is generated, edits there are lost. `clawtilla config render <agent>` shows it without writing.

### `clawtillad` options

| Option | Meaning |
|---|---|
| `-c/--config FILE` | config path |
| `-f/--foreground` | stay in foreground, log to stderr |
| `-b/--bind IP[:PORT]` | listen here **instead of** configured network addresses; repeatable; must bind |
| `--no-bind` | unix socket only |
| `-V/--version` | version |

Signals: SIGINT/SIGTERM = clean stop; SIGHUP = reload config. Debug logging: `G_MESSAGES_DEBUG=Clawtilla clawtillad --foreground` (or `daemon.log_level: debug`).

### As a systemd user service

```bash
clawtilla --generate-systemd-service > ~/.config/systemd/user/clawtilla.service
systemctl --user daemon-reload && systemctl --user enable --now clawtilla
loginctl enable-linger $USER          # sockets live in XDG_RUNTIME_DIR; survive logout
journalctl --user -u clawtilla -n 50  # daemon diagnostics (stderr -> journal)
```

The generated unit runs `/usr/local/bin/clawtillad --foreground --config %h/.clawtilla/config.yaml`, `ExecReload` sends HUP, `Restart=on-failure`, `TimeoutStopSec=30`. Edit `ExecStart` when running from a build tree.

## Reaching a daemon from another machine

- `daemon.tailscale` (default true) listens on this machine's tailnet address at `daemon.tcp_port` (8792) when Tailscale is up. `daemon.tcp_enabled` (false) adds `daemon.tcp_address` (127.0.0.1). Every network listener requires a token; TLS via `daemon.tls_cert` / `daemon.tls_key` (without it the token crosses in clear; the daemon warns).
- Owner token: `daemon.token_file`, else generated `<state_dir>/tcp-token`. Print it on the daemon's machine: `clawtilla daemon token` (read from disk; never sent over IPC).
- Saved profiles live in `$XDG_CONFIG_HOME/clawtilla/connections.yaml` (0600), shared by every client.

```bash
# token route
clawtilla remote add workstation 100.72.0.41 --token "$(ssh box clawtilla daemon token)" [--port N] [--tls] [--insecure]
# pairing route (preferred): a per-device token, nothing pasted
clawtilla device pair --name laptop [--qr]          # on the fleet machine: six-digit code, 5 min, single use
clawtilla remote pair workstation 100.72.0.41 123456 [--device-name N]   # on the new device
clawtilla --profile workstation status
clawtilla remote list | rm NAME                     # list never prints tokens
clawtilla device list | revoke ID                   # owner revokes; a device may revoke itself
```

Ad hoc: `clawtilla -H 100.72.0.41 --token T agent list`. Devices idle past `daemon.device_idle_days` (90) expire. Revoking devices never touches the owner token. Rotate the owner token: delete `tcp-token` (only when `token_file` is unset) and restart, then re-add every saved connection. See [security.md](security.md).

## Status and observation

| Command | Answers |
|---|---|
| `clawtilla status` | version + commit, config path, hold/draining, update check, agents (connected), clients, what each connected agent is doing |
| `clawtilla agent list` / `agent show <id>` | state (running, stopped, error, shadow...), queue, link, detail, identity size |
| `clawtilla agent logs <id>` | the libreclaw child's output (errors in the rendered config show here, not in the daemon log) |
| `clawtilla activity` | every running turn: room, elapsed, origin, task, last step |
| `clawtilla event list [subject]` | event history from `events/*.ndjson`; subject = agent, room or task |
| `clawtilla sentinel [--days N] [--output FILE or -]` | read-only scan for repeated tool failures, long queue waits, spend, stalls; writes `<state_dir>/sentinel.yaml` (clients' Activity > Issues) |
| `clawtilla cost --days 7`, `clawtilla limits` | spend; provider plan allowance. See [spending-and-usage.md](spending-and-usage.md) |
| `clawtilla integration health <agent>`, `clawtilla connector list` | pre-flight checks |

Sentinel thresholds: `sentinel.days` (7), `sentinel.min_failures` (3), `sentinel.queue_wait_seconds` (600), `sentinel.daily_spend_micros` (10000000). It reports, never acts.

Update check: `daemon.update_check` (off), `daemon.update_url`, `daemon.update_interval_hours`; `status` shows `update:` and a failing check says so. It never installs anything.

## What a change needs: reload, restart, reset, rebuild

| Change | Do |
|---|---|
| Edited `config.yaml` by hand (or over ssh) | `clawtilla config reload` (also `config edit`, SIGHUP, `systemctl --user reload clawtilla`). Re-reads, re-renders every agent, adds/removes agents, reloads rooms, routines, notifier, skills, broker, egress allowlist. **Does not restart running agents** |
| A running agent should use the re-rendered config | `clawtilla agent restart <id>` |
| `clawtilla agent set <id> <key> <value>` | writes config, re-renders, re-checks shadow at once. If the reply says `restart_required` (running agent and key is `tools.*`, `persona.*`, `chief_of_staff`, `team_role`, `session.routing_mode`, `runtime.confine`, `runtime.confine_paths`, `runtime.network`) follow it |
| Tool grants, chief/lead role, confinement, network mode | `agent restart` (an AI CLI lists tools once per session) |
| `SOUL.org` etc. or `persona.*` | `agent reset <id>`: stops, clears session files and stored sessions, starts again if it was running. A restart only resumes the old session, which never re-reads the prompt |
| Computer config for a stopped machine | `config reload` then `agent start`: the stale computer is rebuilt |
| Devices of a running machine, VM image or seed | `clawtilla computer rebuild <agent>` (cloud-init reads its seed once). See [computers.md](computers.md) |
| `daemon.socket`, `daemon.state_dir`, listeners, TLS, token | daemon restart (reload does not rebind) |
| New clawtilla binary | `make` (not just `make test`), pause, restart daemon, resume |
| New libreclaw | bump submodule, `make clean-all && make`, then `agent restart` one at a time |

A reload or `agent set` can succeed and still leave an agent on its old file: the reply's `refused` array names it (the CLI prints it and exits non-zero). Validate: `clawtilla config validate` (asks the running daemon) or `clawtilla config validate --file candidate.yaml [--strict]` (offline, no daemon, no state). `-c` does not select offline mode.

## Pause and resume (before any restart)

```bash
clawtilla pause [agent]        # stop delivery; running turns finish; nothing is killed
clawtilla status               # wait for "held, nothing in flight -- safe to restart"
systemctl --user restart clawtilla
clawtilla resume [agent]       # queued work goes out in order
```

- Pause is not stop: no process, link or computer is touched; queued work stays queued. The hold and the running set are written to `<state_dir>/hold.yaml`, so a restart brings back exactly what was running (not what `autostart` says); resume is still required.
- `draining` is derived (held and still busy). Resuming one agent while the fleet is held is refused. Pausing twice does not shrink the resume set.
- Stop is destructive by comparison: it kills the turn, and stopping a container computer destroys it unless `computer.container.keep` (default true).

## Backups

Offline, verified archive (stop the daemon first; it takes `daemon.lock` itself):

```bash
systemctl --user stop clawtilla
clawtilla backup create ~/.clawtilla ~/fleet.clawt   # never overwrites an archive
clawtilla backup verify ~/fleet.clawt                # full validation + manifest, writes nothing
clawtilla backup preview ~/fleet.clawt
clawtilla backup restore ~/fleet.clawt ~/restored    # destination must not exist; never touches the live fleet
systemctl --user start clawtilla
```

- Keeps root `config.yaml` (secret references only), workspaces, mailboxes, memories, `tasks.db`, transcripts, events, `routines.yaml`, `revisions.db`. Excludes at every depth: `secrets`, `credentials`, `token`, `tcp-token`, `daemon.lock`, `sessions`, `.git`, `.env`, `.ssh`, `.mcp.json`, and generated `agents/<id>/config.yaml`. Symlinks fail creation. Limits 256 MiB archive, 64 MiB per file; larger fleets use the tar recipe in `docs/maintenance.org`.
- Restored files are 0600/dirs 0700 (restore executable bits). Not encrypted. VM disks, XDG data, external workspaces and secret stores are not included (VMs are rebuilt from image + seed).
- After restoring and pointing the daemon at it: start, `clawtilla config validate`, `clawtilla agent list`; reconnect connectors (`clawtilla connector connect <name>`). Tokens and rendered configs regenerate.
- Alternatives: `clawtilla fleet export <dir>` / `fleet import <dir> --preview` (portable pack, no secrets); `clawtilla history list|show|diff|restore` for config, persona and skill versions.

## Maintenance

- Event log rotates itself (`daemon.event_log_days`, 30; 0 keeps all). Transcripts are never rotated.
- Prune: `clawtilla image vm list` / `image vm rm NAME`; superseded overlays in `$XDG_DATA_HOME/clawtilla/vms/*/overlay-superseded.qcow2`; orphan domains `virsh -c qemu:///session list --all | grep clawt-`.
- Remove an agent: `agent rm <id>` (config entry only; `agent import <id>` brings it back), `--with-computer` (container, or domain and disk), `--purge` (workspace, state dir, transcripts: irreversible). `agent discover` / `agent forget <id>` (moves aside to `.discarded`).
- Rotate an agent's link token: `agent stop`, delete `<state_dir>/agents/<id>/token`, `agent start`.
- Decommission: `clawtilla connector revoke <name>` for each before deleting state.

## Troubleshooting, top items

| Symptom | Check / fix |
|---|---|
| `could not reach the daemon at ...` | `systemctl --user status clawtilla`; right socket (`-s`); `journalctl --user -u clawtilla -n 50` |
| `another clawtilla daemon is using the state directory ... (held by pid N)` | stop that daemon or move all three of `daemon.state_dir`, `daemon.socket`, `defaults.workspace_root` |
| `socket path is N bytes; the kernel accepts at most 107` | shorter `daemon.socket` / `state_dir` |
| Daemon running, nothing connects | socket file unlinked: `ss -xlp \| grep clawtillad` vs `ls -l $XDG_RUNTIME_DIR/clawtilla/daemon.sock`; restart. Check `pgrep -a clawtillad` for a second daemon |
| Messages missing from a transcript | two daemons on one state dir; rebuild from `events/*.ndjson` (`docs/troubleshooting.org`) |
| Agent `shadow` | config not understood; `agent show` says why; fix and `config reload` |
| Agent `error` | retryable; read the detail, fix, `agent start` |
| `Argument list too long` | identity files over 131072 bytes (`agent show <id> \| grep identity`); move old sections out of `persona.identity_files` |
| `no libpod-module-container.so` | `make -C deps/libreclaw pod-modules` or set `daemon.pod_module_dir` |
| `container create failed: Could not connect` | `systemctl --user enable --now podman.socket` |
| `no such image` | `podman pull <image>` (create never pulls) |
| Exits 0 at once, says nothing | orphan libreclaw from a killed daemon: `ps -o pid=,ppid=,cmd= -C libreclaw`, kill the PPID-1 one |
| `cannot find the libreclaw binary` | `make -C deps/libreclaw`, or check `defaults.libreclaw_binary` |
| Running, LINK down | `config render <id> \| grep -A5 clawtilla:`; token file exists; `agent logs`. `refused a connection claiming to be X`: stop, delete token, start |
| Daemon answers nothing, ignores SIGTERM | main thread blocked on podman/libvirt: `gdb -p $(pgrep -x clawtillad) -batch -ex 'thread 1' -ex bt` |
| Fix built, daemon unchanged | `make test` does not relink `clawtillad`; run `make`, restart; `readlink /proc/$(pgrep -x clawtillad)/exe` |
| Corrected config, agent still refuses | `config reload` then `agent start`; running machine needs `computer rebuild` |
| Granted permission, agent says no tool | `agent restart` (tools) or `agent reset` (persona) |

Never `pkill gsurf`, never `pkill -f` the daemon, never `rm -rf` a state dir: stop through the daemon or by pid.
