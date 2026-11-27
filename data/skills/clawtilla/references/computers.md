# Computers: what an agent runs commands on

Sources: `docs/computers.org`, `docs/browser.org`, AGENTS.md ("Computers"). All keys below are agent-relative (`computer.vm.image` inside an `agents[]` entry, `agents.computer.vm.image` in the schema); defaults and types in [config-keys.md](config-keys.md). Agent process confinement (`runtime.confine`, `runtime.network`) is a different thing: [agents.md](agents.md), [security.md](security.md).

## The rule that explains most confusion

An agent's own `bash`/`read`/`write` run **on the host** (as libreclaw's CLI). Only `clawtilla_computer_exec` (and `clawtilla computer exec`) enters the container/VM/remote machine. So every share has two names: the host path (for the agent's own tools) and the guest path (for exec arguments). `TOOLS.org`'s `# BEGIN clawtilla computer` region lists them as `host path = path inside`; the left side is the host's. A file made inside a VM is reachable by the agent's tools only through a share (`/mnt/clawtilla/workspace`, the exchange).

## Types (`computer.type`, default from `defaults.computer` = `none`)

| Type | What it is | Isolation | Owned by clawtilla |
|---|---|---|---|
| `none` | chat only | total | -- |
| `host` | the daemon's machine; needs `computer.host.confirm_host_control: true` | confinement, not isolation | -- |
| `display` | host + a private virtual screen (gowl / Xvnc / Xvfb) for GUI apps; same two host grants | as host | the screen |
| `container` | podman container `clawt-<agent>` via podomation's REST module | namespaces | yes |
| `distrobox` | distrobox box; runs as you, your sockets/display, `distrobox-host-exec` | deliberately porous | yes |
| `vm` | libvirt (default) or qemu guest from a qcow2 | own kernel | yes |
| `ssh` | a machine you already run; `computer.ssh.host` is an alias from `~/.ssh/config` | another computer | **no** |

Desktop control is not a type: it is the `computer.desktop.*` add-on (below). A shared machine is `computers:` + `computer.use` (below). `clawt_computer_type_has_machine()` (container, vm, distrobox) decides which get start/stop/rebuild/`clawtilla_request_rebuild`.

## Exec semantics (every backend)

- **A command, not a shell line.** Each argument is quoted; `>`, `|`, `&&`, `;`, `$VAR` would be literal. The tool (and `computer.exec`'s string form) **refuses** such a line naming the construct; use `bash -c '...'`. Globs are passed through unrefused. The CLI sends an argv array, so `clawtilla computer exec A -- sh -c 'a; b'` works as typed.
- Output capped at 256 KiB per stream (`CLAWT_COMPUTER_MAX_OUTPUT_BYTES`) with a marker.
- `timeout` (tool default 120 s, capped at 3600, 0 = none) abandons the wait on host/container/distrobox/ssh (the command may still run). On `vm` it is only an SSH ConnectTimeout (<= 30 s): bound long commands inside the guest.
- Runs on a worker; the daemon never waits on its main loop.
- `clawtilla computer exec AGENT -- CMD...` exits with the command's own status.

## When the machine starts

`computer.start`: `on_agent_start` (default; a broken image fails the agent at once) or `on_first_use` (container, distrobox, vm, ssh: booted by the first exec, `computer exec`, wait condition or desktop call; concurrent first uses share one boot; the boot is not charged to the command's timeout). `host` always starts with the agent. Reread at every start (no rebuild). A VM desktop is used at agent start by its relay, so `on_first_use` buys little there. `computer status` shows `absent`/`stopped` until first use.

## host

```yaml
computer:
  type: host
  host:
    confirm_host_control: true     # required
    confine: workspace             # none | workspace (default) | allowlist | bwrap
    root: "~/agents/researcher"
    allow_paths: ["~/src"]         # under bwrap these are bound READ-WRITE
    deny_paths: ["~/.ssh", "~/.gnupg"]
    allow_network: true
    allow_sudo: false              # blocks sudo, pkexec, doas, run0, machinectl, su, setpriv (also inside sh -c)
    nice: 5
```

- `none`: nothing; also requires fleet `daemon.allow_unconfined_host: true`, else shadow.
- `workspace` / `allowlist`: argument inspection (realpath, deny first). A guard rail: `python3 -c "open(...)"` gets past it.
- `bwrap`: real namespace built from nothing (`/usr`, `/etc` ro; root+allow_paths rw; mounts at their mode; deny_paths overlaid; `allow_network: false` = `--unshare-net`). Missing bwrap = shadow.
- All of it constrains `clawtilla_computer_exec` only, never the agent's own tools. `computer.host.shell` is read by nothing.
- Mounts on a host are path translation + allowlist grants, not kernel mounts; fleet `defaults.mounts` do not apply to host agents.
- Audit: a person's host exec is in the event log; an agent's own exec is not.

## container

```yaml
computer:
  type: container
  container:
    image: registry.fedoraproject.org/fedora:45   # default via defaults.container_image; name the registry
    connection: unix          # or ssh://, tcp://, a podman named connection
    keep: true                # default: survives stops
    command: null             # default is long-lived; set only for an image whose entrypoint should run
    cpus: "4"                 # cap: four CPUs' worth of time; > 0 and <= host CPUs
    cpu_weight: 200           # share under contention, 1-10000, 100 = default (sent as --cpu-shares)
    cpuset: "0-7"             # pinning; rootless needs the cpuset controller delegated
    pids_limit: 2048          # -1 none, 0 podman default
    memory: 4g                # whole numbers only: 1.5g is refused, write 1536m
    memory_swap: 6g
```

Limits have three levels, most specific wins key by key: the agent's `computer.container.<k>`, its team's `teams[].computer.container.<k>`, the fleet's `defaults.container.<k>` (not `defaults.computer.*`: that is the computer type). `clawtilla agent show A` prints each with its source (`cpus 6 (agent), memory 8g (team forge)`); `clawtilla team set forge computer.container.cpus 4` sets a team's.

- Refused with the key named by `config validate`, `agent set`, `team set` and agent creation: cpus 0 or > host CPUs, cpu_weight outside 1-10000, a cpuset CPU the host lacks, memory < 6m, memory_swap without or below memory (across levels too). A bad value that reaches the daemon anyway is a warning (log, Fleet page, `container_limits.refused`) and that one limit is left out; the agent keeps its computer.
- A change on a running machine is `podman update`d in place (log: `limits applied with podman update (live)`). Where podman refuses it live (cpuset, rootless cgroup v1) the rest still apply and the refused part waits for the next start (`restart required: ...`, also a Fleet page warning).
- Usage is sampled every 30 s, one `podman stats` per connection: `agent show` prints `usage: CPU 212.4%, memory 3.1g of 8.0g, 143 pids`. 95%+ of the memory limit for two samples is a warning. An OOM kill becomes an incident of kind `out-of-memory`: "killed: out of memory at 8g (container clawt-A, 1 process killed)". Do not just retry it: reduce parallelism or ask for more memory.

- Rootless podman needs `systemctl --user enable --now podman.socket`. First start pulls (silent in `agent logs`).
- `computer.container.network` is stored and never sent to podman.
- Limits are applied with `podman update` in place at every start (and immediately when set on a running machine, at any level); clearing a key leaves the last value (set `-1` or rebuild).
- `keep: false`: a stop destroys the container; `computer stop|restart` then require `--remove`.
- `podman rm -f clawt-<agent>` is recoverable: next `agent start` provisions anew. Only a podman 404 means "gone". `computer status` reports remembered state until a start reconciles.
- `clawtilla image` lists suggested images; add your own under `defaults.container_images` (`"ref -- note"`).

## distrobox

`computer.distrobox.{image,name,packages,flags,init,keep,home,share_home}`. Home is private (`$XDG_DATA_HOME/clawtilla/boxes/<id>/home`) unless `share_home: true` (the operator's real home: ssh keys, dotfiles). `image`, `home`, `share_home`, `packages`, `flags`, `init` are fixed at creation: change them with `clawtilla computer rebuild AGENT`. `keep` defaults true. Container limit keys apply. Volumes are `src:dst:ro,z` (comma options). Needs the `distrobox` package.

## vm

```yaml
computer:
  type: vm
  vm:
    backend: libvirt          # qemu fallback shares NOTHING (no virtiofsd)
    uri: qemu:///session
    image: ~/.local/share/clawtilla/images/fedora-45   # required; clawtilla ships none
    cpus: 2                   # defaults 2 / 8192 / 128, sized for a GNOME desktop
    memory_mb: 8192
    disk_gb: 128              # grown at next provision; shrinking refused
    ssh_user: root            # default root: session libvirt maps guest root to you
    packages: [git]           # guest's own package names; one bad name fails the whole install
    snapshot_on_start: false
```

```bash
clawtilla image vm catalog            # fedora-45 (default), fedora-44/43, centos-stream-10, debian-13/12, ubuntu-24.04, arch
clawtilla image vm get fedora-45      # into defaults.image_dir; any https qcow2 URL works too
clawtilla image vm list | rm NAME [--force] | cancel NAME
clawtilla agent set researcher computer.vm.image ~/.local/share/clawtilla/images/fedora-45
```

What a reachable guest needs (each missing one looks like "did not boot"): a disk image; a cloud-init seed labelled `cidata` (key emitted to both `users:` and top-level `ssh_authorized_keys`); an SSH port fixed in the domain XML via `<portForward>` + passt (Fedora `passt`); a virtio `<video>`; `<cpu mode='host-passthrough'/>` (EL10 needs x86-64-v2); an `<emulator>` path; a UUID derived from the name. clawtilla generates the key, port and address itself.

| Setting | Applies |
|---|---|
| `ssh_user`, SSH key, `vm.packages`, `desktop.*` (enabled, flavour, packages, mcp_repo), guest fstab for shares | first boot only -- `computer rebuild` |
| `cpus`, `memory_mb`, `resolution` | next start |
| `disk_gb` | next provision (grow only) |
| `image` | rebuilds the overlay loudly (old one moved aside) |

`computer rebuild` destroys domain, overlay, seed and key (contents gone). Refused while the agent runs and for `type: none`; also works for container/distrobox. A VM deleted in virt-manager is rebuilt at next start (clawtilla asks the hypervisor, never trusts memory). A share added after first boot arrives as a virtiofs device nothing mounts: rebuild, or `clawtilla computer exec A -- mount -t virtiofs <tag> /target`. Only `virtiofs` mounts reach a VM (the default `bind` is rewritten to it; explicit `9p`/`volume`/`tmpfs` get nothing, warned).

An agent cannot rebuild; it can ask with `clawtilla_request_rebuild` (decision `rebuild-<uuid>`, default decline, no deadline; only the exact answer `approve` rebuilds).

## ssh

`computer.ssh.{host,workspace,shell,allow_sudo,connect_timeout,control_persist,wake_command}`. Host keys are never accepted for you; the mount list is the remote allowlist; nothing is copied or shared (no workspace/exchange share). `wake_command` is an argv (not a shell line), run before a command when the host does not answer, once per wave, at most 120 s. clawtilla cannot kill the remote command on timeout.

## display (since 0.51.0)

```yaml
computer:
  type: display
  host: { confirm_host_control: true, confine: bwrap }
  display: { backend: auto, resolution: 1280x800, startup: [firefox] }   # auto = gowl, then xvnc, then xvfb
```

Tools: `clawtilla_display_screenshot`, `_windows`, `_launch` (needs `display.allow_spawn`), `_click`, `_move`, `_type`, `_key`, `_scroll` (need `display.allow_input`); both grants default **on** here. gowl also exposes its own tools via the `clawtilla-desktop` relay. `display.vnc_port` pins Xvnc's 127.0.0.1-only port; `display.gowl_binary` pins a gowl (must answer `--version`). The agent's own shell gets the display's `DISPLAY`/`WAYLAND_DISPLAY`. Files: `<state_dir>/displays/<id>/`.

## Desktop add-on (`computer.desktop.*`)

`enabled` (false), `backend` `auto|gowl|gnome|guest`, `allow_input` (false = observe only), `allow_spawn` (false; compositor spawn bypasses all confinement), `allow_recording` (false), `socket`, `takeover_lease_seconds` (900), `driver_idle_seconds` (120). `gowl`/`gnome` drive **the operator's own screen**; `guest` drives a desktop inside the agent's VM, which cloud-init *builds* (`computer.vm.desktop.{user,autologin,flavour,packages,mcp,mcp_repo}`; flavour `auto|fedora|enterprise|debian|ubuntu|arch`). `auto` picks `guest` when the agent has a VM. `guest` on a non-VM gets no desktop at all (never falls back to the host). Tools reach the CLI through `.mcp.json` (the relay enforces `allow_input`). Inside a guest session start apps with `clawtilla-desktop-run APP`, not `DISPLAY=:0 APP`.

Operator: `clawtilla computer screen|takeover|release AGENT`. While a person holds the screen, agent input is refused (not queued). An agent asks with `clawtilla_request_hands`.

## Mounts, folders, exchange

```yaml
computer:
  mounts:
    - { source: "~/src", target: /work/src, mode: rw, relabel: shared }   # mode default ro
    - { type: tmpfs, target: /scratch, size: 512M }
```

Fields: `source`, `target` (absolute, unique), `mode` (`ro`/`rw`), `type` (`bind`/`volume`/`virtiofs`/`9p`/`tmpfs`), `create`, `relabel` (`none`/`shared` `:z` default/`private` `:Z`), `required`, `size`.

```bash
clawtilla agent mount list|add|rm AGENT ...      # add AGENT SRC TARGET [ro|rw] [--relabel none|shared|private]
clawtilla folders list
clawtilla folders add ~/source [/work/source] [--ro] [--team T] [--agent A] [--relabel WHICH]
clawtilla folders rm /work/source
clawtilla cp ~/notes.org researcher:/mnt/clawtilla/exchange/researcher/   # one side AGENT:PATH; never agent-to-agent
```

- Fleet folders are `defaults.mounts` (scope with `teams:` / `agents:`); reach container, distrobox and VM agents only. An agent's own mount at the same target wins; `computer.default_mounts: false` declines all.
- Every change applies at the computer's next start ("takes effect at the next start").
- A relabel walks every file under the source at every start; rootless podman cannot relabel files you do not own, so such a mount is **refused** on add (use `relabel: none`, which on SELinux means visible but unreadable -- mount a copy you own).
- Validation does not stop a source inside `daemon.state_dir` or `secrets.dir`: check by hand.
- Every computer that shares host paths gets `/mnt/clawtilla/workspace` (rw; off with `computer.workspace: false`) and `/mnt/clawtilla/exchange` (off with `computer.exchange: false`): `shared/` rw for all, `<agent-id>/` rw for its owner and ro for others. Host side is `defaults.exchange_dir`; `exchange_max_bytes` is never enforced (empty it yourself). On a shared machine workspaces are `/mnt/clawtilla/workspaces/<agent>`.

## SELinux levels on shared folders

The trap: podman gives each container a random MCS pair (`s0:c827,c890`), and a file **created inside** a container gets that pair. On a folder several containers share rw (`~/source` with `relabel: shared`), a file one agent writes from inside its container is `Permission denied` inside every other one -- at mode 0644. It looks like permissions; it is labelling. Host-side writes (the agent's own Read/Write/Edit tools) come out as plain `s0` and are fine.

```yaml
computer:
  container:
    selinux_level: auto        # default: the team's stored pair; no team = podman's random pair
    # selinux_level: random    # podman's random pair, always (the old behaviour)
    # selinux_level: s0:c100,c200   # explicit: one or two categories c0..c1023, ascending, no leading zeros
```

- `disable` / `label=disable` is refused: it would turn SELinux off for the container. Any other bad value disables the agent at load; `config validate` errors.
- A team's pair is picked the first time its container is created -- distinct from every other team's and from every explicit level in the config -- and kept in `tasks.db` (`team_selinux_levels`) under the state dir: it survives restarts and rebuilds. A shared machine under `auto` takes its agents' team if they share one, else random.
- Set only at container **create**. Changing the key, an agent's `team` under auto, or `computer.use` needs the container recreated: `agent set` and `config reload` say so (`recreate`); a stopped agent's next start recreates it; a running one needs stop + start, or stop + `clawtilla computer rebuild AGENT`. Nothing is removed silently; the log names current and wanted level.
- `config validate`, the daemon log and the web Fleet page warn when two containers share a folder (same source, or nested) at different levels with at least one rw. random vs random always warns. Same team under auto, ro-only, and one shared machine don't.
- Old files written inside containers keep their old pair: on the host, `chcon -R -l s0 PATH` (`ls -Z` shows which).
- Limitation: `exchange/shared` is rw for all agents; a file written into it from **inside** a container is readable only at the same level. Write to the exchange from the host instead. Not covered by the warning.

## Shared computers

```yaml
computers:
  - { id: alpha, description: the build box, type: container, container: { image: registry.fedoraproject.org/fedora:45 } }
agents:
  - { id: a, computer: { use: alpha } }
```

One object for every user, named `clawt-c.<id>` in podman, distrobox and libvirt (a dot no agent id can contain); an agent's own `computer.*` keys are ignored (and refused by `agent set`) while `use` is set. `use` naming nothing or a `host` computer = shadow. Stop/restart/rebuild refused while another user runs; `rm --with-computer` never destroys it; one desktop driver at a time. `clawtilla computer shared` lists them.

## Lifecycle verbs

```bash
clawtilla computer status AGENT
clawtilla computer start|stop|restart AGENT [--remove]
clawtilla computer rebuild AGENT            # agent must be stopped
clawtilla computer exec AGENT -- CMD ...
```

Stopping a computer never deletes on an "I could not tell" answer. A computer is rebuilt from config at the next start after `agent set`/reload, but **not while the machine runs** (that would destroy what the agent installed).

## Browser sessions

Build with `make GSURF=1` (gsurf >= 0.5.14, `Xvfb`); otherwise a browser agent is a shadow. `browser.mode`: `none` (default; `clawtilla_web_fetch` only), `own`, `shared` (with `browser.use` naming a `browsers:` entry, which supplies `home`, `allow_scripts`, `idle_minutes`, `width`, `height`). Session = gsurf on a private Xvfb, profile under `<state_dir>/browsers/`, started on first call, stopped after `idle_minutes` (15). Independent of `computer.*`. One driver per shared session (2 min lease, `clawtilla_browser_release`). `browser.allow_scripts` (false) gates `clawtilla_browser_eval`. `browser.secrets: [vault names]` grants `clawtilla_secret_ref` + `clawtilla_browser_fill_secret` (value never seen by the agent; filled only on the entry's `sites`). Never `pkill gsurf` (it is the operator's browser).

```bash
clawtilla browser                         # sessions
clawtilla browser open|stop AGENT [URL]   ; clawtilla browser click AGENT X Y
clawtilla browser screenshot AGENT FILE   ; clawtilla browser secrets AGENT
clawtilla agent set AGENT browser.secrets github,github-2fa
```
